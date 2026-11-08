"""Package a release's committed source and tested Windows executables."""

import argparse
import gzip
import hashlib
import io
import json
import os
import re
import struct
import subprocess
import sys
import sysconfig
import tarfile
import tomllib
import zipfile
from pathlib import Path

import output_layout
from build_version import validate_version

ROOT = Path(__file__).resolve().parents[2]
PYTHON_VERSIONS = ('3.11', '3.12', '3.13', '3.14', '3.15')
WINDOWS_WHEEL_PLATFORMS = {
    'x86': 'win32',
    'x86_64': 'win_amd64',
    'arm64': 'win_arm64',
}
LINUX_WHEEL_ARCHITECTURES = ('x86_64', 'i686', 'aarch64')


def interpreter() -> dict[str, str]:
    """Validate the ordinary CPython wheel ABI and describe its build paths."""
    version: str = '.'.join(map(str, sys.version_info[:2]))
    if (
        sys.implementation.name != 'cpython'
        or version not in PYTHON_VERSIONS
        or struct.calcsize('P') not in (4, 8)
        or sysconfig.get_config_var('Py_GIL_DISABLED')
    ):
        raise ValueError(
            "Wheels require ordinary CPython " + ', '.join(PYTHON_VERSIONS)
        )
    wheel_platform: str = sysconfig.get_platform().replace('-', '_').replace('.', '_')
    if sys.platform == 'win32':
        # The wheel platform describes this interpreter's ABI, including x86/x64
        # interpreters running under emulation on an ARM64 host.
        architecture: str = {
            platform: arch for arch, platform in WINDOWS_WHEEL_PLATFORMS.items()
        }[wheel_platform]
    else:
        architecture = output_layout.architecture()

    # Keep the existing x64 SDK reusable, while other Windows ABIs get separate
    # installations even when built on the same host for the same Python version.
    sdk_name: str = 'boost_python' + version.replace('.', '')
    if sys.platform == 'win32' and architecture != 'x86_64':
        sdk_name += '_' + architecture
    return {
        'version': version,
        'tag': 'cp' + version.replace('.', ''),
        'executable': sys.executable,
        'prefix': sys.base_prefix,
        'include': sysconfig.get_path('include'),
        'architecture': architecture,
        'wheel_platform': wheel_platform,
        'boost_name': sdk_name,
        'wheel_directory': f'lib/pylockd/output/{architecture}/opt/static',
        'build_directory': (
            tomllib.loads((ROOT / 'pyproject.toml').read_text(encoding='utf-8'))[
                'tool'
            ]['scikit-build']['build-dir']
            + '/release'
        ),
    }


def versions(root: Path) -> tuple[str, str]:
    """Read the shared native and Python version from the source tree."""
    version: str = validate_version(
        (root / 'cmake/version.txt').read_text(encoding='utf-8').strip(),
        development=True,
    )
    return version, version


def validate_tag(version: str, tag: str | None) -> None:
    """Reject tags that would attach binaries to a different daemon version."""
    if tag is not None:
        validate_version(version)
    if tag is not None and tag != f'v{version}':
        raise ValueError(f"Tag {tag!r} must match daemon version v{version}")


def source_archive(root: Path, destination: Path, version: str) -> Path:
    """Export HEAD, excluding build state; never include the Git repository itself."""
    destination.mkdir(parents=True, exist_ok=True)
    archive: Path = destination / f'lockd-{version}.tar.gz'
    work: Path = output_layout.directories(root, output_layout.architecture())[
        'staging'
    ]
    work.mkdir(parents=True, exist_ok=True)
    temporary: Path = work / 'source.tar'
    try:
        subprocess.run(
            [
                'git',
                'archive',
                '--format=tar',
                f'--prefix=lockd-{version}/',
                f'--output={temporary}',
                'HEAD',
                '--',
                '.',
                ':(exclude)output/**',
                ':(exclude)**/output/**',
                ':(exclude).private/**',
                ':(exclude)platform/freebsd/dist/**',
                ':(exclude)platform/freebsd/ports/**/distinfo',
            ],
            cwd=root,
            check=True,
        )
        # Only generated version metadata differs from HEAD. The source archive is
        # self-contained: CMake and the wheel backend both read this same file.
        validate_version(version, development=True)
        with (
            tarfile.open(temporary) as source,
            archive.open('wb') as target,
            gzip.GzipFile(
                filename='', mode='wb', fileobj=target, mtime=0
            ) as compressed,
            tarfile.open(fileobj=compressed, mode='w') as packaged,
        ):
            found_version: bool = False
            for member in source:
                contents = source.extractfile(member) if member.isfile() else None
                if member.name == f'lockd-{version}/cmake/version.txt':
                    data: bytes = (version + '\n').encode('utf-8')
                    member.size = len(data)
                    contents = io.BytesIO(data)
                    found_version = True
                packaged.addfile(member, contents)
            if not found_version:
                raise ValueError('Committed source has no cmake/version.txt')
    finally:
        temporary.unlink(missing_ok=True)
    return archive


def windows_archive(
    root: Path, destination: Path, version: str, architecture: str = 'x86_64'
) -> Path:
    """Keep executable names stable inside the versioned distribution ZIP."""
    cache = root / f'output/obj/{architecture}/opt/static/CMakeCache.txt'
    settings: dict[str, str] = {}
    for line in cache.read_text(encoding='utf-8').splitlines():
        if line and not line.startswith(('#', '//')) and '=' in line:
            declaration, value = line.split('=', 1)
            settings[declaration.split(':', 1)[0]] = value
    installed = Path(settings['VCPKG_INSTALLED_DIR'])
    triplet = settings['VCPKG_TARGET_TRIPLET']
    files: dict[str, Path] = {
        'lockd.exe': root / f'output/{architecture}/opt/static/lockd.exe',
        'lockd_test.exe': root
        / (f'utility/lockd_test/output/{architecture}/opt/static/lockd_test.exe'),
        'license.txt': root / 'lib/pylockd/license.txt',
        'boost_license.txt': installed / triplet / 'share/boost-asio/copyright',
    }
    for file in files.values():
        if not file.is_file():
            raise FileNotFoundError(file)
    destination.mkdir(parents=True, exist_ok=True)
    archive: Path = destination / f'lockd-{version}-windows-{architecture}-portable.zip'
    with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED) as packaged:
        for name, file in files.items():
            packaged.write(file, name)
    return archive


def release_asset_name(name: str) -> str:
    """Normalize auditwheel's optional extra platform tags for asset-set validation."""
    if not name.endswith('.whl'):
        return name
    parts: list[str] = name[:-4].split('-')
    if len(parts) != 5:
        raise ValueError(f"Unexpected wheel filename: {name}")
    platforms: set[str] = set(parts[4].split('.'))
    baselines: set[str] = platforms & {
        'manylinux_2_28_x86_64',
        'manylinux_2_28_i686',
        'manylinux_2_28_aarch64',
    }
    if len(baselines) == 1:
        parts[4] = baselines.pop()
    return '-'.join(parts) + '.whl'


def python_wheel_name() -> str:
    """Read the distribution name using the wheel format's normalized separators."""
    project_name: str = tomllib.loads(
        (ROOT / 'pyproject.toml').read_text(encoding='utf-8')
    )['project']['name']
    return re.sub(r'[-_.]+', '_', project_name).lower()


def checksums(destination: Path, version: str, python_version: str) -> None:
    """Require the complete first-release asset set before writing its checksums."""
    expected: set[str] = {
        f'lockd-{version}.tar.gz',
    }
    for architecture in WINDOWS_WHEEL_PLATFORMS:
        expected.add(f'lockd-{version}-windows-{architecture}-portable.zip')
        expected.add(f'lockd-{version}-windows-{architecture}-setup.exe')
    wheel_name: str = python_wheel_name()
    for supported in PYTHON_VERSIONS:
        tag: str = 'cp' + supported.replace('.', '')
        for platform in WINDOWS_WHEEL_PLATFORMS.values():
            expected.add(f'{wheel_name}-{python_version}-{tag}-{tag}-{platform}.whl')
        for architecture in LINUX_WHEEL_ARCHITECTURES:
            expected.add(
                f'{wheel_name}-{python_version}-{tag}-{tag}-'
                f'manylinux_2_28_{architecture}.whl'
            )

    # Checked-out output directories contain a tracked placeholder. It is not a release
    # asset; keep rejecting every other unexpected entry, including other hidden files.
    files: list[Path] = [
        file
        for file in destination.iterdir()
        if file.name not in ('.gitkeep', 'SHA256SUMS')
    ]
    actual: set[str] = {release_asset_name(file.name) for file in files}
    if actual != expected or len(files) != len(expected):
        raise ValueError(f"Release assets differ: expected {expected}, got {actual}")
    lines: list[str] = []
    for name in sorted(file.name for file in files):
        with (destination / name).open('rb') as file:
            digest: str = hashlib.file_digest(file, 'sha256').hexdigest()
        lines.append(f'{digest}  {name}\n')
    (destination / 'SHA256SUMS').write_text(
        ''.join(lines), encoding='utf-8', newline='\n'
    )


def main() -> None:
    """Expose packaging steps identically to local shells and GitHub Actions."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        'operation',
        choices=('metadata', 'interpreter', 'source', 'windows', 'checksums'),
    )
    parser.add_argument('--tag')
    parser.add_argument(
        '--architecture', choices=tuple(WINDOWS_WHEEL_PLATFORMS), default='x86_64'
    )
    parser.add_argument('--output', type=Path)
    arguments = parser.parse_args()
    if arguments.operation == 'interpreter':
        print(json.dumps(interpreter()))
        return
    version, python_version = versions(ROOT)
    if os.environ.get('LOCKD_RELEASE_VERSION'):
        version = validate_version(
            os.environ['LOCKD_RELEASE_VERSION'], development=True
        )
        python_version = version
    validate_tag(version, arguments.tag)
    arch: str = (
        arguments.architecture
        if arguments.operation == 'windows'
        else output_layout.architecture()
    )
    destination: Path = (
        arguments.output
        or output_layout.directories(ROOT, arch, linkage='static')['release']
    ).resolve()
    destination.relative_to(ROOT / 'output')
    match arguments.operation:
        case 'metadata':
            print(f'version={version}')
            print(f'python_version={python_version}')
            print(f'python_matrix={json.dumps(PYTHON_VERSIONS)}')
        case 'source':
            print(source_archive(ROOT, destination, version))
        case 'windows':
            print(windows_archive(ROOT, destination, version, arguments.architecture))
        case 'checksums':
            checksums(destination, version, python_version)


if __name__ == '__main__':
    main()

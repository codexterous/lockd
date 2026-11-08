"""Prepare cached manylinux Boost libraries for one Python ABI."""

import hashlib
import os
import shutil
import subprocess
import sys
import sysconfig
import tarfile
import urllib.request
from pathlib import Path

import output_layout
import package

BOOST_HASH = '3621533e820dcab1e8012afd583c0c73cf0f77694952b81352bf38c1488f9cb4'
DEPENDENCIES = (
    output_layout.directories(package.ROOT, output_layout.architecture())[
        'dependencies'
    ]
    / 'release'
)


def run(*arguments: str, cwd: Path = package.ROOT) -> None:
    """Run a build command, preserving its output and failure status."""
    subprocess.run(arguments, cwd=cwd, check=True)


def boost_source() -> Path:
    """Download and verify the same unmodified Boost archive used on Windows."""
    DEPENDENCIES.mkdir(parents=True, exist_ok=True)
    verified: Path = DEPENDENCIES / '.boost-source-verified'
    verified.unlink(missing_ok=True)
    archive: Path = DEPENDENCIES / 'boost_1_88_0.tar.gz'
    if not archive.exists():
        urllib.request.urlretrieve(
            'https://archives.boost.io/release/1.88.0/source/boost_1_88_0.tar.gz',
            archive,
        )
    with archive.open('rb') as stream:
        if hashlib.file_digest(stream, 'sha256').hexdigest() != BOOST_HASH:
            raise ValueError("Boost archive checksum mismatch")

    # Keep verified downloads even if extraction or compilation fails later.
    verified.write_text('verified\n', encoding='utf-8')
    source: Path = DEPENDENCIES / 'boost_1_88_0'
    if not source.exists():
        with tarfile.open(archive) as compressed:
            compressed.extractall(DEPENDENCIES, filter='data')
    return source


def build_boost(prefix: Path, version: str) -> None:
    """Build shared Boost.Python; Linux also needs a daemon for wheel tests."""
    if os.environ.get('LOCKD_BOOST_CACHE_HIT') == 'true':
        required: list[Path] = [
            prefix / 'include/boost/version.hpp',
            prefix / 'LICENSE_1_0.txt',
        ]
        for component in (
            f'python{version.replace(".", "")}',
            'log',
            'program_options',
            'filesystem',
        ):
            required.append(prefix / f'lib/libboost_{component}.so')
        if not all(path.is_file() for path in required):
            raise RuntimeError('The restored Boost cache is incomplete')
        print('Using cached Boost wheel dependencies', flush=True)
        return

    # A failed rebuild must not reuse a previous attempt's completion marker.
    ready: Path = prefix.parent / '.boost-cache-ready'
    ready.unlink(missing_ok=True)
    source: Path = boost_source()
    config: Path = prefix.parent / f'python{version}.jam'
    config.write_text(
        f'using python : {version} : "{sys.executable}" : '
        f'"{sysconfig.get_path("include")}" : "{sysconfig.get_config_var("LIBDIR")}" ;\n',
        encoding='utf-8',
    )
    if not (source / 'b2').exists():
        run('sh', 'bootstrap.sh', cwd=source)
    components: list[str] = [
        '--with-python',
        '--with-log',
        '--with-program_options',
        '--with-filesystem',
    ]
    run(
        str(source / 'b2'),
        *components,
        f'--user-config={config}',
        f'python={version}',
        'variant=release',
        'link=shared',
        'runtime-link=shared',
        'threading=multi',
        'cxxflags=-fPIC',
        '-j2',
        f'--build-dir={prefix.parent / (prefix.name + "_build")}',
        f'--prefix={prefix}',
        'install',
        cwd=source,
    )
    shutil.copy2(source / 'LICENSE_1_0.txt', prefix / 'LICENSE_1_0.txt')
    ready.write_text('ready\n', encoding='utf-8')

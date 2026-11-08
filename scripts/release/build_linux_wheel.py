"""Build one manylinux wheel with Boost.Python matching its interpreter."""

import argparse
import os
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

import build_boost_linux
import output_layout
import package
import test_linux_wheel


def run(*arguments: str, cwd: Path = package.ROOT) -> None:
    """Run a build command, preserving its output and failure status."""
    subprocess.run(arguments, cwd=cwd, check=True)


def build_wheel(prefix: Path, version: str) -> Path:
    """Build the extension and stage its dependency license notice."""
    details: dict[str, str] = package.interpreter()
    work: Path = (
        output_layout.directories(package.ROOT, details['architecture'])['build']
        / 'python-release'
        / details['tag']
    )
    bundle: Path = work / 'bundle'
    bundle.mkdir(parents=True, exist_ok=True)
    shutil.copy2(prefix / 'LICENSE_1_0.txt', bundle / 'boost_license.txt')

    environment: Path = work / 'build_env'
    run(sys.executable, '-m', 'venv', '--without-pip', str(environment))
    python: str = str(environment / 'bin/python')
    run(
        sys.executable,
        '-m',
        'pip',
        '--python',
        python,
        'install',
        'build==1.6.1',
        'scikit-build-core==1.1.1',
        'ninja>=1.5',
    )

    # Non-isolated builds need the backend's tools in this environment, including
    # when CMake invokes Ninja or we build the test daemon afterward.
    os.environ['PATH'] = str(environment / 'bin') + os.pathsep + os.environ['PATH']
    raw: Path = work / 'raw'
    run(
        python,
        '-m',
        'build',
        '--wheel',
        '--no-isolation',
        '--outdir',
        str(raw),
        '-Ccmake.define.LOCKD_DEPENDENCY_PROVIDER=SYSTEM',
        f'-Ccmake.define.Boost_ROOT={prefix}',
        '-Ccmake.define.Boost_NO_BOOST_CMAKE=ON',
        f'-Ccmake.define.Python3_EXECUTABLE={sys.executable}',
        f'-Ccmake.define.LOCKD_PYTHON_DEPENDENCY_LICENSE={bundle / "boost_license.txt"}',
        '-Ccmake.define.LOCKD_CLANG_TIDY=OFF',
        '-Ccmake.define.LOCKD_CLANG_TIDY_WARNINGS_AS_ERRORS=ON',
        f'-Cbuild-dir={details["build_directory"]}',
    )
    wheels: list[Path] = list(raw.glob('*.whl'))
    if len(wheels) != 1:
        raise ValueError(f"Expected one wheel in {raw}")
    return wheels[0]


def build_daemon(prefix: Path) -> None:
    """Build the Linux test daemon separately from the wheel's embedded client."""
    build: Path = (
        output_layout.directories(package.ROOT, output_layout.architecture())['build']
        / 'wheel_daemon'
    )
    run(
        'cmake',
        '-S',
        str(package.ROOT),
        '-B',
        str(build),
        '-G',
        'Ninja',
        '-DCMAKE_BUILD_TYPE=Release',
        '-DBUILD_SHARED_LIBS=ON',
        '-DLOCKD_STATIC_RUNTIME=OFF',
        '-DLOCKD_BUILD_PYTHON=OFF',
        '-DBUILD_TESTING=OFF',
        '-DLOCKD_CLANG_TIDY=OFF',
        '-DLOCKD_DEPENDENCY_PROVIDER=SYSTEM',
        f'-DBoost_ROOT={prefix}',
    )
    run('cmake', '--build', str(build), '--target', 'lockd', '--parallel', '2')


def main() -> None:
    """Build and repair a wheel; leave installation testing to a clean process."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        '--manylinux', choices=package.LINUX_WHEEL_ARCHITECTURES, required=True
    )
    arguments = parser.parse_args()
    if sys.platform != 'linux':
        raise RuntimeError("Run this script in a manylinux build container")
    version: str = package.interpreter()['version']
    prefix: Path = (
        build_boost_linux.DEPENDENCIES / f'boost_python{version.replace(".", "")}'
    )
    build_boost_linux.build_boost(prefix, version)
    wheel: Path = build_wheel(prefix, version)
    destination: Path = package.ROOT / package.interpreter()['wheel_directory']
    destination.mkdir(parents=True, exist_ok=True)
    os.environ['LD_LIBRARY_PATH'] = str(prefix / 'lib')
    run(
        'auditwheel',
        'repair',
        '--plat',
        f'manylinux_2_28_{arguments.manylinux}',
        '--wheel-dir',
        str(destination),
        str(wheel),
    )
    build_daemon(prefix)
    test_built_wheel(prefix, arguments.manylinux)


def test_built_wheel(prefix: Path, architecture: str) -> None:
    """Hide the build's Boost.Python while retaining libraries for the test daemon."""
    daemon_libs: Path = (
        output_layout.directories(package.ROOT, output_layout.architecture())[
            'verifications'
        ]
        / 'wheel_daemon_libs'
    )
    daemon_libs.mkdir(parents=True, exist_ok=True)
    for library in (prefix / 'lib').glob('*.so*'):
        if not library.name.startswith(('libboost_python', 'libboost_numpy')):
            shutil.copy2(library, daemon_libs / library.name)
    arch: str = {'aarch64': 'arm64', 'i686': 'x86'}.get(architecture, architecture)
    daemon: Path = package.ROOT / f'output/{arch}/opt/shared/lockd'
    launcher: Path = daemon_libs.parent / 'wheel_daemon.sh'
    launcher.write_text(
        '#!/bin/sh\n'
        + f'export LD_LIBRARY_PATH={shlex.quote(str(daemon_libs))}\n'
        + f'exec {shlex.quote(str(daemon))} "$@"\n',
        encoding='utf-8',
    )
    launcher.chmod(0o755)

    hidden: Path = prefix.with_name(prefix.name + '_unavailable')
    prefix.rename(hidden)
    try:
        test_linux_wheel.test_wheel(
            package.ROOT / package.interpreter()['wheel_directory'], launcher
        )
    finally:
        hidden.rename(prefix)


if __name__ == '__main__':
    main()

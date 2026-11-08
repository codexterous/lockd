"""Install a Linux wheel and test it without using build-tree libraries."""

import argparse
import os
import platform
import re
import subprocess
import sys
from pathlib import Path

import output_layout
import package


def check_dependencies(output: str, environment: Path) -> None:
    """Reject missing libraries and Boost or client libraries outside the wheel."""
    if 'not found' in output:
        raise RuntimeError(f"Unresolved wheel dependency:\n{output}")
    for line in output.splitlines():
        match = re.match(r'\s*(\S+)\s+=>\s+(\S+)', line)
        if (
            match
            and match[1].startswith(('libboost_', 'libliblockd'))
            and not Path(match[2]).resolve().is_relative_to(environment.resolve())
        ):
            raise RuntimeError(f"Wheel uses an external project dependency: {line}")


def test_wheel(distributions: Path, daemon: Path) -> None:
    """Run the installed extension in a fresh venv away from source and build outputs."""
    tag: str = package.interpreter()['tag']
    architecture: str = platform.machine()
    wheels: list[Path] = list(
        distributions.glob(f'*-{tag}-{tag}-*manylinux_2_28_{architecture}*.whl')
    )
    if len(wheels) != 1:
        raise ValueError(
            f"Expected one {tag} manylinux wheel for {architecture}: {wheels}"
        )
    work: Path = (
        package.ROOT
        / f'output/obj/{output_layout.architecture()}/opt/shared/verifications/linux_wheel/{tag}'
    )
    work.mkdir(parents=True, exist_ok=True)
    environment: Path = work / 'env'
    python: str = str(environment / 'bin/python')
    subprocess.run(
        [sys.executable, '-m', 'venv', '--clear', '--without-pip', str(environment)],
        check=True,
    )
    subprocess.run(
        [
            sys.executable,
            '-m',
            'pip',
            '--python',
            python,
            'install',
            '--no-index',
            '--no-deps',
            str(wheels[0].resolve()),
        ],
        check=True,
    )
    clean: dict[str, str] = dict(os.environ)
    for name in ('LD_LIBRARY_PATH', 'LD_PRELOAD', 'PYTHONPATH', 'PYTHONHOME'):
        clean.pop(name, None)
    clean['LOCKD_TEST_DAEMON'] = str(daemon.resolve())
    temporary: Path = work / 'tmp'
    temporary.mkdir(exist_ok=True)
    clean['TMPDIR'] = str(temporary)
    module: Path = Path(
        subprocess.check_output(
            [python, '-I', '-c', 'import pylockd; print(pylockd.__file__)'],
            cwd=work,
            env=clean,
            text=True,
        ).strip()
    )
    if not module.resolve().is_relative_to(environment.resolve()):
        raise RuntimeError(f"Imported extension outside test environment: {module}")
    dependencies: str = subprocess.check_output(
        ['ldd', str(module)],
        env=clean,
        text=True,
    )
    print(dependencies, flush=True)
    check_dependencies(dependencies, environment)
    subprocess.run(
        [
            python,
            '-I',
            '-m',
            'unittest',
            'discover',
            '-s',
            str(package.ROOT / 'lib/pylockd/tests'),
            '-v',
        ],
        cwd=work,
        env=clean,
        check=True,
    )


def main() -> None:
    """Test matching wheels with the daemon selected by the caller."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--distributions', type=Path, required=True)
    parser.add_argument('--daemon', type=Path, required=True)
    arguments = parser.parse_args()
    test_wheel(arguments.distributions, arguments.daemon)


if __name__ == '__main__':
    main()

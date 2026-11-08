"""Select the shared build and release directories for project-owned outputs."""

import argparse
import json
import os
import platform
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ARCHITECTURES = ('x86', 'x86_64', 'arm64')


def architecture(machine: str | None = None, pointer_bytes: int | None = None) -> str:
    """Normalize the target process, including 32-bit Python on a 64-bit host."""
    machine = (platform.machine() if machine is None else machine).lower()
    pointer_bytes = struct.calcsize('P') if pointer_bytes is None else pointer_bytes
    if machine in ('arm64', 'aarch64'):
        if pointer_bytes != 8:
            raise ValueError("Only 64-bit ARM builds are supported")
        return 'arm64'
    if machine in ('amd64', 'x86_64', 'x64', 'x86', 'i386', 'i486', 'i586', 'i686'):
        return 'x86' if pointer_bytes == 4 else 'x86_64'
    raise ValueError(f"Unsupported architecture: {machine}")


def directories(
    root: Path,
    arch: str,
    profile: str = 'opt',
    linkage: str = 'shared',
    component: str = '',
) -> dict[str, Path]:
    """Keep component outputs separate while sharing host-tool caches."""
    if arch not in ARCHITECTURES or profile not in ('dbg', 'opt'):
        raise ValueError("Choose x86, x86_64 or arm64 and a dbg or opt profile")
    if linkage not in ('static', 'shared'):
        raise ValueError("Choose static or shared linkage")
    component_path = Path(component)
    if component_path.is_absolute() or '..' in component_path.parts:
        raise ValueError("The component must be a relative path within the project")
    output = root / component_path / 'output'
    host_build = Path(
        os.environ.get('LOCKD_HOST_BUILD_DIR')
        or root / 'output/obj' / architecture() / 'opt/shared'
    )
    return {
        'build': output / 'obj' / arch / profile / linkage,
        'release': output / arch / profile / linkage,
        'dependencies': host_build / 'dependencies',
        'verifications': host_build / 'verifications',
        'staging': host_build / 'release',
        'source': host_build / 'source',
    }


def main() -> None:
    """Print paths for Python, PowerShell, shell, or workflow callers."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--architecture', choices=ARCHITECTURES, default=architecture())
    parser.add_argument('--profile', choices=('dbg', 'opt'), default='opt')
    parser.add_argument('--linkage', choices=('static', 'shared'), default='shared')
    parser.add_argument('--component', default='')
    parser.add_argument('--format', choices=('json', 'env'), default='json')
    arguments = parser.parse_args()
    paths = directories(
        arguments.root.resolve(),
        arguments.architecture,
        arguments.profile,
        arguments.linkage,
        arguments.component,
    )
    if arguments.format == 'json':
        print(json.dumps({key: str(value) for key, value in paths.items()}))
    else:
        for key, value in paths.items():
            print(f'LOCKD_{key.upper()}_DIR={value.as_posix()}')


if __name__ == '__main__':
    main()

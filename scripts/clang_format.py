"""Format the whole project's C and C++ sources with the repository's style."""

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def source_files(root: Path) -> list[Path]:
    """Include existing and new sources without walking ignored build trees."""
    names = subprocess.check_output(
        [
            'git',
            'ls-files',
            '--cached',
            '--others',
            '--exclude-standard',
            '-z',
            '--',
            '*.c',
            '*.cpp',
            '*.h',
            '*.hpp',
        ],
        cwd=root,
    )
    paths = {Path(os.fsdecode(name)) for name in names.split(b'\0') if name}

    # Git can still list a deleted file. Do not follow source symlinks into another
    # checkout or a dependency installation when formatting in place.
    return sorted(
        path
        for path in paths
        if (root / path).is_file() and not (root / path).is_symlink()
    )


def find_formatter(requested: str | None) -> str:
    """Use an explicit tool, standalone Windows LLVM, or clang-format on PATH."""
    if requested:
        executable = shutil.which(requested)
    else:
        executable = None
        program_files = os.environ.get('ProgramFiles')
        if os.name == 'nt' and program_files:
            installed = Path(program_files) / 'LLVM/bin/clang-format.exe'
            if installed.is_file():
                executable = str(installed)
        executable = executable or shutil.which('clang-format')
    if not executable:
        raise FileNotFoundError(
            "Install clang-format 20 or newer on PATH, or pass --clang-format PATH"
        )

    version = subprocess.check_output([executable, '--version'], text=True).strip()
    match = re.search(r'\bversion\s+(\d+)', version)
    if not match or int(match[1]) < 20:
        raise ValueError(f"clang-format 20 or newer is required; found {version}")
    print(f"Using {executable}: {version}", flush=True)
    return executable


def main() -> int:
    """Format every selected file, or check formatting without modifying files."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true', help='check without editing')
    parser.add_argument('--clang-format', metavar='PATH', help='formatter executable')
    arguments = parser.parse_args()
    try:
        executable = find_formatter(arguments.clang_format)
        files = source_files(ROOT)
        flags = ['--dry-run', '--Werror'] if arguments.check else ['-i']
        action = 'Checking' if arguments.check else 'Formatting'
        print(f"{action} all {len(files)} project C/C++ source files...", flush=True)

        # One file per invocation also works when a Windows checkout's complete
        # source list would exceed the native command-line length limit.
        for path in files:
            subprocess.run(
                [executable, '--style=file', *flags, '--', str(path)],
                cwd=ROOT,
                check=True,
            )
        print(f"{action} completed for {len(files)} files.", flush=True)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())

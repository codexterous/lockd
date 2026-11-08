"""Select the runner's installed LLVM for Windows CI without installing tools."""

import os
import subprocess
from pathlib import Path


def main() -> None:
    """Log the installed tool and export it only after its version command succeeds."""
    # Avoid the older LLVM bundled with Visual Studio, which can appear on PATH.
    # This export affects this CI job only; local CMake discovery stays unchanged.
    executable = Path(os.environ['ProgramFiles']) / 'LLVM/bin/clang-tidy.exe'
    if not executable.is_file():
        raise FileNotFoundError(f'Preinstalled clang-tidy is missing: {executable}')

    print(f'Using preinstalled clang-tidy: {executable}', flush=True)
    subprocess.run([str(executable), '--version'], check=True)
    with Path(os.environ['GITHUB_ENV']).open('a', encoding='utf-8') as environment:
        environment.write(f'LOCKD_CI_CLANG_TIDY={executable}\n')


if __name__ == '__main__':
    main()

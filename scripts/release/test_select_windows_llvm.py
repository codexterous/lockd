"""Check Windows CI tool selection without installing LLVM or changing local tools."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import select_windows_llvm as llvm


class WindowsLLVMSelectionTests(unittest.TestCase):
    """Export only an installed tool that can report its version."""

    def test_selection_exports_installed_tool_only_after_success(self) -> None:
        """Missing and broken tools fail; a working tool preserves existing CI exports."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            executable = root / 'LLVM/bin/clang-tidy.exe'
            environment = root / 'environment'
            environment.write_text('EXISTING=value\n', encoding='utf-8')
            with (
                patch.dict(
                    os.environ,
                    {'ProgramFiles': str(root), 'GITHUB_ENV': str(environment)},
                ),
                patch.object(llvm.subprocess, 'run') as command,
            ):
                with self.assertRaises(FileNotFoundError):
                    llvm.main()
                command.assert_not_called()
                executable.parent.mkdir(parents=True)
                executable.touch()
                command.side_effect = subprocess.CalledProcessError(7, 'clang-tidy')
                with self.assertRaises(subprocess.CalledProcessError):
                    llvm.main()
                self.assertEqual(environment.read_text(), 'EXISTING=value\n')

                command.side_effect = None
                llvm.main()
                command.assert_called_with([str(executable), '--version'], check=True)
                self.assertEqual(
                    environment.read_text(),
                    f'EXISTING=value\nLOCKD_CI_CLANG_TIDY={executable}\n',
                )

"""Check Windows signing preparation and rejection of unsigned payloads offline."""

import io
import os
import shutil
import subprocess
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path

import package


@unittest.skipUnless(
    os.name == 'nt' and shutil.which('tar'), 'Windows and tar required'
)
class WindowsSigningTests(unittest.TestCase):
    """Run production PowerShell against isolated archives without contacting Azure."""

    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.scripts = self.root / 'scripts/release'
        self.scripts.mkdir(parents=True)
        for name in ('prepare_windows_signing.ps1', 'verify_windows_signature.ps1'):
            shutil.copyfile(
                package.ROOT / 'scripts/release' / name, self.scripts / name
            )
        self.assets = self.root / 'output/x86_64/opt/static'
        self.assets.mkdir(parents=True)
        self.source_archive('1.0.4')
        for arch in ('x86', 'x86_64', 'arm64'):
            with zipfile.ZipFile(
                self.assets / f'lockd-1.0.4-windows-{arch}-portable.zip', 'w'
            ) as archive:
                for name in (
                    'lockd.exe',
                    'lockd_test.exe',
                    'license.txt',
                    'boost_license.txt',
                ):
                    archive.writestr(name, b'unsigned fixture')

    def source_archive(self, version: str) -> None:
        """Only version metadata is needed by the preparation operation."""
        with tarfile.open(self.assets / 'lockd-1.0.4.tar.gz', 'w:gz') as archive:
            contents = version.encode()
            member = tarfile.TarInfo('lockd-1.0.4/cmake/version.txt')
            member.size = len(contents)
            archive.addfile(member, io.BytesIO(contents))

    def run_script(self, name: str, *arguments: str) -> subprocess.CompletedProcess:
        """Bound the native script invocation and retain diagnostics for assertions."""
        return subprocess.run(
            [
                'powershell',
                '-NoProfile',
                '-NonInteractive',
                '-ExecutionPolicy',
                'Bypass',
                '-File',
                str(self.scripts / name),
                *arguments,
            ],
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )

    def test_all_architectures_prepare_and_unsigned_payload_cannot_rebuild(
        self,
    ) -> None:
        """Prepare six binaries, then require verification before installer compilation."""
        result = self.run_script(
            'prepare_windows_signing.ps1', '-Operation', 'Prepare', '-Version', '1.0.4'
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        payload = self.root / 'output/obj/x86_64/opt/shared/signing/payload'
        self.assertEqual(len(list(payload.rglob('*.exe'))), 6)
        result = self.run_script(
            'prepare_windows_signing.ps1', '-Operation', 'Rebuild', '-Version', '1.0.4'
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Signature verification failed', result.stderr)
        self.assertFalse(list(self.root.rglob('*-setup.exe')))

    def test_unexpected_portable_entry_is_rejected_before_extraction(self) -> None:
        """Do not sign unexpected executable names or extract paths outside the payload."""
        with zipfile.ZipFile(
            self.assets / 'lockd-1.0.4-windows-x86-portable.zip', 'a'
        ) as archive:
            archive.writestr('../unexpected.exe', b'fixture')
        result = self.run_script(
            'prepare_windows_signing.ps1', '-Operation', 'Prepare', '-Version', '1.0.4'
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Unexpected portable payload', result.stderr)
        self.assertFalse(list(self.root.rglob('unexpected.exe')))

    def test_mismatched_source_version_is_rejected(self) -> None:
        """An installer must use the recipe belonging to its binaries' release."""
        self.source_archive('1.0.5')
        result = self.run_script(
            'prepare_windows_signing.ps1', '-Operation', 'Prepare', '-Version', '1.0.4'
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('source version does not match', result.stderr)

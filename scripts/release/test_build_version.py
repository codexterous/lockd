"""Check push version assignment without creating remote tags or spending CI time."""

import json
import os
import re
import shutil
import subprocess
import tempfile
import tomllib
import unittest
from pathlib import Path
from unittest.mock import patch

import build_version
import output_layout
import package


class BuildVersionTests(unittest.TestCase):
    """Keep pushes, retries, release metadata, and the native build in agreement."""

    def test_sequence_and_rollover(self) -> None:
        """The build field stays below 100; minor versions compare numerically."""
        self.assertEqual(build_version.next_version([]), '1.0.1')
        self.assertEqual(build_version.next_version(['1.0.1']), '1.0.2')
        self.assertEqual(build_version.next_version(['1.0.99']), '1.1.0')
        self.assertEqual(build_version.next_version(['1.9.99', '1.10.1']), '1.10.2')
        for version in ('1.0.100', '01.0.1', '1.0', '1.0.-1', '1.0.1.dev0'):
            with self.subTest(version=version), self.assertRaises(ValueError):
                build_version.validate_version(version)

    def test_retry_and_release_reuse_the_commit_assignment(self) -> None:
        """A retry, later push of the same commit, or release never allocates again."""
        for allocate in (False, True):
            with (
                patch.object(
                    build_version, 'read_assignments', return_value={'1.0.1': 'commit'}
                ),
                patch.object(build_version.subprocess, 'run') as create,
            ):
                self.assertEqual(
                    build_version.resolve_version(
                        'owner/repo', 'commit', allocate=allocate
                    ),
                    '1.0.1',
                )
                create.assert_not_called()

    def test_unassigned_non_push_build_is_development_only(self) -> None:
        """Pull requests and manual checks cannot consume a release number."""
        with (
            patch.object(build_version, 'read_assignments', return_value={}),
            patch.object(build_version.subprocess, 'run') as create,
        ):
            self.assertEqual(
                build_version.resolve_version('owner/repo', 'commit', allocate=False),
                '1.0.1.dev0',
            )
            create.assert_not_called()
        with self.assertRaises(ValueError):
            package.validate_tag('1.0.1.dev0', 'v1.0.1.dev0')

    def test_push_allocates_only_a_lightweight_tag(self) -> None:
        """The only write points a tag at the existing commit; no commit is created."""
        with (
            patch.object(build_version, 'read_assignments', return_value={}),
            patch.object(
                build_version.subprocess,
                'run',
                return_value=subprocess.CompletedProcess([], 0),
            ) as create,
        ):
            self.assertEqual(
                build_version.resolve_version('owner/repo', 'commit', allocate=True),
                '1.0.1',
            )
            self.assertEqual(
                create.call_args.args[0],
                [
                    'gh',
                    'api',
                    '--method',
                    'POST',
                    'repos/owner/repo/git/refs',
                    '-f',
                    'ref=refs/tags/build/v1.0.1',
                    '-f',
                    'sha=commit',
                ],
            )

    def test_competing_push_reserves_next_number(self) -> None:
        """A ref collision retries against current assignments instead of overwriting."""
        occupied: dict[str, str] = {'1.0.1': 'other'}
        with (
            patch.object(
                build_version, 'read_assignments', side_effect=[{}, occupied, occupied]
            ),
            patch.object(
                build_version.subprocess,
                'run',
                side_effect=[
                    subprocess.CompletedProcess(
                        [], 1, stderr='Reference already exists'
                    ),
                    subprocess.CompletedProcess([], 0),
                ],
            ) as create,
        ):
            self.assertEqual(
                build_version.resolve_version('owner/repo', 'commit', allocate=True),
                '1.0.2',
            )
            self.assertIn('ref=refs/tags/build/v1.0.2', create.call_args.args[0])

    def test_lost_response_recovers_existing_assignment(self) -> None:
        """A successful creation with a lost response must not allocate a second tag."""
        assigned: dict[str, str] = {'1.0.1': 'commit'}
        with (
            patch.object(
                build_version, 'read_assignments', side_effect=[{}, assigned, assigned]
            ),
            patch.object(
                build_version.subprocess,
                'run',
                return_value=subprocess.CompletedProcess(
                    [], 1, stderr='Connection lost'
                ),
            ),
        ):
            self.assertEqual(
                build_version.resolve_version('owner/repo', 'commit', allocate=True),
                '1.0.1',
            )

    def test_api_errors_and_duplicate_assignments_fail(self) -> None:
        """Permission failures and conflicting metadata must block numbered builds."""
        with (
            patch.object(build_version, 'read_assignments', return_value={}),
            patch.object(
                build_version.subprocess,
                'run',
                return_value=subprocess.CompletedProcess([], 1, stderr='Forbidden'),
            ),
            self.assertRaisesRegex(RuntimeError, 'Forbidden'),
        ):
            build_version.resolve_version('owner/repo', 'commit', allocate=True)
        with (
            patch.object(
                build_version,
                'read_assignments',
                return_value={'1.0.1': 'commit', '1.0.2': 'commit'},
            ),
            self.assertRaisesRegex(ValueError, 'Multiple build versions'),
        ):
            build_version.resolve_version('owner/repo', 'commit', allocate=True)

    def test_assignment_tags_are_validated(self) -> None:
        """Build tags must have bounded versions and point directly at commits."""
        for version, kind in (
            ('1.0.1', 'commit'),
            ('1.0.100', 'commit'),
            ('1.0.1', 'tag'),
        ):
            result = subprocess.CompletedProcess(
                [],
                0,
                stdout=json.dumps(
                    [
                        {
                            'ref': f'refs/tags/build/v{version}',
                            'object': {'type': kind, 'sha': 'abc'},
                        }
                    ]
                ),
            )
            with patch.object(build_version.subprocess, 'run', return_value=result):
                if version == '1.0.1' and kind == 'commit':
                    self.assertEqual(
                        build_version.read_assignments('owner/repo'), {'1.0.1': 'abc'}
                    )
                else:
                    with self.assertRaises(ValueError):
                        build_version.read_assignments('owner/repo')

    def test_python_metadata_reads_shared_version(self) -> None:
        """Wheel metadata uses the same file as CMake, including development suffixes."""
        settings = tomllib.loads((package.ROOT / 'pyproject.toml').read_text())
        self.assertIn('version', settings['project']['dynamic'])
        providers: list[dict] = [
            entry
            for entry in settings['tool']['dynamic-metadata']
            if entry['field'] == 'version'
        ]
        self.assertEqual(len(providers), 1)
        metadata: dict = providers[0]
        self.assertEqual(metadata['provider'], 'scikit_build_core.metadata.regex')
        self.assertEqual(metadata['input'], 'cmake/version.txt')
        for version in ('1.0.1', '1.0.1.dev0', '1.1.0'):
            self.assertEqual(
                re.search(metadata['regex'], version + '\n')['value'], version
            )

    @unittest.skipUnless(
        os.environ.get('CMAKE_COMMAND') or shutil.which('cmake'),
        'CMake is required for generated version checks',
    )
    def test_cmake_versions_match_python_and_reject_build_overflow(self) -> None:
        """CMake generates all native components' constants from the same metadata."""
        temporary_root: Path = (
            package.ROOT
            / f'output/obj/{output_layout.architecture()}/opt/shared/verifications'
        )
        temporary_root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=temporary_root) as temporary:
            root: Path = Path(temporary)
            for name in ('lockd_version.cmake', 'lockd_build_version.hpp.in'):
                shutil.copyfile(package.ROOT / 'cmake' / name, root / name)
            driver: Path = root / 'check.cmake'
            driver.write_text(
                'cmake_minimum_required(VERSION 3.24)\n'
                'include("${CMAKE_CURRENT_LIST_DIR}/lockd_version.cmake")\n'
                'configure_file("${CMAKE_CURRENT_LIST_DIR}/lockd_build_version.hpp.in"\n'
                ' "${CMAKE_CURRENT_LIST_DIR}/version.hpp" @ONLY)\n',
                encoding='utf-8',
            )
            for version in (
                '1.0.1.dev0',
                '1.0.99',
                '1.1.0',
                '1.1.0',
                '1.0.100',
                '01.0.1',
            ):
                (root / 'version.txt').write_text(version + '\n', encoding='utf-8')
                result = subprocess.run(
                    [
                        os.environ.get('CMAKE_COMMAND') or shutil.which('cmake'),
                        '-P',
                        str(driver),
                    ],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                if version in ('1.0.100', '01.0.1'):
                    self.assertNotEqual(result.returncode, 0)
                else:
                    self.assertEqual(
                        result.returncode, 0, result.stdout + result.stderr
                    )
                    header: str = (root / 'version.hpp').read_text()
                    native_version: str = version.replace('.dev0', '-dev')
                    self.assertIn(
                        f'#define LOCKD_VERSION_STRING "{native_version}"', header
                    )
                    self.assertIn(
                        f'#define BUILD_NUMBER {version.split(".")[2]}\n', header
                    )

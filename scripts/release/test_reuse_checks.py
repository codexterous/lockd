"""Check per-job release reuse without contacting GitHub or starting paid jobs."""

import json
import os
import re
import tempfile
import unittest
from pathlib import Path
from unittest.mock import mock_open, patch

import reuse_checks
import reuse_part
from reuse_part import reusable_run


class ReuseChecksTests(unittest.TestCase):
    """Failed or absent parts rerun without discarding other successful results."""

    def test_selector_reads_the_action_download_for_the_output_architecture(
        self,
    ) -> None:
        """An i686 wheel job uses x86 artifacts even with a 64-bit controller Python."""
        action = (
            Path(__file__).resolve().parents[2]
            / '.github/actions/reuse_compatibility/action.yml'
        ).read_text()
        download = re.search(r'name: compatibility_reuse\s*\n\s*path: ([^\n]+)', action)
        self.assertIsNotNone(download)
        for target in ('x86', 'x86_64', 'arm64', ''):
            with (
                self.subTest(target=target),
                tempfile.TemporaryDirectory() as temporary,
            ):
                root = Path(temporary)
                relative = download[1].replace(
                    '${{ env.OUTPUT_ARCH }}', target or 'x86_64'
                )
                directory = root / relative
                directory.mkdir(parents=True)
                (directory / 'reuse_plan.json').write_text(
                    json.dumps(
                        {
                            'commit': 'abc123',
                            'version': '1.0.1',
                            'jobs': {
                                'wheel': {'run': '42', 'artifacts': ['wheel-package']}
                            },
                            'dependencies': {},
                        }
                    ),
                    encoding='utf-8',
                )
                output = root / 'github-output.txt'
                summary = root / 'github-summary.md'
                with (
                    patch.object(reuse_part.Path, 'cwd', return_value=root),
                    patch.object(
                        reuse_part.output_layout, 'architecture', return_value='x86_64'
                    ),
                    patch.object(
                        reuse_part.subprocess, 'check_output', return_value='abc123\n'
                    ),
                    patch.dict(
                        os.environ,
                        OUTPUT_ARCH=target,
                        REUSE_VERSION='1.0.1',
                        REUSE_JOB='wheel',
                        REUSE_ARTIFACT='wheel-package',
                        REUSE_DEPENDENCIES='',
                        GITHUB_OUTPUT=str(output),
                        GITHUB_STEP_SUMMARY=str(summary),
                        GITHUB_SERVER_URL='https://github.com',
                        GITHUB_REPOSITORY='owner/lockd',
                    ),
                ):
                    reuse_part.main()
                self.assertEqual(output.read_text(), 'run=42\nreused=true\n')
                self.assertIn('owner/lockd/actions/runs/42', summary.read_text())

    def run_record(self, **changes: object) -> dict:
        """Make a completed push for the exact selected commit."""
        record: dict = {
            'id': 42,
            'head_sha': 'abc123',
            'event': 'push',
            'head_repository': {'full_name': 'owner/lockd'},
            'status': 'completed',
            'conclusion': 'success',
        }
        record.update(changes)
        return record

    def artifacts(self, version: str = '1.0.1', expired: bool = False) -> list[dict]:
        """Supply matching version evidence and a Windows package."""
        return [
            {
                'artifacts': [
                    {'name': f'build_version_{version}', 'expired': False},
                    {'name': 'release_windows_x86', 'expired': expired},
                ]
            }
        ]

    def test_failed_run_preserves_successful_parts(self) -> None:
        """A Windows failure cannot invalidate the completed Linux analysis."""
        with patch.object(
            reuse_checks,
            'api_pages',
            side_effect=[
                [{'workflow_runs': [self.run_record(conclusion='failure')]}],
                self.artifacts(),
                [
                    {
                        'jobs': [
                            {'name': 'clang-tidy / Linux', 'conclusion': 'success'},
                            {'name': 'clang-tidy / Windows', 'conclusion': 'failure'},
                        ]
                    }
                ],
            ],
        ):
            plan: dict = reuse_checks.reuse_plan('owner/lockd', 'abc123', '1.0.1')
        self.assertEqual(reusable_run(plan, 'clang-tidy / Linux', '', []), '42')
        self.assertEqual(reusable_run(plan, 'clang-tidy / Windows', '', []), '')
        self.assertEqual(reusable_run(plan, 'unmatched job', '', []), '')

    def test_newer_failed_part_does_not_reuse_older_success(self) -> None:
        """Inspect every part's newest result, even across partially successful runs."""
        with patch.object(
            reuse_checks,
            'api_pages',
            side_effect=[
                [
                    {
                        'workflow_runs': [
                            self.run_record(id=43, conclusion='failure'),
                            self.run_record(),
                        ]
                    }
                ],
                self.artifacts(),
                [{'jobs': [{'name': 'Windows', 'conclusion': 'failure'}]}],
                self.artifacts(),
                [
                    {
                        'jobs': [
                            {'name': 'Windows', 'conclusion': 'success'},
                            {'name': 'Linux', 'conclusion': 'success'},
                        ]
                    }
                ],
            ],
        ):
            plan: dict = reuse_checks.reuse_plan('owner/lockd', 'abc123', '1.0.1')
        self.assertEqual(reusable_run(plan, 'Windows', '', []), '')
        self.assertEqual(reusable_run(plan, 'Linux', '', []), '42')

    def test_active_runs_block_planning(self) -> None:
        """Queued, running and approval-pending checks must not be duplicated."""
        for status in ('queued', 'in_progress', 'waiting', 'pending', 'requested'):
            with (
                self.subTest(status=status),
                patch.object(
                    reuse_checks,
                    'api_pages',
                    return_value=[{'workflow_runs': [self.run_record(status=status)]}],
                ) as api,
            ):
                with self.assertRaisesRegex(RuntimeError, 'actions/runs/42'):
                    reuse_checks.reuse_plan('owner/lockd', 'abc123', '1.0.1')
                api.assert_called_once()

    def test_wrong_commit_fork_or_pull_request_is_not_reuse_evidence(self) -> None:
        """Only this repository's push checks can establish release reuse."""
        for changes in (
            {'head_sha': 'other'},
            {'event': 'pull_request'},
            {'head_repository': {'full_name': 'fork/lockd'}},
        ):
            with (
                self.subTest(changes=changes),
                patch.object(
                    reuse_checks,
                    'api_pages',
                    return_value=[{'workflow_runs': [self.run_record(**changes)]}],
                ) as api,
            ):
                self.assertEqual(
                    reuse_checks.reuse_plan('owner/lockd', 'abc123', '1.0.1')['jobs'],
                    {},
                )
                api.assert_called_once()

    def test_wrong_version_does_not_reuse_jobs(self) -> None:
        """The same commit built with development metadata is not a release build."""
        with patch.object(
            reuse_checks,
            'api_pages',
            side_effect=[
                [{'workflow_runs': [self.run_record()]}],
                self.artifacts('1.0.1.dev0'),
            ],
        ) as api:
            self.assertEqual(
                reuse_checks.reuse_plan('owner/lockd', 'abc123', '1.0.1')['jobs'], {}
            )
            self.assertEqual(api.call_count, 2)

    def test_package_job_needs_its_nonexpired_artifact(self) -> None:
        """Successful tests cannot supply a missing or expired portable archive."""
        job: str = 'Compatibility Testing - Windows / x86 (opt, static)'
        for expired in (False, True):
            with (
                self.subTest(expired=expired),
                patch.object(
                    reuse_checks,
                    'api_pages',
                    side_effect=[
                        [{'workflow_runs': [self.run_record()]}],
                        self.artifacts(expired=expired),
                        [{'jobs': [{'name': job, 'conclusion': 'success'}]}],
                    ],
                ),
            ):
                plan: dict = reuse_checks.reuse_plan('owner/lockd', 'abc123', '1.0.1')
            self.assertEqual(
                reusable_run(plan, job, 'release_windows_x86', []),
                '' if expired else '42',
            )
            self.assertEqual(reusable_run(plan, job, 'absent', []), '')

    def test_downstream_tests_must_cover_the_reused_package_inputs(self) -> None:
        """Rebuilt or different-run packages require fresh distribution tests."""
        for origin, available in (
            ('42', True),
            ('42', False),
            ('43', True),
            ('', True),
        ):
            plan: dict = {
                'jobs': {
                    'distro': {'run': '42', 'artifacts': []},
                    'wheel': {
                        'run': origin,
                        'artifacts': [],
                        'package_available': available,
                    },
                }
            }
            self.assertEqual(
                reusable_run(plan, 'distro', '', ['wheel']),
                '42' if origin == '42' and available else '',
            )
        self.assertEqual(reusable_run(plan, 'distro', '', ['missing']), '')

    def test_no_prior_runs_reruns_each_part(self) -> None:
        """A release without historical evidence must execute its normal jobs."""
        with patch.object(
            reuse_checks, 'api_pages', return_value=[{'workflow_runs': []}]
        ):
            self.assertEqual(
                reuse_checks.reuse_plan('owner/lockd', 'abc123', '1.0.1')['jobs'], {}
            )

    def test_service_checks_reuse_only_the_tested_windows_package(self) -> None:
        """Keep service results only while their architecture's input is reusable."""
        for arch in ('x86', 'x86_64', 'arm64'):
            for expired in (False, True):
                job: str = f'Service Testing - Windows / {arch}'
                native: str = f'Compatibility Testing - Windows / {arch} (opt, static)'
                artifacts: list[dict] = self.artifacts(expired=expired)
                artifacts[0]['artifacts'][1]['name'] = f'release_windows_{arch}'
                with (
                    self.subTest(arch=arch, expired=expired),
                    patch.object(
                        reuse_checks,
                        'api_pages',
                        side_effect=[
                            [{'workflow_runs': [self.run_record()]}],
                            artifacts,
                            [
                                {
                                    'jobs': [
                                        {'name': name, 'conclusion': 'success'}
                                        for name in (job, native)
                                    ]
                                }
                            ],
                        ],
                    ),
                ):
                    plan: dict = reuse_checks.reuse_plan(
                        'owner/lockd', 'abc123', '1.0.1'
                    )
                dependencies: list[str] = plan['dependencies'][f'windows_{arch}']
                self.assertEqual(dependencies, [native])
                self.assertEqual(
                    reusable_run(plan, job, '', dependencies), '' if expired else '42'
                )

    def test_plan_must_match_worker_commit_and_version(self) -> None:
        """A downloaded plan cannot silently apply to another checkout or version."""
        for commit, version in (('other', '1.0.1'), ('abc123', '1.0.2')):
            with (
                self.subTest(commit=commit, version=version),
                patch.object(
                    reuse_part.Path,
                    'read_text',
                    return_value=json.dumps({'commit': commit, 'version': version}),
                ),
                patch.object(
                    reuse_part.subprocess, 'check_output', return_value='abc123\n'
                ),
                patch.dict(reuse_part.os.environ, {'REUSE_VERSION': '1.0.1'}),
                patch.object(reuse_part.Path, 'open', mock_open()) as output,
            ):
                with self.assertRaises(ValueError):
                    reuse_part.main()
                output.assert_not_called()

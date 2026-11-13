"""Exercise run deletion boundaries without contacting GitHub."""

import subprocess
import unittest
from unittest.mock import call, patch

import cleanup_workflow_runs as cleanup


class CleanupWorkflowRunsTests(unittest.TestCase):
    """Only older failed or cancelled runs in the successful run's scope are removed."""

    def run_record(self, **changes: object) -> dict:
        """Build a workflow API response with stable run ordering."""
        record: dict = {
            'id': 42,
            'run_number': 12,
            'workflow_id': 7,
            'path': '.github/workflows/compatibility.yml',
            'head_branch': 'main',
            'head_repository': {'full_name': 'owner/lockd'},
            'event': 'push',
            'status': 'completed',
            'conclusion': 'success',
            'updated_at': '2026-10-27T12:00:00Z',
        }
        record.update(changes)
        return record

    def failure(self, **changes: object) -> dict:
        """Create a superseded failure, with optional exclusions under test."""
        return self.run_record(
            **({'id': 40, 'run_number': 10, 'conclusion': 'failure'} | changes)
        )

    def test_api_uses_paginated_reads_and_explicit_deletes(self) -> None:
        """Exercise the CLI boundary without making network requests."""
        with patch.object(
            cleanup.subprocess,
            'run',
            side_effect=[
                subprocess.CompletedProcess([], 0, '[{"workflow_runs": []}]'),
                subprocess.CompletedProcess([], 0, ''),
            ],
        ) as run:
            self.assertEqual(
                cleanup.api('runs', paginate=True), [{'workflow_runs': []}]
            )
            self.assertIsNone(cleanup.api('runs/40', delete=True))
        self.assertEqual(
            run.call_args_list,
            [
                call(
                    ['gh', 'api', 'runs', '--paginate', '--slurp'],
                    check=True,
                    capture_output=True,
                    text=True,
                ),
                call(
                    ['gh', 'api', 'runs/40', '--method', 'DELETE'],
                    check=True,
                    capture_output=True,
                    text=True,
                ),
            ],
        )

    def test_deletes_failures_and_cancellations_across_pages_after_rechecking(
        self,
    ) -> None:
        """Pagination finishes before deletion; every candidate gets a fresh lookup."""
        first: dict = self.failure()
        second: dict = self.failure(id=41, run_number=11, conclusion='cancelled')
        with patch.object(
            cleanup,
            'api',
            side_effect=[
                self.run_record(),
                [{'workflow_runs': [first]}, {'workflow_runs': [second]}],
                first,
                None,
                second,
                None,
            ],
        ) as api:
            self.assertEqual(cleanup.cleanup('owner/lockd', 42), [40, 41])
        self.assertEqual(
            api.call_args_list,
            [
                call('repos/owner/lockd/actions/runs/42'),
                call(
                    'repos/owner/lockd/actions/workflows/7/runs?branch=main&status=completed&per_page=100',
                    paginate=True,
                ),
                call('repos/owner/lockd/actions/runs/40'),
                call('repos/owner/lockd/actions/runs/40', delete=True),
                call('repos/owner/lockd/actions/runs/41'),
                call('repos/owner/lockd/actions/runs/41', delete=True),
            ],
        )

    def test_release_success_only_cleans_release_failures(self) -> None:
        """Release cleanup cannot remove Compatibility runs or another release ref."""
        release: dict = {
            'workflow_id': 8,
            'path': '.github/workflows/release.yml',
            'event': 'workflow_dispatch',
        }
        failure: dict = self.failure(**release)
        with patch.object(
            cleanup,
            'api',
            side_effect=[
                self.run_record(**release),
                [
                    {
                        'workflow_runs': [
                            failure,
                            self.failure(),
                            self.failure(**(release | {'head_branch': 'v1.0.1'})),
                        ]
                    }
                ],
                failure,
                None,
            ],
        ) as api:
            self.assertEqual(cleanup.cleanup('owner/lockd', 42), [40])
        self.assertIn('/workflows/8/runs?', api.call_args_list[1].args[0])
        self.assertEqual(api.call_count, 4)

    def test_push_and_manual_success_clean_each_others_failed_runs(self) -> None:
        """Branch validation supersedes failures and cancellations from either trigger."""
        for workflow in ('compatibility', 'release'):
            for success_event, old_event in (
                ('push', 'workflow_dispatch'),
                ('workflow_dispatch', 'push'),
            ):
                for conclusion in ('failure', 'cancelled'):
                    with self.subTest(
                        workflow=workflow, event=success_event, conclusion=conclusion
                    ):
                        path = f'.github/workflows/{workflow}.yml'
                        old = self.failure(
                            path=path, event=old_event, conclusion=conclusion
                        )
                        with patch.object(
                            cleanup,
                            'api',
                            side_effect=[
                                self.run_record(path=path, event=success_event),
                                [{'workflow_runs': [old]}],
                                old,
                                None,
                            ],
                        ) as api:
                            self.assertEqual(cleanup.cleanup('owner/lockd', 42), [40])
                        self.assertEqual(
                            api.call_args,
                            call('repos/owner/lockd/actions/runs/40', delete=True),
                        )

    def test_preserves_runs_outside_the_cleanup_scope(self) -> None:
        """Successes, active runs, other contexts and later activity remain intact."""
        exclusions: list[dict] = [
            {'conclusion': 'success'},
            {'conclusion': 'timed_out'},
            {'status': 'in_progress'},
            {'status': 'queued'},
            {'run_number': 12},
            {'run_number': 13},
            {'id': 42},
            {'head_branch': 'feature'},
            {'workflow_id': 8},
            {'path': '.github/workflows/release.yml'},
            {'event': 'pull_request'},
            {'head_repository': {'full_name': 'fork/lockd'}},
            {'head_repository': None},
            {'updated_at': '2026-10-27T13:00:00Z'},
        ]
        for changes in exclusions:
            with (
                self.subTest(changes=changes),
                patch.object(
                    cleanup,
                    'api',
                    side_effect=[
                        self.run_record(),
                        [{'workflow_runs': [self.failure(**changes)]}],
                    ],
                ) as api,
            ):
                self.assertEqual(cleanup.cleanup('owner/lockd', 42), [])
                self.assertEqual(api.call_count, 2)

    def test_recheck_preserves_rerunning_or_recovered_failure(self) -> None:
        """A run that changes after listing is not deleted using stale evidence."""
        for changes in (
            {'status': 'in_progress'},
            {'conclusion': 'success'},
            {'updated_at': '2026-10-27T13:00:00Z'},
        ):
            with (
                self.subTest(changes=changes),
                patch.object(
                    cleanup,
                    'api',
                    side_effect=[
                        self.run_record(),
                        [{'workflow_runs': [self.failure()]}],
                        self.failure(**changes),
                    ],
                ) as api,
            ):
                self.assertEqual(cleanup.cleanup('owner/lockd', 42), [])
                self.assertEqual(api.call_count, 3)

    def test_cancelled_runs_obey_scope_and_recheck_guards(self) -> None:
        """Cancellation allows cleanup only for older same-scope, unchanged runs."""
        for workflow in ('compatibility', 'release'):
            success = self.run_record(path=f'.github/workflows/{workflow}.yml')
            cancelled = self.failure(path=success['path'], conclusion='cancelled')
            for changes in (
                {'head_branch': 'other'},
                {'workflow_id': 99},
                {'run_number': 13},
                {'event': 'pull_request'},
                {'head_repository': {'full_name': 'fork/lockd'}},
                {'status': 'in_progress'},
                {'conclusion': 'success'},
                {'updated_at': '2026-10-27T13:00:00Z'},
            ):
                for changed_before_listing in (True, False):
                    with self.subTest(
                        workflow=workflow,
                        changes=changes,
                        changed_before_listing=changed_before_listing,
                    ):
                        candidate = cancelled | changes
                        responses = [
                            success,
                            [
                                {
                                    'workflow_runs': [
                                        candidate
                                        if changed_before_listing
                                        else cancelled
                                    ]
                                }
                            ],
                        ]
                        if not changed_before_listing:
                            responses.append(candidate)
                        with patch.object(cleanup, 'api', side_effect=responses) as api:
                            self.assertEqual(cleanup.cleanup('owner/lockd', 42), [])
                        self.assertFalse(
                            any(
                                entry.kwargs.get('delete')
                                for entry in api.call_args_list
                            )
                        )
            with patch.object(
                cleanup,
                'api',
                side_effect=[
                    success,
                    [{'workflow_runs': [cancelled]}],
                    cancelled,
                    None,
                ],
            ):
                self.assertEqual(cleanup.cleanup('owner/lockd', 42), [40])

    def test_requires_verified_workflow_success(self) -> None:
        """Failed triggers and unrelated workflows cannot authorize deletion."""
        for changes in (
            {'conclusion': 'failure'},
            {'conclusion': 'cancelled'},
            {'status': 'in_progress'},
            {'path': '.github/workflows/freebsd.yml'},
            {'head_repository': {'full_name': 'fork/lockd'}},
            {'head_branch': None},
        ):
            with (
                self.subTest(changes=changes),
                patch.object(
                    cleanup, 'api', return_value=self.run_record(**changes)
                ) as api,
            ):
                with self.assertRaises(ValueError):
                    cleanup.cleanup('owner/lockd', 42)
                api.assert_called_once()

    def test_branch_is_encoded_as_a_query_value(self) -> None:
        """Branch punctuation cannot introduce additional API query parameters."""
        with patch.object(
            cleanup,
            'api',
            side_effect=[
                self.run_record(head_branch='feature/a&b'),
                [{'workflow_runs': []}],
            ],
        ) as api:
            cleanup.cleanup('owner/lockd', 42)
        self.assertIn('branch=feature%2Fa%26b&', api.call_args.args[0])

    def test_skipped_cleanup_runs_are_deleted_after_a_fresh_lookup(self) -> None:
        """Remove old no-op cleanup runs across pages while retaining real work."""
        current = self.run_record(
            path=cleanup.CLEANUP_PATH,
            event='workflow_run',
            status='in_progress',
            conclusion=None,
            created_at='2026-10-27T12:00:00Z',
        )
        skipped = current | {
            'id': 40,
            'run_number': 10,
            'status': 'completed',
            'conclusion': 'skipped',
        }
        for changes in (
            {},
            {'conclusion': 'success'},
            {'conclusion': 'failure'},
            {'status': 'in_progress'},
            {'head_branch': 'other'},
            {'head_repository': {'full_name': 'fork/lockd'}},
            {'workflow_id': 99},
            {'path': '.github/workflows/compatibility.yml'},
            {'event': 'push'},
            {'id': 43},
            {'run_number': 13},
            {'updated_at': '2026-10-27T13:00:00Z'},
        ):
            for changed_before_listing in (True, False):
                with self.subTest(changes=changes, listed=changed_before_listing):
                    candidate = skipped | changes
                    responses = [
                        current,
                        [
                            {'workflow_runs': []},
                            {
                                'workflow_runs': [
                                    candidate if changed_before_listing else skipped
                                ]
                            },
                        ],
                    ]
                    if not changes or not changed_before_listing:
                        responses.append(candidate)
                    if not changes:
                        responses.append(None)
                    with patch.object(cleanup, 'api', side_effect=responses) as api:
                        self.assertEqual(
                            cleanup.cleanup_skipped('owner/lockd', 42),
                            [] if changes else [40],
                        )
                    deletes = [c for c in api.call_args_list if c.kwargs.get('delete')]
                    self.assertEqual(len(deletes), 0 if changes else 1)

    def test_skipped_cleanup_rejects_an_unrelated_current_workflow(self) -> None:
        """A caller cannot use a validation run as authority to clean cleanup history."""
        with patch.object(cleanup, 'api', return_value=self.run_record()) as api:
            with self.assertRaises(ValueError):
                cleanup.cleanup_skipped('owner/lockd', 42)
            api.assert_called_once()


if __name__ == '__main__':
    unittest.main()

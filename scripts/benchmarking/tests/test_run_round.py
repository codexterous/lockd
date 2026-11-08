"""Exercise randomized rounds and recovery offline on every controller platform."""

import copy
import datetime
import io
import json
import os
import shlex
import subprocess
import sys
import tempfile
import time
import unittest
import uuid
from collections.abc import Callable
from contextlib import chdir, redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

from scripts.benchmarking import config, run_round, vultr

ROOT = config.ROOT
PROFILES = (
    'sync_light',
    'sync_medium',
    'sync_high',
    'sync_very_high',
    'sync_capacity',
    'sync_capacity_50ms',
    'async_high',
    'async_very_high',
)


class RoundTests(unittest.TestCase):
    """Use real configuration validation, journal I/O, locking and resume checks."""

    def setUp(self) -> None:
        """Create eight local inputs; replace only billable controller execution."""
        work = config.output_directory('round-tests')
        work.mkdir(parents=True, exist_ok=True)
        temporary = tempfile.TemporaryDirectory(prefix="round's files ", dir=work)
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.configurations = self.root / 'scripts/benchmarking'
        self.configurations.mkdir(parents=True)
        archive = self.root / 'source.tar.gz'
        archive.write_bytes(b'local source fixture')
        key = self.root / 'ssh_key'
        key.write_bytes(b'not a real private key')
        for name in PROFILES:
            settings = config.example() | {
                'os_id': 1234,
                'daemon_plan': 'daemon-' + name,
                'worker_plan': 'worker-fixture',
                'vpc_id': str(uuid.uuid4()),
                'firewall_group_id': str(uuid.uuid4()),
                'ssh_key_id': str(uuid.uuid4()),
                'source_archive': str(archive),
                'source_sha256': config.file_digest(archive),
                'ssh_private_key': str(key),
            }
            self.configuration(name).write_text(json.dumps(settings), encoding='utf-8')
        patch_root = mock.patch.object(config, 'ROOT', self.root)
        patch_root.start()
        self.addCleanup(patch_root.stop)
        api_patch = mock.patch.object(vultr, 'Api', return_value=mock.sentinel.api)
        self.api = api_patch.start()
        self.addCleanup(api_patch.stop)
        self.real_run = run_round.controller.run
        run_patch = mock.patch.object(
            run_round.controller, 'run', side_effect=self.fake_run
        )
        self.run = run_patch.start()
        self.addCleanup(run_patch.stop)
        self.calls: list[str] = []
        self.action: Callable[[str], None] | None = None
        self.output = io.StringIO()
        self.errors = io.StringIO()

    def configuration(self, name: str) -> Path:
        """Return the named profile under this isolated checkout."""
        return self.configurations / f'benchmark-{name}.json'

    def read_round(self) -> tuple[Path, dict]:
        """Require one round, distinct from its individual run manifests."""
        paths = list(
            (self.root / 'output/obj').glob('*/opt/shared/benchmarking/*/round.json')
        )
        self.assertEqual(len(paths), 1)
        return paths[0], json.loads(paths[0].read_text(encoding='utf-8'))

    def invoke(self, *arguments: str) -> int:
        """Run the production CLI and retain its diagnostics."""
        with redirect_stdout(self.output), redirect_stderr(self.errors):
            return run_round.main(list(arguments))

    def fake_run(self, api: object, settings: dict, run_id: str) -> None:
        """Stand in for the cloud boundary, requiring identity to be journaled first."""
        self.assertIs(api, mock.sentinel.api)
        name = settings['daemon_plan'].removeprefix('daemon-')
        _, state = self.read_round()
        profile = next(entry for entry in state['profiles'] if entry['profile'] == name)
        path = config.output_directory(run_id) / 'manifest.json'
        self.assertEqual(profile['run_id'], run_id)
        self.assertEqual(profile['manifest'], str(path))
        self.assertEqual(profile['status'], 'running')
        self.write_manifest(path, run_id, settings)
        self.calls.append(name)
        if self.action is not None:
            self.action(name)

    def write_manifest(
        self, path: Path, run_id: str, settings: dict, *, age: int = 0
    ) -> None:
        """Write a real, locally validated VM journal with recorded completed cleanup."""
        path.parent.mkdir(parents=True, exist_ok=False)
        vultr.save(
            path,
            {
                'schema': 1,
                'run_id': run_id,
                'config': settings,
                'resources': [
                    {
                        'role': 'daemon',
                        'label': f'lockd-bench-{run_id[:8]}-daemon',
                        'token': str(uuid.uuid4()),
                        'id': str(uuid.uuid4()),
                        'status': 'deleted',
                        'intent_unix': time.time() - age,
                    }
                ],
                'outcome': 'completed',
            },
        )

    def saved_round(self, *, legacy: bool = False) -> tuple[Path, dict]:
        """Create a seven-profile journal with a failed third attempt."""
        state = run_round.new_round()
        state['profiles'] = sorted(
            [
                entry
                for entry in state['profiles']
                if entry['profile'] != 'sync_capacity_50ms'
            ],
            key=lambda entry: entry['profile'],
        )
        run_round.validate_profiles(state, resuming=False)
        now = datetime.datetime.now(datetime.UTC)
        start = (now - datetime.timedelta(minutes=10)).isoformat()
        for entry in state['profiles'][:2]:
            entry.update(
                status='completed',
                exit_code=0,
                started_utc=start,
                finished_utc=now.isoformat(),
            )
        attempt = state['profiles'][2]
        identifier = str(uuid.uuid4())
        manifest = self.root / 'previous attempt/manifest.json'
        self.write_manifest(
            manifest, identifier, config.load(Path(attempt['config'])), age=60
        )
        attempt.update(
            status='failed',
            started_utc=start,
            finished_utc=now.isoformat(),
            exit_code=7,
            run_id=identifier,
            manifest=str(manifest),
        )
        if legacy:
            for entry in state['profiles']:
                for field in ('run_id', 'manifest', 'attempts'):
                    del entry[field]
        state['outcome'] = 'failed'
        path = config.output_directory(state['round_id']) / 'round.json'
        path.parent.mkdir(parents=True)
        vultr.save(path, state)
        return path, state

    def test_preview_is_local_and_writes_no_journal(self) -> None:
        """Planning requires no key, API calls or external commands."""
        self.assertEqual(self.invoke('--plan-only'), 0)
        self.api.assert_not_called()
        self.run.assert_not_called()
        self.assertFalse((self.root / 'output').exists())
        for name in PROFILES:
            self.assertIn(name, self.output.getvalue())

    def test_success_runs_all_eight_once_in_saved_shuffled_order(self) -> None:
        """Shuffle without replacement and save identities before every execution."""
        with mock.patch.object(
            run_round.random, 'shuffle', side_effect=lambda values: values.reverse()
        ) as shuffle:
            self.assertEqual(self.invoke(), 0)
        shuffle.assert_called_once()
        _, state = self.read_round()
        self.assertEqual(self.calls, sorted(PROFILES, reverse=True))
        self.assertEqual(self.calls, [entry['profile'] for entry in state['profiles']])
        self.assertEqual(state['outcome'], 'completed')
        for entry in state['profiles']:
            self.assertEqual(entry['status'], 'completed')
            self.assertEqual(entry['exit_code'], 0)
            self.assertEqual(entry['attempts'], [])
            self.assertEqual(
                entry['config_sha256'],
                config.file_digest(Path(entry['config'])).upper(),
            )

    def test_one_custom_profile_can_run_and_resume_without_a_fixed_name_list(
        self,
    ) -> None:
        """Discover a single unfamiliar scenario and keep its failed-attempt history."""
        settings = json.loads(self.configuration(PROFILES[0]).read_text())
        for name in PROFILES:
            self.configuration(name).unlink()
        settings['daemon_plan'] = 'daemon-custom_scenario'
        self.configuration('custom_scenario').write_text(
            json.dumps(settings), encoding='utf-8'
        )

        def fail(name: str) -> None:
            """Leave one cleaned-up failure for the saved custom profile."""
            raise RuntimeError('custom failure')

        self.action = fail
        self.assertEqual(self.invoke(), 1)
        path, state = self.read_round()
        self.assertEqual(
            [entry['profile'] for entry in state['profiles']], ['custom_scenario']
        )
        self.action = None
        self.assertEqual(self.invoke('--resume', str(path)), 0)
        self.assertEqual(self.calls, ['custom_scenario', 'custom_scenario'])
        _, state = self.read_round()
        self.assertEqual(state['outcome'], 'completed')
        self.assertEqual(len(state['profiles'][0]['attempts']), 1)

    def test_discovery_includes_additional_profiles_and_ignores_nonmatching_inputs(
        self,
    ) -> None:
        """Ten matching files all run, while benchmark.json and directories do not."""
        settings = json.loads(self.configuration(PROFILES[0]).read_text())
        for name in ('custom_one', 'custom_two'):
            settings['daemon_plan'] = 'daemon-' + name
            self.configuration(name).write_text(json.dumps(settings), encoding='utf-8')
        (self.configurations / 'benchmark.json').write_text('{}', encoding='utf-8')
        self.configuration('not_a_file').mkdir()
        self.assertEqual(self.invoke(), 0)
        self.assertEqual(len(self.calls), 10)
        self.assertEqual(set(self.calls), {*PROFILES, 'custom_one', 'custom_two'})

    def test_no_matching_configurations_fail_before_any_api_or_journal(self) -> None:
        """An empty configuration set must not appear to complete a useful round."""
        for name in PROFILES:
            self.configuration(name).unlink()
        self.assertEqual(self.invoke(), 1)
        self.assertIn('No benchmark-*.json', self.errors.getvalue())
        self.api.assert_not_called()
        self.assertFalse((self.root / 'output').exists())

    def test_invalid_last_profile_prevents_all_runs(self) -> None:
        """Validate every scenario before starting even the first cloud run."""
        self.configuration(PROFILES[-1]).write_text('{}', encoding='utf-8')
        self.assertEqual(self.invoke(), 1)
        self.assertFalse((self.root / 'output').exists())
        self.api.assert_not_called()

    def test_changed_configuration_stops_before_the_next_profile(self) -> None:
        """An edit after initial validation cannot silently change the study baseline."""

        def change(name: str) -> None:
            """Change the next configuration after the first run completes."""
            path = self.configuration(sorted(PROFILES)[1])
            path.write_text(path.read_text() + ' ', encoding='utf-8')

        self.action = change
        with mock.patch.object(run_round.random, 'shuffle'):
            self.assertEqual(self.invoke(), 1)
        _, state = self.read_round()
        self.assertEqual(self.calls, [min(PROFILES)])
        self.assertEqual(state['profiles'][1]['status'], 'pending')
        self.assertEqual(state['outcome'], 'incomplete')

    def test_changed_archive_is_revalidated_between_runs(self) -> None:
        """Do not keep authorizing a source archive validated before a long prior run."""
        self.action = lambda name: (self.root / 'source.tar.gz').write_bytes(
            b'changed source'
        )
        self.assertEqual(self.invoke(), 1)
        self.assertEqual(len(self.calls), 1)
        self.assertIn('source_sha256', self.errors.getvalue())

    def test_failure_stops_and_resume_preserves_history_and_order(self) -> None:
        """A new UUID retries the failed profile, retaining its old files and outcome."""

        def fail_second(name: str) -> None:
            """Fail only the second attempted scenario."""
            if len(self.calls) == 2:
                raise RuntimeError('worker failed')

        self.action = fail_second
        self.assertEqual(self.invoke(), 1)
        path, before = self.read_round()
        self.assertEqual(before['outcome'], 'failed')
        self.assertEqual(
            [entry['status'] for entry in before['profiles']],
            ['completed', 'failed'] + ['pending'] * 6,
        )
        failed = copy.deepcopy(before['profiles'][1])
        self.action = None
        with mock.patch.object(run_round.random, 'shuffle') as shuffle:
            self.assertEqual(self.invoke('--resume', str(path)), 0)
        shuffle.assert_not_called()
        _, after = self.read_round()
        self.assertEqual(after['outcome'], 'completed')
        self.assertEqual(after['profiles'][0], before['profiles'][0])
        self.assertEqual(
            after['profiles'][1]['attempts'],
            [{field: failed[field] for field in run_round.ATTEMPT_FIELDS}],
        )
        self.assertNotEqual(after['profiles'][1]['run_id'], failed['run_id'])
        self.assertTrue(Path(failed['manifest']).is_file())
        self.assertEqual(
            self.calls[2:], [entry['profile'] for entry in before['profiles'][1:]]
        )

    def test_saved_journal_retains_seven_profiles_and_timestamp_format(
        self,
    ) -> None:
        """Accept seven-digit ISO fractions and keep the successful prefix untouched."""
        path, before = self.saved_round()
        before['profiles'][2]['started_utc'] = (
            before['profiles'][2]['started_utc'][:26] + '0Z'
        )
        vultr.save(path, before)
        self.assertEqual(self.invoke('--resume', str(path)), 0)
        _, after = self.read_round()
        self.assertEqual(after['round_id'], before['round_id'])
        self.assertEqual(after['profiles'][:2], before['profiles'][:2])
        self.assertEqual(
            self.calls, [entry['profile'] for entry in before['profiles'][2:]]
        )
        self.assertNotIn('sync_capacity_50ms', self.calls)
        self.assertEqual(after['profiles'][2]['attempts'][0]['exit_code'], 7)

    def test_running_journal_without_finish_time_can_resume_after_recorded_cleanup(
        self,
    ) -> None:
        """A controller crash leaves running state, but must retain the same cleanup gate."""
        path, state = self.saved_round()
        state['profiles'][2].update(status='running', finished_utc=None, exit_code=None)
        vultr.save(path, state)
        self.assertEqual(self.invoke('--resume', str(path)), 0)
        _, after = self.read_round()
        self.assertEqual(after['outcome'], 'completed')
        previous = after['profiles'][2]['attempts'][0]
        self.assertEqual(previous['status'], 'running')
        self.assertIsNone(previous['finished_utc'])

    def test_legacy_manifest_hint_is_not_reused_for_a_later_failure(self) -> None:
        """Consume the bootstrap association once; hints follow the persisted attempt."""
        path, before = self.saved_round(legacy=True)
        original = path.read_bytes()
        self.assertEqual(self.invoke('--resume', str(path)), 1)
        self.assertEqual(path.read_bytes(), original)
        self.assertIn('--previous-manifest', self.errors.getvalue())

        def fail_second(name: str) -> None:
            """Fail a later scenario after the legacy association was saved."""
            if len(self.calls) == 2:
                raise RuntimeError('later failure')

        self.action = fail_second
        manifest = self.root / 'previous attempt/manifest.json'
        self.assertEqual(
            self.invoke('--resume', str(path), '--previous-manifest', str(manifest)), 1
        )
        self.assertNotIn(
            '--previous-manifest', self.errors.getvalue().split('Resume with:')[-1]
        )
        self.action = None
        self.assertEqual(self.invoke('--resume', str(path)), 0)
        _, after = self.read_round()
        self.assertEqual(after['outcome'], 'completed')
        self.assertEqual(after['profiles'][2]['attempts'][0]['exit_code'], 7)
        self.assertEqual(after['profiles'][3]['attempts'][0]['exit_code'], 1)
        self.assertEqual(
            self.calls,
            [entry['profile'] for entry in before['profiles'][2:4]]
            + [entry['profile'] for entry in before['profiles'][3:]],
        )

    def test_resume_rejects_changed_inputs_unresolved_cleanup_and_wrong_manifest(
        self,
    ) -> None:
        """Validate real manifests and configuration, leaving a rejected journal unchanged."""
        path, before = self.saved_round()
        manifest = Path(before['profiles'][2]['manifest'])
        valid_manifest = manifest.read_bytes()
        for scenario in (
            'configuration',
            'cleanup',
            'uuid',
            'missing',
            'stale',
            'timestamps',
        ):
            with self.subTest(scenario=scenario):
                state = copy.deepcopy(before)
                arguments = ['--resume', str(path)]
                config_path = Path(state['profiles'][0]['config'])
                config_bytes = config_path.read_bytes()
                data = json.loads(valid_manifest)
                if scenario == 'configuration':
                    config_path.write_bytes(config_bytes + b' ')
                elif scenario == 'cleanup':
                    data['resources'][0]['status'] = 'created'
                elif scenario == 'uuid':
                    state['profiles'][2]['run_id'] = str(uuid.uuid4())
                elif scenario == 'missing':
                    state['profiles'][2]['manifest'] = str(self.root / 'missing.json')
                elif scenario == 'stale':
                    stale = self.root / 'stale.json'
                    stale.write_text('{}', encoding='utf-8')
                    arguments += ['--previous-manifest', str(stale)]
                else:
                    data['resources'][0]['intent_unix'] -= 3600
                vultr.save(path, state)
                vultr.save(manifest, data)
                saved = path.read_bytes()
                self.assertEqual(self.invoke(*arguments), 1)
                self.assertEqual(path.read_bytes(), saved)
                self.run.assert_not_called()
                config_path.write_bytes(config_bytes)
                manifest.write_bytes(valid_manifest)

    def test_preview_and_completed_resume_do_not_rewrite_the_journal(self) -> None:
        """Preview validates cleanup locally; completed rounds buy no replacement VMs."""
        path, state = self.saved_round()
        original = path.read_bytes()
        self.assertEqual(self.invoke('--resume', str(path), '--plan-only'), 0)
        self.assertEqual(path.read_bytes(), original)
        self.api.assert_not_called()
        for entry in state['profiles']:
            entry.update(status='completed', exit_code=0)
        state['outcome'] = 'completed'
        vultr.save(path, state)
        original = path.read_bytes()
        self.assertEqual(self.invoke('--resume', str(path)), 0)
        self.assertEqual(path.read_bytes(), original)
        self.api.assert_not_called()

    def test_unresolved_attempt_prints_cleanup_then_resume_commands(self) -> None:
        """Recovery must name the VM manifest for cleanup and the round for resume."""
        path, state = self.saved_round()
        manifest = Path(state['profiles'][2]['manifest'])
        data = vultr.load(manifest)
        data['resources'][0]['status'] = 'created'
        vultr.save(manifest, data)
        original = path.read_bytes()

        self.assertEqual(self.invoke('--resume', str(path)), 1)

        messages = self.errors.getvalue()
        cleanup = run_round.controller.cleanup_command(manifest)
        resume = 'Resume with: ' + run_round.resume_command(path, None)
        self.assertIn(cleanup, messages)
        self.assertIn(resume, messages)
        self.assertLess(messages.index(cleanup), messages.index(resume))
        self.assertEqual(path.read_bytes(), original)
        self.api.assert_not_called()
        self.run.assert_not_called()

    def test_exclusive_lock_blocks_duplicate_fleets_and_releases_after_error(
        self,
    ) -> None:
        """Exercise OS locking rather than a mocked permission check."""
        path, _ = self.saved_round()
        before = path.read_bytes()
        with vultr.journal_lock(path):
            self.assertEqual(self.invoke('--resume', str(path)), 1)
        self.assertEqual(path.read_bytes(), before)
        self.run.assert_not_called()
        self.assertEqual(self.invoke('--resume', str(path)), 0)

    def test_local_setup_failure_preserves_the_previous_recovery_link(self) -> None:
        """Do not replace a failed UUID before a new attempt's paths can be determined."""
        path, before = self.saved_round()
        with mock.patch.object(
            config, 'output_directory', side_effect=OSError('setup failed')
        ):
            self.assertEqual(self.invoke('--resume', str(path)), 1)
        _, after = self.read_round()
        self.assertEqual(after['profiles'][2], before['profiles'][2])
        self.run.assert_not_called()

    def test_interrupt_completes_controller_cleanup_before_round_is_marked_incomplete(
        self,
    ) -> None:
        """Use the real single-run finally block, with no child process to terminate."""
        self.run.side_effect = self.real_run

        def cleanup(api: object, path: Path, state: dict) -> list:
            """Observe that cleanup precedes the round's final interrupted state."""
            _, journal = self.read_round()
            self.assertEqual(journal['profiles'][0]['status'], 'running')
            return []

        with (
            mock.patch.object(
                run_round.controller.shutil, 'which', return_value='fixture-tool'
            ),
            mock.patch.object(
                run_round.controller,
                'preflight',
                return_value={'image': {}, 'plans': {}},
            ),
            mock.patch.object(
                run_round.controller, 'execute_run', side_effect=KeyboardInterrupt()
            ),
            mock.patch.object(run_round.controller.report, 'generate'),
            mock.patch.object(vultr, 'cleanup', side_effect=cleanup) as clean,
        ):
            self.assertEqual(self.invoke(), 130)
        clean.assert_called_once()
        path, state = self.read_round()
        self.assertEqual(state['profiles'][0]['status'], 'incomplete')
        self.assertEqual(state['outcome'], 'incomplete')
        self.action = None
        self.run.side_effect = self.fake_run
        self.assertEqual(self.invoke('--resume', str(path)), 0)

    def test_invalid_saved_sequence_starts_nothing(self) -> None:
        """Refuse duplicate profiles, misplaced attempts and an unsuccessful prefix."""
        path, original = self.saved_round()
        for scenario in (
            'duplicate',
            'completed_after_failure',
            'pending_before_failure',
            'failed_prefix',
            'invalid_round_id',
        ):
            with self.subTest(scenario=scenario):
                state = copy.deepcopy(original)
                if scenario == 'duplicate':
                    state['profiles'][-1]['profile'] = state['profiles'][0]['profile']
                elif scenario == 'completed_after_failure':
                    state['profiles'][3].update(status='completed', exit_code=0)
                elif scenario == 'pending_before_failure':
                    state['profiles'][1]['status'] = 'pending'
                elif scenario == 'invalid_round_id':
                    state['round_id'] = 42
                else:
                    state['profiles'][0]['exit_code'] = 1
                vultr.save(path, state)
                before = path.read_bytes()
                self.assertEqual(self.invoke('--resume', str(path)), 1)
                self.assertEqual(path.read_bytes(), before)
                self.run.assert_not_called()

    def test_resume_hint_quotes_paths_for_the_controller_shell(self) -> None:
        """Spaces and apostrophes stay literal in PowerShell and POSIX shells."""
        path, _ = self.saved_round(legacy=True)
        manifest = self.root / 'previous attempt/manifest.json'
        for platform in ('nt', 'posix'):
            with (
                self.subTest(platform=platform),
                mock.patch.object(run_round.os, 'name', platform),
            ):
                command = run_round.resume_command(path, manifest)
                self.assertTrue(
                    command.startswith(
                        'python -m scripts.benchmarking.run_round --resume '
                    )
                )
                self.assertIn('--previous-manifest', command)
                if platform == 'nt':
                    self.assertIn(str(path).replace("'", "''"), command)
                else:
                    self.assertEqual(shlex.split(command)[-1], str(manifest))

    def test_module_help_needs_no_external_commands_or_cloud_credentials(self) -> None:
        """The public module loads with no external commands on PATH."""
        result = subprocess.run(
            [sys.executable, '-m', 'scripts.benchmarking.run_round', '--help'],
            cwd=ROOT,
            env={**os.environ, 'PATH': '', 'VULTR_API_KEY': ''},
            capture_output=True,
            text=True,
            check=False,
            timeout=15,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('--plan-only', result.stdout)
        self.assertIn('--resume', result.stdout)

    def test_relative_resume_path_preserves_the_callers_directory(self) -> None:
        """Resolve supplied paths from the caller rather than changing to the checkout."""
        path, _ = self.saved_round()
        with chdir(self.root):
            self.assertEqual(
                self.invoke(
                    '--resume', str(path.relative_to(self.root)), '--plan-only'
                ),
                0,
            )
            self.assertEqual(Path.cwd(), self.root)
        self.api.assert_not_called()

    def test_previous_manifest_requires_an_explicit_resume(self) -> None:
        """Reject a misplaced recovery argument before any round is started."""
        with self.assertRaises(SystemExit) as error:
            self.invoke('--previous-manifest', 'manifest.json')
        self.assertEqual(error.exception.code, 2)
        self.api.assert_not_called()


if __name__ == '__main__':
    unittest.main()

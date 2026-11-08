"""Test native profiler boundaries offline, without installing tools or creating VMs."""

import json
import os
import signal
import subprocess
import sys
import threading
from pathlib import Path
from unittest import mock

from scripts.benchmarking import __main__ as controller
from scripts.benchmarking import agent, config, report, vultr
from scripts.benchmarking.remote import Session
from scripts.benchmarking.tests import test_benchmarking as fixtures


class ProfilingTests(fixtures.BenchmarkTest):
    """Keep profiling opt-in, daemon-only and separate from public benchmark assets."""

    def test_configuration_requires_an_explicit_boolean(self) -> None:
        """
        Reject truthy strings before remote work; old configurations stay disabled.
        """
        archive = self.directory / 'source.tar.gz'
        archive.write_bytes(b'source')
        key = self.directory / 'key'
        key.write_text('test key placeholder')
        settings = self.settings | {
            'source_archive': str(archive),
            'source_sha256': config.file_digest(archive),
            'ssh_private_key': str(key),
        }
        path = self.directory / 'settings.json'
        for value in ('false', 0, 1, None):
            vultr.save(path, settings | {'profile_daemon': value})
            with (
                self.subTest(value=value),
                self.assertRaisesRegex(ValueError, 'profile_daemon'),
            ):
                config.load(path)
        settings.pop('profile_daemon')
        vultr.save(path, settings)
        self.assertFalse(config.load(path)['profile_daemon'])
        vultr.save(path, settings | {'profile_daemon': True})
        self.assertTrue(config.load(path)['profile_daemon'])

    def test_only_daemon_jobs_profile_and_worker_jobs_keep_errors_only(self) -> None:
        """Reserve profiling for the daemon and suppress generator stdout summaries."""
        self.settings['profile_daemon'] = True
        daemon = self.create()
        daemon['private_ip'] = '10.20.0.2'
        worker = self.create('worker-0')
        worker['worker_index'] = 0
        remote = mock.Mock(work='/root/test-run')
        for record in (daemon, worker):
            controller.write_job(remote, self.state, record, self.directory)
        jobs = [
            json.loads((self.directory / (role + '-job.json')).read_text())
            for role in ('daemon', 'worker-0')
        ]
        self.assertTrue(jobs[0]['profile_daemon'])
        self.assertFalse(jobs[0]['errors_only'])
        self.assertIn('--log-level=info', jobs[0]['command'])
        self.assertFalse(jobs[1]['profile_daemon'])
        self.assertTrue(jobs[1]['errors_only'])
        self.assertNotIn('--log-level=error', jobs[1]['command'])
        self.assertIn('errors only', worker['logging'])

    def test_remote_build_enables_profiling_only_when_requested(self) -> None:
        """Forward the optional build flag without changing unprofiled provisioning."""
        for enabled in (False, True):
            remote = mock.Mock(work='/root/test-run')
            controller.build_bundle(
                remote, self.settings | {'profile_daemon': enabled}, self.directory
            )
            self.assertEqual('--profile' in remote.call.call_args.args[0], enabled)

    def test_perf_setup_uses_running_kernel_and_full_pipeline_probe(self) -> None:
        """Mock installation and require attachment/report preflight before workers."""
        with (
            mock.patch.object(agent, 'execute') as execute,
            mock.patch.object(agent.platform, 'release', return_value='test-kernel'),
            mock.patch.object(agent, 'probe_perf') as probe,
        ):
            agent.prepare_perf(self.directory)
        self.assertEqual(
            execute.call_args_list[0].args[0],
            ['apt-get', 'install', '-y', 'linux-tools-test-kernel'],
        )
        probe.assert_called_once_with(self.directory)

    def test_failed_sampling_probe_is_not_silently_ignored(self) -> None:
        """Handshake errors stop setup instead of creating unprofiled worker VMs."""
        with (
            mock.patch.object(
                agent,
                'execute',
            ),
            mock.patch.object(
                agent,
                'probe_perf',
                side_effect=RuntimeError("Invalid perf acknowledgement b'ack\\n\\0'"),
            ),
            self.assertRaisesRegex(RuntimeError, 'perf acknowledgement'),
        ):
            agent.prepare_perf(self.directory)

    def test_profile_preflight_failure_prevents_worker_creation_and_cleans_daemon(
        self,
    ) -> None:
        """A rejected perf handshake must fail during the sole daemon VM's build."""
        self.settings.update(profile_daemon=True, worker_count=10)
        location = self.directory / 'profile-failure'
        self.api.journal = location / 'manifest.json'
        remote = mock.Mock(work='/root/test-run')

        def connect(api, path, state, record):
            """Provide a fake reachable daemon without touching an actual VM."""
            record.update(public_ip='192.0.2.10', private_ip='10.20.0.2')
            return remote

        def fail_build(arguments, timeout, log) -> None:
            """Report the native preflight error at the remote command boundary."""
            self.assertIn('--profile', arguments)
            log.write_text('Invalid perf acknowledgement: test boundary failure\n')
            raise subprocess.CalledProcessError(1, ['ssh', 'build'])

        remote.call.side_effect = fail_build
        with (
            mock.patch.object(controller.shutil, 'which', return_value='ssh'),
            mock.patch.object(controller, 'preflight', return_value=self.selection),
            mock.patch.object(config, 'output_directory', return_value=location),
            mock.patch.object(controller, 'connect', side_effect=connect),
            mock.patch.object(controller, 'Session') as session,
            mock.patch.object(report, 'generate'),
            self.assertRaisesRegex(RuntimeError, 'Daemon build failed'),
        ):
            controller.run(self.api, self.settings)
        session.assert_not_called()
        saved = vultr.load(self.api.journal)
        self.assertEqual([record['role'] for record in saved['resources']], ['daemon'])
        self.assertEqual(saved['resources'][0]['status'], 'deleted')
        self.assertEqual(len(self.api.deleted), 1)
        self.assertFalse(self.api.instances)
        self.assertIn('Invalid perf acknowledgement', self.output.getvalue())

    def test_release_reports_daemon_error_before_monitoring_failed_workers(
        self,
    ) -> None:
        """
        The enable failure must reach the controller without a generic worker error.
        """
        remote = mock.Mock(work='/unused')
        script = '''
import json
import sys
print(json.dumps({'event': 'ready'}), flush=True)
sys.stdin.readline()
print(json.dumps({
    'event': 'error', 'message': 'Invalid perf acknowledgement',
}), flush=True)
raise SystemExit(1)
'''
        remote.command.return_value = [
            sys.executable,
            '-u',
            '-c',
            script,
        ]
        session = Session(remote, self.directory, 'daemon')
        self.addCleanup(session.close)
        session.wait_ready(3)
        with self.assertRaisesRegex(RuntimeError, 'Invalid perf acknowledgement'):
            session.release(3)
        self.assertFalse(session.has_released_event)

    def test_collect_accepts_only_known_private_profile_artifacts(self) -> None:
        """A remote inventory cannot nominate arbitrary SCP paths for collection."""
        self.settings['profile_daemon'] = True
        daemon = self.create()
        daemon.update(public_ip='192.0.2.1', monitoring_started=True)
        remote = mock.Mock()

        def copy(local: Path, name: str, *, download: bool = False) -> None:
            """Replace SCP with the remote inventory, including an untrusted path."""
            if name == 'profile.json':
                local.write_text(
                    json.dumps(
                        {
                            'artifacts': [
                                'perf.data',
                                'perf.log',
                                '../../../private-key',
                            ],
                        }
                    )
                )

        remote.copy.side_effect = copy
        with mock.patch.object(controller, 'remote_for', return_value=remote):
            self.assertEqual(controller.collect(self.state, self.directory), [])
        names = [call.args[1] for call in remote.copy.call_args_list]
        self.assertIn('perf.data', names)
        self.assertIn('perf.log', names)
        self.assertNotIn('../../../private-key', names)
        self.assertNotIn('perf-report.txt', names)

    def test_report_labels_profiles_and_keeps_stack_data_private(self) -> None:
        """
        Successful counters remain usable diagnostics with explicit profile overhead.
        """
        self.create()
        self.create('worker-0')
        self.settings.update(profile_daemon=True, worker_count=1)
        self.state['outcome'] = 'completed'
        fixtures.ReportTests.csv(self)
        for role in ('daemon', 'worker-0'):
            (self.directory / (role + '-metrics.jsonl')).write_text(
                '\n'.join(
                    json.dumps(fixtures.ReportTests.metric(value, value * 100))
                    for value in (1, 2)
                )
            )
        profile = {
            'status': 'completed',
            'event': 'cpu-clock',
            'frequency_hz': 49,
            'call_graph': 'dwarf,8192',
            'scope': 'launch through drain',
        }
        (self.directory / 'daemon-profile.json').write_text(json.dumps(profile))
        for name in ('daemon-perf.data', 'daemon-perf-report.txt', 'daemon-perf.log'):
            (self.directory / name).write_text('PRIVATE_STACK_CONTENT_0x1234')
        with mock.patch.object(
            report, 'output_directory', return_value=self.directory / 'public'
        ):
            destination = report.generate(self.path, self.state)
        summary = json.loads((destination / 'summary.json').read_text())
        self.assertTrue(summary['profiled'])
        self.assertTrue(summary['valid_measurement'])
        self.assertEqual(summary['daemon_profile'], profile)
        rendered = (destination / 'report.html').read_text()
        self.assertIn('Completed profiled measurement', rendered)
        self.assertIn('CPU profiling adds overhead', rendered)
        for path in destination.iterdir():
            self.assertNotIn('PRIVATE_STACK_CONTENT', path.read_text())
        self.assertFalse((destination / 'daemon-perf.data').exists())

        (self.directory / 'daemon-perf.data').unlink()
        with mock.patch.object(
            report, 'output_directory', return_value=self.directory / 'incomplete'
        ):
            destination = report.generate(self.path, self.state)
        self.assertFalse(
            json.loads((destination / 'summary.json').read_text())['valid_measurement']
        )

    def recorder(self) -> agent.PerfProfile:
        """Use real owned files and pipe descriptors, replacing the native boundary."""
        profile = agent.PerfProfile(self.directory)
        self.addCleanup(profile.finish)
        return profile

    def test_disabled_profile_never_starts_a_native_process(self) -> None:
        """Stopping before gate release closes its log and records not_started."""
        profile = self.recorder()
        with mock.patch.object(agent.subprocess, 'Popen') as popen:
            profile.finish()
            profile.finish()
        popen.assert_not_called()
        self.assertTrue(profile.log.closed)
        self.assertEqual(profile.metadata['status'], 'not_started')

    def test_recording_waits_for_enable_ack_and_cleans_owned_descriptors(self) -> None:
        """Verify PID scope, handshake, symbolization and idempotent stop ordering."""
        process = mock.Mock()
        process.poll.return_value = None

        def wait(*args, **kwargs) -> int:
            """Publish native exit after flushing the recording."""
            process.poll.return_value = 0
            return 0

        process.wait.side_effect = wait
        profile = self.recorder()
        (self.directory / 'perf.data').write_bytes(b'private recording')
        with (
            mock.patch.object(agent.subprocess, 'Popen', return_value=process) as popen,
            mock.patch.object(agent.os, 'write', return_value=7) as write,
            mock.patch.object(agent.os, 'read', side_effect=[b'a', b'ck\n', b'\0']),
            mock.patch.object(agent.select, 'select', return_value=([1], [], [])),
            mock.patch.object(agent.subprocess, 'run') as run,
        ):
            profile.start(12345, threading.Event())
            descriptors = profile.descriptors.copy()
            self.assertIn('--pid', popen.call_args.args[0])
            self.assertIn('12345', popen.call_args.args[0])
            self.assertIn('--delay=-1', popen.call_args.args[0])
            write.assert_called_once_with(descriptors[0], b'enable\n')
            self.assertIn('start_monotonic', profile.metadata)
            profile.finish()
            profile.finish()
            process.send_signal.assert_called_once_with(signal.SIGINT)
            self.assertEqual(run.call_args.args[0][:2], ['perf', 'report'])
        self.assertEqual(profile.metadata['status'], 'completed')
        for descriptor in descriptors:
            with self.assertRaises(OSError):
                os.fstat(descriptor)

    def test_failed_or_stalled_recorder_cannot_be_reported_complete(self) -> None:
        """Early exit and flush timeouts persist diagnostics and kill only perf."""
        for failure in ('early_exit', 'timeout', 'report_error'):
            with self.subTest(failure=failure):
                profile = self.recorder()
                process = mock.Mock()
                profile.process = process
                profile.metadata.update(status='incomplete', start_monotonic=1)
                process.poll.return_value = 1 if failure == 'early_exit' else None
                if failure == 'timeout':
                    process.wait.side_effect = [
                        subprocess.TimeoutExpired('perf', 30),
                        0,
                    ]
                else:
                    process.wait.return_value = 0
                with (
                    mock.patch.object(
                        agent.subprocess,
                        'run',
                        side_effect=subprocess.CalledProcessError(
                            1, ['perf', 'report']
                        ),
                    ),
                    self.assertRaises((RuntimeError, subprocess.SubprocessError)),
                ):
                    profile.finish()
                self.assertEqual(profile.metadata['status'], 'incomplete')
                self.assertTrue(profile.log.closed)
                if failure != 'early_exit':
                    process.kill.assert_called_once()

    def test_cancel_timeout_and_invalid_ack_leave_no_open_control_pipes(self) -> None:
        """Control failures remain incomplete and all owned descriptors are closed."""
        for failure in (
            'cancel',
            'timeout',
            'invalid_ack',
            'invalid_nul',
            'missing_nul',
            'exec_error',
        ):
            with self.subTest(failure=failure):
                profile = self.recorder()
                process = mock.Mock()
                process.poll.return_value = None

                def wait(*args, recorder=process, **kwargs) -> int:
                    """Let the fake recorder exit after its requested interrupt."""
                    recorder.poll.return_value = 0
                    return 0

                process.wait.side_effect = wait
                cancelled = threading.Event()
                if failure == 'cancel':
                    cancelled.set()
                with (
                    mock.patch.object(
                        agent.subprocess,
                        'Popen',
                        return_value=process,
                        side_effect=OSError('exec failed')
                        if failure == 'exec_error'
                        else None,
                    ),
                    mock.patch.object(agent.os, 'write', return_value=7),
                    mock.patch.object(
                        agent.os,
                        'read',
                        side_effect=[b'ack\n', b'']
                        if failure == 'missing_nul'
                        else None,
                        return_value=b'ack\nX'
                        if failure == 'invalid_nul'
                        else b'invalid\n',
                    ),
                    mock.patch.object(
                        agent.select,
                        'select',
                        return_value=([1], [], []),
                    ),
                    mock.patch.object(
                        agent.time,
                        'monotonic',
                        side_effect=[0, 31, 32] if failure == 'timeout' else None,
                        return_value=0,
                    ),
                    mock.patch.object(agent.subprocess, 'run'),
                ):
                    with self.assertRaises((OSError, RuntimeError)):
                        profile.start(12345, cancelled)
                    descriptors = profile.descriptors.copy()
                    profile.finish()
                self.assertEqual(profile.metadata['status'], 'incomplete')
                self.assertTrue(profile.log.closed)
                for descriptor in descriptors:
                    with self.assertRaises(OSError):
                        os.fstat(descriptor)


if __name__ == '__main__':
    import unittest

    unittest.main()

"""
Run Linux supervisors against isolated process and TCP boundaries, without SSH or cloud
VMs.
"""

import contextlib
import io
import json
import os
import socket
import subprocess
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from scripts.benchmarking import agent, config
from scripts.benchmarking.remote import Session

# This external protocol stub supplies only the lock used for launching agents.
# It does not implement or replace any load measurement or mutual-exclusion test.
SERVER = '''
import socketserver
import threading
import sys
gate = threading.Lock()
class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        owned = False
        self.wfile.write(b'LOCKD\\r\\n')
        try:
            for line in self.rfile:
                if line.startswith(b'LOCK '):
                    self.wfile.write(b'LOCK_ACQUIRING\\r\\n')
                    gate.acquire()
                    owned = True
                    self.wfile.write(b'LOCK_ACQUIRED\\r\\n')
                elif line == b'UNLOCK\\n':
                    gate.release()
                    owned = False
                    self.wfile.write(b'LOCK_RELEASED\\r\\n')
        finally:
            if owned:
                gate.release()
class Server(socketserver.ThreadingTCPServer):
    daemon_threads = True
with Server(('127.0.0.1', int(sys.argv[1])), Handler) as server:
    server.serve_forever()
'''

# Replace only the perf executable. Its pipe protocol, process signals and output
# files still go through the real agent; no kernel sampling is claimed by this stub.
PERF = '''#!/usr/bin/python3
import os
from pathlib import Path
import signal
import sys
if sys.argv[1] == 'report':
    data = Path(sys.argv[sys.argv.index('--input') + 1])
    os.kill(int(data.read_text()), 0)
    print('50.00% lockd [.] example_hot_function')
else:
    pid = int(sys.argv[sys.argv.index('--pid') + 1])
    os.kill(pid, 0)
    ctl, ack = next(x for x in sys.argv if x.startswith('--control=')).split(':')[1].split(',')
    def stop(signum, frame):
        raise SystemExit(0)
    signal.signal(signal.SIGINT, stop)
    command = b''
    while not command.endswith(b'\\n'):
        command += os.read(int(ctl), 1)
    assert command == b'enable\\n'
    Path(sys.argv[sys.argv.index('--output') + 1]).write_text(str(pid))
    os.write(int(ack), b'ack\\n\\0')
    while True:
        signal.pause()
'''


@unittest.skipUnless(sys.platform == 'linux', 'Linux process supervision and /proc')
class LinuxAgentTests(unittest.TestCase):
    """Exercise actual agent processes, monitoring, gate handoff, EOF and deadlines."""

    def setUp(self) -> None:
        """
        Reserve a private test directory and a loopback endpoint for one stub daemon.
        """
        output = config.ROOT / 'output/obj' / config.host_architecture()
        output = output / 'opt/shared/verifications/benchmarking'
        output.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=output)
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            self.port = listener.getsockname()[1]
        self.sessions: list[Session] = []
        self.addCleanup(self.close_sessions)
        self.agent = Path(__file__).resolve().parents[1] / 'agent.py'

    def close_sessions(self) -> None:
        """Stop only the sessions and their children created by this test."""
        for session in reversed(self.sessions):
            session.close()

    def start(
        self, role: str, command: list[str], timeout: float = 10, **options
    ) -> Session:
        """Run the real agent with a local process boundary replacing SSH transport."""
        directory = self.directory / role
        directory.mkdir()
        job = {
            'role': role,
            'work': str(directory),
            'host': '127.0.0.1',
            'port': self.port,
            'barrier': 'agent_test_gate',
            'interval': 0.05,
            'timeout': timeout,
            'nofile': 1024,
            'command': command,
            **options,
        }
        (directory / 'job.json').write_text(json.dumps(job))
        remote = mock.Mock(work=str(directory))
        remote.command.return_value = [
            sys.executable,
            '-B',
            str(self.agent),
            'run',
            str(directory),
        ]
        session = Session(remote, self.directory, role)
        self.sessions.append(session)
        return session

    def events(self, role: str) -> list[dict]:
        """Read complete streamed events while an agent may still be appending."""
        path = self.directory / (role + '-stream.jsonl')
        return [json.loads(line) for line in path.read_text().splitlines()]

    def test_two_generators_wait_on_lockd_protocol_gate_and_collect_metrics(
        self,
    ) -> None:
        """
        Both agents queue before launch, then report their own monitored child
        processes.
        """
        server = self.start(
            'daemon', [sys.executable, '-u', '-c', SERVER, str(self.port)]
        )
        server.wait_ready(5)
        command = [sys.executable, '-c', 'import time; time.sleep(0.3)']
        first = self.start('worker-0', command)
        second = self.start('worker-1', command)
        first.wait_ready(5)
        second.wait_ready(5)
        for role in ('worker-0', 'worker-1'):
            self.assertNotIn(
                'launched', [event['event'] for event in self.events(role)]
            )
        server.control('release')
        self.assertEqual(first.finish(5), 0)
        self.assertEqual(second.finish(5), 0)
        for role in ('worker-0', 'worker-1'):
            events = self.events(role)
            self.assertIn('sample', [event['event'] for event in events])
            self.assertEqual(events[-1]['returncode'], 0)
        self.assertIsNone(server.process.poll())

    def test_eof_stops_child_and_does_not_touch_other_processes(self) -> None:
        """
        Closing the controller pipe terminates its daemon child and releases the
        endpoint.
        """
        server = self.start(
            'daemon', [sys.executable, '-u', '-c', SERVER, str(self.port)]
        )
        server.wait_ready(5)
        pid = next(
            event['pid']
            for event in self.events('daemon')
            if event['event'] == 'launched'
        )
        server.process.stdin.close()
        self.assertEqual(server.finish(5), 0)
        with self.assertRaises(ProcessLookupError):
            os.kill(pid, 0)
        self.assertTrue(Path('/proc/self').exists())

    def test_daemon_shutdown_releases_the_agents_coordination_lock_first(self) -> None:
        """
        A graceful daemon waiting for lock owners must not wait on its own agent.
        """
        graceful = '''
import signal
def stop(signum, frame):
    released = gate.acquire(timeout=2)
    raise SystemExit(0 if released else 7)
signal.signal(signal.SIGTERM, stop)
'''
        script = SERVER.replace('class Server(', graceful + '\nclass Server(')
        server = self.start(
            'daemon', [sys.executable, '-u', '-c', script, str(self.port)]
        )
        server.wait_ready(5)
        server.close()
        finished = [
            event for event in self.events('daemon') if event['event'] == 'finished'
        ]
        self.assertEqual(len(finished), 1)
        self.assertEqual(finished[0]['returncode'], 0)

    def test_eof_cancels_a_queued_generator_without_launching_it(self) -> None:
        """
        A controller disconnect during barrier wait must not start an orphan load
        process.
        """
        server = self.start(
            'daemon', [sys.executable, '-u', '-c', SERVER, str(self.port)]
        )
        server.wait_ready(5)
        worker = self.start('worker-0', [sys.executable, '-c', 'raise SystemExit(0)'])
        worker.wait_ready(5)
        worker.process.stdin.close()
        self.assertNotEqual(worker.finish(5), 0)
        self.assertNotIn(
            'launched', [event['event'] for event in self.events('worker-0')]
        )
        self.assertIsNone(server.process.poll())

    def test_deadline_stops_only_the_supervised_child(self) -> None:
        """
        A disconnected or unresponsive controller cannot run a daemon without a
        deadline.
        """
        server = self.start(
            'daemon', [sys.executable, '-u', '-c', SERVER, str(self.port)], timeout=2
        )
        server.wait_ready(5)
        pid = next(
            event['pid']
            for event in self.events('daemon')
            if event['event'] == 'launched'
        )
        self.assertNotEqual(server.finish(5), 0)
        with self.assertRaises(ProcessLookupError):
            os.kill(pid, 0)

    def test_builder_bundles_linked_libraries_and_records_build_identity(self) -> None:
        """Build the ordinary unprofiled bundle without profiling packages or flags."""
        self.exercise_builder(profile=False)

    def test_profiled_builder_keeps_optimization_and_adds_debug_information(
        self,
    ) -> None:
        """The optional bundle keeps Release and records its profiling build choice."""
        self.exercise_builder(profile=True)

    def test_async_builder_checks_the_built_binary_before_bundling(self) -> None:
        """Require parser support through the actual build path, with tools mocked."""
        self.exercise_builder(profile=False, require_async=True)

    def test_async_builder_rejects_an_old_parser_before_bundling(self) -> None:
        """An old binary must fail locally on the daemon VM before generators exist."""
        self.exercise_builder(profile=False, require_async=True, unsupported=True)

    def exercise_builder(
        self, *, profile: bool, require_async: bool = False, unsupported: bool = False
    ) -> None:
        """
        Package both tools and their real dependency paths after one configure/build.
        """
        archive = self.directory / 'source.tar.gz'
        with tarfile.open(archive, 'w:gz') as target:
            for name, data in (
                ('lockd-source/CMakeLists.txt', b'project(lockd)'),
                ('lockd-source/cmake/version.txt', b'1.0.42\n'),
            ):
                member = tarfile.TarInfo(name)
                member.size = len(data)
                target.addfile(member, io.BytesIO(data))
        library = self.directory / 'libboost-example.so.1'
        library.write_bytes(b'library data')
        calls = []

        def execute(arguments: list[str], **kwargs) -> str:
            """
            Replace package-manager/compiler boundaries while using real archive code.
            """
            calls.append(arguments)
            if '--engine=async' in arguments and unsupported:
                raise subprocess.CalledProcessError(1, arguments, 'Unknown engine')
            if (
                arguments[:2] == ['apt-get', 'install']
                and 'build-essential' in arguments
            ):
                self.assertIn('libboost-dev', arguments)
                self.assertNotIn('libboost-system-dev', arguments)
            if arguments[:2] == ['cmake', '--build']:
                root = self.directory / 'source/lockd-source'
                for relative in (
                    'output/x86_64/opt/shared/lockd',
                    'utility/lockd_test/output/x86_64/opt/shared/lockd_test',
                ):
                    binary = root / relative
                    binary.parent.mkdir(parents=True, exist_ok=True)
                    binary.write_bytes(b'executable data')
                    binary.chmod(0o755)
            if arguments[0] == 'ldd':
                return f'libboost-example.so.1 => {library} (0x1234)\n'
            return 'test build metadata'

        with (
            mock.patch.object(agent, 'execute', side_effect=execute),
            mock.patch.object(agent, 'probe_perf') as probe,
            mock.patch.object(agent.platform, 'machine', return_value='x86_64'),
            mock.patch.object(
                agent.platform,
                'freedesktop_os_release',
                return_value={'ID': 'ubuntu', 'VERSION_ID': '24.04'},
            ),
        ):
            if unsupported:
                with self.assertRaises(subprocess.CalledProcessError):
                    agent.build(
                        self.directory,
                        config.file_digest(archive),
                        profile=profile,
                        require_async=require_async,
                    )
                self.assertFalse((self.directory / 'bundle.tar.gz').exists())
                self.assertFalse(any(command[0] == 'ldd' for command in calls))
                return
            agent.build(
                self.directory,
                config.file_digest(archive),
                profile=profile,
                require_async=require_async,
            )
        parser_checks = [command for command in calls if '--engine=async' in command]
        self.assertEqual(len(parser_checks), int(require_async))
        if require_async:
            self.assertEqual(
                parser_checks[0][1:], ['--', '-B', '--engine=async', '--help']
            )
        self.assertEqual(
            sum(command[:2] == ['cmake', '--build'] for command in calls), 1
        )
        unpacked = self.directory / 'worker-unpack'
        agent.extract(self.directory / 'bundle.tar.gz', unpacked)
        for name in ('lockd', 'lockd_test', 'lib/libboost-example.so.1', 'build.json'):
            self.assertTrue((unpacked / 'bundle' / name).is_file())
        provenance = json.loads((unpacked / 'bundle/build.json').read_text())
        self.assertEqual(provenance['source_sha256'], config.file_digest(archive))
        self.assertEqual(provenance['version'], '1.0.42')
        self.assertEqual(provenance['profile_daemon'], profile)
        configure = next(command for command in calls if command[:2] == ['cmake', '-S'])
        self.assertIn('-DCMAKE_BUILD_TYPE=Release', configure)
        self.assertEqual('-DCMAKE_CXX_FLAGS=-g' in configure, profile)
        self.assertEqual(probe.call_count, 1 if profile else 0)

    def test_profile_ack_precedes_release_and_report_precedes_daemon_exit(self) -> None:
        """Use a perf protocol stub to exercise real descriptors and orderly cleanup."""
        tool = self.directory / 'perf'
        tool.write_text(PERF)
        tool.chmod(0o755)
        with mock.patch.dict(
            os.environ, {'PATH': str(self.directory) + os.pathsep + os.environ['PATH']}
        ):
            server = self.start(
                'daemon',
                [sys.executable, '-u', '-c', SERVER, str(self.port)],
                profile_daemon=True,
            )
            server.wait_ready(5)
            worker = self.start(
                'worker-0',
                [sys.executable, '-c', 'import time; time.sleep(.2)'],
            )
            worker.wait_ready(5)
            self.assertFalse((self.directory / 'daemon/perf.data').exists())
            server.release(5)
            self.assertEqual(worker.finish(5), 0)
            server.close()
        events = self.events('daemon')
        names = [event['event'] for event in events]
        self.assertLess(names.index('profiling'), names.index('released'))
        self.assertLess(names.index('profile_finished'), names.index('finished'))
        metadata = json.loads((self.directory / 'daemon/profile.json').read_text())
        self.assertEqual(metadata['status'], 'completed')
        self.assertIn(
            'example_hot_function',
            (self.directory / 'daemon/perf-report.txt').read_text(),
        )

    def test_perf_preflight_exercises_attachment_enable_and_report(self) -> None:
        """Run the production preflight with real pipes and the NUL-terminated stub."""
        tool = self.directory / 'perf'
        tool.write_text(PERF)
        tool.chmod(0o755)
        with mock.patch.dict(
            os.environ, {'PATH': str(self.directory) + os.pathsep + os.environ['PATH']}
        ):
            agent.probe_perf(self.directory)
        metadata = json.loads((self.directory / 'perf-probe/profile.json').read_text())
        self.assertEqual(metadata['status'], 'completed')
        self.assertTrue((self.directory / 'perf-probe/perf-report.txt').is_file())

    def test_perf_preflight_rejects_invalid_ack_without_report_masking_it(self) -> None:
        """A failed enable frame must remain the cause, without a no-samples report."""
        tool = self.directory / 'perf'
        tool.write_text(PERF.replace("b'ack\\n\\0'", "b'bad\\n\\0'"))
        tool.chmod(0o755)
        with (
            mock.patch.dict(
                os.environ,
                {'PATH': str(self.directory) + os.pathsep + os.environ['PATH']},
            ),
            self.assertRaisesRegex(RuntimeError, 'Invalid perf acknowledgement'),
        ):
            agent.probe_perf(self.directory)
        metadata = json.loads((self.directory / 'perf-probe/profile.json').read_text())
        self.assertEqual(metadata['status'], 'incomplete')
        self.assertFalse((self.directory / 'perf-probe/perf-report.txt').exists())

    def test_worker_output_retains_errors_without_stdout_chatter(self) -> None:
        """
        Error-only output keeps stderr diagnostics while discarding routine banners.
        """
        server = self.start(
            'daemon', [sys.executable, '-u', '-c', SERVER, str(self.port)]
        )
        server.wait_ready(5)
        worker = self.start(
            'worker-0',
            [
                sys.executable,
                '-c',
                'import sys; print("routine"); print("failure", file=sys.stderr)',
            ],
            errors_only=True,
        )
        worker.wait_ready(5)
        server.control('release')
        self.assertEqual(worker.finish(5), 0)
        self.assertEqual(
            (self.directory / 'worker-0/process.log').read_text(), 'failure\n'
        )

    def test_cancelled_profile_before_release_starts_no_recorder(self) -> None:
        """
        Stopping during fleet preparation records not_started without invoking perf.
        """
        server = self.start(
            'daemon',
            [sys.executable, '-u', '-c', SERVER, str(self.port)],
            profile_daemon=True,
        )
        server.wait_ready(5)
        server.close()
        metadata = json.loads((self.directory / 'daemon/profile.json').read_text())
        self.assertEqual(metadata['status'], 'not_started')
        self.assertNotIn('perf.data', metadata['artifacts'])

    def test_perf_failure_stops_the_daemon_and_retains_incomplete_metadata(
        self,
    ) -> None:
        """
        A failed recorder cannot leave an owned daemon running or claim completion.
        """
        tool = self.directory / 'perf'
        tool.write_text('#!/usr/bin/python3\nraise SystemExit(3)\n')
        tool.chmod(0o755)
        with mock.patch.dict(
            os.environ, {'PATH': str(self.directory) + os.pathsep + os.environ['PATH']}
        ):
            server = self.start(
                'daemon',
                [sys.executable, '-u', '-c', SERVER, str(self.port)],
                profile_daemon=True,
            )
            server.wait_ready(5)
            pid = next(
                event['pid']
                for event in self.events('daemon')
                if event['event'] == 'launched'
            )
            server.control('release')
            self.assertNotEqual(server.finish(5), 0)
            with self.assertRaisesRegex(RuntimeError, 'profile supervision failed'):
                server.close()
            self.sessions.remove(server)
        with self.assertRaises(ProcessLookupError):
            os.kill(pid, 0)
        metadata = json.loads((self.directory / 'daemon/profile.json').read_text())
        self.assertEqual(metadata['status'], 'incomplete')


if __name__ == '__main__':
    with contextlib.suppress(BrokenPipeError):
        unittest.main()

"""Check Linux privilege-drop failures and, when run as root, the resulting identity."""

import os
import pwd
import socket
import subprocess
import tempfile
import time
import unittest
from pathlib import Path


class StartupTests(unittest.TestCase):
    """Run only temporary daemon processes, with bounded startup and shutdown."""

    directory: Path
    temporary: tempfile.TemporaryDirectory[str]
    shim: Path
    executable: str

    @classmethod
    def setUpClass(cls) -> None:
        """Build the syscall fault boundary without changing any system libraries."""
        cls.executable = os.environ['LOCKD_TEST_DAEMON']
        cls.temporary = tempfile.TemporaryDirectory(prefix='lockd-startup-')
        cls.directory = Path(cls.temporary.name)
        cls.shim = cls.directory / 'startup_failures.so'
        subprocess.run(
            [
                'cc',
                '-shared',
                '-fPIC',
                '-Wall',
                '-Wextra',
                '-Werror',
                str(
                    Path(__file__).resolve().parents[1]
                    / 'source'
                    / 'startup_failures.c'
                ),
                '-o',
                str(cls.shim),
            ],
            check=True,
            timeout=30,
        )

    @classmethod
    def tearDownClass(cls) -> None:
        """Remove the temporary shim and files made by these processes."""
        cls.temporary.cleanup()

    def command(self) -> list[str]:
        """Use explicit local paths and numeric identities available on this system."""
        return [
            self.executable,
            '--interface',
            '127.0.0.1',
            '--port',
            '0',
            '--threads',
            '1',
            '--user',
            str(os.getuid()),
            '--group',
            str(os.getgid()),
            '--pid-file',
            str(self.directory / f'{self._testMethodName}.pid'),
            '--log-file',
            str(self.directory / 'lockd.log'),
            '--emit-running',
        ]

    @unittest.skipUnless(os.geteuid() == 0, "Privilege-drop startup requires root")
    def test_privilege_failure_stops_startup(self) -> None:
        """Reject supplementary-group, primary-group, and UID failures individually."""
        for operation, diagnostic in (
            ('initgroups', "Unable to initialize groups"),
            ('setgid', "Unable to drop gid"),
            ('setuid', "Unable to drop uid"),
        ):
            with self.subTest(operation=operation):
                result: subprocess.CompletedProcess[str] = subprocess.run(
                    self.command() + ['--no-fork'],
                    cwd=self.directory,
                    env={
                        **os.environ,
                        'LD_PRELOAD': str(self.shim),
                        'LOCKD_TEST_FAIL': operation,
                    },
                    capture_output=True,
                    text=True,
                    timeout=5,
                    check=False,
                )
                output: str = result.stdout + result.stderr
                self.assertNotEqual(result.returncode, 0, output)
                self.assertIn(diagnostic, output)
                self.assertNotIn("lockd is running", output)

    def test_fork_failure_stops_startup(self) -> None:
        """Report fork failure instead of treating pid -1 as a successful parent."""
        result: subprocess.CompletedProcess[str] = subprocess.run(
            self.command() + ['--no-drop'],
            cwd=self.directory,
            env={**os.environ, 'LD_PRELOAD': str(self.shim)},
            capture_output=True,
            text=True,
            timeout=5,
            check=False,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Unable to fork", result.stdout + result.stderr)

    @unittest.skipUnless(
        os.geteuid() == 0, "Real privilege-drop verification requires root"
    )
    def test_real_drop_removes_root_identity_and_groups(self) -> None:
        """Inspect the running child after real initgroups, setgid, and setuid calls."""
        user: pwd.struct_passwd = pwd.getpwnam('nobody')
        with tempfile.TemporaryDirectory(prefix='lockd-drop-') as temporary:
            directory: Path = Path(temporary)
            directory.chmod(0o777)
            log: Path = directory / 'lockd.log'
            log.touch()
            log.chmod(0o666)
            with socket.socket() as reservation:
                reservation.bind(('127.0.0.1', 0))
                port: int = reservation.getsockname()[1]
            child: subprocess.Popen[bytes] = subprocess.Popen(
                [
                    self.executable,
                    '--no-fork',
                    '--user',
                    str(user.pw_uid),
                    '--group',
                    str(user.pw_gid),
                    '--interface',
                    '127.0.0.1',
                    '--threads',
                    '1',
                    '--port',
                    str(port),
                    '--pid-file',
                    str(directory / 'lockd.pid'),
                    '--log-file',
                    str(log),
                ],
                cwd=directory,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            try:
                deadline: float = time.monotonic() + 5
                while time.monotonic() < deadline:
                    self.assertIsNone(
                        child.poll(), "Daemon exited before accepting connections"
                    )
                    try:
                        with socket.create_connection(('127.0.0.1', port), timeout=0.1):
                            break
                    except OSError:
                        time.sleep(0.01)
                else:
                    self.fail("Daemon did not start within five seconds")
                status: dict[str, list[str]] = {
                    line.split(':', 1)[0]: line.split(':', 1)[1].split()
                    for line in Path(f'/proc/{child.pid}/status')
                    .read_text()
                    .splitlines()
                }
                self.assertEqual(status['Uid'], [str(user.pw_uid)] * 4)
                self.assertEqual(status['Gid'], [str(user.pw_gid)] * 4)
                self.assertEqual(
                    set(map(int, status['Groups'])),
                    set(os.getgrouplist(user.pw_name, user.pw_gid)),
                )

                # Supplying a PID file in an existing directory must not give that directory away.
                self.assertEqual(directory.stat().st_uid, 0)
            finally:
                child.terminate()
                child.wait(timeout=5)


if __name__ == '__main__':
    unittest.main()

"""Exercise the installed wheel against a temporary loopback daemon."""

import argparse
import asyncio
import logging
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from typing import Any

import pylockd

LOGGER = logging.getLogger(__name__)


def create_mutex(port: int, name: str, timed: bool = False) -> Any:
    """Create a native mutex; the extension is the dynamic boundary of these tests."""
    mutex_type: Any = pylockd.TimedMutex if timed else pylockd.Mutex
    return mutex_type(
        hostname='127.0.0.1', port=port, connect_timeout=3, lock_name=name
    )


def ownership_scenario(port: int) -> None:
    """Retain a temporary or explicitly deleted mutex until its lock is gone."""
    for timed in (False, True):
        lock_type: Any = pylockd.ScopedTimedLock if timed else pylockd.ScopedLock
        lock: Any = lock_type(create_mutex(port, f'temporary-{timed}', timed))
        with lock as entered:
            assert entered is lock
            assert lock.owns_lock()
        assert not lock.owns_lock()

        mutex: Any = create_mutex(port, f'deleted-{timed}', timed)
        lock = lock_type(mutex)
        del mutex
        try:
            with lock:
                raise ValueError("Preserve the exception from the with body")
        except ValueError as error:
            assert str(error) == "Preserve the exception from the with body"
        else:
            raise AssertionError("The context manager suppressed the exception")
        assert not lock.owns_lock()
        del lock


def gil_scenario(port: int, name: str) -> None:
    """Let one Python thread release a lock while another thread waits for it."""

    # Keep both mutex wrappers here so that this scenario isolates the GIL contract from the
    # separate temporary-owner regression above.
    owner_mutex: Any = create_mutex(port, name)
    waiter_mutex: Any = create_mutex(port, name)
    owner: Any = pylockd.ScopedLock(owner_mutex)
    waiter: Any = pylockd.ScopedLock(waiter_mutex)
    owner.lock()
    acquired: threading.Event = threading.Event()
    errors: list[BaseException] = []

    def wait_for_lock() -> None:
        """Acquire through a second connection and record exceptions for the parent."""
        try:
            waiter.lock()
            acquired.set()
            waiter.unlock()
        except BaseException as error:
            LOGGER.exception('Background lock acquisition failed')
            errors.append(error)

    thread: threading.Thread = threading.Thread(target=wait_for_lock, daemon=True)
    thread.start()

    # The parent sends this byte only after the daemon has received both LOCK commands. This
    # makes the wait real without a timing guess. With the old binding the waiting thread keeps
    # the GIL, so this thread cannot return from read(), and the parent bounds the whole process.
    assert sys.stdin.buffer.read(1) == b'x'
    owner.unlock()
    thread.join(timeout=3)
    assert not thread.is_alive(), "The waiting Python thread did not finish"
    assert acquired.is_set(), errors
    assert not errors, errors


def exception_scenario(port: int) -> None:
    """Restore the GIL when a blocking constructor or lock operation throws."""
    lock: Any = pylockd.ScopedLock(create_mutex(port, 'exception'))
    with lock:
        try:
            lock.lock()
        except pylockd.Exception as error:
            assert error.cause.error_type() == pylockd.ErrorType.log_error
        else:
            raise AssertionError("Locking an owned unique_lock should throw")

    # A bound, non-listening socket reserves a port which must refuse the constructor's connect.
    with socket.socket() as unavailable:
        unavailable.bind(('127.0.0.1', 0))
        try:
            create_mutex(unavailable.getsockname()[1], 'unavailable')
        except pylockd.Exception:
            pass
        else:
            raise AssertionError("Connecting to a non-listening port should throw")
    assert not lock.owns_lock()


def deadline_scenario() -> None:
    """Check fractional absolute deadlines without waiting for real expiry."""
    commands: list[bytes] = []
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        listener.listen(1)
        listener.settimeout(3)

        def serve_deadline() -> None:
            """Record the timed request and return a protocol timeout."""
            connection, _ = listener.accept()
            with connection:
                connection.settimeout(3)
                connection.sendall(b'LOCKD\n')
                with connection.makefile('rb') as stream:
                    commands.append(stream.readline())
                    connection.sendall(b'LOCK_ACQUIRING\nLOCK_TIMED_OUT\n')

        thread: threading.Thread = threading.Thread(target=serve_deadline, daemon=True)
        thread.start()
        mutex: Any = create_mutex(listener.getsockname()[1], 'deadline', timed=True)
        lock: Any = pylockd.ScopedTimedLock(mutex)
        now: float = time.time()
        deadline: float = float(int(now) + 11) + 0.875
        expected_ms: int = int((deadline - now) * 1000)
        assert not lock.try_lock_until(deadline)
        thread.join(timeout=3)
        assert not thread.is_alive()
        time_spec: bytes = commands[0].split()[2]
        assert time_spec.endswith(b'ms'), time_spec
        assert abs(int(time_spec[:-2]) - expected_ms) < 150, (time_spec, expected_ms)


def async_shutdown_scenario(port: int) -> tuple[Any, ...]:
    """Leave held and pending sockets alive through event-loop/interpreter shutdown."""
    loop: asyncio.AbstractEventLoop = asyncio.new_event_loop()

    async def scopes() -> tuple[Any, Any]:
        """Connect both clients on the event loop used for their operations."""
        return (
            pylockd.AsyncScopedLock(await create_async_mutex(port)),
            pylockd.AsyncScopedLock(await create_async_mutex(port)),
        )

    owner, waiter = loop.run_until_complete(scopes())
    loop.run_until_complete(owner.lock())

    async def pending() -> tuple[Any, Any]:
        """Start an awaitable directly so no abandoned Task diagnostic is expected."""
        operation: Any = waiter.lock()
        iterator: Any = operation.__await__()
        next(iterator)
        return operation, iterator

    operation, iterator = loop.run_until_complete(pending())
    loop.close()

    # Retain these until interpreter teardown: the native atexit hook must close its worker
    # before Python destroys scopes and awaitables belonging to a closed event loop.
    return owner, waiter, operation, iterator, loop


def create_async_mutex(port: int) -> Any:
    """Use one daemon lock name to leave the shutdown scenario genuinely contended."""
    return pylockd.AsyncMutex('127.0.0.1', port, 1, 'async-shutdown')


class BindingApiTests(unittest.TestCase):
    """Check exported types and exceptions without requiring a running daemon."""

    def test_sync_mutex_types_reject_invalid_names_before_connecting(self) -> None:
        """Reject invalid names through each binding before connection setup."""
        alphabet: str = (
            'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-'
        )
        names: list[str] = ['', 'n' * 257, 'injected\nUNLOCK', 'caf\u00e9']
        names += [
            f'prefix{chr(byte)}suffix'
            for byte in range(256)
            if chr(byte) not in alphabet
        ]
        with socket.socket() as reservation:
            reservation.bind(('127.0.0.1', 0))
            port: int = reservation.getsockname()[1]
            for mutex_type in (
                pylockd.Mutex,
                pylockd.TimedMutex,
                pylockd.RecursiveMutex,
            ):
                for name in names:
                    with self.subTest(mutex_type=mutex_type.__name__, name=name):
                        with self.assertRaises(pylockd.Exception) as caught:
                            mutex_type('127.0.0.1', port, 1, name)
                        self.assertEqual(
                            caught.exception.cause.error_type(),
                            pylockd.ErrorType.log_error,
                        )
                        self.assertEqual(
                            Path(caught.exception.cause.file()).as_posix(),
                            'lib/liblockd/source/detail/client_contract.cpp',
                        )

    def test_sync_timeouts_cannot_overflow_the_native_clock(self) -> None:
        """Pass signed 64-bit seconds through each wrapper and validate before connecting."""
        for mutex_type in (pylockd.Mutex, pylockd.TimedMutex, pylockd.RecursiveMutex):
            for timeout in (-1, (1 << 63) - 1):
                with self.subTest(mutex_type=mutex_type, timeout=timeout):
                    with self.assertRaises(pylockd.Exception) as caught:
                        mutex_type('127.0.0.1', 1, timeout, 'invalid-timeout')
                    self.assertIn('Connection timeout must', str(caught.exception))

    def test_async_mutex_construction_needs_neither_a_loop_nor_a_connection(
        self,
    ) -> None:
        """Allow connection settings and scopes to be created before asyncio.run()."""
        mutex: Any = pylockd.AsyncMutex('127.0.0.1', 1, 1, 'lazy')
        scope: Any = pylockd.AsyncScopedLock(mutex)
        self.assertFalse(scope.owns_lock())

    def test_async_runtime_shutdown_uses_the_public_exception(self) -> None:
        """Reject work after shutdown in a child without stopping this test runtime."""
        script: str = """
import asyncio
import pylockd

async def main():
    pylockd._shutdown_async()
    try:
        await pylockd.AsyncMutex('127.0.0.1', 1, 1, 'shutdown')
    except pylockd.Exception as error:
        assert 'shut down' in str(error), error
        assert error.cause.error_type() == pylockd.ErrorType.log_error, error
        assert error.what == error.cause.what(), error
    else:
        raise AssertionError('Acquisition after shutdown must fail')

asyncio.run(main())
"""
        result = subprocess.run(
            [sys.executable, '-c', script],
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_exported_types_use_camel_case(self) -> None:
        """Check class names, snake_case methods, and enum values."""
        exported_types: dict[str, type] = {
            name: value
            for name, value in vars(pylockd).items()
            if isinstance(value, type) and not name.startswith('_')
        }
        self.assertEqual(
            set(exported_types),
            {
                'Mutex',
                'TimedMutex',
                'ScopedLock',
                'ScopedTimedLock',
                'Exception',
                'ErrorType',
                'AsyncMutex',
                'AsyncScopedLock',
                'AsyncTimedMutex',
                'AsyncScopedTimedLock',
                'RecursiveMutex',
                'ScopedRecursiveLock',
                'AsyncRecursiveMutex',
                'AsyncScopedRecursiveLock',
            },
        )
        for name, exported_type in exported_types.items():
            with self.subTest(name=name):
                self.assertEqual(exported_type.__name__, name)
        self.assertTrue(callable(pylockd.ScopedLock.try_lock))
        self.assertTrue(callable(pylockd.ScopedTimedLock.try_lock_until))
        self.assertTrue(callable(pylockd.ScopedTimedLock.try_lock_for))
        self.assertIsInstance(pylockd.ErrorType.not_connected, pylockd.ErrorType)

    def test_exception_inherits_from_python_exception(self) -> None:
        """Allow normal Python construction, raising, and generic exception handling."""
        self.assertTrue(issubclass(pylockd.Exception, Exception))
        message: str = 'A lockd error raised from Python'
        instance: Exception = pylockd.Exception(message)
        self.assertIsInstance(instance, Exception)
        self.assertEqual(instance.args, (message,))
        self.assertEqual(str(instance), message)
        with self.assertRaises(Exception) as caught:
            raise instance
        self.assertIs(caught.exception, instance)

    def test_native_exception_is_caught_by_python_exception(self) -> None:
        """Keep native error metadata when catching the Python base class."""
        with self.assertRaises(Exception) as caught:
            pylockd.raiser()
        error = caught.exception
        self.assertIsInstance(error, pylockd.Exception)
        self.assertEqual(error.args, ('Not connected to lockd.',))
        self.assertEqual(str(error), error.what)
        self.assertEqual(error.what, error.cause.what())
        self.assertEqual(error.cause.error_type(), pylockd.ErrorType.not_connected)


class InstalledWheelTests(unittest.TestCase):
    """Keep native hangs or ownership crashes isolated from the test runner."""

    temporary: tempfile.TemporaryDirectory[str]
    directory: Path
    daemon: subprocess.Popen[bytes]
    port: int
    log_path: Path

    @classmethod
    def setUpClass(cls) -> None:
        """Launch one daemon with trace logs on a free loopback port."""
        cls.temporary = tempfile.TemporaryDirectory(prefix='pylockd-test-')
        cls.directory = Path(cls.temporary.name)
        cls.log_path = cls.directory / 'lockd.log'
        with socket.socket() as reservation:
            reservation.bind(('127.0.0.1', 0))
            cls.port = reservation.getsockname()[1]
        executable: str = os.environ['LOCKD_TEST_DAEMON']
        command: list[str] = [
            executable,
            '--interface',
            '127.0.0.1',
            '--port',
            str(cls.port),
            '--threads',
            '2',
            '--log-level',
            'trace',
        ]
        if os.name != 'nt':
            command += ['--no-fork', '--no-drop']
        cls.daemon = subprocess.Popen(
            command,
            cwd=cls.directory,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            cls._wait_for_startup()
        except BaseException:
            cls.tearDownClass()
            raise

    @classmethod
    def _wait_for_startup(cls) -> None:
        """Bound startup without depending on how quickly the daemon is scheduled."""
        deadline: float = time.monotonic() + 5
        while time.monotonic() < deadline:
            if cls.daemon.poll() is not None:
                raise AssertionError(f"Daemon exited with {cls.daemon.returncode}")
            try:
                with socket.create_connection(('127.0.0.1', cls.port), timeout=0.2):
                    return
            except OSError:
                time.sleep(0.01)
        raise AssertionError("Daemon did not start within five seconds")

    @classmethod
    def tearDownClass(cls) -> None:
        """Stop only this suite's daemon and remove its temporary logs."""
        if cls.daemon.poll() is None:
            cls.daemon.terminate()
        cls.daemon.wait(timeout=5)
        cls.temporary.cleanup()

    def _command(self, scenario: str, name: str = '') -> list[str]:
        """Run each native scenario in a process whose lifetime the parent can bound."""
        return [
            sys.executable,
            str(Path(__file__).resolve()),
            '--scenario',
            scenario,
            '--port',
            str(self.port),
            '--name',
            name,
        ]

    def test_sync_connection_timeout_accepts_64_bit_seconds(self) -> None:
        """Connect with seconds beyond signed 32-bit range using every synchronous wrapper."""
        for mutex_type, guard_type in (
            (pylockd.Mutex, pylockd.ScopedLock),
            (pylockd.TimedMutex, pylockd.ScopedTimedLock),
            (pylockd.RecursiveMutex, pylockd.ScopedRecursiveLock),
        ):
            with self.subTest(mutex_type=mutex_type):
                mutex = mutex_type('127.0.0.1', self.port, 1 << 31, 'wide-timeout')
                with guard_type(mutex) as guard:
                    self.assertTrue(guard.owns_lock())

    def test_mutex_ownership_and_context_manager(self) -> None:
        """Keep mutex ownership and propagate errors from a with body."""
        subprocess.run(
            self._command('ownership'), cwd=self.directory, check=True, timeout=10
        )

    def test_sync_guard_misuse_uses_the_public_exception(self) -> None:
        """Reject invalid guard operations before unique_lock, preserving ownership and reuse."""
        for mutex_type, scope_type, timed in (
            (pylockd.Mutex, pylockd.ScopedLock, False),
            (pylockd.TimedMutex, pylockd.ScopedTimedLock, True),
            (pylockd.RecursiveMutex, pylockd.ScopedRecursiveLock, False),
        ):
            with self.subTest(mutex_type=mutex_type.__name__):
                scope: Any = scope_type(
                    mutex_type('127.0.0.1', self.port, 1, 'guard-errors')
                )
                with self.assertRaises(pylockd.Exception) as caught:
                    scope.unlock()
                self.assertIn('_checkOwnership', caught.exception.cause.function())
                self.assertEqual(
                    caught.exception.cause.error_type(), pylockd.ErrorType.log_error
                )
                with scope:
                    operations: dict[str, Any] = (
                        {
                            'timed_lock': scope.timed_lock,
                            'try_lock_for': lambda scope=scope: scope.try_lock_for(1),
                            'try_lock_until': lambda scope=scope: scope.try_lock_until(
                                time.time() + 1
                            ),
                        }
                        if timed
                        else {'lock': scope.lock, 'try_lock': scope.try_lock}
                    )
                    for name, operation in operations.items():
                        with self.subTest(operation=name):
                            with self.assertRaises(pylockd.Exception) as caught:
                                operation()
                            self.assertIn(
                                '_checkOwnership', caught.exception.cause.function()
                            )
                            self.assertEqual(
                                caught.exception.cause.error_type(),
                                pylockd.ErrorType.log_error,
                            )
                            self.assertEqual(
                                caught.exception.what, caught.exception.cause.what()
                            )
                            self.assertTrue(scope.owns_lock())
                self.assertFalse(scope.owns_lock())
                with scope:
                    self.assertTrue(scope.owns_lock())

    def test_gil_is_restored_after_an_exception(self) -> None:
        """Keep executing Python after C++ unwinds through a released-GIL scope."""
        subprocess.run(
            self._command('exception'), cwd=self.directory, check=True, timeout=10
        )

    def test_absolute_deadline_keeps_fractional_seconds(self) -> None:
        """Preserve fractional epoch seconds when sending the timed request."""
        subprocess.run(
            self._command('deadline'), cwd=self.directory, check=True, timeout=10
        )

    def test_async_runtime_stops_with_live_scopes_and_a_closed_loop(self) -> None:
        """Bound interpreter exit while the native runtime still owns connections."""
        result = subprocess.run(
            self._command('async-shutdown'),
            check=False,
            cwd=self.directory,
            capture_output=True,
            timeout=10,
        )
        self.assertEqual(result.returncode, 0, (result.stdout, result.stderr))
        self.assertNotIn(b'Exception ignored', result.stderr)

    def test_exception_details_belong_to_each_instance(self) -> None:
        """Keep an older exception's metadata after a different native call fails."""
        retained: Any = None
        try:
            pylockd.raiser()
        except pylockd.Exception as error:
            retained = error
        self.assertIsNotNone(retained)
        self.assertEqual(
            Path(retained.cause.file()).as_posix(),
            'lib/pylockd/source/detail/sync_detail.cpp',
        )
        original: tuple[str, str, str, int] = (
            str(retained),
            retained.what,
            retained.cause.what(),
            retained.cause.line(),
        )
        with socket.socket() as unavailable:
            unavailable.bind(('127.0.0.1', 0))
            with self.assertRaises(pylockd.Exception) as second:
                create_mutex(unavailable.getsockname()[1], 'second-error')
        self.assertNotEqual(str(second.exception), str(retained))
        self.assertIsNot(retained.cause, second.exception.cause)
        self.assertEqual(
            original,
            (
                str(retained),
                retained.what,
                retained.cause.what(),
                retained.cause.line(),
            ),
        )
        self.assertNotIn('cause', vars(pylockd.Exception))

    def test_fragmented_reply_waits_for_a_complete_line(self) -> None:
        """Hold back a reply suffix while the native client processes its prefix."""
        errors: list[BaseException] = []
        prefix_sent: threading.Event = threading.Event()
        release_suffix: threading.Event = threading.Event()
        finished: threading.Event = threading.Event()
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            listener.listen(1)
            listener.settimeout(3)

            def serve_fragments() -> None:
                """Control TCP framing and acknowledge the later unlock normally."""
                try:
                    connection, _ = listener.accept()
                    with connection:
                        connection.settimeout(3)
                        connection.sendall(b'LOCKD\n')
                        with connection.makefile('rb') as stream:
                            assert stream.readline().startswith(b'TRY_LOCK ')
                            connection.sendall(b'LOCK_')
                            prefix_sent.set()
                            assert release_suffix.wait(timeout=3)
                            connection.sendall(b'ACQUIRED\n')
                            assert stream.readline() == b'UNLOCK\n'
                            connection.sendall(b'LOCK_RELEASED\n')
                except BaseException as error:
                    LOGGER.exception('Fragmented-reply peer failed')
                    errors.append(error)

            def acquire() -> None:
                """Run the native call separately so we can observe its completion."""
                try:
                    lock: Any = pylockd.ScopedLock(
                        create_mutex(listener.getsockname()[1], 'fragmented'),
                    )
                    assert lock.try_lock()
                    lock.unlock()
                except BaseException as error:
                    LOGGER.exception('Fragmented-reply acquisition failed')
                    errors.append(error)
                finally:
                    finished.set()

            server: threading.Thread = threading.Thread(
                target=serve_fragments, daemon=True
            )
            client: threading.Thread = threading.Thread(target=acquire, daemon=True)
            server.start()
            client.start()
            try:
                self.assertTrue(prefix_sent.wait(timeout=3))

                # Give an incorrect early completion a bounded opportunity to arrive. The suffix
                # is controlled by this event, so the client cannot legitimately finish yet.
                self.assertFalse(finished.wait(timeout=0.1), errors)
            finally:
                release_suffix.set()
                client.join(timeout=4)
                server.join(timeout=4)
            self.assertFalse(client.is_alive())
            self.assertFalse(server.is_alive())
            self.assertEqual(errors, [])

    def test_waiting_for_a_lock_releases_the_gil(self) -> None:
        """Coordinate the lock handoff through observed daemon requests, not a sleep."""
        name: str = 'gil-handoff'
        child: subprocess.Popen[bytes] = subprocess.Popen(
            self._command('gil', name),
            cwd=self.directory,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        try:
            deadline: float = time.monotonic() + 5
            while time.monotonic() < deadline:
                log: str = self.log_path.read_text() if self.log_path.exists() else ''
                requests: int = sum(
                    "Lock acquisition request" in line and f"'{name}'" in line
                    for line in log.splitlines()
                )
                if requests >= 2:
                    break
                if child.poll() is not None:
                    self.fail(f"Scenario exited early: {child.communicate()}")
                time.sleep(0.01)
            else:
                self.fail(
                    "The daemon did not receive the waiting client's LOCK command"
                )
            output, errors = child.communicate(input=b'x', timeout=5)
            self.assertEqual(child.returncode, 0, (output, errors))
        finally:
            if child.poll() is None:
                child.kill()
                child.communicate(timeout=5)


if __name__ == '__main__':
    if '--scenario' in sys.argv:
        parser: argparse.ArgumentParser = argparse.ArgumentParser()
        parser.add_argument(
            '--scenario',
            choices=['ownership', 'gil', 'exception', 'deadline', 'async-shutdown'],
            required=True,
        )
        parser.add_argument('--port', type=int, required=True)
        parser.add_argument('--name', default='')
        arguments: argparse.Namespace = parser.parse_args()
        match arguments.scenario:
            case 'ownership':
                ownership_scenario(arguments.port)
            case 'gil':
                gil_scenario(arguments.port, arguments.name)
            case 'exception':
                exception_scenario(arguments.port)
            case 'deadline':
                deadline_scenario()
            case 'async-shutdown':
                shutdown_objects = async_shutdown_scenario(arguments.port)
    else:
        unittest.main()

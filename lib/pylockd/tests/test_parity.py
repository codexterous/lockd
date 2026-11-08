"""Check common synchronous and asynchronous lock contracts against the same daemon."""

import asyncio
import subprocess
import sys
import unittest
from typing import Any

import pylockd
import test_async as async_tests
import test_lifetimes as lifetime_tests

MUTEX_PAIRS: tuple[tuple[str, str], ...] = (
    ('Mutex', 'ScopedLock'),
    ('TimedMutex', 'ScopedTimedLock'),
    ('RecursiveMutex', 'ScopedRecursiveLock'),
)


class BindingParityTests(unittest.TestCase):
    """Compare public methods and reject invalid objects at the binding boundary."""

    def test_scopes_expose_the_same_operations(self) -> None:
        """Keep ordinary and timed operations available in both Python interfaces."""
        for _, scope_name in MUTEX_PAIRS:
            with self.subTest(scope=scope_name):
                sync_type: Any = getattr(pylockd, scope_name)
                async_type: Any = getattr(pylockd, f'Async{scope_name}')
                self.assertEqual(
                    {name for name in dir(sync_type) if not name.startswith('_')},
                    {name for name in dir(async_type) if not name.startswith('_')},
                )

    def test_inheritance_matches_the_cpp_mutex_families(self) -> None:
        """Expose the same base classes for corresponding mutexes and guards."""
        for prefix in ('', 'Async'):
            with self.subTest(interface=prefix):
                for derived in ('TimedMutex', 'RecursiveMutex'):
                    self.assertTrue(
                        issubclass(
                            getattr(pylockd, prefix + derived),
                            getattr(pylockd, prefix + 'Mutex'),
                        )
                    )
                for derived in ('ScopedTimedLock', 'ScopedRecursiveLock'):
                    self.assertTrue(
                        issubclass(
                            getattr(pylockd, prefix + derived),
                            getattr(pylockd, prefix + 'ScopedLock'),
                        )
                    )

    def test_constructor_validation_matches(self) -> None:
        """Reject the same names, integer ranges, and unsupported constructor options."""
        for mutex_name, _ in MUTEX_PAIRS:
            for arguments in (
                ('127.0.0.1', 1, 1, ''),
                ('127.0.0.1', 1, 1, 'x' * 257),
                ('127.0.0.1', 1, 1, 'a\nUNLOCK'),
                ('127.0.0.1', 1, 1, '\u00e9'),
                ('127.0.0.1', 1, -1, 'name'),
                ('127.0.0.1', 1, (1 << 63) - 1, 'name'),
                ('127.0.0.1', 1, 0.5, 'name'),
                ('127.0.0.1', 1, float('nan'), 'name'),
                ('127.0.0.1', 1, 1, 'name', 1),
            ):
                results: list[tuple[type[BaseException], object, str]] = []
                for prefix in ('', 'Async'):
                    with self.subTest(mutex=prefix + mutex_name, args=arguments):
                        try:
                            getattr(pylockd, prefix + mutex_name)(*arguments)
                        except pylockd.Exception as error:
                            results.append(
                                (type(error), error.cause.error_type(), error.what)
                            )
                        except TypeError:
                            results.append((TypeError, None, ''))
                        else:
                            self.fail('Invalid constructor arguments were accepted')
                self.assertEqual(results[0], results[1])

    def test_invalid_scope_arguments_raise_type_errors(self) -> None:
        """Reject None and wrong object types without crashing the interpreter."""
        for _, scope_name in MUTEX_PAIRS:
            for prefix in ('', 'Async'):
                name: str = prefix + scope_name
                with self.subTest(scope=name):
                    code: str = (
                        'import pylockd\n'
                        'for value in (None, object(), 1):\n'
                        '    try:\n'
                        f'        pylockd.{name}(value)\n'
                        '    except TypeError:\n'
                        '        pass\n'
                        '    else:\n'
                        '        raise AssertionError("Invalid mutex accepted")\n'
                    )
                    result: subprocess.CompletedProcess[str] = subprocess.run(
                        [sys.executable, '-X', 'faulthandler', '-c', code],
                        capture_output=True,
                        text=True,
                        timeout=10,
                        check=False,
                    )
                    self.assertEqual(
                        result.returncode, 0, (result.stdout, result.stderr)
                    )


class DaemonParityTests(unittest.TestCase):
    """Run identical ownership transitions through each public lock family."""

    @classmethod
    def setUpClass(cls) -> None:
        """Launch the existing real-daemon fixture without inheriting its tests."""
        lifetime_tests.InstalledWheelTests.setUpClass()
        cls.port: int = lifetime_tests.InstalledWheelTests.port

    @classmethod
    def tearDownClass(cls) -> None:
        """Stop this suite's daemon and remove its temporary files."""
        lifetime_tests.InstalledWheelTests.tearDownClass()

    def test_ownership_contention_and_reuse_match(self) -> None:
        """Match acquisition, failed try, misuse, release, and reuse in every family."""
        for mutex_name, scope_name in MUTEX_PAIRS:
            for asynchronous in (False, True):
                with self.subTest(mutex=mutex_name, asynchronous=asynchronous):
                    asyncio.run(
                        self._check_ownership(mutex_name, scope_name, asynchronous)
                    )

    async def _check_ownership(
        self, mutex_name: str, scope_name: str, asynchronous: bool
    ) -> None:
        """Use one sequence of assertions for synchronous and awaited operations."""
        prefix: str = 'Async' if asynchronous else ''
        mutex_type: Any = getattr(pylockd, prefix + mutex_name)
        scope_type: Any = getattr(pylockd, prefix + scope_name)
        name: str = f'parity-{mutex_name}'
        owner_mutex: Any = mutex_type('127.0.0.1', self.port, 1, name)
        contender_mutex: Any = mutex_type('127.0.0.1', self.port, 1, name)
        if asynchronous:
            owner_mutex = await owner_mutex
            contender_mutex = await contender_mutex
        owner: Any = scope_type(owner_mutex)
        contender: Any = scope_type(contender_mutex)

        async def invoke(scope: Any, operation: str) -> Any:
            """Await only the asynchronous interface; preserve results and errors."""
            result: Any = getattr(scope, operation)()
            if asynchronous:
                async with asyncio.timeout(3):
                    return await result
            return result

        self.assertFalse(owner.owns_lock())

        with self.assertRaises(pylockd.Exception) as caught:
            await invoke(owner, 'unlock')
        self.assertEqual(
            caught.exception.cause.error_type(), pylockd.ErrorType.log_error
        )
        try:
            self.assertIsNone(await invoke(owner, 'lock'))
            self.assertTrue(owner.owns_lock())
            with self.assertRaises(pylockd.Exception) as caught:
                await invoke(owner, 'try_lock')
            self.assertEqual(
                caught.exception.cause.error_type(), pylockd.ErrorType.log_error
            )
            self.assertTrue(owner.owns_lock())
            self.assertIs(await invoke(contender, 'try_lock'), False)
            self.assertFalse(contender.owns_lock())
        finally:
            if owner.owns_lock():
                self.assertIsNone(await invoke(owner, 'unlock'))
        self.assertFalse(owner.owns_lock())
        try:
            self.assertIs(await invoke(contender, 'try_lock'), True)
            self.assertTrue(contender.owns_lock())
        finally:
            if contender.owns_lock():
                await invoke(contender, 'unlock')

        # Reuse the original scope, retaining the exact exception from its body.
        body_error: RuntimeError = RuntimeError("Preserve the context body")
        with self.assertRaises(RuntimeError) as caught_body:
            if asynchronous:
                async with owner as entered:
                    self.assertIs(entered, owner)
                    self.assertTrue(owner.owns_lock())
                    raise body_error
            else:
                with owner as entered:
                    self.assertIs(entered, owner)
                    self.assertTrue(owner.owns_lock())
                    raise body_error
        self.assertIs(caught_body.exception, body_error)
        self.assertFalse(owner.owns_lock())


class WireParityTests(async_tests.ScriptedPeerFixture):
    """Compare protocol and cleanup failures through real native implementations."""

    async def test_greeting_validation_and_reply_limits_match(self) -> None:
        """Reject bad and oversized greetings before a client can request a lock."""
        for reply, expected in (
            (b'OTHER\n', pylockd.ErrorType.unexpected),
            (b'x' * 5000 + b'\n', pylockd.ErrorType.read_error),
            (b'LOCKD', pylockd.ErrorType.read_error),
        ):
            for mutex_name, _ in MUTEX_PAIRS:
                for asynchronous in (False, True):
                    with self.subTest(
                        reply=reply[:20], mutex=mutex_name, async_api=asynchronous
                    ):

                        async def greet(
                            reader: asyncio.StreamReader,
                            writer: asyncio.StreamWriter,
                            greeting: bytes = reply,
                        ) -> None:
                            """Supply one invalid greeting and close without a command."""
                            writer.write(greeting)
                            await writer.drain()

                        port: int = await self.peer(greet)
                        prefix: str = 'Async' if asynchronous else ''
                        mutex_type: Any = getattr(pylockd, prefix + mutex_name)
                        with self.assertRaises(pylockd.Exception) as caught:
                            if asynchronous:
                                await mutex_type('127.0.0.1', port, 1, 'example')
                            else:
                                await asyncio.to_thread(
                                    mutex_type, '127.0.0.1', port, 1, 'example'
                                )
                        self.assertEqual(caught.exception.cause.error_type(), expected)

    async def test_release_failure_and_body_exception_match(self) -> None:
        """Report failed explicit cleanup while preserving an existing body exception."""
        for mutex_name, scope_name in MUTEX_PAIRS:
            for asynchronous in (False, True):
                for body_fails in (False, True):
                    with self.subTest(
                        mutex=mutex_name, async_api=asynchronous, body=body_fails
                    ):
                        port: int = await self.peer(self._bad_release)
                        prefix: str = 'Async' if asynchronous else ''
                        mutex_type: Any = getattr(pylockd, prefix + mutex_name)
                        scope_type: Any = getattr(pylockd, prefix + scope_name)
                        original: RuntimeError = RuntimeError('body failure')
                        if asynchronous:
                            mutex: Any = await mutex_type(
                                '127.0.0.1', port, 1, 'example'
                            )
                            scope: Any = scope_type(mutex)
                            try:
                                async with scope:
                                    if body_fails:
                                        raise original
                            except (pylockd.Exception, RuntimeError) as error:
                                self._check_cleanup_error(error, original, body_fails)
                            else:
                                self.fail('Failed release was silently accepted')
                            self.assertFalse(scope.owns_lock())
                        else:
                            await asyncio.to_thread(
                                self._sync_cleanup,
                                mutex_type,
                                scope_type,
                                port,
                                original,
                                body_fails,
                            )

    async def _bad_release(
        self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter
    ) -> None:
        """Reject UNLOCK and require the uncertain connection to be discarded."""
        writer.write(b'LOCKD\n')
        self.assertEqual(await reader.readline(), b'LOCK example\n')
        writer.write(b'LOCK_ACQUIRING\nLOCK_ACQUIRED\n')
        self.assertEqual(await reader.readline(), b'UNLOCK\n')
        writer.write(b'NOT_RELEASED\n')
        self.assertEqual(await reader.read(), b'')

    def _sync_cleanup(
        self,
        mutex_type: Any,
        scope_type: Any,
        port: int,
        original: RuntimeError,
        body_fails: bool,
    ) -> None:
        """Keep synchronous recursive acquisition and release on one native thread."""
        scope: Any = scope_type(mutex_type('127.0.0.1', port, 1, 'example'))
        try:
            with scope:
                if body_fails:
                    raise original
        except (pylockd.Exception, RuntimeError) as error:
            self._check_cleanup_error(error, original, body_fails)
        else:
            self.fail('Failed release was silently accepted')
        self.assertFalse(scope.owns_lock())

    def _check_cleanup_error(
        self, error: BaseException, original: RuntimeError, body_fails: bool
    ) -> None:
        """Require identical exception identity, category, and diagnostic text."""
        cleanup: Any = error
        if body_fails:
            self.assertIs(error, original)
            cleanup = error.__cause__
        self.assertIsInstance(cleanup, pylockd.Exception)
        self.assertEqual(cleanup.cause.error_type(), pylockd.ErrorType.unexpected)
        self.assertEqual(
            cleanup.what, 'lockd returned unexpected response (NOT_RELEASED)'
        )

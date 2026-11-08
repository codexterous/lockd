"""
Exercise async lock ownership against scripted peers and the real daemon.

Install pylockd, set LOCKD_TEST_DAEMON to the built daemon's absolute path, and run
python -m unittest discover -s lib/pylockd/tests -v from the repository root.
"""

import asyncio
import socket
import unittest
from collections.abc import Awaitable, Callable, Coroutine
from pathlib import Path
from typing import Any
from unittest.mock import patch

import pylockd
import test_lifetimes as lifetime_tests

PeerHandler = Callable[[asyncio.StreamReader, asyncio.StreamWriter], Awaitable[None]]


class ScriptedPeerFixture(unittest.IsolatedAsyncioTestCase):
    """Provide bounded loopback peers for native async protocol tests."""

    async def asyncSetUp(self) -> None:
        """Track every test-owned connection and task for bounded cleanup."""
        self.servers: list[asyncio.AbstractServer] = []
        self.handlers: list[asyncio.Task[None]] = []
        self.clients: list[asyncio.Task[Any]] = []

    async def asyncTearDown(self) -> None:
        """Cancel unfinished work and report unexpected scripted-peer errors."""
        for task in self.clients:
            if not task.done():
                task.cancel()
        await asyncio.gather(*self.clients, return_exceptions=True)
        for server in self.servers:
            server.close()
            await server.wait_closed()
        for task in self.handlers:
            if not task.done():
                task.cancel()
        results = await asyncio.gather(*self.handlers, return_exceptions=True)
        self.assertEqual(
            [
                error
                for error in results
                if isinstance(error, BaseException)
                and not isinstance(error, asyncio.CancelledError)
            ],
            [],
        )

    def start(self, coroutine: Coroutine[Any, Any, Any]) -> asyncio.Task[Any]:
        """Collect a client task even when its test fails."""
        task: asyncio.Task[Any] = asyncio.create_task(coroutine)
        self.clients.append(task)
        return task

    async def peer(self, handler: PeerHandler, timeout: float = 3) -> int:
        """Listen on loopback and bound each scripted connection to three seconds."""

        async def serve(
            reader: asyncio.StreamReader, writer: asyncio.StreamWriter
        ) -> None:
            """Run the peer script and close its socket on every outcome."""
            try:
                await asyncio.wait_for(handler(reader, writer), timeout)
            finally:
                writer.close()
                try:
                    await writer.wait_closed()
                except ConnectionError:
                    pass

        def accepted(
            reader: asyncio.StreamReader, writer: asyncio.StreamWriter
        ) -> None:
            """Retain the server task so errors reach the test runner."""
            self.handlers.append(asyncio.create_task(serve(reader, writer)))

        server: asyncio.AbstractServer = await asyncio.start_server(
            accepted, '127.0.0.1', 0
        )
        self.servers.append(server)
        return server.sockets[0].getsockname()[1]

    async def completed_session(
        self,
        reader: asyncio.StreamReader,
        writer: asyncio.StreamWriter,
    ) -> None:
        """Acknowledge one acquisition and release, then require connection cleanup."""
        writer.write(b'LOCKD\r\n')
        while command := await reader.readline():
            if (
                command.startswith(b'TIMED_LOCK example ')
                or command == b'LOCK example\n'
            ):
                writer.write(b'LOCK_ACQUIRING\r\nLOCK_ACQUIRED\r\n')
            elif command == b'TRY_LOCK example\n':
                writer.write(b'LOCK_ACQUIRED\r\n')
            else:
                self.fail(f"Unexpected lock command: {command!r}")
            self.assertEqual(await reader.readline(), b'UNLOCK\n')
            writer.write(b'LOCK_RELEASED\r\n')


class ScriptedAsyncTests(ScriptedPeerFixture):
    """Control wire replies while running the production async implementation."""

    async def test_context_releases_after_body_exception_and_can_be_reused(
        self,
    ) -> None:
        """Preserve the body error and reuse the existing connection."""
        port: int = await self.peer(self.completed_session)
        mutex: Any = await pylockd.AsyncMutex('127.0.0.1', port, 1, 'example')
        scope: Any = pylockd.AsyncScopedLock(mutex)
        with self.assertRaisesRegex(ValueError, 'body error'):
            async with scope as entered:
                self.assertIs(entered, scope)
                self.assertTrue(scope.owns_lock())
                raise ValueError('body error')
        self.assertFalse(scope.owns_lock())
        async with scope:
            self.assertTrue(scope.owns_lock())
        self.assertFalse(scope.owns_lock())

    async def test_dropping_a_held_scope_closes_its_native_connection(self) -> None:
        """Retain a temporary mutex and disconnect when its held scope is dropped."""
        closed: asyncio.Event = asyncio.Event()

        async def held(
            reader: asyncio.StreamReader, writer: asyncio.StreamWriter
        ) -> None:
            """Observe destructor release before the final mutex closes its connection."""
            writer.write(b'LOCKD\n')
            self.assertEqual(await reader.readline(), b'LOCK example\n')
            writer.write(b'LOCK_ACQUIRING\nLOCK_ACQUIRED\n')
            self.assertEqual(await reader.readline(), b'UNLOCK\n')
            writer.write(b'LOCK_RELEASED\n')
            self.assertEqual(await reader.read(), b'')
            closed.set()

        port: int = await self.peer(held)
        scope: Any = pylockd.AsyncScopedLock(
            await pylockd.AsyncMutex('127.0.0.1', port, 1, 'example')
        )
        operation: Any = scope.lock()
        await operation
        self.assertTrue(scope.owns_lock())
        del scope
        await asyncio.wait_for(closed.wait(), 2)

    async def test_closing_an_unstarted_operation_prevents_acquisition(self) -> None:
        """Closing a lazy coroutine must prevent later network work."""
        port: int = await self.peer(self.completed_session)
        scope: Any = pylockd.AsyncScopedLock(
            await pylockd.AsyncMutex('127.0.0.1', port, 1, 'example')
        )
        operation: Any = scope.lock()
        operation.close()
        with self.assertRaisesRegex(pylockd.Exception, 'reuse') as caught:
            await operation
        self.assertEqual(
            caught.exception.cause.error_type(), pylockd.ErrorType.log_error
        )
        self.assertFalse(scope.owns_lock())

    async def test_invalid_throw_arguments_use_the_public_exception(self) -> None:
        """Reject malformed coroutine throw calls before starting lock acquisition."""
        port: int = await self.peer(self.completed_session)
        scope: Any = pylockd.AsyncScopedLock(
            await pylockd.AsyncMutex('127.0.0.1', port, 1, 'example')
        )
        operation: Any = scope.lock()
        for arguments, keywords in (
            ((), {}),
            ((ValueError, None, None, None), {}),
            ((), {'exception': ValueError}),
        ):
            with self.subTest(arguments=arguments, keywords=keywords):
                with self.assertRaisesRegex(
                    pylockd.Exception, 'throw expects'
                ) as caught:
                    operation.throw(*arguments, **keywords)
                self.assertEqual(
                    caught.exception.cause.error_type(), pylockd.ErrorType.log_error
                )
        operation.close()
        self.assertFalse(scope.owns_lock())

    async def test_fragmented_greeting_grant_and_release_yield_to_other_tasks(
        self,
    ) -> None:
        """Do not complete operations before their newline arrives."""
        prefix: asyncio.Queue[str] = asyncio.Queue()
        proceed: asyncio.Queue[None] = asyncio.Queue()

        async def fragments(
            reader: asyncio.StreamReader, writer: asyncio.StreamWriter
        ) -> None:
            """Hold back each reply suffix until the test has inspected the client."""
            for label, first, last in (
                ('greeting', b'LO', b'CKD\n'),
                ('grant', b'LOCK_ACQUIRING\nLOCK_AC', b'QUIRED\n'),
                ('release', b'LOCK_REL', b'EASED\n'),
            ):
                writer.write(first)
                await writer.drain()
                await prefix.put(label)
                await proceed.get()
                writer.write(last)
                if label == 'greeting':
                    self.assertEqual(await reader.readline(), b'LOCK example\n')
                elif label == 'grant':
                    self.assertEqual(await reader.readline(), b'UNLOCK\n')
            self.assertEqual(await reader.read(), b'')

        port: int = await self.peer(fragments)
        settings: Any = pylockd.AsyncMutex('127.0.0.1', port, 1, 'example')
        connection = self.start(settings.__await__())
        self.assertEqual(await asyncio.wait_for(prefix.get(), 2), 'greeting')
        self.assertFalse(connection.done())
        await proceed.put(None)
        mutex: Any = await asyncio.wait_for(connection, 2)
        scope: Any = pylockd.AsyncScopedLock(mutex)
        acquisition = self.start(scope.lock())
        self.assertEqual(await asyncio.wait_for(prefix.get(), 2), 'grant')
        self.assertFalse(acquisition.done())
        await proceed.put(None)
        await asyncio.wait_for(acquisition, 2)
        release = self.start(scope.unlock())
        self.assertEqual(await asyncio.wait_for(prefix.get(), 2), 'release')
        self.assertFalse(release.done())
        await proceed.put(None)
        await asyncio.wait_for(release, 2)

        # A completed connection task retains its mutex result. Drop all owners so this script
        # can verify final disconnect as well as successful release on a persistent connection.
        self.clients.remove(connection)
        del connection, settings, mutex, scope

    async def test_release_failure_does_not_replace_a_body_exception_or_cancellation(
        self,
    ) -> None:
        """Preserve the original body error and retain cleanup failure as its cause."""

        async def bad_release(
            reader: asyncio.StreamReader, writer: asyncio.StreamWriter
        ) -> None:
            """Grant a lock, reject its release, and observe cleanup."""
            writer.write(b'LOCKD\n')
            self.assertEqual(await reader.readline(), b'LOCK example\n')
            writer.write(b'LOCK_ACQUIRING\nLOCK_ACQUIRED\n')
            self.assertEqual(await reader.readline(), b'UNLOCK\n')
            writer.write(b'NOT_RELEASED\n')
            self.assertEqual(await reader.read(), b'')

        port: int = await self.peer(bad_release)
        for original in (ValueError('body error'), asyncio.CancelledError()):
            with self.subTest(exception=type(original).__name__):
                scope: Any = pylockd.AsyncScopedLock(
                    (await pylockd.AsyncMutex('127.0.0.1', port, 1, 'example')),
                )
                with self.assertRaises(type(original)) as caught:
                    async with scope:
                        raise original
                self.assertIs(caught.exception, original)
                self.assertIsInstance(caught.exception.__cause__, pylockd.Exception)
                self.assertFalse(scope.owns_lock())

    async def test_cancelled_setup_and_remote_wait_close_the_connection(self) -> None:
        """Cancel while waiting for a greeting, acknowledgement, or grant."""
        for stage in ('greeting', 'acknowledgement', 'grant'):
            with self.subTest(stage=stage):
                await self._cancel_setup_stage(stage)

    async def test_cancelled_connection_may_be_reported_as_reset(self) -> None:
        """Observe real socket closure, then deliver the reset Windows may report."""
        read = asyncio.StreamReader.read

        async def reset_after_close(
            reader: asyncio.StreamReader, size: int = -1
        ) -> bytes:
            """Keep the native client's closure observable before changing its status."""
            data = await read(reader, size)
            if not data:
                raise ConnectionResetError(
                    22, 'The specified network name is no longer available', None, 64
                )
            return data

        with patch.object(asyncio.StreamReader, 'read', reset_after_close):
            for stage in ('greeting', 'acknowledgement', 'grant'):
                with self.subTest(stage=stage):
                    await self._cancel_setup_stage(stage)

    async def _cancel_setup_stage(self, stage: str) -> None:
        """Keep one cancellation stage's peer state separate from later stages."""
        waiting: asyncio.Event = asyncio.Event()
        closed: asyncio.Event = asyncio.Event()
        first: bool = True

        async def cancellable(
            reader: asyncio.StreamReader,
            writer: asyncio.StreamWriter,
        ) -> None:
            """Stall only the first connection and allow a normal retry."""
            nonlocal first
            if not first:
                await self.completed_session(reader, writer)
                return
            first = False
            if stage != 'greeting':
                writer.write(b'LOCKD\n')
                self.assertEqual(await reader.readline(), b'LOCK example\n')
            if stage == 'grant':
                writer.write(b'LOCK_ACQUIRING\n')
            waiting.set()
            # Cancellation discards the connection. Windows may report a reset
            # when a reply races that closure; either reset or EOF confirms it.
            # Other read errors and unexpected bytes must still fail the test.
            try:
                data = await reader.read()
            except ConnectionResetError:
                pass
            else:
                self.assertEqual(data, b'')
            closed.set()

        port: int = await self.peer(cancellable)
        settings: Any = pylockd.AsyncMutex('127.0.0.1', port, 1, 'example')
        if stage == 'greeting':
            task = self.start(settings.__await__())
            scope = None
        else:
            scope = pylockd.AsyncScopedLock(await settings)
            task = self.start(scope.lock())
        await asyncio.wait_for(waiting.wait(), 2)
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task
        await asyncio.wait_for(closed.wait(), 2)
        if scope is not None:
            self.assertFalse(scope.owns_lock())
            with self.assertRaises(pylockd.Exception) as caught:
                await scope.lock()
            self.assertEqual(
                caught.exception.cause.error_type(), pylockd.ErrorType.not_connected
            )
        async with pylockd.AsyncScopedLock(
            await pylockd.AsyncMutex('127.0.0.1', port, 1, 'example')
        ) as retry_scope:
            self.assertTrue(retry_scope.owns_lock())

    async def test_cancellation_after_context_entry_releases_and_preserves_connection(
        self,
    ) -> None:
        """Cancel inside the body, acknowledge release, and reuse the same connection."""
        entered: asyncio.Event = asyncio.Event()
        connections: int = 0

        async def grant(
            reader: asyncio.StreamReader, writer: asyncio.StreamWriter
        ) -> None:
            """Require both lock scopes to acquire and release on one connection."""
            nonlocal connections
            connections += 1
            writer.write(b'LOCKD\n')
            for _ in range(2):
                self.assertEqual(await reader.readline(), b'LOCK example\n')
                writer.write(b'LOCK_ACQUIRING\nLOCK_ACQUIRED\n')
                self.assertEqual(await reader.readline(), b'UNLOCK\n')
                writer.write(b'LOCK_RELEASED\n')

        port: int = await self.peer(grant)
        scope: Any = pylockd.AsyncScopedLock(
            await pylockd.AsyncMutex('127.0.0.1', port, 1, 'example')
        )

        async def hold() -> None:
            """Finish acquisition before the test delivers cancellation to the body."""
            async with scope:
                entered.set()
                await asyncio.Event().wait()

        task = self.start(hold())
        await asyncio.wait_for(entered.wait(), 2)
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task
        self.assertFalse(scope.owns_lock())

        # Acknowledged context cleanup retains the socket. Reuse proves release completed
        # without requiring EOF, which belongs to cancellation during a network operation.
        async with scope:
            self.assertTrue(scope.owns_lock())
        self.assertFalse(scope.owns_lock())
        self.assertEqual(connections, 1)

    async def test_second_cancellation_during_unlock_still_closes_the_connection(
        self,
    ) -> None:
        """A second cancellation must not interrupt the final local cleanup."""
        entered: asyncio.Event = asyncio.Event()
        unlocking: asyncio.Event = asyncio.Event()
        closed: asyncio.Event = asyncio.Event()

        async def stalled_unlock(
            reader: asyncio.StreamReader, writer: asyncio.StreamWriter
        ) -> None:
            """Withhold the release reply until cancellation closes the socket."""
            writer.write(b'LOCKD\n')
            self.assertEqual(await reader.readline(), b'LOCK example\n')
            writer.write(b'LOCK_ACQUIRING\nLOCK_ACQUIRED\n')
            self.assertEqual(await reader.readline(), b'UNLOCK\n')
            unlocking.set()
            self.assertEqual(await reader.read(), b'')
            closed.set()

        port: int = await self.peer(stalled_unlock)
        scope: Any = pylockd.AsyncScopedLock(
            await pylockd.AsyncMutex('127.0.0.1', port, 1, 'example')
        )

        async def hold() -> None:
            """Enter before delivering the first cancellation."""
            async with scope:
                entered.set()
                await asyncio.Event().wait()

        task = self.start(hold())
        await asyncio.wait_for(entered.wait(), 2)
        task.cancel()
        await asyncio.wait_for(unlocking.wait(), 2)
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task
        await asyncio.wait_for(closed.wait(), 2)
        self.assertFalse(scope.owns_lock())

    async def test_cancellation_at_native_grant_delivery_closes_the_connection(
        self,
    ) -> None:
        """Cancel just before or after the native callback completes its Future."""
        for stage in ('before', 'after'):
            with self.subTest(stage=stage):
                await self._cancel_at_grant_delivery(stage)

    async def _cancel_at_grant_delivery(self, stage: str) -> None:
        """Control grant delivery for one cancellation stage and observe disconnect."""
        closed: asyncio.Event = asyncio.Event()

        async def grant(
            reader: asyncio.StreamReader,
            writer: asyncio.StreamWriter,
        ) -> None:
            """Grant the lock and require a disconnect on cancellation."""
            writer.write(b'LOCKD\n')
            self.assertEqual(await reader.readline(), b'LOCK example\n')
            writer.write(b'LOCK_ACQUIRING\nLOCK_ACQUIRED\n')
            self.assertEqual(await reader.read(), b'')
            closed.set()

        port: int = await self.peer(grant)
        scope: Any = pylockd.AsyncScopedLock(
            (await pylockd.AsyncMutex('127.0.0.1', port, 1, 'example')),
        )
        loop = asyncio.get_running_loop()
        enqueue = loop.call_soon_threadsafe

        def cancel_at_delivery(callback: Callable[..., Any], *args: Any) -> Any:
            """Control delivery without replacing native lock operations."""

            def deliver() -> None:
                """Cancel before task resume, on either side of set_result()."""
                if stage == 'before':
                    task.cancel()
                callback(*args)
                if stage == 'after':
                    task.cancel()

            return enqueue(deliver)

        with patch.object(loop, 'call_soon_threadsafe', new=cancel_at_delivery):
            task = self.start(scope.lock())
            with self.assertRaises(asyncio.CancelledError):
                await task
        await asyncio.wait_for(closed.wait(), 2)
        self.assertFalse(scope.owns_lock())

    async def test_malformed_and_incomplete_replies_are_public_exceptions(self) -> None:
        """Reject malformed replies, incomplete EOF, and oversized lines."""
        for stage, reply in (
            ('greeting', b'OTHER\n'),
            ('grant', b'LOCK_UNAVAILABLE\n'),
            ('grant', b'LOCK_ACQUIRED'),
            ('grant', b'x' * 5000 + b'\n'),
            ('release', b'NOT_RELEASED\n'),
        ):
            with self.subTest(stage=stage, reply=reply[:30]):
                await self._check_malformed_reply(stage, reply)

    async def _check_malformed_reply(self, stage: str, reply: bytes) -> None:
        """Check one malformed reply without sharing peer state between scenarios."""

        async def malformed(
            reader: asyncio.StreamReader,
            writer: asyncio.StreamWriter,
        ) -> None:
            """Supply one bad reply at the selected boundary."""
            if stage == 'greeting':
                writer.write(reply)
                return
            writer.write(b'LOCKD\n')
            self.assertEqual(await reader.readline(), b'LOCK example\n')
            writer.write(b'LOCK_ACQUIRING\n')
            if stage == 'release':
                writer.write(b'LOCK_ACQUIRED\n')
                self.assertEqual(await reader.readline(), b'UNLOCK\n')
            writer.write(reply)

        port: int = await self.peer(malformed)
        with self.assertRaises(pylockd.Exception) as caught:
            mutex: Any = await pylockd.AsyncMutex('127.0.0.1', port, 1, 'example')
            scope: Any = pylockd.AsyncScopedLock(mutex)
            async with scope:
                pass
        self.assertEqual(caught.exception.what, str(caught.exception))
        if stage != 'greeting':
            self.assertFalse(scope.owns_lock())

    async def test_greeting_timeout_and_connection_refusal_release_local_state(
        self,
    ) -> None:
        """Bound setup failures and retain native error categories."""
        closed: asyncio.Event = asyncio.Event()

        async def silent(
            reader: asyncio.StreamReader, writer: asyncio.StreamWriter
        ) -> None:
            """Accept TCP but never supply the daemon greeting."""
            self.assertEqual(await reader.read(), b'')
            closed.set()

        port: int = await self.peer(silent, timeout=12)
        with self.assertRaises(pylockd.Exception) as caught:
            await pylockd.AsyncMutex('127.0.0.1', port, 1, 'example')
        self.assertEqual(
            caught.exception.cause.error_type(), pylockd.ErrorType.read_error
        )
        self.assertEqual(
            Path(caught.exception.cause.file()).as_posix(),
            'lib/pylockd/source/detail/async_detail.cpp',
        )
        await asyncio.wait_for(closed.wait(), 2)
        with socket.socket() as unavailable:
            unavailable.bind(('127.0.0.1', 0))
            with self.assertRaises(pylockd.Exception) as caught:
                await pylockd.AsyncMutex(
                    '127.0.0.1', unavailable.getsockname()[1], 1, 'example'
                )
        self.assertEqual(
            caught.exception.cause.error_type(), pylockd.ErrorType.lockd_unavailable
        )

    async def test_only_the_owning_scope_can_release_and_relocking_is_rejected(
        self,
    ) -> None:
        """Wrong-scope and duplicate operations must not disturb a held connection."""
        port: int = await self.peer(self.completed_session)
        mutex: Any = await pylockd.AsyncMutex('127.0.0.1', port, 1, 'example')
        owner: Any = pylockd.AsyncScopedLock(mutex)
        other: Any = pylockd.AsyncScopedLock(mutex)
        with self.assertRaises(pylockd.Exception):
            await other.unlock()
        await asyncio.wait_for(owner.lock(), 2)
        with self.assertRaises(pylockd.Exception):
            await owner.lock()
        with self.assertRaises(pylockd.Exception):
            await other.unlock()
        self.assertTrue(owner.owns_lock())
        await asyncio.wait_for(owner.unlock(), 2)
        with self.assertRaises(pylockd.Exception):
            await owner.unlock()

    async def test_wait_for_timeout_closes_the_request_and_allows_retry(self) -> None:
        """Keep asyncio.wait_for cancellation distinct from a protocol timeout."""
        waiting: asyncio.Event = asyncio.Event()
        closed: asyncio.Event = asyncio.Event()
        first: bool = True

        async def contended(
            reader: asyncio.StreamReader, writer: asyncio.StreamWriter
        ) -> None:
            """Leave the first request queued and grant the retry normally."""
            nonlocal first
            if not first:
                await self.completed_session(reader, writer)
                return
            first = False
            writer.write(b'LOCKD\n')
            self.assertEqual(await reader.readline(), b'LOCK example\n')
            writer.write(b'LOCK_ACQUIRING\n')
            waiting.set()
            self.assertEqual(await reader.read(), b'')
            closed.set()

        port: int = await self.peer(contended)
        scope: Any = pylockd.AsyncScopedLock(
            await pylockd.AsyncMutex('127.0.0.1', port, 1, 'example')
        )
        task = self.start(scope.lock())
        await asyncio.wait_for(waiting.wait(), 2)
        with self.assertRaises(asyncio.TimeoutError):
            await asyncio.wait_for(task, 0.05)
        await asyncio.wait_for(closed.wait(), 2)
        self.assertFalse(scope.owns_lock())
        with self.assertRaises(pylockd.Exception) as caught:
            await scope.lock()
        self.assertEqual(
            caught.exception.cause.error_type(), pylockd.ErrorType.not_connected
        )
        async with pylockd.AsyncScopedLock(
            await pylockd.AsyncMutex('127.0.0.1', port, 1, 'example')
        ):
            pass

    async def test_cross_loop_use_does_not_disturb_the_owner(self) -> None:
        """Reject a different event loop before accessing the held transport."""
        port: int = await self.peer(self.completed_session)
        mutex: Any = await pylockd.AsyncMutex('127.0.0.1', port, 1, 'example')
        owner: Any = pylockd.AsyncScopedLock(mutex)
        await owner.lock()

        async def different_loop() -> None:
            """Attempt acquisition on a separate thread's event loop."""
            with self.assertRaisesRegex(pylockd.Exception, 'event loops') as caught:
                await pylockd.AsyncScopedLock(mutex).lock()
            self.assertEqual(
                caught.exception.cause.error_type(), pylockd.ErrorType.log_error
            )

        await asyncio.wait_for(asyncio.to_thread(asyncio.run, different_loop()), 2)
        self.assertTrue(owner.owns_lock())
        await owner.unlock()

    async def test_constructor_rejects_protocol_injection_and_invalid_timeouts(
        self,
    ) -> None:
        """Reject invalid names and timeouts with native error metadata before connecting."""
        alphabet: str = (
            'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-'
        )
        names: list[str] = ['', 'n' * 257, '\u00e9', '\u00e9' * 128, 'a\nQUIT']
        names += [
            f'prefix{chr(byte)}suffix'
            for byte in range(256)
            if chr(byte) not in alphabet
        ]
        for mutex_type in (
            pylockd.AsyncMutex,
            pylockd.AsyncTimedMutex,
            pylockd.AsyncRecursiveMutex,
        ):
            for name in ('n', 'n' * 256, alphabet):
                mutex_type('127.0.0.1', 1, 1, name)
            for name in names:
                with (
                    self.subTest(mutex_type=mutex_type.__name__, name=repr(name)),
                    self.assertRaises(pylockd.Exception) as caught,
                ):
                    mutex_type('127.0.0.1', 1, 1, name)
                self.assertEqual(
                    caught.exception.cause.error_type(), pylockd.ErrorType.log_error
                )
                self.assertEqual(caught.exception.what, caught.exception.cause.what())
                self.assertEqual(
                    Path(caught.exception.cause.file()).as_posix(),
                    'lib/liblockd/source/detail/client_contract.cpp',
                )
            for timeout in (-1, (1 << 63) - 1):
                with self.subTest(mutex_type=mutex_type.__name__, timeout=timeout):
                    with self.assertRaises(pylockd.Exception) as caught:
                        mutex_type('127.0.0.1', 1, timeout, 'example')
                    self.assertEqual(
                        caught.exception.cause.error_type(), pylockd.ErrorType.log_error
                    )
                    self.assertEqual(
                        caught.exception.what, caught.exception.cause.what()
                    )
            for timeout in (0.5, float('inf'), float('-inf'), float('nan'), 1e100):
                with (
                    self.subTest(mutex_type=mutex_type.__name__, timeout=timeout),
                    self.assertRaises(TypeError),
                ):
                    mutex_type('127.0.0.1', 1, timeout, 'example')


class AsyncDaemonFixture(unittest.IsolatedAsyncioTestCase):
    """Provide a real daemon and bounded task cleanup for async tests."""

    @classmethod
    def setUpClass(cls) -> None:
        """Reuse the Python integration fixture without loading its tests twice."""
        lifetime_tests.InstalledWheelTests.setUpClass()
        cls.port: int = lifetime_tests.InstalledWheelTests.port

    @classmethod
    def tearDownClass(cls) -> None:
        """Stop only this suite's daemon."""
        lifetime_tests.InstalledWheelTests.tearDownClass()

    async def scope(self, name: str) -> Any:
        """Create an independent lazy connection for this contender."""
        return self.track_scope(
            pylockd.AsyncScopedLock(
                (await pylockd.AsyncMutex('127.0.0.1', self.port, 1, name)),
            )
        )

    def track_scope(self, scope: Any) -> Any:
        """Release a manually acquired scope even when an assertion fails."""

        async def cleanup() -> None:
            """Bound cleanup and let cancellation close an unresponsive connection."""
            if scope.owns_lock():
                await asyncio.wait_for(scope.unlock(), 3)

        self.addAsyncCleanup(cleanup)
        return scope

    def start(self, coroutine: Coroutine[Any, Any, Any]) -> asyncio.Task[Any]:
        """Collect a contender before its scope's cleanup runs."""
        task: asyncio.Task[Any] = asyncio.create_task(coroutine)

        async def cleanup() -> None:
            """Cancel an unfinished contender without abandoning its remote request."""
            if not task.done():
                task.cancel()
            await asyncio.wait_for(asyncio.gather(task, return_exceptions=True), 3)

        self.addAsyncCleanup(cleanup)
        return task

    async def wait_for_requests(self, name: str, count: int) -> None:
        """Observe daemon receipt before testing queue cancellation or handoff."""

        async def observed() -> None:
            """Poll a concrete server event with a bounded outer wait."""
            while True:
                log: str = await asyncio.to_thread(
                    lifetime_tests.InstalledWheelTests.log_path.read_text,
                )
                if (
                    sum(
                        'Lock acquisition request' in line and f"'{name}'" in line
                        for line in log.splitlines()
                    )
                    >= count
                ):
                    return
                await asyncio.sleep(0.01)

        await asyncio.wait_for(observed(), 3)


class AsyncDaemonTests(AsyncDaemonFixture):
    """Check asynchronous ownership and cancellation against an actual lockd process."""

    async def test_body_exception_releases_the_remote_lock(self) -> None:
        """Match the advertised async-with API and preserve a Python body exception."""
        scope: Any = await self.scope('async-body')
        with self.assertRaisesRegex(ValueError, 'body error'):
            async with scope:
                self.assertTrue(scope.owns_lock())
                raise ValueError('body error')
        self.assertFalse(scope.owns_lock())
        async with await self.scope('async-body'):
            pass

    async def test_contended_lock_yields_and_hands_off_after_release(self) -> None:
        """Keep the event loop running while another connection holds the lock."""
        owner: Any = await self.scope('async-handoff')
        waiter: Any = await self.scope('async-handoff')
        await owner.lock()
        task: asyncio.Task[None] = self.start(waiter.lock())
        try:
            await self.wait_for_requests('async-handoff', 2)
            self.assertFalse(task.done())
        finally:
            await owner.unlock()
        await asyncio.wait_for(task, 3)
        self.assertTrue(waiter.owns_lock())
        await waiter.unlock()

    async def test_cancelled_remote_waiter_requires_a_new_connection_without_leaking_a_lock(
        self,
    ) -> None:
        """Cancel a queued request, reject its dead connection, and acquire through a new one."""
        owner: Any = await self.scope('async-cancel-queue')
        waiter: Any = await self.scope('async-cancel-queue')
        await owner.lock()
        task: asyncio.Task[None] = self.start(waiter.lock())
        try:
            await self.wait_for_requests('async-cancel-queue', 2)
            task.cancel()
            with self.assertRaises(asyncio.CancelledError):
                await task
            self.assertFalse(waiter.owns_lock())
        finally:
            await owner.unlock()
        with self.assertRaises(pylockd.Exception) as caught:
            await waiter.lock()
        self.assertEqual(
            caught.exception.cause.error_type(), pylockd.ErrorType.not_connected
        )
        async with await self.scope('async-cancel-queue'):
            pass

    async def test_cancelled_local_waiter_does_not_close_the_current_owner(
        self,
    ) -> None:
        """Cancel a local waiter without disturbing the current owner."""
        mutex: Any = await pylockd.AsyncMutex('127.0.0.1', self.port, 1, 'async-local')
        owner: Any = self.track_scope(pylockd.AsyncScopedLock(mutex))
        waiter: Any = self.track_scope(pylockd.AsyncScopedLock(mutex))
        started: asyncio.Event = asyncio.Event()
        await owner.lock()

        async def wait() -> None:
            """Reach the local gate before the test delivers cancellation."""
            started.set()
            await waiter.lock()

        task: asyncio.Task[None] = self.start(wait())
        await asyncio.wait_for(started.wait(), 3)
        with self.assertRaises(pylockd.Exception):
            await asyncio.wait_for(waiter.lock(), 1)
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task
        self.assertTrue(owner.owns_lock())
        await owner.unlock()
        async with waiter:
            self.assertTrue(waiter.owns_lock())

    async def test_cancelling_the_context_body_releases_the_remote_lock(self) -> None:
        """Run async context cleanup when cancellation unwinds an acquired scope."""
        scope: Any = await self.scope('async-cancel-body')
        entered: asyncio.Event = asyncio.Event()

        async def hold() -> None:
            """Keep the task inside the protected body until cancelled."""
            async with scope:
                entered.set()
                await asyncio.Event().wait()

        task: asyncio.Task[None] = self.start(hold())
        await asyncio.wait_for(entered.wait(), 3)
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task
        self.assertFalse(scope.owns_lock())
        async with await self.scope('async-cancel-body'):
            pass

    async def test_async_and_synchronous_python_clients_contend_for_the_same_lock(
        self,
    ) -> None:
        """Use the real synchronous binding as an independent protocol contender."""

        def probe() -> bool:
            """Run synchronous I/O outside the test's event loop."""
            lock: Any = pylockd.ScopedLock(
                pylockd.Mutex(
                    '127.0.0.1',
                    self.port,
                    1,
                    'async-sync',
                )
            )
            acquired: bool = lock.try_lock()
            if acquired:
                lock.unlock()
            return acquired

        async with await self.scope('async-sync'):
            self.assertFalse(await asyncio.wait_for(asyncio.to_thread(probe), 3))
        self.assertTrue(await asyncio.wait_for(asyncio.to_thread(probe), 3))

    async def test_shared_mutex_serializes_entire_scopes(self) -> None:
        """Serialize protected bodies across tasks sharing a mutex."""
        mutex: Any = await pylockd.AsyncMutex('127.0.0.1', self.port, 1, 'async-shared')
        active: int = 0
        completed: int = 0

        async def work() -> None:
            """Yield inside each acquired scope to expose any overlap."""
            nonlocal active, completed
            async with pylockd.AsyncScopedLock(mutex):
                active += 1
                self.assertEqual(active, 1)
                await asyncio.sleep(0)
                active -= 1
                completed += 1

        await asyncio.wait_for(asyncio.gather(*(work() for _ in range(8))), 10)
        self.assertEqual(completed, 8)


if __name__ == '__main__':
    unittest.main()

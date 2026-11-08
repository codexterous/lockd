"""Exercise native immediate and timed async acquisition against peers and lockd."""

import asyncio
import time
from typing import Any

import pylockd
import test_async as async_tests


class TimedPeerTests(async_tests.ScriptedPeerFixture):
    """Check the same server-timed protocol used by the C++ timed mutex."""

    async def scope(self, port: int) -> Any:
        """Connect before constructing an unacquired timed guard."""
        return pylockd.AsyncScopedTimedLock(
            await pylockd.AsyncTimedMutex('127.0.0.1', port, 1, 'example')
        )

    async def test_immediate_attempt_returns_availability_and_can_retry(self) -> None:
        """Keep one connection through a failed try, a later grant, and release."""

        async def attempt(
            reader: asyncio.StreamReader, writer: asyncio.StreamWriter
        ) -> None:
            """Serve two attempts without accepting another connection."""
            writer.write(b'LOCKD\n')
            self.assertEqual(await reader.readline(), b'TRY_LOCK example\n')
            writer.write(b'LOCK_UNAVAILABLE\n')
            self.assertEqual(await reader.readline(), b'TRY_LOCK example\n')
            writer.write(b'LOCK_ACQUIRED\n')
            self.assertEqual(await reader.readline(), b'UNLOCK\n')
            writer.write(b'LOCK_RELEASED\n')
            self.assertEqual(await reader.read(), b'')

        scope: Any = await self.scope(await self.peer(attempt))
        self.assertIs(await scope.try_lock(), False)
        self.assertFalse(scope.owns_lock())
        self.assertIs(await scope.try_lock(), True)
        self.assertTrue(scope.owns_lock())
        await scope.unlock()

    async def test_server_timeout_rounding_and_retry_preserve_the_connection(
        self,
    ) -> None:
        """Round fractional milliseconds up and trust the daemon's timeout reply."""
        requested: asyncio.Event = asyncio.Event()
        reply: asyncio.Event = asyncio.Event()

        async def timed(
            reader: asyncio.StreamReader, writer: asyncio.StreamWriter
        ) -> None:
            """Hold the outcome until the test observes a pending timed request."""
            writer.write(b'LOCKD\n')
            self.assertEqual(await reader.readline(), b'TIMED_LOCK example 1ms\n')
            writer.write(b'LOCK_ACQUIRING\n')
            requested.set()
            await reply.wait()
            writer.write(b'LOCK_TIMED_OUT\n')
            self.assertEqual(await reader.readline(), b'TRY_LOCK example\n')
            writer.write(b'LOCK_ACQUIRED\n')
            self.assertEqual(await reader.readline(), b'UNLOCK\n')
            writer.write(b'LOCK_RELEASED\n')
            self.assertEqual(await reader.read(), b'')

        scope: Any = await self.scope(await self.peer(timed))
        task: asyncio.Task[Any] = self.start(scope.try_lock_for(0.25))
        await asyncio.wait_for(requested.wait(), 2)
        self.assertFalse(task.done())
        reply.set()
        self.assertIs(await asyncio.wait_for(task, 2), False)
        self.assertFalse(scope.owns_lock())
        self.assertIs(await scope.try_lock_for(0), True)
        await scope.unlock()

    async def test_positive_duration_deadline_and_context_all_acquire(self) -> None:
        """Use timed and ordinary acquisitions repeatedly on the same connection."""
        scope: Any = await self.scope(await self.peer(self.completed_session))
        self.assertIs(await scope.try_lock_for(2000), True)
        await scope.unlock()
        self.assertIs(await scope.try_lock_until(time.time() + 2), True)
        await scope.unlock()
        async with scope as entered:
            self.assertIs(entered, scope)
        await scope.timed_lock()
        await scope.unlock()

    async def test_failed_try_reply_is_an_exception_and_not_unavailability(
        self,
    ) -> None:
        """An unexpected reply must not be treated as ordinary lock contention."""

        async def malformed(
            reader: asyncio.StreamReader, writer: asyncio.StreamWriter
        ) -> None:
            """Return a response which cannot mean contention."""
            writer.write(b'LOCKD\n')
            self.assertEqual(await reader.readline(), b'TRY_LOCK example\n')
            writer.write(b'OTHER\n')

        scope: Any = await self.scope(await self.peer(malformed))
        with self.assertRaises(pylockd.Exception) as caught:
            await scope.try_lock()
        self.assertEqual(
            caught.exception.cause.error_type(), pylockd.ErrorType.unexpected
        )
        self.assertFalse(scope.owns_lock())

    async def test_invalid_waits_are_rejected_before_network_io(self) -> None:
        """Reject nonfinite and excessive waits through the shared C++ validation."""
        scope: Any = await self.scope(await self.peer(self.completed_session))
        for invalid in (float('nan'), float('inf'), float('-inf'), 1e100):
            with self.subTest(value=invalid):
                for operation in (scope.try_lock_for, scope.try_lock_until):
                    with self.assertRaises(pylockd.Exception) as caught:
                        operation(invalid)
                    self.assertEqual(
                        caught.exception.cause.error_type(), pylockd.ErrorType.log_error
                    )
                    self.assertEqual(
                        caught.exception.what, caught.exception.cause.what()
                    )

    async def test_delayed_await_does_not_extend_an_absolute_deadline(self) -> None:
        """An already expired deadline makes one TRY_LOCK attempt."""

        async def immediate(
            reader: asyncio.StreamReader, writer: asyncio.StreamWriter
        ) -> None:
            """Reject availability without queueing at the server."""
            writer.write(b'LOCKD\n')
            self.assertEqual(await reader.readline(), b'TRY_LOCK example\n')
            writer.write(b'LOCK_UNAVAILABLE\n')

        scope: Any = await self.scope(await self.peer(immediate))
        operation: Any = scope.try_lock_until(time.time() - 1)
        self.assertIs(await operation, False)


class TimedDaemonTests(async_tests.AsyncDaemonFixture):
    """Check local and remote contention using the actual daemon."""

    async def timed_scope(self, name: str) -> Any:
        """Track a timed scope so failed assertions still release its connection."""
        return self.track_scope(
            pylockd.AsyncScopedTimedLock(
                (await pylockd.AsyncTimedMutex('127.0.0.1', self.port, 1, name)),
            )
        )

    async def test_remote_timeout_retry_and_nonpositive_attempts(self) -> None:
        """A deadline must not release a competing owner's remote lock."""
        owner: Any = await self.timed_scope('timed-remote')
        waiter: Any = await self.timed_scope('timed-remote')
        await owner.lock()
        self.assertIs(await waiter.try_lock(), False)
        self.assertIs(await waiter.try_lock_for(0), False)
        self.assertIs(await waiter.try_lock_until(time.time() - 1), False)
        self.assertIs(await waiter.try_lock_for(50), False)
        self.assertTrue(owner.owns_lock())
        await owner.unlock()
        self.assertIs(await waiter.try_lock_until(time.time() + 2), True)
        await waiter.unlock()

    async def test_local_timeout_and_try_do_not_disturb_owner(self) -> None:
        """Sharing one mutex must honor the local queue and deadline."""
        mutex: Any = await pylockd.AsyncTimedMutex(
            '127.0.0.1', self.port, 1, 'timed-local'
        )
        owner: Any = self.track_scope(pylockd.AsyncScopedTimedLock(mutex))
        waiter: Any = self.track_scope(pylockd.AsyncScopedTimedLock(mutex))
        await owner.lock()
        self.assertIs(await waiter.try_lock(), False)
        self.assertIs(await waiter.try_lock_for(50), False)
        self.assertTrue(owner.owns_lock())
        await owner.unlock()
        self.assertIs(await waiter.try_lock_for(2000), True)
        await waiter.unlock()

    async def test_timed_acquisition_cancellation_remains_cancelled_error(self) -> None:
        """Cancelling a timed contender must not be translated into False."""
        owner: Any = await self.timed_scope('timed-cancel')
        waiter: Any = await self.timed_scope('timed-cancel')
        await owner.lock()
        task: asyncio.Task[Any] = self.start(waiter.try_lock_for(10000))
        await self.wait_for_requests('timed-cancel', 2)
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task
        self.assertTrue(owner.owns_lock())
        await owner.unlock()
        with self.assertRaises(pylockd.Exception) as caught:
            await waiter.try_lock_for(2000)
        self.assertEqual(
            caught.exception.cause.error_type(), pylockd.ErrorType.not_connected
        )
        async with await self.timed_scope('timed-cancel'):
            pass

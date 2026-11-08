"""Check thread and task recursion without weakening remote lock exclusion."""

import asyncio
import threading
import unittest
from concurrent.futures import ThreadPoolExecutor
from typing import Any

import pylockd
import test_async as async_tests
import test_lifetimes as lifetime_tests


class RecursiveThreadTests(unittest.TestCase):
    """Exercise native recursive clients through their synchronous Python bindings."""

    @classmethod
    def setUpClass(cls) -> None:
        """Start a real daemon without importing its unrelated test cases."""
        lifetime_tests.InstalledWheelTests.setUpClass()
        cls.port: int = lifetime_tests.InstalledWheelTests.port

    @classmethod
    def tearDownClass(cls) -> None:
        """Stop only the daemon owned by this fixture."""
        lifetime_tests.InstalledWheelTests.tearDownClass()

    def mutex(self, name: str) -> Any:
        """Create a native recursive client for this test's remote name."""
        return pylockd.RecursiveMutex(
            hostname='127.0.0.1',
            port=self.port,
            connect_timeout=1,
            lock_name=name,
        )

    def test_nested_scopes_retain_the_remote_lock_until_final_release(self) -> None:
        """Distinct scopes can reenter while separate clients remain competitors."""
        mutex: Any = self.mutex('recursive-scopes')
        observer: Any = pylockd.ScopedLock(
            pylockd.Mutex(
                '127.0.0.1',
                self.port,
                1,
                'recursive-scopes',
            )
        )
        with pylockd.ScopedRecursiveLock(mutex) as outer:
            with pylockd.ScopedRecursiveLock(mutex) as inner:
                self.assertTrue(outer.owns_lock())
                self.assertTrue(inner.owns_lock())
                self.assertFalse(observer.try_lock())
            self.assertFalse(observer.try_lock())
            with pylockd.ScopedRecursiveLock(mutex):
                self.assertFalse(observer.try_lock())
        self.assertTrue(observer.try_lock())
        observer.unlock()

    def test_a_waiting_thread_cannot_block_the_owner_from_unlocking(self) -> None:
        """The Python operation guard must not create a cross-scope deadlock."""
        mutex: Any = self.mutex('recursive-threads')
        started: threading.Event = threading.Event()

        def contender() -> bool:
            """Acquire and release in the same native thread."""
            started.set()
            with pylockd.ScopedRecursiveLock(mutex):
                return True

        # Exit the owner's scopes before executor cleanup, including assertion failures.
        with ThreadPoolExecutor(max_workers=1) as executor:
            with pylockd.ScopedRecursiveLock(mutex):
                with pylockd.ScopedRecursiveLock(mutex):
                    waiting = executor.submit(contender)
                    self.assertTrue(started.wait(2))
                    self.assertFalse(waiting.done())
                self.assertFalse(waiting.done())
            self.assertTrue(waiting.result(timeout=3))

    def test_one_guard_cannot_be_locked_twice_and_distinct_instances_compete(
        self,
    ) -> None:
        """Recursive mutex ownership does not change unique_lock ownership rules."""
        mutex: Any = self.mutex('recursive-guards')
        other: Any = pylockd.ScopedRecursiveLock(self.mutex('recursive-guards'))
        with pylockd.ScopedRecursiveLock(mutex) as scope:
            with self.assertRaises(pylockd.Exception):
                scope.lock()
            self.assertFalse(other.try_lock())
        self.assertTrue(other.try_lock())
        other.unlock()

    def test_body_errors_release_all_nested_acquisitions(self) -> None:
        """Unwinding must release each native recursion level."""
        mutex: Any = self.mutex('recursive-unwind')
        with (
            self.assertRaisesRegex(ValueError, 'body'),
            pylockd.ScopedRecursiveLock(mutex),
            pylockd.ScopedRecursiveLock(mutex),
        ):
            raise ValueError('body')
        with pylockd.ScopedRecursiveLock(self.mutex('recursive-unwind')):
            pass

    def test_wrong_thread_release_preserves_the_owning_guard(self) -> None:
        """Reject a foreign thread's release without losing ownership or leaking the lock."""
        owner: Any = pylockd.ScopedRecursiveLock(self.mutex('recursive-wrong-thread'))
        observer: Any = pylockd.ScopedRecursiveLock(
            self.mutex('recursive-wrong-thread')
        )

        def release_from_another_thread() -> None:
            """Exercise the native ownership check through the public Python operation."""
            with self.assertRaises(pylockd.Exception) as caught:
                owner.unlock()
            self.assertEqual(
                caught.exception.cause.error_type(), pylockd.ErrorType.log_error
            )
            self.assertTrue(owner.owns_lock())

        with owner:
            with ThreadPoolExecutor(max_workers=1) as executor:
                executor.submit(release_from_another_thread).result(timeout=3)
            self.assertTrue(owner.owns_lock())
            self.assertFalse(observer.try_lock())
        self.assertFalse(owner.owns_lock())
        with observer:
            self.assertTrue(observer.owns_lock())


class RecursiveTaskTests(async_tests.AsyncDaemonFixture):
    """Distinguish asyncio tasks even though they run on the same native thread."""

    async def mutex(self, name: str) -> Any:
        """Create task-recursive settings without making a connection yet."""
        return await pylockd.AsyncRecursiveMutex('127.0.0.1', self.port, 1, name)

    def recursive_scope(self, mutex: Any) -> Any:
        """Create a scope to be released by its owning task's context cleanup."""
        return pylockd.AsyncScopedRecursiveLock(mutex)

    async def test_nested_scopes_share_one_remote_acquisition(self) -> None:
        """Inner release leaves the remote mutex owned by the outer scope."""
        mutex: Any = await self.mutex('recursive-async-nested')
        observer: Any = await self.scope('recursive-async-nested')
        async with pylockd.AsyncScopedRecursiveLock(mutex):
            async with pylockd.AsyncScopedRecursiveLock(mutex) as inner:
                self.assertTrue(inner.owns_lock())
                self.assertIs(await observer.try_lock(), False)
            self.assertIs(await observer.try_lock(), False)
        self.assertIs(await observer.try_lock(), True)
        await observer.unlock()
        await self.wait_for_requests('recursive-async-nested', 1)

    async def test_another_task_waits_until_the_final_scope_releases(self) -> None:
        """Native thread identity must not let a competing task reenter."""
        mutex: Any = await self.mutex('recursive-async-tasks')
        outer: Any = self.recursive_scope(mutex)
        inner: Any = self.recursive_scope(mutex)
        started: asyncio.Event = asyncio.Event()

        async def contend() -> None:
            """Use a separate scope in another asyncio task."""
            started.set()
            async with pylockd.AsyncScopedRecursiveLock(mutex):
                pass

        async with outer:
            async with inner:
                task: asyncio.Task[Any] = self.start(contend())
                await asyncio.wait_for(started.wait(), 2)
                self.assertFalse(task.done())
            self.assertFalse(task.done())
        await asyncio.wait_for(task, 2)

    async def test_wrong_task_cannot_release_a_scope_and_waiter_cancel_preserves_owner(
        self,
    ) -> None:
        """Task ownership guards release as well as recursive acquisition."""
        mutex: Any = await self.mutex('recursive-async-owner')
        outer: Any = self.recursive_scope(mutex)
        started: asyncio.Event = asyncio.Event()

        async def wrong_owner() -> None:
            """Reject cross-task unlock, then queue a cancellable acquisition."""
            with self.assertRaises(pylockd.Exception):
                await outer.unlock()
            started.set()
            async with pylockd.AsyncScopedRecursiveLock(mutex):
                self.fail('The owner has not released its lock')

        async with outer:
            task: asyncio.Task[Any] = self.start(wrong_owner())
            await asyncio.wait_for(started.wait(), 2)
            task.cancel()
            with self.assertRaises(asyncio.CancelledError):
                await task
            self.assertTrue(outer.owns_lock())

    async def test_non_lifo_release_and_reuse_keep_remaining_acquisitions(self) -> None:
        """Keep ownership when scopes release in a different order."""
        mutex: Any = await self.mutex('recursive-async-order')
        first: Any = self.recursive_scope(mutex)
        second: Any = self.recursive_scope(mutex)
        observer: Any = await self.scope('recursive-async-order')
        async with first, second:
            await first.unlock()
            self.assertTrue(second.owns_lock())
            self.assertIs(await observer.try_lock(), False)
            await first.lock()
            await second.unlock()
            self.assertIs(await observer.try_lock(), False)
            await second.lock()
        self.assertIs(await observer.try_lock(), True)
        await observer.unlock()

    async def test_cancelling_nested_contexts_releases_every_acquisition(self) -> None:
        """Unwind both scopes without leaking a remote lock on cancellation."""
        mutex: Any = await self.mutex('recursive-async-cancel')
        entered: asyncio.Event = asyncio.Event()

        async def hold() -> None:
            """Keep both acquisitions alive until cancellation reaches the body."""
            async with (
                pylockd.AsyncScopedRecursiveLock(mutex),
                pylockd.AsyncScopedRecursiveLock(mutex),
            ):
                entered.set()
                await asyncio.Event().wait()

        task: asyncio.Task[Any] = self.start(hold())
        await asyncio.wait_for(entered.wait(), 2)
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task
        async with await self.scope('recursive-async-cancel'):
            pass

    async def test_cancelling_or_dropping_an_inner_acquisition_preserves_outer(
        self,
    ) -> None:
        """A nested scope can disappear without releasing the outer remote ownership."""
        mutex: Any = await self.mutex('recursive-async-inner')
        observer: Any = await self.scope('recursive-async-inner')
        async with pylockd.AsyncScopedRecursiveLock(mutex) as outer:
            inner: Any = pylockd.AsyncScopedRecursiveLock(mutex)
            operation: Any = inner.lock()
            with self.assertRaises(asyncio.CancelledError):
                operation.throw(asyncio.CancelledError())
            self.assertFalse(inner.owns_lock())
            self.assertTrue(outer.owns_lock())
            await inner.lock()
            del inner
            self.assertTrue(outer.owns_lock())
            self.assertIs(await observer.try_lock(), False)
        self.assertIs(await observer.try_lock(), True)
        await observer.unlock()

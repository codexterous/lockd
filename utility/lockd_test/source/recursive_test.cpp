/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Codexterous - Software. Thought through.
 * http://www.codexterous.com
 *
 * Copyright (C) 2026 Brett Gmoser <lockd@codexterous.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 * and associated documentation files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * - The above copyright notice and this permission notice shall be included in all copies or
 *   substantial portions of the Software.
 *
 * - THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
 *   BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 *   NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 *   DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * File       : recursive_test.cpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Test recursive mutex ownership and exclusion between threads and connections.
 * Start Date : 10/22/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <chrono>
#include <future>
#include <mutex>

#include <boost/test/tools/interface.hpp> // IWYU pragma: keep
#include <boost/test/unit_test.hpp>       // IWYU pragma: keep
#include <boost/test/unit_test_suite.hpp>

#include <liblockd/exceptions.hpp>
#include <liblockd/mutex.hpp>
#include <liblockd/recursive_mutex.hpp>

#include "../include/daemon_fixture.hpp"

namespace {

BOOST_AUTO_TEST_SUITE(lifetimes)


BOOST_FIXTURE_TEST_CASE(
        recursive_locking_through_a_base_reference_preserves_ownership, ldt::running_server_fixture
)
{
    // Acquire nested guards through a base mutex reference. Verify virtual dispatch retains
    // recursive ownership and excludes another client until the final release.
    const auto port = lkd::detail::lifetime_test_access::endpoint(server).port();
    lld::recursive_mutex mutex("127.0.0.1", port, std::chrono::seconds(1), "recursive-base");
    lld::mutex observer("127.0.0.1", port, std::chrono::seconds(1), "recursive-base");
    lld::mutex& base = mutex;
    {
        const lld::mutex::scoped_lock outer(base);
        {
            lld::mutex::scoped_lock inner(base, std::defer_lock);
            BOOST_REQUIRE_NO_THROW(inner.lock());
            BOOST_CHECK_MESSAGE(!observer.try_lock(), "Nested base guards exclude other clients");
        }
        {
            const lld::mutex::scoped_lock inner(base, std::try_to_lock);
            BOOST_REQUIRE_MESSAGE(
                    inner.owns_lock(), "Base try_lock dispatches to recursive ownership"
            );
        }
        BOOST_CHECK_MESSAGE(
                !observer.try_lock(), "Inner base releases retain the outer acquisition"
        );
    }
    BOOST_REQUIRE_MESSAGE(observer.try_lock(), "The final base release unlocks the remote mutex");
    observer.unlock();
}


BOOST_FIXTURE_TEST_CASE(
        recursive_scopes_release_only_the_final_remote_acquisition, ldt::running_server_fixture
)
{
    // Nest recursive guards while an independent client contends. Verify inner destruction
    // retains the remote lock and only the outer release makes it available.
    const auto port = lkd::detail::lifetime_test_access::endpoint(server).port();
    lld::recursive_mutex mutex("127.0.0.1", port, std::chrono::seconds(1), "recursive");
    lld::mutex observer("127.0.0.1", port, std::chrono::seconds(1), "recursive");
    {
        const lld::recursive_mutex::scoped_lock outer(mutex);
        {
            const lld::recursive_mutex::scoped_lock inner(mutex, std::try_to_lock);
            BOOST_REQUIRE_MESSAGE(inner.owns_lock(), "The owning thread may reenter");
            BOOST_CHECK_MESSAGE(!observer.try_lock(), "A separate client remains excluded");
        }
        BOOST_CHECK_MESSAGE(!observer.try_lock(), "The outer acquisition still owns the lock");
    }
    BOOST_REQUIRE_MESSAGE(observer.try_lock(), "The final guard releases the remote lock");
    observer.unlock();
}


BOOST_FIXTURE_TEST_CASE(recursive_native_gate_excludes_another_thread, ldt::running_server_fixture)
{
    // Queue another thread behind two recursive acquisitions. Verify releasing one level still
    // excludes it and the final release allows that thread to acquire.
    const auto port = lkd::detail::lifetime_test_access::endpoint(server).port();
    lld::recursive_mutex mutex("127.0.0.1", port, std::chrono::seconds(1), "threads");
    std::future<bool> waiter;
    std::promise<void> started;
    const auto ready = started.get_future();
    {
        const lld::recursive_mutex::scoped_lock outer(mutex);
        lld::recursive_mutex::scoped_lock inner(mutex);
        waiter = std::async(std::launch::async, [&mutex, &started] {
            started.set_value();
            const lld::recursive_mutex::scoped_lock lock(mutex);
            return lock.owns_lock();
        });
        BOOST_CHECK_MESSAGE(
                ready.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
                "The competing thread starts"
        );
        BOOST_CHECK_MESSAGE(
                waiter.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout,
                "Another thread cannot reenter"
        );
        inner.unlock();
        BOOST_CHECK_MESSAGE(
                waiter.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout,
                "One remaining native level still excludes the contender"
        );
    }
    BOOST_REQUIRE_MESSAGE(
            waiter.wait_for(std::chrono::seconds(3)) == std::future_status::ready,
            "The final release wakes the competing thread"
    );
    BOOST_CHECK_MESSAGE(waiter.get(), "The contender acquires and releases on its own thread");
}


BOOST_FIXTURE_TEST_CASE(
        failed_recursive_try_lock_leaves_the_native_gate_available, ldt::running_server_fixture
)
{
    // Fail a recursive try-lock against a remote owner, then retry from another thread after
    // release. Verify the failed attempt retained no native recursion level.
    const auto port = lkd::detail::lifetime_test_access::endpoint(server).port();
    lld::mutex owner("127.0.0.1", port, std::chrono::seconds(1), "try-recursive");
    lld::recursive_mutex mutex("127.0.0.1", port, std::chrono::seconds(1), "try-recursive");
    {
        const lld::mutex::scoped_lock lock(owner);
        BOOST_CHECK_MESSAGE(!mutex.try_lock(), "Remote contention reports unavailability");
    }
    auto contender = std::async(std::launch::async, [&mutex] {
        const lld::recursive_mutex::scoped_lock lock(mutex, std::try_to_lock);
        return lock.owns_lock();
    });
    BOOST_REQUIRE_MESSAGE(
            contender.wait_for(std::chrono::seconds(3)) == std::future_status::ready,
            "A failed remote attempt must not retain a native recursion level"
    );
    BOOST_CHECK_MESSAGE(contender.get(), "Another thread can acquire after the failed attempt");
}


BOOST_FIXTURE_TEST_CASE(
        wrong_thread_recursive_unlock_cannot_release_the_owner, ldt::running_server_fixture
)
{
    // Attempt unlock from a thread that does not own the recursive mutex. Verify a prompt
    // ownership error while the original guard still reports ownership.
    const auto port = lkd::detail::lifetime_test_access::endpoint(server).port();
    lld::recursive_mutex mutex("127.0.0.1", port, std::chrono::seconds(1), "wrong-thread");
    const lld::recursive_mutex::scoped_lock owner(mutex);
    auto attempt = std::async(std::launch::async, [&mutex] {
        try {
            mutex.unlock(lld::mutex::tt_throw_on_failure);
        }
        catch (const lld::exception& error) {
            return error.error_type() == lld::exception::et_logic_error;
        }
        return false;
    });
    BOOST_REQUIRE_MESSAGE(
            attempt.wait_for(std::chrono::seconds(3)) == std::future_status::ready,
            "Wrong-thread unlock returns without waiting for the owner"
    );
    BOOST_CHECK_MESSAGE(attempt.get(), "Wrong-thread unlock reports the ownership error");
    BOOST_CHECK_MESSAGE(owner.owns_lock(), "The original guard remains the owner");
}


BOOST_AUTO_TEST_SUITE_END()

} // namespace

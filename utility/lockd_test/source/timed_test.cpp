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
 * File       : timed_test.cpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Test timed mutex deadlines and standard lock-guard compatibility.
 * Start Date : 10/22/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <chrono>
#include <functional>
#include <mutex>
#include <ratio>
#include <string>
#include <vector>

#include <boost/test/tools/interface.hpp> // IWYU pragma: keep
#include <boost/test/unit_test.hpp>       // IWYU pragma: keep
#include <boost/test/unit_test_suite.hpp>

#include <liblockd/mutex.hpp>
#include <liblockd/timed_mutex.hpp>

#include "../include/daemon_fixture.hpp"
#include "../include/fixed_clock.hpp"
#include "../include/scripted_peer.hpp"

namespace {

BOOST_AUTO_TEST_SUITE(lifetimes)


BOOST_AUTO_TEST_CASE(fine_precision_deadlines_round_up_or_try_immediately)
{
    // Use fixed microsecond and nanosecond deadlines against a scripted peer. Verify positive
    // fractions round up to protocol milliseconds and expired deadlines use immediate attempts.
    typedef ldt::fixed_clock<std::chrono::microseconds> microsecond_clock;
    typedef ldt::fixed_clock<std::chrono::nanoseconds> nanosecond_clock;

    // Deadline precision may be finer than the protocol's milliseconds. Positive remainders
    // round up; expired deadlines make an immediate attempt, even when a lock is available.
    const std::vector<std::string> commands = {
            "TIMED_LOCK precision 1ms",
            "TIMED_LOCK precision 2ms",
            "TRY_LOCK precision",
            "TIMED_LOCK precision 1ms",
            "TIMED_LOCK precision 2ms",
            "TRY_LOCK precision"
    };
    ldt::scripted_peer peer([commands](ldt::scripted_peer::tcp::socket& socket) {
        ldt::scripted_peer::send(socket, "LOCKD\n");
        for (const auto& command : commands) {
            const bool timed = command.find("TIMED_LOCK") == 0;
            ldt::scripted_peer::expect(socket, command);
            ldt::scripted_peer::send(
                    socket, timed ? "LOCK_ACQUIRING\nLOCK_TIMED_OUT\n" : "LOCK_UNAVAILABLE\n"
            );
            ldt::scripted_peer::expect(socket, command);
            ldt::scripted_peer::send(
                    socket, timed ? "LOCK_ACQUIRING\nLOCK_ACQUIRED\n" : "LOCK_ACQUIRED\n"
            );
            ldt::scripted_peer::expect(socket, "UNLOCK");
            ldt::scripted_peer::send(socket, "LOCK_RELEASED\n");
        }
    });
    lld::timed_mutex mutex("127.0.0.1", peer.port(), std::chrono::seconds(1), "precision");
    const std::vector<std::function<bool()>> attempts = {
            [&mutex] {
                return mutex.try_lock_until(
                        microsecond_clock::time_point(std::chrono::microseconds(1))
                );
            },
            [&mutex] {
                return mutex.try_lock_until(
                        microsecond_clock::time_point(std::chrono::microseconds(1001))
                );
            },
            [&mutex] {
                return mutex.try_lock_until(
                        microsecond_clock::time_point(std::chrono::microseconds(-1))
                );
            },
            [&mutex] {
                return mutex.try_lock_until(
                        nanosecond_clock::time_point(std::chrono::nanoseconds(1))
                );
            },
            [&mutex] {
                return mutex.try_lock_until(
                        nanosecond_clock::time_point(std::chrono::nanoseconds(1000001))
                );
            },
            [&mutex] {
                return mutex.try_lock_until(
                        nanosecond_clock::time_point(std::chrono::nanoseconds(-1))
                );
            }
    };
    BOOST_REQUIRE_EQUAL(attempts.size(), commands.size());
    for (const auto& attempt : attempts) {
        BOOST_CHECK_MESSAGE(!attempt(), "An unavailable lock reports failure");
        BOOST_REQUIRE_MESSAGE(attempt(), "An available lock can be acquired at either precision");
        mutex.unlock(lld::mutex::tt_throw_on_failure);
    }
    peer.finish();
}


BOOST_AUTO_TEST_CASE(expired_and_nonpositive_timeouts_send_try_lock)
{
    // Check the wire command as well as both outcomes. Returning false locally would miss an
    // available lock; sending TIMED_LOCK would unnecessarily enqueue an expired acquisition.
    constexpr unsigned int attempt_count = 14;
    ldt::scripted_peer peer([count = attempt_count](ldt::scripted_peer::tcp::socket& socket) {
        ldt::scripted_peer::send(socket, "LOCKD\n");
        for (unsigned int attempt = 0; attempt < count; ++attempt) {
            ldt::scripted_peer::expect(socket, "TRY_LOCK immediate");
            ldt::scripted_peer::send(socket, "LOCK_UNAVAILABLE\n");
            ldt::scripted_peer::expect(socket, "TRY_LOCK immediate");
            ldt::scripted_peer::send(socket, "LOCK_ACQUIRED\n");
            ldt::scripted_peer::expect(socket, "UNLOCK");
            ldt::scripted_peer::send(socket, "LOCK_RELEASED\n");
        }
    });
    lld::timed_mutex mutex("127.0.0.1", peer.port(), std::chrono::seconds(1), "immediate");
    const std::vector<std::function<bool()>> attempts = {
            [&mutex] { return mutex.try_lock_for(std::chrono::milliseconds(0)); },
            [&mutex] { return mutex.try_lock_for(std::chrono::milliseconds(-1)); },
            [&mutex] { return mutex.try_lock_until(std::chrono::steady_clock::now()); },
            [&mutex] {
                return mutex.try_lock_until(
                        std::chrono::system_clock::now() - std::chrono::hours(1)
                );
            },
            [&mutex] { return mutex.timed_lock(std::chrono::system_clock::now()); },
            [&mutex] {
                return mutex.timed_lock(std::chrono::system_clock::now() - std::chrono::hours(1));
            },
            [&mutex] { return mutex.timed_lock(std::chrono::milliseconds(0)); },
            [&mutex] { return mutex.timed_lock(std::chrono::milliseconds(-1)); },
            [&mutex] { return mutex.timed_lock(std::chrono::seconds(0)); },
            [&mutex] { return mutex.try_lock_for(std::chrono::seconds(-1)); },
            [&mutex] {
                std::unique_lock<lld::timed_mutex> lock(mutex, std::defer_lock);
                const bool acquired = lock.try_lock_for(std::chrono::milliseconds(-1));
                lock.release();
                return acquired;
            },
            [&mutex] {
                std::unique_lock<lld::timed_mutex> lock(mutex, std::defer_lock);
                const bool acquired = lock.try_lock_until(std::chrono::system_clock::now());
                lock.release();
                return acquired;
            },
            [&mutex] {
                std::unique_lock<lld::timed_mutex> lock(mutex, std::defer_lock);
                const bool acquired = lock.try_lock_for(std::chrono::milliseconds(0));
                lock.release();
                return acquired;
            },
            [&mutex] {
                std::unique_lock<lld::timed_mutex> lock(mutex, std::defer_lock);
                const bool acquired = lock.try_lock_until(std::chrono::steady_clock::now());
                lock.release();
                return acquired;
            }
    };
    BOOST_REQUIRE_EQUAL(attempts.size(), attempt_count);
    for (const auto& attempt : attempts) {
        BOOST_CHECK_MESSAGE(!attempt(), "An unavailable lock reports failure without waiting");
        BOOST_REQUIRE_MESSAGE(attempt(), "An expired timeout may still acquire an available lock");
        mutex.unlock(lld::mutex::tt_throw_on_failure);
    }
    peer.finish();
}


BOOST_FIXTURE_TEST_CASE(chrono_timeouts_work_with_standard_lock_guards, ldt::running_server_fixture)
{
    // Try durations and deadlines while another client holds the lock and after release. Verify
    // standard and scoped guards acquire correctly and release on destruction.
    const auto port = lkd::detail::lifetime_test_access::endpoint(server).port();
    lld::mutex owner("127.0.0.1", port, std::chrono::seconds(1), "chrono");
    lld::timed_mutex mutex("127.0.0.1", port, std::chrono::seconds(1), "chrono");
    const std::vector<std::function<bool()>> attempts = {
            [&mutex] { return mutex.try_lock_for(std::chrono::milliseconds(1)); },
            [&mutex] { return mutex.try_lock_for(std::chrono::duration<double, std::milli>(1.5)); },
            [&mutex] {
                return mutex.try_lock_until(
                        std::chrono::steady_clock::now() + std::chrono::milliseconds(1)
                );
            },
            [&mutex] {
                return mutex.try_lock_until(
                        std::chrono::system_clock::now() + std::chrono::milliseconds(1)
                );
            }
    };
    for (const auto& attempt : attempts) {
        {
            const std::lock_guard<lld::mutex> lock(owner);
            BOOST_CHECK_MESSAGE(!attempt(), "A positive timeout respects remote ownership");
        }
        BOOST_REQUIRE_MESSAGE(attempt(), "The next acquisition succeeds after remote release");
        mutex.unlock(lld::mutex::tt_throw_on_failure);
    }
    {
        const std::unique_lock<lld::timed_mutex> lock(mutex, std::chrono::milliseconds(1));
        BOOST_REQUIRE_MESSAGE(lock.owns_lock(), "Standard duration constructor acquires");
    }
    {
        const std::unique_lock<lld::timed_mutex> lock(
                mutex, std::chrono::steady_clock::now() + std::chrono::seconds(1)
        );
        BOOST_REQUIRE_MESSAGE(lock.owns_lock(), "Standard deadline constructor acquires");
    }
    {
        const lld::timed_mutex::scoped_lock lock(
                mutex, std::chrono::system_clock::now() + std::chrono::seconds(1)
        );
        BOOST_REQUIRE_MESSAGE(lock.owns_lock(), "Scoped deadline constructor acquires");
    }
    BOOST_CHECK_MESSAGE(owner.try_lock(), "All guard destructors release their acquisition");
    owner.unlock();
}


BOOST_AUTO_TEST_SUITE_END()

} // namespace

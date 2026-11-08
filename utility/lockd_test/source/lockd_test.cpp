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
 * File       : lockd_test.cpp
 * Author     : Brett Gmoser
 * Part Of    : lockd_test, a unit test suite for lockd and liblockd.
 * Purpose    : Shared test-runner entry points and suite coordination.
 * Start Date : 08/03/2023
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#define BOOST_TEST_MODULE lockd_test
#define BOOST_TEST_MAIN
#define BOOST_TEST_NO_MAIN

#if LOCKD_PLATFORM_POSIX && LOCKD_HAS_FORK
# include <filesystem>
# include <fstream>

#endif

#include <chrono>
#include <cstdlib>
#include <exception>
#include <future>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef WIN32
// Initialize Winsock2 before Boost.Test can include the older winsock.h through windows.h.
# include <boost/asio.hpp> // IWYU pragma: keep
#endif
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/config/helper_macros.hpp>

// The public Format header supplies the template definitions used by free_funcs.hpp.
#include <boost/format.hpp> // IWYU pragma: keep
#include <boost/format/free_funcs.hpp>
#include <boost/test/framework.hpp>

// Provides the header-only Boost.Test framework implementation and initialization.
#include <boost/test/included/unit_test.hpp> // IWYU pragma: keep
#include <boost/test/tools/interface.hpp>    // IWYU pragma: keep
#include <boost/test/tree/decorator.hpp>
#include <boost/test/unit_test_log.hpp>
#include <boost/test/unit_test_suite.hpp>

#if LOCKD_PLATFORM_POSIX && LOCKD_HAS_FORK
# include <grp.h>
# include <pwd.h>

#endif


#include <liblockd/detail/lockd_test_access.hpp>
#include <liblockd/exceptions.hpp>
#include <liblockd/mutex.hpp>
#include <liblockd/timed_mutex.hpp>

#include "../include/benchmark_test.hpp"
#include "../include/lockd_test.hpp"

// Selects the supported Process v1 API across Boost versions.
#include "../include/process.hpp" // IWYU pragma: keep
#include "../include/program_options.hpp"
#include "../include/test_output.hpp"

#define LOCKD_TEST_LOCK_NAME "lockd_test"
#define LOCKD_TEST_RIDICULOUS_LOCK_NAME                                                            \
    "REALLY_REALLY_REALLY_REALLY_REALLY_REALLY_REALLY_REALLY_REALLY_REALLY_REALLY_REALLY_"         \
    "REALLY_REALLY_REALLY_REALLY_REALLY_REALLY_REALLY_REALLY_REALLY_REALLY_REALLY_REALLY_"         \
    "REALLY_REALLY_REALLY_REALLY_REALLY_REALLY_REALLY_REALLY_REALLY_REALLY_REALLY_REALLY_"         \
    "REALLY_LONG_LOCK_NAME"


// A macro that simulates the missing boost.test BOOST_<level>_NO_THROW_MESSAGE for a named test,
// and also displays the exception error message if an exception is thrown.
#define LOCKD_BASE_NO_THROW_MESSAGE(boost_function, t, message)                                    \
    boost_function(                                                                                \
            true ==                                                                                \
                    [&] {                                                                          \
                        try {                                                                      \
                            t;                                                                     \
                            return true;                                                           \
                        }                                                                          \
                        catch (std::exception & e) {                                               \
                            BOOST_ERROR(                                                           \
                                    "unexpected exception thrown by " BOOST_STRINGIZE(t) ": ("     \
                                    << (message) << "): " << e.what()                              \
                            );                                                                     \
                        }                                                                          \
                        catch (...) {                                                              \
                            BOOST_ERROR(                                                           \
                                    "unexpected exception thrown by " BOOST_STRINGIZE(             \
                                            t                                                      \
                                    ) ": (unknown message)"                                        \
                            );                                                                     \
                        }                                                                          \
                        return true;                                                               \
                    }(),                                                                           \
            message                                                                                \
    ) /**/


// A macro that simulates the missing boost.test BOOST_<level>_THROW_MESSAGE for a named test that
// expects an exception to be thrown. Throwing an exception of type exc is successful, not throwing
// or throwing an exception other than type exc is unsuccessful.
#define LOCKD_BASE_THROW_MESSAGE_FAIL(boost_function, t, exc, message, fail_message)               \
    boost_function(                                                                                \
            true ==                                                                                \
                    [&] {                                                                          \
                        typedef exc expected_exception_t;                                          \
                        try {                                                                      \
                            t;                                                                     \
                            if (!std::string(fail_message).empty())                                \
                                BOOST_ERROR(fail_message);                                         \
                            return false;                                                          \
                        }                                                                          \
                        catch (expected_exception_t&) {                                            \
                            return true;                                                           \
                        }                                                                          \
                        catch (...) {                                                              \
                            if (!std::string(fail_message).empty())                                \
                                BOOST_ERROR(fail_message);                                         \
                            return false;                                                          \
                        }                                                                          \
                    }(),                                                                           \
            message                                                                                \
    ) /**/


// As above, but with no additional call to BOOST_ERROR
#define LOCKD_REQUIRE_THROW_MESSAGE(t, exc, message)                                               \
    LOCKD_BASE_THROW_MESSAGE_FAIL(BOOST_REQUIRE_MESSAGE, t, exc, message, "")

#define LOCKD_REQUIRE_THROW_MESSAGE_FAIL(t, exc, message, fail_message)                            \
    LOCKD_BASE_THROW_MESSAGE_FAIL(BOOST_REQUIRE_MESSAGE, t, exc, message, fail_message)

#define LOCKD_CHECK_THROW_MESSAGE_FAIL(t, exc, message, fail_message)                              \
    LOCKD_BASE_THROW_MESSAGE_FAIL(BOOST_CHECK_MESSAGE, t, exc, message, fail_message)

#define LOCKD_CHECK_THROW_MESSAGE(t, exc, message)                                                 \
    LOCKD_BASE_THROW_MESSAGE_FAIL(BOOST_CHECK_MESSAGE, t, exc, message, "")

#define LOCKD_CHECK_NO_THROW_MESSAGE(t, message)                                                   \
    LOCKD_BASE_NO_THROW_MESSAGE(BOOST_CHECK_MESSAGE, t, message)

#define LOCKD_REQUIRE_NO_THROW_MESSAGE(t, message)                                                 \
    LOCKD_BASE_NO_THROW_MESSAGE(BOOST_CHECK_MESSAGE, t, message)


namespace {

std::unique_ptr<lld::mutex> lockd_mutex;
std::unique_ptr<std::unique_lock<lld::mutex>> lockd_unique_lock;
std::unique_ptr<lld::timed_mutex> lockd_timed_mutex;
std::unique_ptr<lld::timed_mutex::scoped_timed_lock> lockd_timed_unique_lock;

} // namespace

using namespace boost::unit_test;


int ldt::master_test_suite_argc()
{
    return boost::unit_test::framework::master_test_suite().argc;
}


char** ldt::master_test_suite_argv()
{
    return boost::unit_test::framework::master_test_suite().argv;
}


void ldt::throw_framework_setup_error(const std::string& error_message)
{
    throw framework::setup_error(error_message);
}


namespace {

template <typename M> M* create_test_mutex()
{
    // new a mutex and return it. This is meant to be passed to a scoped_ptr or similar.
    return new M(
            PROGRAM_OPTIONS_FIXTURE_PTR->interface(),
            PROGRAM_OPTIONS_FIXTURE_PTR->port(),
            std::chrono::seconds(5), // timeout
            LOCKD_TEST_LOCK_NAME
    );
}


template <typename L> void destroy_lock(L& lock)
{
    lock.reset();
}


template <typename M> void destroy_mutex(M& mutex)
{
    mutex.reset();
}


template <typename M>
bool test_response(M& mutex, const std::string& command, const std::string& response)
{
    const lld::detail::lockd_test_access<M> test_access(&mutex);
    LOCKD_CHECK_NO_THROW_MESSAGE(test_access.blocking_write(command), "blocking_write_no_throw");
    return test_access.blocking_read_line(std::chrono::seconds(10)) == response;
}


template <typename M, typename D>
bool test_duration_to_time_spec(M& mutex, const D& duration, const std::string& expected_value)
{
    const lld::detail::lockd_test_access<M> test_access(&mutex);
    return expected_value == test_access.duration_to_time_spec(duration);
}


template <typename M>
bool test_lock_status(M& mutex, const lld::mutex::lock_status_t expected_status)
{
    // The server returns the lock status that the server itself thinks to be true. We test all over
    // with the client library's lock status, but, the `info()` method on the mutex is a useful way
    // of examining the server's state as opposed to our own.
    const lld::mutex::client_info client_info = mutex.info();
    return client_info.lock_status == expected_status;
}


template <typename M> bool test_connected_clients(M& mutex, const unsigned int expected_clients)
{
    // A peer can observe socket closure before the server's outstanding callbacks release
    // the client object and its count. Wait for that cleanup, but still require the exact count.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    auto client_info = mutex.info();
    while (client_info.connected_clients != expected_clients &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        client_info = mutex.info();
    }
    const bool valid = client_info.connected_clients == expected_clients;
    if (!valid) {
        BOOST_TEST_MESSAGE(
                "Expected " << expected_clients << " connected clients, received "
                            << client_info.connected_clients
        );
    }
    return valid;
}


template <typename E> bool test_ostream(const E value, const std::string& expected_value)
{
    std::stringstream ss;
    ss << value;
    return ss.str() == expected_value;
}


template <typename E> bool test_istream(const std::string& value, const E expected_value)
{
    E returned_value;
    std::stringstream ss;
    ss << value;
    ss >> returned_value;
    return returned_value == expected_value;
}


template <typename M>
void get_client_info(lld::mutex::client_info& info, std::unique_ptr<M>& mutex_ptr)
{
    info = mutex_ptr->info();
}


} // namespace

void ldt::release_protocol_clients()
{
    // Guards must release their mutexes before the client objects and daemon are destroyed.
    lockd_unique_lock.reset();
    lockd_timed_unique_lock.reset();
    lockd_mutex.reset();
    lockd_timed_mutex.reset();
}


// Only the protocol suite needs command-line options and a shared daemon process.
namespace {

BOOST_AUTO_TEST_SUITE(protocol, *boost::unit_test::fixture<program_options_fixture>())


BOOST_AUTO_TEST_CASE(no_other_lockd_running)
{
    // At this point, no server exists, so, it should be impossible to create a mutex. This is
    // really a test for the test suite - if an old instance of lockd is left running on the same
    // interface and port as we're expecting to start the server on, the tests may not work as
    // expected. If we can connect, fail this prerequisite and skip the dependent tests.
    LOCKD_REQUIRE_THROW_MESSAGE_FAIL(
            static_cast<void>(std::unique_ptr<lld::mutex>(create_test_mutex<lld::mutex>())),
            lld::exception,
            "no_existing_lockd_listening",
            str(boost::format(
                        "it seems that something is already listening on the requested "
                        "interface ('%1%' port %2%). look for running 'lockd' processes and "
                        "terminate them, or choose a different interface or port, and try "
                        "again."
                ) %
                PROGRAM_OPTIONS_FIXTURE_PTR->interface() % PROGRAM_OPTIONS_FIXTURE_PTR->port())
    );
}


void function_throws_std_runtime_error()
{
    throw std::runtime_error("this test is expected to fail and this is the exception message");
}
void function_throws_logic_error()
{
    throw std::logic_error("you should never see this");
}
void function_doesnt_throw()
{
}
BOOST_AUTO_TEST_CASE(
        lockd_test_meta_test_cases, *boost::unit_test::expected_failures(7)

        // Due to an apparent bug in Boost.Test, the additional BOOST_FAIL that we do is not counted
        // for purposes of expected failures, but, it is counted in the final report which documents
        // how many expected failures we have. I prefer the report to read that there were no
        // failures, and to disregard the info message about the number of failures being less
        // than expected. Punt for now.
        // *boost::unit_test::expected_failures(4)
)
{
    // Meta tests for the unit test suite. Test our custom LOCKD_<level>_THROW_MESSAGE and
    // LOCKD_<level>_NO_THROW macros.
    LOCKD_CHECK_NO_THROW_MESSAGE(function_doesnt_throw(), "check_no_throw_message_success");

    // This meta-test deliberately always throws. MSVC proves the macro's success path
    // unreachable; allow that one diagnostic here without hiding it in other tests.
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma warning(push)
# pragma warning(disable : 4702) // unreachable success path in the intentional failure case
#endif
    LOCKD_CHECK_NO_THROW_MESSAGE(
            function_throws_std_runtime_error(), "check_no_throw_message_expectedly_fails"
    );
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma warning(pop)
#endif

    LOCKD_CHECK_THROW_MESSAGE(
            function_throws_std_runtime_error(), std::exception, "check_throw_message_success"
    );
    LOCKD_CHECK_THROW_MESSAGE(
            function_throws_logic_error(),
            std::runtime_error,
            "check_throw_message_wrong_exception_expectedly_fails"
    );
    LOCKD_CHECK_THROW_MESSAGE(
            function_doesnt_throw(),
            std::exception,
            "check_throw_message_no_exception_expectedly_fails"
    );
    LOCKD_CHECK_THROW_MESSAGE_FAIL(
            function_doesnt_throw(),
            std::runtime_error,
            "check_throw_message_no_exception_expectedly_fails_with_additional_message",
            "This is an additional error that triggers"
    );
    LOCKD_CHECK_THROW_MESSAGE_FAIL(
            function_throws_logic_error(),
            std::runtime_error,
            "check_throw_message_wrong_exception_expectedly_fails_with_additional_message",
            "This is an additional error that triggers"
    );
}


BOOST_AUTO_TEST_CASE(
        mutex_lock_lifecycle, *boost::unit_test::depends_on("protocol/no_other_lockd_running")
)
{
    // Exercise native mutex and guard lifetimes against the launched daemon, including
    // contention, timed waits, disconnect, and restart. Verify ownership, release, and
    // connection state through each transition.
    lld::mutex::client_info mutex_client_info, timed_mutex_client_info;

    // Launch our lockd process.
    BOOST_REQUIRE_MESSAGE(PROGRAM_OPTIONS_FIXTURE_PTR->launcher()->launch(), "start_lockd");

    // Test most of the mutex lifecycle. First, test that we can create the mutex and connect to
    // the server.
    LOCKD_REQUIRE_NO_THROW_MESSAGE(
            lockd_mutex.reset(create_test_mutex<lld::mutex>()), "create_mutex_no_throw"
    );
    BOOST_CHECK_MESSAGE(nullptr != lockd_mutex.get(), "mutex_created_successfully");

    // Test that locking the mutex works.
    LOCKD_CHECK_NO_THROW_MESSAGE(lockd_mutex->lock(), "mutex_locks");
    BOOST_CHECK_MESSAGE(
            test_lock_status(*lockd_mutex, lld::mutex::ls_locked), "lock_status_locked"
    );

    // Test INFO command stuff that is relevant with our currently locked lock.
    BOOST_CHECK_NO_THROW(get_client_info(mutex_client_info, lockd_mutex));
    BOOST_CHECK_MESSAGE(
            mutex_client_info.lock_name == LOCKD_TEST_LOCK_NAME, "info_current_lock_name_correct"
    );
    BOOST_CHECK_MESSAGE(
            mutex_client_info.lock_status == lld::mutex::ls_locked,
            "info_current_lock_status_correct"
    );

    // Re-locking the mutex must fail.
    LOCKD_CHECK_THROW_MESSAGE(
            lockd_mutex->lock(), lld::exception, "mutex_lock_cant_relock_exception"
    );
    BOOST_CHECK_MESSAGE(
            test_lock_status(*lockd_mutex, lld::mutex::ls_locked), "lock_status_locked"
    );

    // Test that unlocking the mutex works.
    LOCKD_CHECK_NO_THROW_MESSAGE(lockd_mutex->unlock(), "mutex_unlock_no_throw");
    BOOST_CHECK_MESSAGE(
            test_lock_status(*lockd_mutex, lld::mutex::ls_none), "lock_status_unlocked"
    );

    // Test that unlocking the mutex a second time does not work.
    LOCKD_CHECK_THROW_MESSAGE(
            lockd_mutex->unlock(lld::mutex::tt_throw_on_failure),
            lld::exception,
            "mutex_unlock_second_time_fails"
    );
    BOOST_CHECK_MESSAGE(
            test_lock_status(*lockd_mutex, lld::mutex::ls_none), "lock_status_unlocked"
    );

    // Test that we can use our mutex with std::unique_lock.
    LOCKD_CHECK_NO_THROW_MESSAGE(
            lockd_unique_lock.reset(new std::unique_lock<lld::mutex>(*lockd_mutex)),
            "mutex_recreation_no_throw"
    );
    BOOST_CHECK_MESSAGE(
            test_lock_status(*lockd_mutex, lld::mutex::ls_locked), "lock_status_locked"
    );

    // Test that destroying our std::unique_lock works OK.
    LOCKD_CHECK_NO_THROW_MESSAGE(destroy_lock(lockd_unique_lock), "lock_destroy_no_throw");
    BOOST_CHECK_MESSAGE(
            test_lock_status(*lockd_mutex, lld::mutex::ls_none), "lock_status_unlocked"
    );

    // Re-lock our unique lock.
    LOCKD_CHECK_NO_THROW_MESSAGE(
            lockd_unique_lock.reset(new std::unique_lock<lld::mutex>(*lockd_mutex)),
            "unique_lock_construct_no_throw"
    );
    BOOST_CHECK_MESSAGE(
            test_lock_status(*lockd_mutex, lld::mutex::ls_locked), "lock_status_locked"
    );

    // A Boost lock refers to its mutex; it does not own it. Destroying the mutex first would
    // make the guard's destructor call unlock() through a dangling reference.
    LOCKD_CHECK_NO_THROW_MESSAGE(
            destroy_lock(lockd_unique_lock), "lock_destroy_before_mutex_no_throw"
    );
    BOOST_CHECK_MESSAGE(
            test_lock_status(*lockd_mutex, lld::mutex::ls_none), "lock_destruction_releases_mutex"
    );
    LOCKD_CHECK_NO_THROW_MESSAGE(destroy_mutex(lockd_mutex), "mutex_destroy_no_throw");

    // Create another mutex for further testing, and lock our lock again.
    LOCKD_REQUIRE_NO_THROW_MESSAGE(
            lockd_mutex.reset(create_test_mutex<lld::mutex>()), "mutex_reconstruct_no_throw"
    );
    LOCKD_CHECK_NO_THROW_MESSAGE(
            lockd_unique_lock.reset(new std::unique_lock<lld::mutex>(*lockd_mutex)),
            "unique_lock_reconstruct_no_throw"
    );
    BOOST_CHECK_MESSAGE(
            test_lock_status(*lockd_mutex, lld::mutex::ls_locked), "lock_status_locked"
    );

    // In another thread, test that a scoped try lock discovers that the mutex is not currently
    // lockable.
    std::thread try_lock_thread([&] {
        // Create another mutex/connection to use for this.
        const std::unique_ptr<lld::mutex> mutex(create_test_mutex<lld::mutex>());
        std::unique_lock<lld::mutex> lock(*mutex, std::defer_lock);

        // Test that both the scoped_try_lock and mutex itself have the same behavior.
        BOOST_CHECK_MESSAGE(false == lock.try_lock(), "scoped_try_lock_false_if_locked");
        BOOST_CHECK_MESSAGE(false == mutex->try_lock(), "mutex_try_lock_false_if_locked");

        // Also create a timed mutex, and try that out.
        std::unique_ptr<lld::timed_mutex> timed_mutex;
        std::unique_ptr<lld::timed_mutex::scoped_timed_lock> timed_lock;
        LOCKD_REQUIRE_NO_THROW_MESSAGE(
                timed_mutex.reset(create_test_mutex<lld::timed_mutex>()),
                "timed_mutex_construct_no_throw"
        );
        LOCKD_CHECK_NO_THROW_MESSAGE(
                timed_lock.reset(
                        new lld::timed_mutex::scoped_timed_lock(*timed_mutex, std::defer_lock)
                ),
                "timed_mutex_scoped_timed_lock_construct_no_throw"
        );
        BOOST_CHECK_MESSAGE(
                timed_lock->try_lock_for(std::chrono::seconds(1)) == false, "timed_lock_times_out"
        );
        BOOST_CHECK_MESSAGE(
                test_lock_status(*timed_mutex, lld::mutex::ls_none), "lock_status_unlocked"
        );

        // Reset the outer lock so we can try again.
        LOCKD_CHECK_NO_THROW_MESSAGE(
                destroy_lock(lockd_unique_lock), "unique_lock_destroy_no_throw"
        );

        // Do the timed lock, this time successfully.
        BOOST_CHECK_MESSAGE(
                timed_lock->try_lock_for(std::chrono::seconds(1)) == true,
                "timed_lock_successfully_locks"
        );

        // Kill the timed lock, releasing the lock.
        LOCKD_CHECK_NO_THROW_MESSAGE(destroy_lock(timed_lock), "timed_lock_destroy_no_throw");
    });
    try_lock_thread.join();

    // Create a bunch of threads which will all try to get the same lock, and hold it for a second
    // each.
    std::vector<std::future<void>> threads;
    std::mutex test_mutex;
    unsigned short successful_locks = 0;
    constexpr unsigned short lock_count = 5;
    threads.reserve(lock_count);
    for (int i = 0; i < lock_count; i++) {
        threads.push_back(std::async(std::launch::async, [&] {
            const std::unique_ptr<lld::mutex> mutex(create_test_mutex<lld::mutex>());

            // Tests can't be run concurrently. We could use our own mutex to prevent problems, but
            // if other threads needed to synchronize before creating a mutex or lock, that would
            // somewhat defeat the purpose of this. So, rather than BOOST_CHECK_NO_THROW these
            // calls, just increase a timer.
            const lld::mutex::scoped_lock lock(*mutex);
            std::this_thread::sleep_for(std::chrono::seconds(1));
            const std::lock_guard<std::mutex> test_lock(test_mutex);
            ++successful_locks;
        }));
    }

    // Join the group.
    for (auto& thread : threads)
        thread.get();

    // Test that we acquired the correct amount of locks.
    BOOST_CHECK_MESSAGE(lock_count == successful_locks, "multithread_multiple_locks_successful");

    // Terminate and join the process thread, we're going to test that broken stuff reports as
    // broken.
    PROGRAM_OPTIONS_FIXTURE_PTR->launcher()->terminate();
    PROGRAM_OPTIONS_FIXTURE_PTR->launcher()->join();

    // Process termination closes the peer's sockets, but our local descriptor remains open.
    // Observe EOF or reset before checking the recorded connection state. A read timeout must
    // fail this test rather than masquerading as evidence that the peer closed the connection.
    const lld::detail::lockd_test_access<lld::mutex> dead_client(lockd_mutex.get());
    BOOST_CHECK_THROW(
            static_cast<void>(dead_client.blocking_read_line(std::chrono::seconds(3))),
            lld::exception
    );
    BOOST_CHECK_MESSAGE(
            dead_client.peer_closed(),
            "Terminated daemon closes its connection: " << dead_client.error()
    );
    BOOST_CHECK_MESSAGE(false == lockd_mutex->connected(), "lockd_dead_mutex_not_connected");

    // This should fail, as we aren't connected any longer.
    LOCKD_CHECK_THROW_MESSAGE(
            lockd_unique_lock.reset(new std::unique_lock<lld::mutex>(*lockd_mutex)),
            lld::exception,
            "lock_disconnected_mutex_throws"
    );

    // Launch another lockd process.
    BOOST_REQUIRE_MESSAGE(PROGRAM_OPTIONS_FIXTURE_PTR->launcher()->launch(), "start_lockd_again");

    // Now that lockd is running again, we can create more mutexes.
    lockd_unique_lock.reset();
    LOCKD_REQUIRE_NO_THROW_MESSAGE(
            lockd_mutex.reset(create_test_mutex<lld::mutex>()),
            "mutex_construct_after_restart_no_throw"
    );
    BOOST_CHECK_MESSAGE(
            test_lock_status(*lockd_mutex, lld::mutex::ls_none), "lock_status_unlocked"
    );
    LOCKD_REQUIRE_NO_THROW_MESSAGE(
            lockd_unique_lock.reset(new std::unique_lock<lld::mutex>(*lockd_mutex)),
            "lock_construct_after_restart_no_throw"
    );
    BOOST_CHECK_MESSAGE(
            test_lock_status(*lockd_mutex, lld::mutex::ls_locked), "lock_status_unlocked"
    );
    LOCKD_REQUIRE_NO_THROW_MESSAGE(
            lockd_timed_mutex.reset(create_test_mutex<lld::timed_mutex>()),
            "timed_mutex_construct_after_restart_no_throw"
    );
    BOOST_CHECK_MESSAGE(
            test_lock_status(*lockd_timed_mutex, lld::mutex::ls_none), "lock_status_unlocked"
    );

    // Test several accessors.
    BOOST_CHECK_MESSAGE(true == lockd_mutex->connected(), "mutex_connected");
    BOOST_CHECK_MESSAGE(true == lockd_timed_mutex->connected(), "timed_mutex_connected");
    BOOST_CHECK_MESSAGE(
            lockd_mutex->lock_name() == LOCKD_TEST_LOCK_NAME, "timed_mutex_lock_name_valid"
    );
    BOOST_CHECK_MESSAGE(
            lockd_timed_mutex->lock_name() == LOCKD_TEST_LOCK_NAME, "timed_mutex_lock_name_valid"
    );

    // Attempting to change the lock name while locked must throw.
    LOCKD_CHECK_THROW_MESSAGE(
            lockd_mutex->lock_name("CHANGED_LOCK_NAME"),
            lld::exception,
            "mutex_change_lock_name_throws_when_locked"
    );

    // Conversely, the timed mutex isn't currently locked, so, we should be able to change the lock
    // name.
    LOCKD_CHECK_NO_THROW_MESSAGE(
            lockd_timed_mutex->lock_name("CHANGED_LOCK_NAME"),
            "timed_mutex_change_lock_name_no_throw"
    );
    BOOST_CHECK_MESSAGE(
            lockd_timed_mutex->lock_name() == "CHANGED_LOCK_NAME", "timed_mutex_lock_name_changed"
    );

    // Change it back.
    LOCKD_CHECK_NO_THROW_MESSAGE(
            lockd_timed_mutex->lock_name(LOCKD_TEST_LOCK_NAME),
            "timed_mutex_lock_name_change_back_no_throw"
    );

    // More timed mutex tests.
    LOCKD_CHECK_NO_THROW_MESSAGE(
            lockd_timed_unique_lock.reset(
                    new lld::timed_mutex::scoped_timed_lock(*lockd_timed_mutex, std::defer_lock)
            ),
            "scoped_timed_lock_construct_no_throw"
    );

    // A bunch of timed locks which should all fail. We try both on the lock object and the mutex
    // itself.
    BOOST_CHECK_MESSAGE(
            false == lockd_timed_unique_lock->try_lock_for(std::chrono::milliseconds(100)),
            "try_lock_for_100ms"
    );
    BOOST_CHECK_MESSAGE(
            test_lock_status(*lockd_timed_mutex, lld::mutex::ls_none), "lock_status_unlocked"
    );
    BOOST_CHECK_MESSAGE(
            false == lockd_timed_mutex->try_lock_for(std::chrono::milliseconds(100)),
            "timed_mutex_try_lock_for_100ms"
    );
    BOOST_CHECK_MESSAGE(
            test_lock_status(*lockd_timed_mutex, lld::mutex::ls_none), "lock_status_unlocked"
    );

    // Test all duration_to_time_spec methods of the timed_mutex
    BOOST_CHECK_MESSAGE(
            test_duration_to_time_spec(*lockd_timed_mutex, std::chrono::milliseconds(1), "1ms"),
            "duration_to_time_chrono_milliseconds_eq_1ms"
    );
    BOOST_CHECK_MESSAGE(
            test_duration_to_time_spec(*lockd_timed_mutex, std::chrono::seconds(1), "1s"),
            "duration_to_time_chrono_seconds_eq_1s"
    );
    BOOST_CHECK_MESSAGE(
            test_duration_to_time_spec(*lockd_timed_mutex, std::chrono::minutes(1), "1m"),
            "duration_to_time_chrono_minutes_eq_1m"
    );
    BOOST_CHECK_MESSAGE(
            test_duration_to_time_spec(*lockd_timed_mutex, std::chrono::hours(1), "1h"),
            "duration_to_time_chrono_hours_eq_1h"
    );
    BOOST_CHECK_MESSAGE(
            test_duration_to_time_spec(*lockd_timed_mutex, 1, "1ms"), "duration_to_time_int_eq_1ms"
    );

    // try_lock_until absolute times
    BOOST_CHECK_MESSAGE(
            false == lockd_timed_unique_lock->try_lock_until(
                             std::chrono::steady_clock::now() + std::chrono::milliseconds(100)
                     ),
            "try_lock_until_100ms_from_now"
    );
    BOOST_CHECK_MESSAGE(
            test_lock_status(*lockd_timed_mutex, lld::mutex::ls_none), "lock_status_unlocked"
    );
    BOOST_CHECK_MESSAGE(
            false == lockd_timed_mutex->try_lock_until(
                             std::chrono::steady_clock::now() + std::chrono::milliseconds(100)
                     ),
            "timed_mutex_try_lock_until_100ms_from_now"
    );
    BOOST_CHECK_MESSAGE(
            test_lock_status(*lockd_timed_mutex, lld::mutex::ls_none), "lock_status_unlocked"
    );

    // timed_lock with relative times
    BOOST_CHECK_MESSAGE(
            false == lockd_timed_unique_lock->try_lock_for(std::chrono::milliseconds(100)),
            "timed_lock_chrono_100ms"
    );
    BOOST_CHECK_MESSAGE(
            false == lockd_timed_mutex->timed_lock(std::chrono::milliseconds(100)),
            "timed_mutex_chrono_100ms"
    );
    BOOST_CHECK_MESSAGE(
            test_lock_status(*lockd_timed_mutex, lld::mutex::ls_none), "lock_status_unlocked"
    );

    // timed_lock with absolute times
    BOOST_CHECK_MESSAGE(
            false == lockd_timed_unique_lock->try_lock_until(
                             std::chrono::system_clock::now() + std::chrono::milliseconds(100)
                     ),
            "timed_lock_chrono_100ms_from_now"
    );
    BOOST_CHECK_MESSAGE(
            false == lockd_timed_mutex->timed_lock(
                             std::chrono::system_clock::now() + std::chrono::milliseconds(100)
                     ),
            "timed_mutex_chrono_100ms_from_now"
    );
    BOOST_CHECK_MESSAGE(
            test_lock_status(*lockd_timed_mutex, lld::mutex::ls_none), "lock_status_unlocked"
    );


    // timed_lock with absolute times far in the past
    BOOST_CHECK_MESSAGE(
            false == lockd_timed_unique_lock->try_lock_until(
                             std::chrono::system_clock::now() - std::chrono::hours(100)
                     ),
            "timed_lock_lock_abs_time_in_past_unavailable"
    );
    BOOST_CHECK_MESSAGE(
            test_lock_status(*lockd_timed_mutex, lld::mutex::ls_none), "lock_status_unlocked"
    );
    BOOST_CHECK_MESSAGE(
            false == lockd_timed_mutex->timed_lock(
                             std::chrono::system_clock::now() - std::chrono::hours(100)
                     ),
            "timed_mutex_lock_abs_time_in_past_unavailable"
    );
    BOOST_CHECK_MESSAGE(
            test_lock_status(*lockd_timed_mutex, lld::mutex::ls_none), "lock_status_unlocked"
    );

    // Test that blocking_read_line successfully throws when it times out. Note that this will cause
    // the mutex to report as disconnected.
    const lld::detail::lockd_test_access<lld::mutex> access(lockd_mutex.get());
    LOCKD_CHECK_THROW_MESSAGE(
            static_cast<void>(access.blocking_read_line(std::chrono::seconds(1))),
            lld::exception,
            "mutex_blocking_read_line_timeout_throws"
    );
    BOOST_CHECK_MESSAGE(
            lockd_mutex->connected() == false, "mutex_disconnected_after_timed_out_read_line"
    );

    // The timed-out read closed the connection, but the guard still refers to this mutex.
    // Destroy the guard while its mutex is alive before replacing the connection.
    lockd_unique_lock.reset();
    LOCKD_REQUIRE_NO_THROW_MESSAGE(
            lockd_mutex.reset(create_test_mutex<lld::mutex>()),
            "recreate_mutex_after_read_timeout_no_throw"
    );
}


#if LOCKD_PLATFORM_POSIX && LOCKD_HAS_FORK
BOOST_AUTO_TEST_CASE(help_displays_default_users_primary_group)
{
    // Read daemon help using the real default account database. Verify the primary-group default
    // and service PID/log paths without requiring daemon startup or elevation.
    const auto* const user = getpwnam("nobody");
    if (!user) {
        BOOST_TEST_MESSAGE("The default user nobody is unavailable; no group can be resolved");
        return;
    }
    const auto gid = user->pw_gid;
    const auto* const group_entry = getgrgid(gid);
    const std::string expected_group = group_entry ? group_entry->gr_name : std::to_string(gid);

    // The installed daemon directory need not be writable by the test user.
    const auto executable = std::filesystem::absolute(PROGRAM_OPTIONS_FIXTURE_PTR->lockd_path());
    const auto help_path = std::filesystem::current_path() / "group_help_test.log";
    ldt::process::child child(
            executable.string(), "--help", ldt::process::std_out > help_path.string()
    );
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (child.running() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool exited = !child.running();
    if (!exited)
        child.terminate();
    child.wait();
    BOOST_REQUIRE_MESSAGE(exited, "Help must exit without starting the daemon");
    BOOST_CHECK_MESSAGE(child.exit_code() == 0, "Help must succeed without elevated privileges");

    std::ifstream help_file(help_path);
    std::ostringstream help_output;
    help_output << help_file.rdbuf();
    help_file.close();
    std::filesystem::remove(help_path);
    const std::string default_pid = std::filesystem::is_directory("/run")
                                            ? "/run/lockd/lockd.pid"
                                            : "/var/run/lockd/lockd.pid";
    BOOST_CHECK_MESSAGE(
            help_output.str().find(default_pid) != std::string::npos,
            "The runtime PID location does not depend on an existing lockd directory"
    );
    BOOST_CHECK_MESSAGE(
            help_output.str().find("/var/log/lockd/lockd.log") != std::string::npos,
            "Privilege-dropping startup defaults to the service log directory"
    );
    const auto group_option = help_output.str().find("--group");
    BOOST_REQUIRE_MESSAGE(group_option != std::string::npos, "Help must describe --group");
    const auto group_line = help_output.str().substr(
            group_option, help_output.str().find('\n', group_option) - group_option
    );
    BOOST_CHECK_MESSAGE(
            group_line.find("(=" + expected_group + ")") != std::string::npos,
            "Help must show nobody's primary group: " << group_line
    );
}
#endif


BOOST_AUTO_TEST_CASE(info_responses, *boost::unit_test::depends_on("protocol/mutex_lock_lifecycle"))
{
    // Query both connected clients and round-trip lock-status text. Verify INFO reports distinct
    // client identities, the expected count, and idle ownership state.
    lld::mutex::client_info mutex_client_info, timed_mutex_client_info;

    // Check that the output from INFO looks okay.
    LOCKD_CHECK_NO_THROW_MESSAGE(
            mutex_client_info = lockd_mutex->info(), "mutex_get_client_info_no_throw"
    );
    BOOST_CHECK_NO_THROW(timed_mutex_client_info = lockd_timed_mutex->info());

    // We currently have two connected clients - a regular mutex, and a timed mutex.
    BOOST_CHECK_MESSAGE(mutex_client_info.connected_clients == 2, "info_connected_clients_count");
    BOOST_CHECK_MESSAGE(
            lld::mutex::ls_none == mutex_client_info.lock_status, "info_lock_status_none_when_none"
    );
    BOOST_CHECK_MESSAGE(!mutex_client_info.lock_name.has_value(), "info_no_current_lock_name");
    BOOST_CHECK_MESSAGE(
            boost::algorithm::starts_with(mutex_client_info.client_address, "0x"),
            "info_client_address_hex"
    );

    // Both addresses must contain exactly one prefix followed by hexadecimal digits.
    for (const auto* const info : {&mutex_client_info, &timed_mutex_client_info}) {
        const auto& address = info->client_address;
        BOOST_CHECK_MESSAGE(
                boost::algorithm::starts_with(address, "0x") && address.size() > 2 &&
                        address.find_first_not_of("0123456789abcdef", 2) == std::string::npos,
                "info_client_address_hex_formatting: " << address
        );
    }

    // Ensure we're getting a remote client address
    BOOST_CHECK_MESSAGE(!mutex_client_info.client_ip.empty(), "client_address_present");
    BOOST_TEST_MESSAGE("IP address is " << mutex_client_info.client_ip);

    // Our two clients should definitely have different addresses.
    BOOST_CHECK_MESSAGE(
            mutex_client_info.client_address != timed_mutex_client_info.client_address,
            "info_clients_different_addresses"
    );

    // Test our operator<< and operator>> on lld::mutex::lock_status_t.
    BOOST_CHECK_MESSAGE(test_ostream(lld::mutex::ls_none, "none"), "ostream_lock_status_none");
    BOOST_CHECK_MESSAGE(
            test_ostream(lld::mutex::ls_acquiring, "acquiring"), "ostream_lock_status_acquiring"
    );
    BOOST_CHECK_MESSAGE(
            test_ostream(lld::mutex::ls_locked, "locked"), "ostream_lock_status_locked"
    );
    BOOST_CHECK_MESSAGE(test_istream("none", lld::mutex::ls_none), "istream_lock_status_none");
    BOOST_CHECK_MESSAGE(
            test_istream("acquiring", lld::mutex::ls_acquiring), "istream_lock_status_acquiring"
    );
    BOOST_CHECK_MESSAGE(
            test_istream("locked", lld::mutex::ls_locked), "istream_lock_status_locked"
    );
}


BOOST_AUTO_TEST_CASE(exceptions)
{
    // Create an exception, and test that std::move works for the ctor.
    const std::string file = "__file__";
    const std::string function = "__function__";
    const lld::exception exception(lld::exception::et_unimplemented, file, 0, function, "message");
    BOOST_CHECK_MESSAGE(!file.empty(), "std_move_no_delete_original_file");
    BOOST_CHECK_MESSAGE(!function.empty(), "std_move_no_delete_original_function");
    BOOST_CHECK_MESSAGE(!exception.file().empty(), "exception_has_file");
    BOOST_CHECK_MESSAGE(!exception.function().empty(), "exception_has_function");
    BOOST_CHECK_MESSAGE(std::string(exception.what()) == "message", "exception_has_message");
}


BOOST_AUTO_TEST_CASE(server_responses, *boost::unit_test::depends_on("protocol/info_responses"))
{
    // Send invalid requests through test access so daemon validation remains covered
    // independently of the client's earlier lock-name validation.
    LOCKD_REQUIRE_NO_THROW_MESSAGE(
            lockd_mutex.reset(create_test_mutex<lld::mutex>()),
            "mutex_construct_for_server_responses_no_throw"
    );
    BOOST_CHECK_MESSAGE(
            test_response(
                    *lockd_mutex, "LOCK " LOCKD_TEST_RIDICULOUS_LOCK_NAME "\n", "LOCK_NAME_TOO_LONG"
            ),
            "lock_name_gt_256_chars_rejected_by_server"
    );
    BOOST_CHECK_MESSAGE(
            test_lock_status(*lockd_mutex, lld::mutex::ls_none), "lock_status_unlocked"
    );

    // Test that invalid commands are detected.
    BOOST_CHECK_MESSAGE(
            test_response(*lockd_mutex, "INVALID_COMMAND\n", "INVALID_COMMAND"),
            "invalid_command_response"
    );

    // Test that requests for locks without a name are detected.
    BOOST_CHECK_MESSAGE(
            test_response(*lockd_mutex, "LOCK     \n", "MISSING_LOCK_NAME"),
            "lock_missing_lock_time_response"
    );
    BOOST_CHECK_MESSAGE(
            test_response(*lockd_mutex, "TRY_LOCK     \n", "MISSING_LOCK_NAME"),
            "try_lock_missing_lock_time_response"
    );
    BOOST_CHECK_MESSAGE(
            test_response(*lockd_mutex, "TIMED_LOCK     \n", "MISSING_LOCK_NAME"),
            "timed_lock_missing_lock_time_response"
    );

    // Test that requests for timed locks without a time are detected.
    BOOST_CHECK_MESSAGE(
            test_response(
                    *lockd_mutex, "TIMED_LOCK " LOCKD_TEST_LOCK_NAME "  \n", "MISSING_LOCK_TIME"
            ),
            "missing_lock_time_response"
    );

    // Test that requests for timed locks with an invalid time type are detected.
    BOOST_CHECK_MESSAGE(
            test_response(
                    *lockd_mutex, "TIMED_LOCK " LOCKD_TEST_LOCK_NAME " 1l\n", "INVALID_TIME_TYPE"
            ),
            "invalid_time_specifier_detected"
    );

    // Test that non-timed lock requests with a lock time are detected.
    BOOST_CHECK_MESSAGE(
            test_response(*lockd_mutex, "LOCK " LOCKD_TEST_LOCK_NAME " 1s\n", "INVALID_ARGS"),
            "invalid_command_response"
    );

    // Commands must consume the whole line; a valid prefix doesn't make extra arguments valid.
    for (const auto* const command :
         {"VERSION extra\n",
          "INFO extra\n",
          "UNLOCK extra\n",
          "QUIT extra\n",
          "LOCK grammar_case 1s extra\n",
          "TIMED_LOCK grammar_case -1s\n"}) {
        BOOST_CHECK_MESSAGE(
                test_response(*lockd_mutex, command, "INVALID_COMMAND"),
                "reject_trailing_arguments_or_negative_time: " << command
        );
    }

    // Keep command and unit matching case-insensitive without changing the lock name. A zero
    // timeout on an available lock exercises each unit without waiting for a deadline.
    for (const auto* const time : {"0", "0S", "0mS", "0M", "0H"}) {
        const std::string command = std::string("\t tImEd_LoCk\tGrammar_Case\t") + time + " \r\n";
        BOOST_REQUIRE_MESSAGE(
                test_response(*lockd_mutex, command, "LOCK_ACQUIRING"),
                "mixed_case_timed_lock: " << time
        );
        const lld::detail::lockd_test_access<lld::mutex> test_access(lockd_mutex.get());
        BOOST_REQUIRE_MESSAGE(
                test_access.blocking_read_line(std::chrono::seconds(10)) == "LOCK_ACQUIRED",
                "mixed_case_timed_lock_acquired: " << time
        );
        const auto info = lockd_mutex->info();
        BOOST_CHECK_MESSAGE(info.lock_name == "Grammar_Case", "preserve_lock_name_case");
        BOOST_CHECK_MESSAGE(info.lock_status == lld::mutex::ls_locked, "parse_locked_status");
        BOOST_REQUIRE_MESSAGE(
                test_response(*lockd_mutex, "uNlOcK\n", "LOCK_RELEASED"),
                "mixed_case_unlock: " << time
        );
    }
}


BOOST_AUTO_TEST_CASE(quit_methods, *boost::unit_test::depends_on("protocol/server_responses"))
{
    // Reset our global mutexes if applicable, we want no clients left to start.
    lockd_unique_lock.reset();
    lockd_timed_unique_lock.reset();
    lockd_mutex.reset();
    lockd_timed_mutex.reset();

    // Test all the methods of quitting the server, ensuring each time that re-connecting a
    // client shows just the new client. Side note: back when this project originally started, I
    // couldn't imagine such a neat way of doing this for loop in C++. Feels very similar to Python
    // `for i in ['a', 'b']:`. Neat.
    for (auto&& quit_message :
         {// Some of these deliberately have more data after the quit command to ensure the server
          // actually drops the connection.
          "QUIT\n",
          "QUIT\nLOCK somelock\n",
          "SOME_COMMAND\x04",
          "SOME_COMMAND\x04MORE",
          "SOME_COMMAND\x1a",
          "SOME_COMMAND\x1aMORE",
          "\x04",
          "\x1a"
         }) {
        std::string replaced_message(quit_message);
        boost::algorithm::replace_all(replaced_message, "\n", "\\n_newline_");    // newline
        boost::algorithm::replace_all(replaced_message, "\x1a", "\\x1a_ctrl_z_"); // CTRL+Z
        boost::algorithm::replace_all(replaced_message, "\x04", "\\x04_ctrl_d_"); // CTRL+D
        const std::string quit_method_sent =
                str(boost::format("quit_method_\"%1%\"_sent_message") % replaced_message);
        const std::string quit_method_successful =
                str(boost::format("quit_method_\"%1%\"_ended_session_lock_throws") %
                    replaced_message);

        // Create a new connection for this test.
        LOCKD_REQUIRE_NO_THROW_MESSAGE(
                lockd_mutex.reset(create_test_mutex<lld::mutex>()),
                "mutex_construct_for_quit_method_no_throw"
        );

        // Send the quit message to the server.
        LOCKD_CHECK_NO_THROW_MESSAGE(
                lld::detail::lockd_test_access<lld::mutex>(lockd_mutex.get())
                        .blocking_write(quit_message),
                quit_method_sent
        );

        // Sending QUIT does not wait for the server to process it. Observe the peer's close
        // without sending another command, then check the client's recorded connection state.
        const lld::detail::lockd_test_access<lld::mutex> quit_client(lockd_mutex.get());
        BOOST_CHECK_THROW(
                static_cast<void>(quit_client.blocking_read_line(std::chrono::seconds(3))),
                lld::exception
        );
        BOOST_CHECK_MESSAGE(
                quit_client.peer_closed(),
                "QUIT closes the server connection: " << quit_client.error()
        );
        BOOST_CHECK_MESSAGE(false == lockd_mutex->connected(), "quit_closes_socket");

        // The client is dead, ensure we can't lock it now.
        LOCKD_CHECK_THROW_MESSAGE(lockd_mutex->lock(), lld::exception, quit_method_successful);

        // Connect one more time, before destroying our original mutex, and ensure the server
        // only sees one connected client.
        std::unique_ptr<lld::mutex> client_check_mutex;

        LOCKD_CHECK_NO_THROW_MESSAGE(
                client_check_mutex.reset(create_test_mutex<lld::mutex>()),
                "mutex_construct_recreate_for_client_count_no_throw"
        );
        BOOST_CHECK_MESSAGE(
                true == test_connected_clients(*client_check_mutex, 1), "one_client_remaining"
        );
    }
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace


int main(const int argc, char** const argv)
{
    if (const auto result = ldt::benchmark_test(argc, argv))
        return *result;

    // Child contenders use the same executable, but do not enter the test framework.
    if (argc > 1 && std::string(argv[1]) == "--child-process-contender")
        return ldt::child_process_contender(argc - 1, argv + 1);

    if (const auto result = program_options_fixture::process_options(argc, argv))
        return *result;

    const ldt::test_output output;
    const int result = boost::unit_test::unit_test_main(&init_unit_test_suite, argc, argv);
    return program_options_fixture::invalid_options() ? EXIT_FAILURE : result;
}

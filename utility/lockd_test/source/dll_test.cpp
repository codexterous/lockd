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
 * File       : dll_test.cpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Test C interface handles, lock operations, errors, and string buffers.
 * Start Date : 10/19/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */


#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <future>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/test/tools/interface.hpp> // IWYU pragma: keep
#include <boost/test/unit_test.hpp>       // IWYU pragma: keep
#include <boost/test/unit_test_suite.hpp>

#include <liblockd_dll/liblockd_dll.h>

#include "../include/daemon_fixture.hpp"
#include "../include/scripted_peer.hpp"

extern "C" bool dll_c_header_test();
extern "C" void* dll_c_create_mutex(
        const char* server, const std::uint16_t port, const std::int64_t timeout, const char* name
);


namespace {

BOOST_AUTO_TEST_SUITE(dll)


BOOST_FIXTURE_TEST_CASE(
        fixed_width_timeouts_cross_the_c_and_cpp_boundary, ldt::running_server_fixture
)
{
    // Call through both C++ and a separately compiled C wrapper. A timeout beyond signed
    // 32-bit range must connect, while unrepresentable deadlines must fail before connecting.
    const auto port = lkd::detail::lifetime_test_access::endpoint(server).port();
    constexpr std::int64_t large_timeout = std::int64_t{1} << 31;
    for (const auto create : {&create_mutex, &dll_c_create_mutex}) {
        const std::unique_ptr<void, decltype(&destroy_mutex)> mutex(
                create("127.0.0.1", port, large_timeout, "wide-timeout"), destroy_mutex
        );
        BOOST_REQUIRE_MESSAGE(mutex, "64-bit timeout seconds survive the C ABI on every target");
        for (const auto timeout : {std::int64_t{-1}, std::numeric_limits<std::int64_t>::max()}) {
            const std::unique_ptr<void, decltype(&destroy_mutex)> invalid(
                    create("127.0.0.1", port, timeout, "invalid-timeout"), destroy_mutex
            );
            BOOST_CHECK_MESSAGE(!invalid, "Invalid timeouts cannot overflow the native timer");
            char error[512];
            BOOST_REQUIRE_MESSAGE(get_last_error(error, sizeof error), "Timeout error is retained");
            BOOST_CHECK_MESSAGE(
                    std::string(error).find("Connection timeout must") != std::string::npos,
                    "Timeout validation reports the cause: " << error
            );
        }
    }
}


BOOST_AUTO_TEST_CASE(invalid_lock_names_fail_creation_with_a_validation_error)
{
    // The C API must return no handle and retain a validation error before trying
    // to connect, including when a lock name contains another protocol command.
    boost::asio::io_context service;
    boost::asio::ip::tcp::socket reservation(service);
    reservation.open(boost::asio::ip::tcp::v4());
    reservation.bind({boost::asio::ip::address_v4::loopback(), 0});
    const auto port = reservation.local_endpoint().port();
    for (const auto& name :
         {std::string(),
          std::string("injected\nUNLOCK"),
          std::string("has space"),
          std::string(257, 'n'),
          std::string("invalid.name"),
          std::string("caf\xc3\xa9")}) {
        const std::unique_ptr<void, decltype(&destroy_mutex)> mutex(
                create_mutex("127.0.0.1", port, 1, name.c_str()), destroy_mutex
        );
        BOOST_CHECK_MESSAGE(!mutex, "Invalid names do not produce a C API mutex handle");
        char error[512];
        BOOST_REQUIRE_MESSAGE(get_last_error(error, sizeof error), "Validation error is retained");
        BOOST_CHECK_MESSAGE(
                std::string(error).find("Lock name must contain") != std::string::npos,
                "Invalid names fail before connection setup: " << error
        );
    }
}


BOOST_FIXTURE_TEST_CASE(dll_handles_contend_and_release_on_destruction, ldt::running_server_fixture)
{
    // Use two C API handles against a real daemon. Verify contention, explicit handoff, and
    // release when an owning lock handle is destroyed.
    const auto port = lkd::detail::lifetime_test_access::endpoint(server).port();
    const std::unique_ptr<void, decltype(&destroy_mutex)> first(
            create_mutex("127.0.0.1", port, 1, "live-dll"), destroy_mutex
    );
    const std::unique_ptr<void, decltype(&destroy_mutex)> second(
            create_mutex("127.0.0.1", port, 1, "live-dll"), destroy_mutex
    );
    BOOST_REQUIRE(first && second);
    const std::unique_ptr<void, decltype(&destroy_lock)> owner(
            create_lock(first.get()), destroy_lock
    );
    std::unique_ptr<void, decltype(&destroy_lock)> contender(
            create_lock(second.get()), destroy_lock
    );
    BOOST_REQUIRE(owner && contender);
    BOOST_CHECK(::lock(owner.get()));
    BOOST_CHECK_EQUAL(::try_lock(contender.get()), 1);
    BOOST_CHECK(::unlock(owner.get()));
    BOOST_CHECK_EQUAL(::try_lock(contender.get()), 0);
    contender.reset();
    BOOST_CHECK_EQUAL(::try_lock(owner.get()), 0);
}

BOOST_AUTO_TEST_CASE(public_header_links_from_c_and_cpp)
{
    // Call the separately compiled C probe from C++. Verify the public header and exported
    // declarations compile and link across both languages.
    BOOST_CHECK(dll_c_header_test());
    destroy_mutex(nullptr);
    destroy_lock(nullptr);
}


BOOST_AUTO_TEST_CASE(exported_lock_operations_and_destruction_follow_the_protocol)
{
    // Drive C exports against a scripted peer. Verify wire commands, repeated-lock error
    // results, explicit unlock, and destructor-driven release of an owned lock.
    typedef ldt::scripted_peer peer_t;
    peer_t peer([](peer_t::tcp::socket& socket) {
        peer_t::send(socket, "LOCKD\n");
        peer_t::expect(socket, "LOCK dll-lock");
        peer_t::send(socket, "LOCK_ACQUIRING\nLOCK_ACQUIRED\n");
        peer_t::expect(socket, "UNLOCK");
        peer_t::send(socket, "LOCK_RELEASED\n");
        peer_t::expect(socket, "TRY_LOCK dll-lock");
        peer_t::send(socket, "LOCK_ACQUIRED\n");
        peer_t::expect(socket, "UNLOCK");
        peer_t::send(socket, "LOCK_RELEASED\n");
        peer_t::expect_disconnect(socket);
    });

    // Custom deleters preserve the API's destruction order even if an assertion aborts the case.
    std::unique_ptr<void, decltype(&destroy_mutex)> mutex(
            create_mutex("127.0.0.1", peer.port(), 1, "dll-lock"), destroy_mutex
    );
    BOOST_REQUIRE_MESSAGE(mutex, "C export creates a connected mutex");
    std::unique_ptr<void, decltype(&destroy_lock)> guard(create_lock(mutex.get()), destroy_lock);
    BOOST_REQUIRE_MESSAGE(guard, "C export creates a deferred lock");
    BOOST_CHECK(::lock(guard.get()));
    BOOST_CHECK_MESSAGE(!::lock(guard.get()), "A second lock returns the error result");
    BOOST_CHECK_EQUAL(::try_lock(guard.get()), 2);
    char error[1024];
    BOOST_CHECK(get_last_error(error, sizeof error));
    BOOST_CHECK(error[0] != '\0');
    BOOST_CHECK(::unlock(guard.get()));
    BOOST_CHECK_EQUAL(::try_lock(guard.get()), 0);
    guard.reset();
    destroy_mutex(mutex.release());
    peer.finish();
}


BOOST_AUTO_TEST_CASE(unavailable_dll_lock_is_not_reported_as_an_error_or_released)
{
    // Return LOCK_UNAVAILABLE to a C API try-lock. Verify ordinary contention preserves the
    // previous error and destruction sends no unlock for an unowned lock.
    typedef ldt::scripted_peer peer_t;
    peer_t peer([](peer_t::tcp::socket& socket) {
        peer_t::send(socket, "LOCKD\n");
        peer_t::expect(socket, "TRY_LOCK dll-contended");
        peer_t::send(socket, "LOCK_UNAVAILABLE\n");
        peer_t::expect_disconnect(socket);
    });
    std::unique_ptr<void, decltype(&destroy_mutex)> mutex(
            create_mutex("127.0.0.1", peer.port(), 1, "dll-contended"), destroy_mutex
    );
    BOOST_REQUIRE(mutex);
    std::unique_ptr<void, decltype(&destroy_lock)> guard(create_lock(mutex.get()), destroy_lock);
    BOOST_REQUIRE(guard);
    char before[1024], after[1024];
    const bool previous_error = get_last_error(before, sizeof before);
    BOOST_CHECK_EQUAL(::try_lock(guard.get()), 1);
    BOOST_CHECK_EQUAL(get_last_error(after, sizeof after), previous_error);
    BOOST_CHECK_EQUAL(std::string(before), after);
    guard.reset();
    mutex.reset();
    peer.finish();
}


BOOST_AUTO_TEST_CASE(dll_errors_remain_local_to_each_thread)
{
    // Generate failures concurrently on two threads and inspect them after both have failed.
    // Verify each thread retains its own last-error text.
    boost::asio::io_context service;
    boost::asio::ip::tcp::socket first(service), second(service);
    for (auto* const socket : {&first, &second}) {
        socket->open(boost::asio::ip::tcp::v4());
        socket->bind({boost::asio::ip::address_v4::loopback(), 0});
    }
    std::promise<void> first_recorded, second_recorded, inspect_errors;
    const auto inspect = inspect_errors.get_future().share();
    const auto first_ready = first_recorded.get_future();
    const auto second_ready = second_recorded.get_future();
    const auto fail = [&](const std::uint16_t port, std::promise<void>& recorded_signal) {
        char initial[1024];
        const bool empty_initially = !get_last_error(initial, sizeof initial) && !initial[0];
        const auto mutex = create_mutex("127.0.0.1", port, 1, "thread-error");
        char before[1024];
        const bool recorded = get_last_error(before, sizeof before);

        // Keep both threads alive until both errors have been set. A global error string would
        // let one thread overwrite the other; exiting either thread first could hide that bug.
        recorded_signal.set_value();
        if (inspect.wait_for(std::chrono::seconds(3)) != std::future_status::ready)
            throw std::runtime_error("Test did not release the error inspection");
        char after[1024];
        const bool retained = get_last_error(after, sizeof after);
        destroy_mutex(mutex);
        return empty_initially && !mutex && recorded && retained && std::string(before) == after &&
               std::string(after).find(std::to_string(port)) != std::string::npos;
    };

    // Use actual threads for the TLS precondition. MSVC's async tasks can reuse a pool worker,
    // including the DLL error left by a previous task on that worker.
    std::packaged_task<bool()> first_task([&] {
        return fail(first.local_endpoint().port(), first_recorded);
    });
    std::packaged_task<bool()> second_task([&] {
        return fail(second.local_endpoint().port(), second_recorded);
    });
    auto one = first_task.get_future();
    auto two = second_task.get_future();
    std::thread first_worker(std::move(first_task)), second_worker(std::move(second_task));
    BOOST_CHECK(first_ready.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
    BOOST_CHECK(second_ready.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
    inspect_errors.set_value();
    first_worker.join();
    second_worker.join();
    BOOST_CHECK_MESSAGE(one.get(), "First thread retains its own error");
    BOOST_CHECK_MESSAGE(two.get(), "Second thread retains its own error");
}

BOOST_AUTO_TEST_CASE(string_buffers_respect_capacity_and_terminate)
{
    // Read version and error strings into null, short, and sufficient buffers guarded by
    // sentinel bytes. Verify return values, truncation, termination, and capacity boundaries.
    char full[1024];
    BOOST_REQUIRE(liblockd_version(full, sizeof full));
    const std::string version(full);
    BOOST_CHECK(version.find("liblockd_dll ") == 0);

    // The DLL identifies the target CPU, even when an x64 compiler cross-builds ARM64.
#if defined(_MSC_VER)
# if defined(_M_ARM64)
    BOOST_CHECK(version.find(" arm64/") != std::string::npos);
# elif defined(_M_X64)
    BOOST_CHECK(version.find(" x86_64/") != std::string::npos);
# else
    BOOST_CHECK(version.find(" x86/") != std::string::npos);
# endif
#endif
    BOOST_CHECK(!liblockd_version(nullptr, 0));
    BOOST_CHECK(!liblockd_version(nullptr, 10));

    // Cover a zero capacity, one-byte buffer, truncation, exact text length, and room for NUL.
    // Sentinels on both sides catch an incorrect pointer-size capacity or terminator overflow.
    for (const auto capacity :
         {static_cast<size_t>(0),
          static_cast<size_t>(1),
          static_cast<size_t>(8),
          version.size(),
          version.size() + 1}) {
        std::vector<char> buffer(capacity + 2, '#');
        const bool result = liblockd_version(buffer.data() + 1, capacity);
        BOOST_CHECK_EQUAL(result, capacity > version.size());
        BOOST_CHECK_EQUAL(buffer.front(), '#');
        BOOST_CHECK_EQUAL(buffer.back(), '#');
        if (capacity) {
            BOOST_CHECK_EQUAL(buffer[capacity], '\0');
            BOOST_CHECK_EQUAL(std::string(buffer.data() + 1), version.substr(0, capacity - 1));
        }
    }

    // Other cases deliberately leave a last error on the test thread. A fresh thread provides
    // the empty-error precondition without depending on Boost.Test's case ordering.
    std::promise<std::pair<bool, char>> empty_result;
    auto result = empty_result.get_future();
    std::thread fresh_thread([&] {
        char buffer[1024];
        std::memset(buffer, 'x', sizeof buffer);
        const bool have_error = get_last_error(buffer, sizeof buffer);
        empty_result.set_value(std::make_pair(have_error, buffer[0]));
    });
    fresh_thread.join();
    const auto [fst, snd] = result.get();
    BOOST_CHECK(!fst);
    BOOST_CHECK_EQUAL(snd, '\0');

    // Reserve a local, non-listening port: no DNS, remote service, or assumed unused port.
    boost::asio::io_context service;
    boost::asio::ip::tcp::socket unavailable(service);
    unavailable.open(boost::asio::ip::tcp::v4());
    unavailable.bind({boost::asio::ip::address_v4::loopback(), 0});
    BOOST_REQUIRE(!create_mutex("127.0.0.1", unavailable.local_endpoint().port(), 1, "error"));
    BOOST_REQUIRE(get_last_error(full, sizeof full));
    const std::string error(full);
    BOOST_CHECK(!error.empty());
    BOOST_CHECK(!get_last_error(nullptr, 0));
    for (const auto capacity :
         {static_cast<size_t>(0),
          static_cast<size_t>(1),
          static_cast<size_t>(8),
          error.size(),
          error.size() + 1}) {
        std::vector<char> buffer(capacity + 2, '#');
        BOOST_CHECK_EQUAL(get_last_error(buffer.data() + 1, capacity), capacity > error.size());
        BOOST_CHECK_EQUAL(buffer.front(), '#');
        BOOST_CHECK_EQUAL(buffer.back(), '#');
        if (capacity) {
            BOOST_CHECK_EQUAL(buffer[capacity], '\0');
            BOOST_CHECK_EQUAL(std::string(buffer.data() + 1), error.substr(0, capacity - 1));
        }
    }
}


BOOST_AUTO_TEST_SUITE_END()

} // namespace

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
 * File       : network_test.cpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Test protocol framing, buffer limits, connection failures, and lock handoff.
 * Start Date : 10/19/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/cstdlib.hpp>
#include <boost/io/ios_state.hpp>
#include <boost/system/error_code.hpp> // IWYU pragma: keep
#include <boost/system/system_error.hpp>
#include <boost/test/framework.hpp>
#include <boost/test/tools/context.hpp>
#include <boost/test/tools/interface.hpp> // IWYU pragma: keep
#include <boost/test/unit_test.hpp>       // IWYU pragma: keep
#include <boost/test/unit_test_suite.hpp>

#include <liblockd/detail/client_contract.hpp>
#include <liblockd/detail/lockd_test_access.hpp>
#include <liblockd/exceptions.hpp>
#include <liblockd/mutex.hpp>
#include <liblockd/recursive_mutex.hpp>
#include <liblockd/timed_mutex.hpp>

#include "../../../include/client.hpp"
#include "../../../include/lock_store.hpp"
#include "../../../include/lockd.hpp"
#include "../../../include/logging.hpp"
#include "../../../include/mutex.hpp"
#include "../include/daemon_fixture.hpp"
#include "../include/lockd_test.hpp"

// Selects the supported Process v1 API across Boost versions.
#include "../include/process.hpp" // IWYU pragma: keep
#include "../include/scripted_peer.hpp"

using ldt::scripted_peer;
using lkd::detail::lifetime_test_access;


int ldt::child_process_contender(const int argc, const char* const argv[])
{
    if (argc != 4) {
        std::cerr << "Usage: --child-process-contender <port> <lock-name> <yes|no>\n";
        return EXIT_FAILURE;
    }

    try {
        lld::mutex mutex(
                "127.0.0.1",
                static_cast<std::uint16_t>(std::stoul(argv[1])),
                std::chrono::seconds(1),
                argv[2]
        );
        if (mutex.try_lock() != (std::string(argv[3]) == "yes")) {
            std::cerr << "Unexpected child lock result for '" << argv[2]
                      << "'; expected available=" << argv[3] << '\n';
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error) {
        std::cerr << "Child contender failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}


namespace {

BOOST_AUTO_TEST_SUITE(lifetimes)


std::vector<std::string> invalid_lock_names()
{
    // Exercise every byte outside the documented alphabet, including malformed
    // UTF-8, plus the size boundaries and the complete command-injection payload.
    const std::string alphabet = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-";
    std::vector<std::string> names = {"", std::string(257, 'n'), "injected\nUNLOCK", "caf\xc3\xa9"};
    for (unsigned int byte = 0; byte <= 255; ++byte)
        if (alphabet.find(static_cast<char>(byte)) == std::string::npos)
            names.push_back("prefix" + std::string(1, static_cast<char>(byte)) + "suffix");
    return names;
}


BOOST_AUTO_TEST_CASE(invalid_lock_names_are_rejected_before_connecting)
{
    // Construct each synchronous client with invalid names against a reserved,
    // non-listening port. A logic error must precede any attempt to connect.
    boost::asio::io_context service;
    scripted_peer::tcp::socket reservation(service);
    reservation.open(scripted_peer::tcp::v4());
    reservation.bind({boost::asio::ip::address_v4::loopback(), 0});
    const auto port = reservation.local_endpoint().port();
    const auto invalid_name = [](const lld::exception& error) {
        return error.error_type() == lld::exception::et_logic_error &&
               std::filesystem::path(error.file()).generic_string() ==
                       "lib/liblockd/source/detail/client_contract.cpp";
    };
    for (const auto& name : invalid_lock_names()) {
        BOOST_TEST_CONTEXT("Invalid name with " << name.size() << " bytes")
        {
            BOOST_CHECK_EXCEPTION(
                    lld::mutex("127.0.0.1", port, std::chrono::seconds(1), name),
                    lld::exception,
                    invalid_name
            );
            BOOST_CHECK_EXCEPTION(
                    lld::timed_mutex("127.0.0.1", port, std::chrono::seconds(1), name),
                    lld::exception,
                    invalid_name
            );
            BOOST_CHECK_EXCEPTION(
                    lld::recursive_mutex("127.0.0.1", port, std::chrono::seconds(1), name),
                    lld::exception,
                    invalid_name
            );
        }
    }
}


BOOST_FIXTURE_TEST_CASE(
        immediate_acquisitions_batch_the_acknowledgement_and_grant, ldt::network_fixture
)
{
    // Stop after the command's own handler, before any write completion can remove its buffer.
    // The exact queue entry proves the daemon submitted both protocol lines together; their
    // arrival in one peer read would not prove that, since TCP may combine separate writes.
    const std::string combined = "LOCK_ACQUIRING\r\nLOCK_ACQUIRED\r\n";
    const auto connection = connect();
    const auto client = lifetime_test_access::connected_client(server, connection->endpoint());
    ldt::poll_until(server.io_service(), [&] {
        return lifetime_test_access::pending_reply_bytes(client) == 0;
    });
    for (const std::string command :
         {"LOCK batch_immediate\n",
          "TIMED_LOCK batch_immediate 1s\n",
          "TRY_LOCK batch_immediate\n"}) {
        BOOST_TEST_CONTEXT("command=" << command)
        {
            connection->send(command);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (lifetime_test_access::status(client) != lld::mutex::ls_locked &&
                   std::chrono::steady_clock::now() < deadline) {
                server.io_service().poll_one();
                std::this_thread::yield();
            }
            BOOST_REQUIRE(lifetime_test_access::status(client) == lld::mutex::ls_locked);
            const bool acknowledged = command.compare(0, 8, "TRY_LOCK") != 0;
            const std::string expected = acknowledged ? combined : "LOCK_ACQUIRED\r\n";
            BOOST_CHECK_EQUAL(lifetime_test_access::replies(client, expected), 1);
            BOOST_CHECK_EQUAL(lifetime_test_access::pending_reply_bytes(client), expected.size());
            if (acknowledged) {
                BOOST_CHECK_EQUAL(lifetime_test_access::replies(client, "LOCK_ACQUIRING\r\n"), 0);
                BOOST_CHECK_EQUAL(lifetime_test_access::replies(client, "LOCK_ACQUIRED\r\n"), 0);
                BOOST_CHECK_EQUAL(connection->line(), "LOCK_ACQUIRING");
            }
            BOOST_CHECK_EQUAL(connection->line(), "LOCK_ACQUIRED");
            connection->send("UNLOCK\n");
            BOOST_CHECK_EQUAL(connection->line(), "LOCK_RELEASED");
            BOOST_CHECK_EQUAL(lifetime_test_access::status(client), lld::mutex::ls_none);
            ldt::poll_until(server.io_service(), [&] {
                return lifetime_test_access::pending_reply_bytes(client) == 0;
            });
        }
    }
}


BOOST_FIXTURE_TEST_CASE(contended_acquisitions_acknowledge_before_handoff, ldt::network_fixture)
{
    // A queued request must acknowledge while the owner still holds the lock. Its later grant
    // must be exactly one line, without repeating the acknowledgement used by the fast path.
    const auto owner = connect();
    const auto waiter = connect();
    const auto waiting_client = lifetime_test_access::connected_client(server, waiter->endpoint());
    const auto mutex = server.lock_store().get_mutex("batch_contended");
    for (const std::string command :
         {"LOCK batch_contended\n", "TIMED_LOCK batch_contended 1s\n"}) {
        BOOST_TEST_CONTEXT("command=" << command)
        {
            owner->send("TRY_LOCK batch_contended\n");
            BOOST_CHECK_EQUAL(owner->line(), "LOCK_ACQUIRED");
            waiter->send(command);
            BOOST_CHECK_EQUAL(waiter->line(), "LOCK_ACQUIRING");
            BOOST_CHECK_EQUAL(
                    lifetime_test_access::status(waiting_client), lld::mutex::ls_acquiring
            );
            BOOST_CHECK_EQUAL(lifetime_test_access::queued_requests(mutex), 1);
            owner->send("UNLOCK\n");
            BOOST_CHECK_EQUAL(owner->line(), "LOCK_RELEASED");
            BOOST_CHECK_EQUAL(waiter->line(), "LOCK_ACQUIRED");
            BOOST_CHECK_EQUAL(lifetime_test_access::status(waiting_client), lld::mutex::ls_locked);
            waiter->send("UNLOCK\n");
            BOOST_CHECK_EQUAL(waiter->line(), "LOCK_RELEASED");
            BOOST_CHECK_EQUAL(lifetime_test_access::queued_requests(mutex), 0);
        }
    }

    // A zero-duration queued request may expire as soon as this command returns. It must still
    // acknowledge before reporting timeout, and must never release the current owner's lock.
    owner->send("TRY_LOCK batch_contended\n");
    BOOST_CHECK_EQUAL(owner->line(), "LOCK_ACQUIRED");
    waiter->send("TIMED_LOCK batch_contended 0ms\n");
    BOOST_CHECK_EQUAL(waiter->line(), "LOCK_ACQUIRING");
    BOOST_CHECK_EQUAL(waiter->line(), "LOCK_TIMED_OUT");
    BOOST_CHECK_EQUAL(lifetime_test_access::status(waiting_client), lld::mutex::ls_none);
    BOOST_CHECK_EQUAL(lifetime_test_access::queued_requests(mutex), 0);
    waiter->send("TRY_LOCK batch_contended\n");
    BOOST_CHECK_EQUAL(waiter->line(), "LOCK_UNAVAILABLE");
    owner->send("UNLOCK\n");
    BOOST_CHECK_EQUAL(owner->line(), "LOCK_RELEASED");
}


BOOST_FIXTURE_TEST_CASE(
        rejected_lock_name_changes_preserve_name_and_remote_ownership, ldt::running_server_fixture
)
{
    // Reject invalid setter inputs without changing the previous name or releasing
    // its lock. Boundary lengths and the complete ASCII alphabet must still work.
    const auto port = lkd::detail::lifetime_test_access::endpoint(server).port();
    const std::array<std::unique_ptr<lld::mutex>, 3> clients = {
            std::make_unique<lld::mutex>("127.0.0.1", port, std::chrono::seconds(1), "original"),
            std::make_unique<lld::timed_mutex>(
                    "127.0.0.1", port, std::chrono::seconds(1), "original"
            ),
            std::make_unique<lld::recursive_mutex>(
                    "127.0.0.1", port, std::chrono::seconds(1), "original"
            )
    };
    const auto invalid_name = [](const lld::exception& error) {
        return error.error_type() == lld::exception::et_logic_error;
    };
    for (const auto& client : clients) {
        for (const auto& name : invalid_lock_names()) {
            BOOST_CHECK_EXCEPTION(client->lock_name(name), lld::exception, invalid_name);
            BOOST_CHECK_MESSAGE(client->lock_name() == "original", "Rejected name is not stored");
        }
        BOOST_REQUIRE_MESSAGE(client->try_lock(), "The original name can still be acquired");
        BOOST_CHECK_EXCEPTION(client->lock_name("original\nUNLOCK"), lld::exception, invalid_name);
        lld::mutex observer("127.0.0.1", port, std::chrono::seconds(1), "original");
        BOOST_CHECK_MESSAGE(!observer.try_lock(), "Rejected injection does not release ownership");
        client->unlock(lld::mutex::tt_throw_on_failure);

        for (const auto& name :
             {std::string("n"),
              std::string(256, 'n'),
              std::string(
                      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
                      "0123456789_-"
              )}) {
            client->lock_name(name);
            observer.lock_name(name);
            BOOST_REQUIRE_MESSAGE(client->try_lock(), "Valid names remain usable after rejection");
            BOOST_CHECK_MESSAGE(!observer.try_lock(), "Valid name bytes identify the same lock");
            client->unlock(lld::mutex::tt_throw_on_failure);
            BOOST_REQUIRE_MESSAGE(observer.try_lock(), "Explicit release transfers ownership");
            observer.unlock(lld::mutex::tt_throw_on_failure);
        }
    }
}


BOOST_FIXTURE_TEST_CASE(protocol_runs_unwind_the_framework_without_leaks, ldt::network_fixture)
{
    // Run occupied-port, independent-selection, and successful protocol scenarios in child
    // processes. Verify normal reporting and teardown, prerequisite skips, and no reported CRT
    // leaks.
    const auto output_path = std::filesystem::current_path() / "protocol_failure_test.txt";
    const auto endpoint = lkd::detail::lifetime_test_access::endpoint(server);
    const std::string executable = ldt::master_test_suite_argv()[0];

    // Exercise failed prerequisites, independent selection, and normal teardown in fresh
    // processes so the debug CRT also checks allocations retained until process exit.
    for (const std::string scenario : {"occupied", "independent", "successful"}) {
        if (scenario == "successful") {
            server.stop();
            server.io_service().poll();
        }
        const std::vector<std::string> arguments = {
                scenario == "independent" ? "--run_test=protocol/lockd_test_meta_test_cases"
                                          : "--run_test=protocol",
                "--report_level=detailed",
                "--log_level=test_suite",
                "--color_output=no",
                "--detect_memory_leaks=1",
                "--",
                "--interface",
                "127.0.0.1",
                "--port",
                std::to_string(endpoint.port()),
                "--threads",
                "2"
        };
        ldt::process::child child(
                executable,
                ldt::process::args(arguments),
                (ldt::process::std_out & ldt::process::std_err) > output_path.string()
        );
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (child.running() && std::chrono::steady_clock::now() < deadline) {
            server.io_service().poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const bool exited = !child.running();
        if (!exited)
            child.terminate();

#ifdef WIN32
        // Process v1 caches the exit status in running(), after which wait() does nothing.
        // Wait on the process itself before accessing its output file, including after
        // terminate(), which only requests termination on Windows.
        const auto wait_result = boost::winapi::WaitForSingleObject(child.native_handle(), 30000);
        if (wait_result == boost::winapi::wait_failed)
            throw boost::system::system_error(
                    static_cast<int>(boost::winapi::GetLastError()),
                    boost::system::system_category()
            );
        BOOST_REQUIRE_MESSAGE(
                wait_result == boost::winapi::wait_object_0,
                "The child process finishes releasing its handles"
        );
#endif

        child.wait();
        BOOST_REQUIRE_MESSAGE(exited, "Framework shutdown completes without a forced process exit");
        std::ifstream file(output_path);
        const std::string output(
                (std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>()
        );
        if (scenario == "occupied") {
            BOOST_CHECK_MESSAGE(
                    child.exit_code() == boost::exit_test_failure,
                    "An occupied port returns a normal test-failure status: " << output
            );
            BOOST_CHECK_MESSAGE(
                    output.find("no_existing_lockd_listening") != std::string::npos,
                    "The prerequisite reports the occupied port"
            );
            BOOST_CHECK_MESSAGE(
                    output.find("Test case \"protocol/mutex_lock_lifecycle\" is skipped") !=
                            std::string::npos,
                    "Daemon-dependent tests are skipped: " << output
            );
        }
        else {
            BOOST_CHECK_MESSAGE(
                    child.exit_code() == 0, "The " << scenario << " run succeeds: " << output
            );
        }
        BOOST_CHECK_MESSAGE(
                output.find("Leaving test module") != std::string::npos,
                "Boost.Test finishes reporting and teardown"
        );
        BOOST_CHECK_MESSAGE(
                output.find("Detected memory leaks!") == std::string::npos,
                "MSVC debug CRT leak checking reports no surviving allocations: " << output
        );
        BOOST_CHECK_MESSAGE(
                output.find("bailing out") == std::string::npos,
                "The framework does not take a process-exit shortcut"
        );
        file.close();
        std::filesystem::remove(output_path);
    }
}


BOOST_FIXTURE_TEST_CASE(
        shutdown_waits_for_all_holders_and_flushes_unlock_replies, ldt::network_fixture
)
{
    // Stop a server with two lock holders and release them separately. Verify the fixed grace
    // deadline, refusal of new connections, and delivery of both unlock replies before shutdown.
    const auto first = connect();
    const auto second = connect();
    first->send("TRY_LOCK first\n");
    BOOST_REQUIRE_EQUAL(first->line(), "LOCK_ACQUIRED");
    second->send("TRY_LOCK second\n");
    BOOST_REQUIRE_EQUAL(second->line(), "LOCK_ACQUIRED");
    const auto endpoint = lkd::detail::lifetime_test_access::endpoint(server);

    const auto before = std::chrono::steady_clock::now();
    server.stop();
    ldt::poll_until(server.io_service(), [&] { return server.stopping(); });
    const auto deadline = lkd::detail::lifetime_test_access::shutdown_deadline(server);
    BOOST_CHECK_MESSAGE(
            deadline >= before + std::chrono::seconds(30),
            "Shutdown grants holders the full 30-second grace period"
    );
    BOOST_CHECK_MESSAGE(
            deadline <= std::chrono::steady_clock::now() + std::chrono::seconds(30),
            "Shutdown uses a 30-second deadline"
    );

    // Repeated stop requests and canceled callbacks must not extend or cut short the grace.
    server.stop();
    server.io_service().poll();
    lkd::detail::lifetime_test_access::canceled_shutdown(server);
    BOOST_CHECK(lkd::detail::lifetime_test_access::shutdown_deadline(server) == deadline);
    BOOST_CHECK_EQUAL(server.client_count(), 2);
    BOOST_CHECK(!server.io_service().stopped());
    boost::asio::ip::tcp::socket newcomer(server.io_service());
    boost::system::error_code error;

    // NOLINTNEXTLINE(bugprone-unused-return-value)
    static_cast<void>(newcomer.connect(endpoint, error));
    BOOST_CHECK_MESSAGE(!!error, "The listener refuses new connections during shutdown");

    first->send("UNLOCK\n");
    BOOST_CHECK_EQUAL(first->line(), "LOCK_RELEASED");
    ldt::poll_until(server.io_service(), [&] { return server.client_count() == 1; });
    BOOST_CHECK(!server.io_service().stopped());
    second->send("UNLOCK\n");
    BOOST_CHECK_EQUAL(second->line(), "LOCK_RELEASED");
    ldt::poll_until(server.io_service(), [&] { return server.client_count() == 0; });
    BOOST_CHECK(server.io_service().stopped());
    BOOST_CHECK_THROW(first->line(), boost::system::system_error);
    BOOST_CHECK_THROW(second->line(), boost::system::system_error);
}


BOOST_FIXTURE_TEST_CASE(
        shutdown_drops_idle_and_queued_clients_without_granting_more_locks, ldt::network_fixture
)
{
    // Stop a server with a holder, ordinary and timed waiters, and an idle client. Verify only
    // the holder drains and its release does not grant ownership to a closed waiter.
    const auto owner = connect();
    const auto waiter = connect();
    const auto timed = connect();
    const auto idle = connect();
    owner->send("LOCK drain\n");
    BOOST_REQUIRE_EQUAL(owner->line(), "LOCK_ACQUIRING");
    BOOST_REQUIRE_EQUAL(owner->line(), "LOCK_ACQUIRED");
    waiter->send("LOCK drain\n");
    BOOST_REQUIRE_EQUAL(waiter->line(), "LOCK_ACQUIRING");
    timed->send("TIMED_LOCK drain 60s\n");
    BOOST_REQUIRE_EQUAL(timed->line(), "LOCK_ACQUIRING");
    server.stop();
    ldt::poll_until(server.io_service(), [&] { return server.client_count() == 1; });
    BOOST_CHECK_THROW(waiter->line(), boost::system::system_error);
    BOOST_CHECK_THROW(timed->line(), boost::system::system_error);
    BOOST_CHECK_THROW(idle->line(), boost::system::system_error);

    owner->send("UNLOCK\n");
    BOOST_CHECK_EQUAL(owner->line(), "LOCK_RELEASED");
    ldt::poll_until(server.io_service(), [&] { return server.client_count() == 0; });
    const auto mutex = server.lock_store().get_mutex("drain");
    BOOST_CHECK_MESSAGE(mutex->try_lock(), "Shutdown does not hand ownership to a closed waiter");
    mutex->unlock();
}


BOOST_FIXTURE_TEST_CASE(
        shutdown_deadline_drops_holders_and_clients_report_the_disconnect,
        ldt::running_server_fixture
)
{
    // Expire the shutdown deadline while a native client holds a lock. Verify forced disconnect
    // clears connection state and later operations report the disconnected client.
    lld::mutex mutex(
            "127.0.0.1",
            lkd::detail::lifetime_test_access::endpoint(server).port(),
            std::chrono::seconds(1),
            "deadline"
    );
    mutex.lock();
    server.stop();
    ldt::poll_until(server.io_service(), [&] { return server.stopping(); });

    // Deliver the real timeout handler on the I/O thread without a 30-second test delay.
    const auto expired = std::make_shared<std::promise<void>>();
    const auto expiry = expired->get_future();
    boost::asio::post(server.io_service(), [server_ptr = &server, expired] {
        lkd::detail::lifetime_test_access::expire_shutdown(*server_ptr);
        expired->set_value();
    });
    BOOST_REQUIRE(expiry.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
    const lld::detail::lockd_test_access<lld::mutex> access(&mutex);
    BOOST_CHECK_THROW(
            static_cast<void>(access.blocking_read_line(std::chrono::seconds(3))), lld::exception
    );
    BOOST_CHECK_MESSAGE(access.peer_closed(), "Shutdown deadline produces EOF or reset");
    BOOST_CHECK(!mutex.connected());
    BOOST_CHECK_EXCEPTION(mutex.try_lock(), lld::exception, [](const lld::exception& error) {
        return error.error_type() == lld::exception::et_not_connected;
    });
    ldt::poll_until(server.io_service(), [&] { return server.client_count() == 0; });
}


BOOST_FIXTURE_TEST_CASE(
        shutdown_without_holders_finishes_without_waiting_for_idle_clients, ldt::network_fixture
)
{
    // Stop a server whose only connection is idle. Verify shutdown closes that connection and
    // completes without consuming the holder grace period.
    const auto idle = connect();
    server.stop();
    ldt::poll_until(server.io_service(), [&] { return server.io_service().stopped(); });
    BOOST_CHECK_EQUAL(server.client_count(), 0);
    BOOST_CHECK_THROW(idle->line(), boost::system::system_error);
}


BOOST_FIXTURE_TEST_CASE(shutdown_of_an_empty_server_finishes_immediately, ldt::network_fixture)
{
    // Stop a server with no clients. Verify it enters shutdown and finishes its I/O work without
    // waiting for a grace-period timer.
    server.stop();
    ldt::poll_until(server.io_service(), [&] { return server.io_service().stopped(); });
    BOOST_CHECK(server.stopping());
    BOOST_CHECK_EQUAL(server.client_count(), 0);
}


BOOST_FIXTURE_TEST_CASE(shutdown_finishes_when_a_holder_disconnects, ldt::network_fixture)
{
    // Disconnect a lock holder while the server drains. Verify shutdown completes and the
    // abandoned mutex becomes available.
    const auto owner = connect();
    owner->send("TRY_LOCK disconnected-holder\n");
    BOOST_REQUIRE_EQUAL(owner->line(), "LOCK_ACQUIRED");
    server.stop();
    ldt::poll_until(server.io_service(), [&] { return server.stopping(); });
    owner->close();
    ldt::poll_until(server.io_service(), [&] { return server.io_service().stopped(); });
    BOOST_CHECK_EQUAL(server.client_count(), 0);
    const auto mutex = server.lock_store().get_mutex("disconnected-holder");
    BOOST_REQUIRE_MESSAGE(mutex->try_lock(), "Disconnect releases the draining holder's lock");
    mutex->unlock();
}


BOOST_FIXTURE_TEST_CASE(shutdown_rejects_lock_commands_from_existing_holders, ldt::network_fixture)
{
    // Send a new acquisition from a draining holder. Verify shutdown rejects the command, closes
    // the client, and releases its original lock.
    const auto owner = connect();
    owner->send("TRY_LOCK original\n");
    BOOST_REQUIRE_EQUAL(owner->line(), "LOCK_ACQUIRED");
    server.stop();
    ldt::poll_until(server.io_service(), [&] { return server.stopping(); });
    owner->send("TRY_LOCK another\n");
    BOOST_CHECK_THROW(owner->line(), boost::system::system_error);
    ldt::poll_until(server.io_service(), [&] { return server.client_count() == 0; });
    const auto original = server.lock_store().get_mutex("original");
    BOOST_REQUIRE_MESSAGE(original->try_lock(), "Rejected acquisition releases the original lock");
    original->unlock();
}


BOOST_FIXTURE_TEST_CASE(
        owner_disconnect_hands_the_lock_to_one_waiter_at_a_time, ldt::network_fixture
)
{
    // Disconnect an owner with two queued waiters. Verify only the first acquires, the second
    // remains queued, and release hands ownership to the second.
    const auto owner = connect();
    owner->send("LOCK handoff\n");
    BOOST_REQUIRE_EQUAL(owner->line(), "LOCK_ACQUIRING");
    BOOST_REQUIRE_EQUAL(owner->line(), "LOCK_ACQUIRED");
    const auto first = connect();
    const auto second = connect();
    first->send("LOCK handoff\n");
    BOOST_REQUIRE_EQUAL(first->line(), "LOCK_ACQUIRING");
    second->send("LOCK handoff\n");
    BOOST_REQUIRE_EQUAL(second->line(), "LOCK_ACQUIRING");

    // Reading each acknowledgement above establishes queue order before the owner's EOF.
    owner->close();
    BOOST_CHECK_EQUAL(first->line(), "LOCK_ACQUIRED");
    second->send("INFO\n");
    BOOST_CHECK(second->line().find("you are client ") == 0);
    BOOST_CHECK_EQUAL(second->line(), "lock status is acquiring (lock=<none>)");
    BOOST_CHECK(second->line().find("current connected clients: ") == 0);
    first->send("UNLOCK\n");
    BOOST_CHECK_EQUAL(first->line(), "LOCK_RELEASED");
    BOOST_CHECK_EQUAL(second->line(), "LOCK_ACQUIRED");
    second->send("UNLOCK\n");
    BOOST_CHECK_EQUAL(second->line(), "LOCK_RELEASED");
}


BOOST_FIXTURE_TEST_CASE(
        disconnected_front_and_middle_waiters_do_not_strand_the_queue, ldt::network_fixture
)
{
    // Disconnect waiters at the front and middle of a queue before releasing its owner. Verify
    // the surviving waiters acquire in order and can release normally.
    const auto owner = connect();
    owner->send("TRY_LOCK queue\n");
    BOOST_REQUIRE_EQUAL(owner->line(), "LOCK_ACQUIRED");
    std::vector<std::shared_ptr<ldt::protocol_connection>> waiters;
    for (int i = 0; i < 4; ++i) {
        const auto waiter = connect();
        waiter->send("LOCK queue\n");
        BOOST_REQUIRE_EQUAL(waiter->line(), "LOCK_ACQUIRING");
        waiters.push_back(waiter);
    }
    waiters[0]->close();
    waiters[2]->close();
    ldt::poll_until(server.io_service(), [&] { return server.client_count() == 3; });
    owner->send("UNLOCK\n");
    BOOST_CHECK_EQUAL(owner->line(), "LOCK_RELEASED");
    BOOST_CHECK_EQUAL(waiters[1]->line(), "LOCK_ACQUIRED");
    waiters[1]->send("UNLOCK\n");
    BOOST_CHECK_EQUAL(waiters[1]->line(), "LOCK_RELEASED");
    BOOST_CHECK_EQUAL(waiters[3]->line(), "LOCK_ACQUIRED");
    waiters[3]->send("UNLOCK\n");
    BOOST_CHECK_EQUAL(waiters[3]->line(), "LOCK_RELEASED");
}


BOOST_FIXTURE_TEST_CASE(fragmented_and_batched_commands_preserve_reply_order, ldt::network_fixture)
{
    // Split one command across writes and append several commands in one write. Verify framing
    // produces one reply per complete command in the original order.
    const auto connection = connect();
    connection->send("TRY_");
    connection->send("LOCK framing\r");
    connection->send("\nUNLOCK\nTRY_LOCK framing\nUNLOCK\n");
    for (const auto expected : {"LOCK_ACQUIRED", "LOCK_RELEASED", "LOCK_ACQUIRED", "LOCK_RELEASED"})
        BOOST_CHECK_EQUAL(connection->line(), expected);
}


BOOST_FIXTURE_TEST_CASE(backspace_and_del_edit_only_the_current_command, ldt::network_fixture)
{
    // Send backspace and DEL edits across fragmented and batched commands. Verify the edited
    // lock name and reply order, including edits at the start of a new command.
    const auto connection = connect();
    for (const auto* const command :
         {"TRY_LOCX\bK edited\n",
          "TRY_LOCX\bK edited\r\n",
          "\b\b\bTRY_LOCK edited\n",
          "TRY_LOCK editedXX\b\b\n",
          "TRY_LOCX\x7fK edited\n",
          "TRY_LOCX\x7fK edited\r\n",
          "\x7f\x7f\x7fTRY_LOCK edited\n",
          "TRY_LOCK editedXX\x7f\x7f\n",
          "TRY_LOCK edited X\x7f\b\n"}) {
        connection->send(command);
        BOOST_REQUIRE_EQUAL(connection->line(), "LOCK_ACQUIRED");
        connection->send("INFO\n");
        BOOST_CHECK(connection->line().find("you are client ") == 0);
        BOOST_CHECK_EQUAL(connection->line(), "lock status is locked (lock=edited)");
        BOOST_CHECK(connection->line().find("current connected clients: ") == 0);
        connection->send("UNLOCK\n");
        BOOST_REQUIRE_EQUAL(connection->line(), "LOCK_RELEASED");
    }

    // A deletion received separately must edit the buffered prefix, not discard that prefix or
    // produce terminal-control bytes in the protocol replies.
    connection->send("TRY_LOCK edited_bad");
    server.io_service().poll();
    connection->send("\x7f\b");
    server.io_service().poll();
    connection->send("\b\x7f\nINFO\nUNLOCK\n\x7fTRY_LOCK edited\nUNLOCK\n");
    BOOST_REQUIRE_EQUAL(connection->line(), "LOCK_ACQUIRED");
    BOOST_CHECK(connection->line().find("you are client ") == 0);
    BOOST_CHECK_EQUAL(connection->line(), "lock status is locked (lock=edited)");
    BOOST_CHECK(connection->line().find("current connected clients: ") == 0);
    for (const auto* const expected : {"LOCK_RELEASED", "LOCK_ACQUIRED", "LOCK_RELEASED"})
        BOOST_REQUIRE_EQUAL(connection->line(), expected);
}


BOOST_FIXTURE_TEST_CASE(
        lock_name_boundaries_and_rejected_requests_preserve_state, ldt::network_fixture
)
{
    // Exercise valid and oversized names plus malformed requests while idle and locked. Verify
    // rejection replies leave the connection usable and preserve an existing owner.
    const auto connection = connect();
    const std::string valid_name(256, 'n');
    connection->send("TRY_LOCK " + valid_name + "\n");
    BOOST_REQUIRE_EQUAL(connection->line(), "LOCK_ACQUIRED");
    connection->send("UNLOCK\n");
    BOOST_REQUIRE_EQUAL(connection->line(), "LOCK_RELEASED");

    struct rejected_request
    {
        std::string command;
        std::string idle_reply;
        std::string locked_reply;
    };

    // The one-lock-per-client guard precedes interpretation of timeout units, whereas syntax
    // checks precede that guard. Keep both expected replies explicit for each connection state.
    const std::vector<rejected_request> rejected = {
            {"TRY_LOCK " + std::string(257, 'n'), "LOCK_NAME_TOO_LONG", "LOCK_NAME_TOO_LONG"},
            {"TRY_LOCK invalid.name", "INVALID_COMMAND", "INVALID_COMMAND"},
            {"TRY_LOCK invalid/name", "INVALID_COMMAND", "INVALID_COMMAND"},
            {"TRY_LOCK caf\xc3\xa9", "INVALID_COMMAND", "INVALID_COMMAND"},
            {"TRY_LOCK invalid\x80", "INVALID_COMMAND", "INVALID_COMMAND"},
            {"LOCK", "MISSING_LOCK_NAME", "MISSING_LOCK_NAME"},
            {"TIMED_LOCK boundary", "MISSING_LOCK_TIME", "MISSING_LOCK_TIME"},
            {"TIMED_LOCK boundary 1fortnight", "INVALID_TIME_TYPE", "UNLOCK_FIRST"},
            {"TRY_LOCK boundary 1s", "INVALID_ARGS", "INVALID_ARGS"},
            {"UNLOCK trailing", "INVALID_COMMAND", "INVALID_COMMAND"},
    };
    for (const auto& request : rejected) {
        BOOST_TEST_CONTEXT(request.command)
        {
            connection->send(request.command + "\nTRY_LOCK boundary\nUNLOCK\n");
            BOOST_CHECK_EQUAL(connection->line(), request.idle_reply);
            BOOST_CHECK_EQUAL(connection->line(), "LOCK_ACQUIRED");
            BOOST_CHECK_EQUAL(connection->line(), "LOCK_RELEASED");
        }
    }

    // Negative and overflowing durations have no documented distinct error code. They must be
    // rejected, leave no pending acquisition, and allow the same connection to continue normally.
    for (const auto& duration : {std::string("-1ms"), std::string(100, '9') + "ms"}) {
        connection->send("TIMED_LOCK boundary " + duration + "\nTRY_LOCK boundary\nUNLOCK\n");
        const auto reply = connection->line();
        BOOST_CHECK_MESSAGE(
                reply == "INVALID_COMMAND" || reply == "MISSING_LOCK_TIME",
                "Invalid duration rejected: " << reply
        );
        BOOST_CHECK_EQUAL(connection->line(), "LOCK_ACQUIRED");
        BOOST_CHECK_EQUAL(connection->line(), "LOCK_RELEASED");
    }

    // Rejecting a malformed request while already locked must also preserve the existing owner.
    connection->send("TRY_LOCK boundary\n");
    BOOST_REQUIRE_EQUAL(connection->line(), "LOCK_ACQUIRED");
    const auto observer = connect();
    for (const auto& request : rejected) {
        connection->send(request.command + "\n");
        BOOST_CHECK_EQUAL(connection->line(), request.locked_reply);
        observer->send("TRY_LOCK boundary\n");
        BOOST_CHECK_EQUAL(observer->line(), "LOCK_UNAVAILABLE");
    }
    connection->send("UNLOCK\n");
    BOOST_CHECK_EQUAL(connection->line(), "LOCK_RELEASED");
    observer->send("TRY_LOCK boundary\nUNLOCK\n");
    BOOST_CHECK_EQUAL(observer->line(), "LOCK_ACQUIRED");
    BOOST_CHECK_EQUAL(observer->line(), "LOCK_RELEASED");
}


BOOST_FIXTURE_TEST_CASE(
        unterminated_command_disconnects_and_releases_its_lock, ldt::network_fixture
)
{
    // Fill the command budget without a delimiter while holding a lock. The daemon must close
    // this connection and release ownership, rather than retaining or growing its input buffer.
    const auto connection = connect();
    connection->send("TRY_LOCK oversized_request\n");
    BOOST_REQUIRE_EQUAL(connection->line(), "LOCK_ACQUIRED");
    connection->send(std::string(1024, 'x'));
    ldt::poll_until(server.io_service(), [&] { return server.client_count() == 0; });
    const auto observer = connect();
    observer->send("TRY_LOCK oversized_request\nUNLOCK\n");
    BOOST_CHECK_EQUAL(observer->line(), "LOCK_ACQUIRED");
    BOOST_CHECK_EQUAL(observer->line(), "LOCK_RELEASED");
}


BOOST_FIXTURE_TEST_CASE(
        command_byte_limit_preserves_boundary_lines_and_batches, ldt::network_fixture
)
{
    // Submit exact-limit commands in batches larger than the input buffer. The limit applies to
    // each wire command, including CRLF, and must not discard or combine the following commands.
    const auto connection = connect();
    for (const std::string ending : {"\n", "\r\n"}) {
        std::string command = "TRY_LOCK " + std::string(256, 'n');
        command.append(1024 - ending.size() - command.size(), ' ');
        command += ending;
        std::string batch;
        for (unsigned int index = 0; index < 4; ++index)
            batch += command + "UNLOCK\n";
        connection->send(batch);
        for (unsigned int index = 0; index < 4; ++index) {
            BOOST_CHECK_EQUAL(connection->line(), "LOCK_ACQUIRED");
            BOOST_CHECK_EQUAL(connection->line(), "LOCK_RELEASED");
        }
    }
}


BOOST_FIXTURE_TEST_CASE(
        command_byte_limit_counts_whitespace_and_editing_bytes, ldt::network_fixture
)
{
    // Exceed the wire budget using padding and edits that would shrink before parsing. Verify
    // the raw bytes still cause a disconnect, even with valid commands following the long line.
    for (const auto& payload :
         {std::string(1024, ' ') + "TRY_LOCK bypass\n",
          std::string(512, 'x') + std::string(512, '\b') + "TRY_LOCK bypass\n",
          std::string(512, 'x') + std::string(512, '\x7f') + "TRY_LOCK bypass\n"}) {
        const auto connection = connect();
        connection->send(payload);
        ldt::poll_until(server.io_service(), [&] { return server.client_count() == 0; });
        const auto mutex = server.lock_store().get_mutex("bypass");
        BOOST_REQUIRE_MESSAGE(mutex->try_lock(), "A disconnected client leaves no owned lock");
        mutex->unlock();
    }
}


BOOST_FIXTURE_TEST_CASE(
        buffered_command_batches_preserve_order_and_partial_deadlines, ldt::network_fixture
)
{
    // Exceed both one dispatch's command budget and the wire buffer capacity. Reads resumed
    // by reply completions must not duplicate commands or lose the unfinished tail's deadline.
    const auto connection = connect();
    const auto client = lifetime_test_access::connected_client(server, connection->endpoint());
    std::string batch;
    for (unsigned int index = 0; index < 128; ++index)
        batch += "TRY_LOCK fast_" + std::to_string(index) + "\nUNLOCK\n";
    connection->send(batch + "TRY_");
    for (unsigned int index = 0; index < 128; ++index) {
        BOOST_REQUIRE_EQUAL(connection->line(), "LOCK_ACQUIRED");
        BOOST_REQUIRE_EQUAL(connection->line(), "LOCK_RELEASED");
    }
    ldt::poll_until(server.io_service(), [&] {
        return lifetime_test_access::command_bytes(client) == 4 &&
               lifetime_test_access::command_deadline(client) !=
                       std::chrono::steady_clock::time_point::max();
    });
    const auto timeout = lifetime_test_access::command_deadline(client);
    connection->send("LOCK fast_tail\nUNLOCK\nVERSION\n");
    BOOST_CHECK_EQUAL(connection->line(), "LOCK_ACQUIRED");
    BOOST_CHECK_EQUAL(connection->line(), "LOCK_RELEASED");
    BOOST_CHECK(connection->line().find("lockd ") == 0);
    BOOST_CHECK(
            lifetime_test_access::command_deadline(client) ==
            std::chrono::steady_clock::time_point::max()
    );
    lifetime_test_access::check_deadlines(client, timeout);
    BOOST_CHECK_MESSAGE(
            client->socket().is_open(), "Completed tails have no deadline for maintenance to expire"
    );
}


BOOST_FIXTURE_TEST_CASE(buffered_quit_delimiters_stop_the_following_commands, ldt::network_fixture)
{
    // CTRL+D and CTRL+Z terminate at their original byte boundary. Processing a complete
    // buffered quit must not continue into a following lock request in the same socket read.
    for (const char delimiter : {'\x04', '\x1a'}) {
        const auto connection = connect();
        auto client = lifetime_test_access::connected_client(server, connection->endpoint());
        connection->send(std::string(1, delimiter) + "TRY_LOCK after_quit\n");
        ldt::poll_until(server.io_service(), [&] { return !client->socket().is_open(); });
        client.reset();
        ldt::poll_until(server.io_service(), [&] { return server.client_count() == 0; });
        const auto mutex = server.lock_store().get_mutex("after_quit");
        BOOST_REQUIRE(mutex->try_lock());
        mutex->unlock();
    }
}


BOOST_FIXTURE_TEST_CASE(
        command_deadline_starts_on_first_byte_and_is_not_extended, ldt::network_fixture
)
{
    // Idle clients have no deadline. Once a partial command arrives, additional bytes keep the
    // original 30-second deadline; maintenance before expiry must leave the connection open.
    const auto connection = connect();
    auto client = lifetime_test_access::connected_client(server, connection->endpoint());
    BOOST_CHECK(
            lifetime_test_access::command_deadline(client) ==
            std::chrono::steady_clock::time_point::max()
    );
    const auto before = std::chrono::steady_clock::now();
    connection->send("I");
    ldt::poll_until(server.io_service(), [&] {
        return lifetime_test_access::command_bytes(client) == 1;
    });
    const auto deadline = lifetime_test_access::command_deadline(client);
    BOOST_CHECK(deadline >= before + std::chrono::seconds(30));
    BOOST_CHECK(deadline <= std::chrono::steady_clock::now() + std::chrono::seconds(30));
    connection->send("N");
    ldt::poll_until(server.io_service(), [&] {
        return lifetime_test_access::command_bytes(client) == 2;
    });
    BOOST_CHECK(lifetime_test_access::command_deadline(client) == deadline);
    lifetime_test_access::check_deadlines(client, deadline - std::chrono::nanoseconds(1));
    BOOST_CHECK_MESSAGE(client->socket().is_open(), "A pass before the deadline preserves input");
    BOOST_CHECK(lifetime_test_access::command_deadline(client) == deadline);
    lifetime_test_access::expire_command_read(client);
    BOOST_CHECK_MESSAGE(!client->socket().is_open(), "A stalled command closes its connection");
    client.reset();
    ldt::poll_until(server.io_service(), [&] { return server.client_count() == 0; });
}


BOOST_FIXTURE_TEST_CASE(completed_commands_replace_their_maintenance_deadline, ldt::network_fixture)
{
    // Finish a partial command and start another in the same batch. Maintenance at the old
    // deadline must inspect the new deadline, or leave an idle owner alone after completion.
    const auto connection = connect();
    const auto client = lifetime_test_access::connected_client(server, connection->endpoint());
    connection->send("VER");
    ldt::poll_until(server.io_service(), [&] {
        return lifetime_test_access::command_bytes(client) == 3;
    });
    const auto old_timeout = lifetime_test_access::command_deadline(client);
    connection->send("SION\nTRY_");
    BOOST_REQUIRE_MESSAGE(connection->line().find("lockd ") == 0, "First command completes");
    ldt::poll_until(server.io_service(), [&] {
        return lifetime_test_access::command_bytes(client) == 4;
    });
    const auto next_deadline = lifetime_test_access::command_deadline(client);
    const auto next_timeout = lifetime_test_access::command_deadline(client);
    lifetime_test_access::check_deadlines(client, old_timeout);
    lifetime_test_access::check_deadlines(client, old_timeout);
    BOOST_CHECK_MESSAGE(
            client->socket().is_open(), "A pass at the old deadline cannot close the next command"
    );
    BOOST_CHECK(lifetime_test_access::command_deadline(client) == next_deadline);
    connection->send("LOCK deadline_owner\n");
    BOOST_REQUIRE_EQUAL(connection->line(), "LOCK_ACQUIRED");
    BOOST_CHECK(
            lifetime_test_access::command_deadline(client) ==
            std::chrono::steady_clock::time_point::max()
    );
    lifetime_test_access::check_deadlines(client, old_timeout);
    lifetime_test_access::check_deadlines(client, next_timeout);
    BOOST_CHECK_MESSAGE(client->socket().is_open(), "Completed requests leave no read deadline");
    const auto observer = connect();
    observer->send("TRY_LOCK deadline_owner\n");
    BOOST_CHECK_EQUAL(observer->line(), "LOCK_UNAVAILABLE");
    connection->send("UNLOCK\n");
    BOOST_CHECK_EQUAL(connection->line(), "LOCK_RELEASED");
}


BOOST_FIXTURE_TEST_CASE(
        idle_connections_before_and_between_locks_remain_usable, ldt::network_fixture
)
{
    // An application can connect before it needs a lock, then do unrelated work between
    // acquisitions. None of those idle periods may arm a deadline or require reconnection.
    const auto connection = connect();
    const auto client = lifetime_test_access::connected_client(server, connection->endpoint());
    const auto no_deadline = std::chrono::steady_clock::time_point::max();
    BOOST_CHECK_MESSAGE(
            lifetime_test_access::command_deadline(client) == no_deadline,
            "A connection that has sent no data has no read deadline"
    );
    ldt::poll_until(server.io_service(), [&] {
        return lifetime_test_access::pending_reply_bytes(client) == 0;
    });
    lifetime_test_access::check_clients(
            server, std::chrono::steady_clock::now() + std::chrono::hours(1)
    );
    BOOST_REQUIRE_MESSAGE(
            client->socket().is_open(), "Maintenance preserves a never-used connection"
    );
    for (unsigned int attempt = 0; attempt < 2; ++attempt) {
        connection->send("TRY_LOCK idle_between_locks\n");
        BOOST_REQUIRE_EQUAL(connection->line(), "LOCK_ACQUIRED");
        BOOST_CHECK_MESSAGE(
                lifetime_test_access::command_deadline(client) == no_deadline,
                "Holding a lock does not require further commands"
        );
        server.io_service().poll();
        lifetime_test_access::check_clients(
                server, std::chrono::steady_clock::now() + std::chrono::hours(1)
        );
        BOOST_REQUIRE_MESSAGE(client->socket().is_open(), "Maintenance preserves an idle owner");
        connection->send("UNLOCK\n");
        BOOST_REQUIRE_EQUAL(connection->line(), "LOCK_RELEASED");
        server.io_service().poll();
        lifetime_test_access::check_clients(
                server, std::chrono::steady_clock::now() + std::chrono::hours(1)
        );
        BOOST_CHECK_MESSAGE(
                lifetime_test_access::command_deadline(client) == no_deadline,
                "Unprotected work between acquisitions has no read deadline"
        );
    }
}


BOOST_FIXTURE_TEST_CASE(
        maintenance_sweeps_stalls_without_expiring_idle_clients, ldt::network_fixture
)
{
    // One sweep must find clients that never supplied a lock name, retain legitimate idle
    // owners/waiters, and tolerate another pass before canceled I/O has released closed clients.
    const auto idle = connect();
    const auto owner = connect();
    const auto waiter = connect();
    const auto incomplete = connect();
    const auto unread = connect(4096);
    const auto idle_client = lifetime_test_access::connected_client(server, idle->endpoint());
    const auto owner_client = lifetime_test_access::connected_client(server, owner->endpoint());
    const auto waiter_client = lifetime_test_access::connected_client(server, waiter->endpoint());
    auto incomplete_client = lifetime_test_access::connected_client(server, incomplete->endpoint());
    auto unread_client = lifetime_test_access::connected_client(server, unread->endpoint());
    owner->send("TRY_LOCK maintenance_owner\n");
    BOOST_REQUIRE_EQUAL(owner->line(), "LOCK_ACQUIRED");
    waiter->send("LOCK maintenance_owner\n");
    BOOST_REQUIRE_EQUAL(waiter->line(), "LOCK_ACQUIRING");
    ldt::poll_until(server.io_service(), [&] {
        return lifetime_test_access::pending_reply_bytes(idle_client) == 0 &&
               lifetime_test_access::pending_reply_bytes(owner_client) == 0 &&
               lifetime_test_access::pending_reply_bytes(waiter_client) == 0;
    });
    incomplete->send("I");
    ldt::poll_until(server.io_service(), [&] {
        return lifetime_test_access::command_bytes(incomplete_client) == 1;
    });
    unread_client->socket().set_option(boost::asio::socket_base::send_buffer_size(16 * 1024));
    ldt::fill_unread_replies(unread_client, server.io_service());
    const auto read_deadline = lifetime_test_access::command_deadline(incomplete_client);
    const auto write_deadline = lifetime_test_access::write_deadline(unread_client);
    lifetime_test_access::check_clients(
            server, std::min(read_deadline, write_deadline) - std::chrono::nanoseconds(1)
    );
    BOOST_CHECK(incomplete_client->socket().is_open());
    BOOST_CHECK(unread_client->socket().is_open());

    // Observe the first pass after expiry, not an exact timer callback at 30 seconds. No test
    // clock sleeps are needed, and the production checks still decide which clients to close.
    const auto pass = std::max(read_deadline, write_deadline) + std::chrono::seconds(5);
    lifetime_test_access::check_clients(server, pass);
    lifetime_test_access::check_clients(server, pass);
    BOOST_CHECK(!incomplete_client->socket().is_open());
    BOOST_CHECK(!unread_client->socket().is_open());
    BOOST_REQUIRE(idle_client->socket().is_open());
    BOOST_REQUIRE(owner_client->socket().is_open());
    BOOST_REQUIRE(waiter_client->socket().is_open());
    BOOST_CHECK(lifetime_test_access::status(owner_client) == lld::mutex::ls_locked);
    BOOST_CHECK(lifetime_test_access::status(waiter_client) == lld::mutex::ls_acquiring);
    incomplete_client.reset();
    unread_client.reset();
    ldt::poll_until(server.io_service(), [&] { return server.client_count() == 3; });
    owner->send("UNLOCK\n");
    BOOST_CHECK_EQUAL(owner->line(), "LOCK_RELEASED");
    BOOST_CHECK_EQUAL(waiter->line(), "LOCK_ACQUIRED");
    waiter->send("UNLOCK\n");
    BOOST_CHECK_EQUAL(waiter->line(), "LOCK_RELEASED");
    idle->send("VERSION\n");
    BOOST_CHECK(idle->line().find("lockd ") == 0);
}


BOOST_FIXTURE_TEST_CASE(maintenance_rearms_once_and_stops_during_shutdown, ldt::network_fixture)
{
    // Deliver the registered event and its canceled OS completion separately. Only success
    // rearms the shared wait. A success already dispatched when shutdown begins cannot rearm it.
    const auto owner = connect();
    owner->send("TRY_LOCK maintenance_shutdown\n");
    BOOST_REQUIRE_EQUAL(owner->line(), "LOCK_ACQUIRED");
    const auto client = lifetime_test_access::connected_client(server, owner->endpoint());
    const auto original = lifetime_test_access::maintenance_deadline(server);
    lifetime_test_access::maintenance(server, boost::asio::error::operation_aborted);
    BOOST_CHECK(lifetime_test_access::maintenance_deadline(server) == original);
    const auto before = std::chrono::steady_clock::now();
    lifetime_test_access::expire_maintenance(server);
    const auto next = lifetime_test_access::maintenance_deadline(server);
    BOOST_CHECK(next >= before + std::chrono::seconds(5));
    BOOST_CHECK(next <= std::chrono::steady_clock::now() + std::chrono::seconds(5));
    server.io_service().poll();
    BOOST_CHECK(lifetime_test_access::maintenance_deadline(server) == next);
    server.stop();
    ldt::poll_until(server.io_service(), [&] { return server.stopping(); });
    BOOST_CHECK_EQUAL(lifetime_test_access::cancel_maintenance(server), 0);
    lifetime_test_access::maintenance(server, {});
    lifetime_test_access::maintenance(server, boost::asio::error::operation_aborted);
    BOOST_CHECK_EQUAL(lifetime_test_access::cancel_maintenance(server), 0);
    BOOST_CHECK_MESSAGE(
            client->socket().is_open(), "Shutdown still gives owners their release grace"
    );
    owner->send("UNLOCK\n");
    BOOST_CHECK_EQUAL(owner->line(), "LOCK_RELEASED");
}


BOOST_FIXTURE_TEST_CASE(stalled_command_disconnect_hands_the_lock_to_a_waiter, ldt::network_fixture)
{
    // Expire a partial command from a current owner while another connection waits. Draining
    // canceled I/O must destroy the owner promptly and hand off its lock exactly once.
    const auto owner = connect();
    auto client = lifetime_test_access::connected_client(server, owner->endpoint());
    owner->send("TRY_LOCK stalled_owner\n");
    BOOST_REQUIRE_EQUAL(owner->line(), "LOCK_ACQUIRED");
    const auto waiter = connect();
    waiter->send("LOCK stalled_owner\n");
    BOOST_REQUIRE_EQUAL(waiter->line(), "LOCK_ACQUIRING");
    owner->send("UN");
    ldt::poll_until(server.io_service(), [&] {
        return lifetime_test_access::command_bytes(client) == 2;
    });
    const auto timeout = lifetime_test_access::command_deadline(client);
    lifetime_test_access::expire_command_read(client);
    lifetime_test_access::check_deadlines(client, timeout);
    client.reset();
    ldt::poll_until(server.io_service(), [&] { return server.client_count() == 1; });
    BOOST_CHECK_EQUAL(waiter->line(), "LOCK_ACQUIRED");
    waiter->send("UNLOCK\n");
    BOOST_CHECK_EQUAL(waiter->line(), "LOCK_RELEASED");
}


BOOST_FIXTURE_TEST_CASE(stalled_queued_client_cannot_receive_a_late_grant, ldt::network_fixture)
{
    // Expire a partial command while the same connection is queued for a lock. Even if a callback
    // retains the closed client, releasing the owner must skip it and leave the lock available.
    const auto owner = connect();
    owner->send("TRY_LOCK stalled_waiter\n");
    BOOST_REQUIRE_EQUAL(owner->line(), "LOCK_ACQUIRED");
    const auto waiter = connect();
    const auto client = lifetime_test_access::connected_client(server, waiter->endpoint());
    waiter->send("LOCK stalled_waiter\n");
    BOOST_REQUIRE_EQUAL(waiter->line(), "LOCK_ACQUIRING");
    BOOST_CHECK(
            lifetime_test_access::command_deadline(client) ==
            std::chrono::steady_clock::time_point::max()
    );
    waiter->send("IN");
    ldt::poll_until(server.io_service(), [&] {
        return lifetime_test_access::command_bytes(client) == 2;
    });
    lifetime_test_access::expire_command_read(client);
    owner->send("UNLOCK\n");
    BOOST_CHECK_EQUAL(owner->line(), "LOCK_RELEASED");
    owner->send("TRY_LOCK stalled_waiter\nUNLOCK\n");
    BOOST_CHECK_EQUAL(owner->line(), "LOCK_ACQUIRED");
    BOOST_CHECK_EQUAL(owner->line(), "LOCK_RELEASED");
    BOOST_CHECK_EQUAL(lifetime_test_access::replies(client, "LOCK_ACQUIRED\r\n"), 0);
}


BOOST_FIXTURE_TEST_CASE(peer_disconnect_clears_an_incomplete_command_deadline, ldt::network_fixture)
{
    // Close the peer during a partial command. Its deadline must be cleared, and later
    // maintenance must not retain the disconnected owner or delay lock release.
    const auto connection = connect();
    auto client = lifetime_test_access::connected_client(server, connection->endpoint());
    connection->send("TRY_LOCK interrupted_owner\n");
    BOOST_REQUIRE_EQUAL(connection->line(), "LOCK_ACQUIRED");
    connection->send("U");
    ldt::poll_until(server.io_service(), [&] {
        return lifetime_test_access::command_bytes(client) == 1;
    });
    const auto timeout = lifetime_test_access::command_deadline(client);
    connection->close();
    ldt::poll_until(server.io_service(), [&] { return !client->socket().is_open(); });
    BOOST_CHECK(
            lifetime_test_access::command_deadline(client) ==
            std::chrono::steady_clock::time_point::max()
    );
    lifetime_test_access::check_deadlines(client, timeout);
    client.reset();
    ldt::poll_until(server.io_service(), [&] { return server.client_count() == 0; });
    const auto observer = connect();
    observer->send("TRY_LOCK interrupted_owner\nUNLOCK\n");
    BOOST_CHECK_EQUAL(observer->line(), "LOCK_ACQUIRED");
    BOOST_CHECK_EQUAL(observer->line(), "LOCK_RELEASED");
}


BOOST_FIXTURE_TEST_CASE(reply_backpressure_pauses_and_resumes_command_reads, ldt::network_fixture)
{
    // Hold a large reply in the bounded queue while commands arrive. One pending read may finish;
    // subsequent commands must wait for output to drain, then resume in order on this connection.
    const auto connection = connect(4096);
    const auto client = lifetime_test_access::connected_client(server, connection->endpoint());
    client->socket().set_option(boost::asio::socket_base::send_buffer_size(16 * 1024));
    client->socket().set_option(boost::asio::ip::tcp::no_delay(true));
    const auto replies = ldt::fill_unread_replies(client, server.io_service());
    const auto stale_expiry = lifetime_test_access::write_deadline(client);
    BOOST_REQUIRE_MESSAGE(
            lifetime_test_access::reads_paused(client), "High queue size pauses reads"
    );
    connection->send("VERSION\nTRY_LOCK after_backpressure\n");
    ldt::poll_until(server.io_service(), [&] {
        return !lifetime_test_access::read_pending(client);
    });
    BOOST_CHECK_MESSAGE(
            lifetime_test_access::reads_paused(client), "Unread output keeps reads paused"
    );
    BOOST_CHECK(lifetime_test_access::pending_reply_bytes(client) <= 128 * 1024);

    // Reading the queued payloads must free the queue and resume the buffered acquisition.
    const std::string expected(16 * 1024 - 1, 'x');
    for (std::size_t index = 0; index < replies; ++index)
        BOOST_REQUIRE_EQUAL(connection->line(), expected);
    BOOST_CHECK_MESSAGE(
            connection->line().find("lockd ") == 0, "The first command's reply is ordered"
    );
    BOOST_REQUIRE_EQUAL(connection->line(), "LOCK_ACQUIRED");
    connection->send("UNLOCK\n");
    BOOST_REQUIRE_EQUAL(connection->line(), "LOCK_RELEASED");
    ldt::poll_until(server.io_service(), [&] {
        return lifetime_test_access::pending_reply_bytes(client) == 0;
    });
    BOOST_CHECK_MESSAGE(
            !lifetime_test_access::reads_paused(client), "Drained output resumes reads"
    );
    BOOST_CHECK(
            lifetime_test_access::write_deadline(client) ==
            std::chrono::steady_clock::time_point::max()
    );
    lifetime_test_access::check_deadlines(client, stale_expiry);
    BOOST_CHECK_MESSAGE(
            client->socket().is_open(), "Maintenance after draining cannot close an idle connection"
    );
}


BOOST_AUTO_TEST_CASE(info_flood_stays_bounded_and_recovers_when_replies_are_read)
{
    // Send an INFO batch with reads paused by a known reply backlog. Small and large windows
    // must both drain the backlog, return every ordered reply, and leave the connection usable.
    // Unread INFO replies alone need not fill the kernel's buffers within any fixed request count.
    for (const int buffer_size : {4096, 1024 * 1024}) {
        BOOST_TEST_CONTEXT("Requested receive buffer: " << buffer_size)
        {
            ldt::network_fixture fixture;
            auto& server = fixture.server;
            const auto connection = fixture.connect(buffer_size);
            const auto client =
                    lifetime_test_access::connected_client(server, connection->endpoint());
            const int send_buffer_size = buffer_size == 4096 ? 16 * 1024 : buffer_size;
            client->socket().set_option(
                    boost::asio::socket_base::send_buffer_size(send_buffer_size)
            );
            client->socket().set_option(boost::asio::ip::tcp::no_delay(true));

            // Receiving the greeting does not guarantee that its write completion has run.
            // Start with an empty queue so the threshold assertions count only our backlog.
            ldt::poll_until(server.io_service(), [&] {
                return lifetime_test_access::pending_reply_bytes(client) == 0;
            });

            // Queue four 16 KiB replies without polling completions. Even if the kernel accepts
            // the active write immediately, Asio retains its storage until we run the handler.
            // This exercises the real pause threshold without trying to fill OS socket buffers.
            constexpr unsigned int backlog_replies = 4;
            const std::string payload(16 * 1024 - 1, 'x');
            for (unsigned int index = 0; index < backlog_replies; ++index) {
                BOOST_REQUIRE_MESSAGE(
                        !lifetime_test_access::reads_paused(client),
                        "Replies below the high-water mark leave reads enabled"
                );
                lifetime_test_access::send(client, payload + "\n");
            }
            BOOST_REQUIRE_EQUAL(lifetime_test_access::pending_reply_bytes(client), 64 * 1024);
            BOOST_REQUIRE_MESSAGE(
                    lifetime_test_access::reads_paused(client), "The reply backlog pauses reads"
            );

            // The request batch fits in the peer's send buffer while daemon reads are paused.
            // Draining output below must resume command reads, including commands already buffered
            // by the one read that was pending when the backlog reached the high-water mark.
            constexpr unsigned int requests = 1024;
            std::string commands;
            for (unsigned int index = 0; index < requests; ++index)
                commands += "INFO\n";
            connection->send(commands);

            // The injected replies and all INFO responses share the production write queue.
            // Check their wire order before proving that the same connection can acquire a lock.
            for (unsigned int index = 0; index < backlog_replies; ++index)
                BOOST_REQUIRE_EQUAL(connection->line(), payload);
            for (unsigned int index = 0; index < requests; ++index) {
                BOOST_TEST_CONTEXT("INFO reply " << index + 1 << " of " << requests)
                {
                    BOOST_REQUIRE_MESSAGE(
                            connection->line().find("you are client ") == 0,
                            "INFO begins with identity"
                    );
                    BOOST_REQUIRE_EQUAL(connection->line(), "lock status is none (lock=<none>)");
                    BOOST_REQUIRE_EQUAL(connection->line(), "current connected clients: 1");
                    BOOST_REQUIRE_MESSAGE(
                            lifetime_test_access::pending_reply_bytes(client) <= 128 * 1024,
                            "Retained reply bytes stay within the cap while reads resume"
                    );
                }
            }
            connection->send("TRY_LOCK after_info_flood\nUNLOCK\n");
            BOOST_CHECK_EQUAL(connection->line(), "LOCK_ACQUIRED");
            BOOST_CHECK_EQUAL(connection->line(), "LOCK_RELEASED");
            ldt::poll_until(server.io_service(), [&] {
                return lifetime_test_access::pending_reply_bytes(client) == 0;
            });
            BOOST_CHECK_MESSAGE(
                    !lifetime_test_access::reads_paused(client),
                    "Drained replies leave reads enabled"
            );
        }
    }
}


BOOST_FIXTURE_TEST_CASE(
        write_progress_renews_the_deadline_and_maintenance_closes_stalls, ldt::network_fixture
)
{
    // Enqueuing cannot renew a stalled write, but socket progress must advance its deadline.
    // Maintenance checks current state and leaves the in-flight buffer alive after closing
    // until the canceled write completes.
    const auto connection = connect(4096);
    auto client = lifetime_test_access::connected_client(server, connection->endpoint());
    client->socket().set_option(boost::asio::socket_base::send_buffer_size(16 * 1024));
    client->socket().set_option(boost::asio::ip::tcp::no_delay(true));
    BOOST_CHECK(
            lifetime_test_access::write_deadline(client) ==
            std::chrono::steady_clock::time_point::max()
    );
    const auto before = std::chrono::steady_clock::now();
    const std::string first_reply = std::string(16 * 1024 - 1, 'x') + "\n";
    lifetime_test_access::send(client, first_reply);
    const auto deadline = lifetime_test_access::write_deadline(client);
    BOOST_CHECK(deadline >= before + std::chrono::seconds(30));
    BOOST_CHECK(deadline <= std::chrono::steady_clock::now() + std::chrono::seconds(30));
    const auto old_expiry = lifetime_test_access::write_deadline(client);
    lifetime_test_access::send(client, "another queued reply\n");
    BOOST_CHECK(lifetime_test_access::write_deadline(client) == deadline);

    // Receiving the first reply forces real progress even if the following reply remains queued.
    // Socket buffer capacities do not guarantee that an entirely unread queue will drain.
    BOOST_REQUIRE_EQUAL(connection->line(), first_reply.substr(0, first_reply.size() - 1));
    ldt::fill_unread_replies(client, server.io_service());
    BOOST_CHECK(lifetime_test_access::write_deadline(client) >= deadline);
    lifetime_test_access::check_deadlines(client, old_expiry);
    lifetime_test_access::check_deadlines(client, old_expiry);
    BOOST_REQUIRE_MESSAGE(
            client->socket().is_open(),
            "Progress moves the current deadline beyond the earlier maintenance time"
    );
    lifetime_test_access::check_deadlines(
            client, lifetime_test_access::write_deadline(client) - std::chrono::nanoseconds(1)
    );
    BOOST_REQUIRE_MESSAGE(
            client->socket().is_open(), "A pre-deadline pass preserves pending output"
    );
    const auto pending = lifetime_test_access::pending_reply_bytes(client);
    const auto current_expiry = lifetime_test_access::write_deadline(client);
    lifetime_test_access::expire_write_progress(client);
    lifetime_test_access::check_deadlines(client, current_expiry);
    BOOST_CHECK_MESSAGE(!client->socket().is_open(), "No-progress expiry closes the socket");
    BOOST_CHECK_EQUAL(lifetime_test_access::pending_reply_bytes(client), pending);
    lifetime_test_access::send(client, "cannot enqueue after close\n");
    BOOST_CHECK_EQUAL(lifetime_test_access::pending_reply_bytes(client), pending);
    ldt::poll_until(server.io_service(), [&] {
        return lifetime_test_access::pending_reply_bytes(client) == 0;
    });
    client.reset();
    ldt::poll_until(server.io_service(), [&] { return server.client_count() == 0; });
}


BOOST_FIXTURE_TEST_CASE(
        stalled_writer_disconnect_releases_its_lock_to_a_waiter, ldt::network_fixture
)
{
    // Fill an owner's output while another client waits for its lock. Expiry must cancel both I/O
    // directions, release retained ownership, and hand off the lock without waiting for the peer.
    const auto owner = connect(4096);
    auto client = lifetime_test_access::connected_client(server, owner->endpoint());
    client->socket().set_option(boost::asio::socket_base::send_buffer_size(16 * 1024));
    client->socket().set_option(boost::asio::ip::tcp::no_delay(true));
    owner->send("TRY_LOCK stalled_writer\n");
    BOOST_REQUIRE_EQUAL(owner->line(), "LOCK_ACQUIRED");
    const auto waiter = connect();
    waiter->send("LOCK stalled_writer\n");
    BOOST_REQUIRE_EQUAL(waiter->line(), "LOCK_ACQUIRING");
    ldt::fill_unread_replies(client, server.io_service());
    lifetime_test_access::expire_write_progress(client);
    client.reset();
    ldt::poll_until(server.io_service(), [&] { return server.client_count() == 1; });
    BOOST_REQUIRE_EQUAL(waiter->line(), "LOCK_ACQUIRED");
    waiter->send("UNLOCK\n");
    BOOST_CHECK_EQUAL(waiter->line(), "LOCK_RELEASED");
}


BOOST_FIXTURE_TEST_CASE(
        peer_disconnect_during_replies_clears_the_write_deadline, ldt::network_fixture
)
{
    // Reset the peer while replies remain unwritten. Both error paths must clear the write
    // deadline and drain its buffers without waiting for a maintenance pass.
    const auto connection = connect(4096);
    auto client = lifetime_test_access::connected_client(server, connection->endpoint());
    client->socket().set_option(boost::asio::socket_base::send_buffer_size(16 * 1024));
    client->socket().set_option(boost::asio::ip::tcp::no_delay(true));
    ldt::fill_unread_replies(client, server.io_service());
    const auto old_expiry = lifetime_test_access::write_deadline(client);
    connection->close();
    ldt::poll_until(server.io_service(), [&] {
        return !client->socket().is_open() &&
               lifetime_test_access::pending_reply_bytes(client) == 0;
    });
    BOOST_CHECK(
            lifetime_test_access::write_deadline(client) ==
            std::chrono::steady_clock::time_point::max()
    );
    lifetime_test_access::check_deadlines(client, old_expiry);
    client.reset();
    ldt::poll_until(server.io_service(), [&] { return server.client_count() == 0; });
}


BOOST_FIXTURE_TEST_CASE(
        timeout_units_and_zero_wait_do_not_release_another_owner, ldt::network_fixture
)
{
    // Acquire using each supported time unit, then time out a zero-wait contender. Verify the
    // timeout leaves the competing owner intact and retry succeeds after release.
    const auto connection = connect();
    for (const auto duration : {"1", "1ms", "1s", "1m", "1h"}) {
        connection->send(std::string("TIMED_LOCK units ") + duration + "\nUNLOCK\n");
        BOOST_CHECK_EQUAL(connection->line(), "LOCK_ACQUIRING");
        BOOST_CHECK_EQUAL(connection->line(), "LOCK_ACQUIRED");
        BOOST_CHECK_EQUAL(connection->line(), "LOCK_RELEASED");
    }
    connection->send("TRY_LOCK units\n");
    BOOST_REQUIRE_EQUAL(connection->line(), "LOCK_ACQUIRED");
    const auto contender = connect();
    contender->send("TIMED_LOCK units 0ms\n");
    BOOST_CHECK_EQUAL(contender->line(), "LOCK_ACQUIRING");
    BOOST_CHECK_EQUAL(contender->line(), "LOCK_TIMED_OUT");
    contender->send("TRY_LOCK units\n");
    BOOST_CHECK_EQUAL(contender->line(), "LOCK_UNAVAILABLE");
    connection->send("UNLOCK\n");
    BOOST_CHECK_EQUAL(connection->line(), "LOCK_RELEASED");
    contender->send("TRY_LOCK units\nUNLOCK\n");
    BOOST_CHECK_EQUAL(contender->line(), "LOCK_ACQUIRED");
    BOOST_CHECK_EQUAL(contender->line(), "LOCK_RELEASED");
}


BOOST_FIXTURE_TEST_CASE(excessive_lock_waits_are_rejected_before_acquisition, ldt::network_fixture)
{
    // These counts fit even a 32-bit protocol integer, but their minutes/hours overflow a
    // nanosecond timer. Reject them for free and contended names, preserving the connection,
    // the existing owner, and the absence of any queued request.
    const auto owner = connect();
    const auto contender = connect();
    owner->send("TRY_LOCK excessive_held\n");
    BOOST_REQUIRE_EQUAL(owner->line(), "LOCK_ACQUIRED");
    for (const auto name : {"excessive_held", "excessive_free"})
        for (const auto duration : {"4294967295m", "4294967295h"}) {
            contender->send(std::string("TIMED_LOCK ") + name + " " + duration + "\n");
            BOOST_CHECK_EQUAL(contender->line(), "INVALID_ARGS");
        }
    contender->send("TRY_LOCK excessive_held\nTRY_LOCK excessive_free\nUNLOCK\n");
    BOOST_CHECK_EQUAL(contender->line(), "LOCK_UNAVAILABLE");
    BOOST_CHECK_EQUAL(contender->line(), "LOCK_ACQUIRED");
    BOOST_CHECK_EQUAL(contender->line(), "LOCK_RELEASED");
    owner->send("UNLOCK\n");
    BOOST_CHECK_EQUAL(owner->line(), "LOCK_RELEASED");
}


BOOST_FIXTURE_TEST_CASE(
        named_mutexes_exclude_other_processes_and_keep_other_names_independent, ldt::network_fixture
)
{
    // Launch a separate process against held and independent lock names. Verify exclusion
    // applies across processes only for the same name and ends after release.
    namespace bp = ldt::process;
    const auto owner = connect();
    owner->send("TRY_LOCK process-owner\n");
    BOOST_REQUIRE_EQUAL(owner->line(), "LOCK_ACQUIRED");
    const auto executable =
            std::filesystem::absolute(boost::unit_test::framework::master_test_suite().argv[0])
                    .string();
    const auto check_child = [&](const std::string& name, const bool available) {
        const auto port =
                std::to_string(lkd::detail::lifetime_test_access::endpoint(server).port());
        bp::child child(
                executable,
                "--child-process-contender",
                port,
                name,
                available ? "yes" : "no",
                bp::std_out > bp::null
        );
        try {
            ldt::poll_until(server.io_service(), [&] { return !child.running(); });
        }
        catch (...) {
            child.terminate();
            child.wait();
            throw;
        }
        child.wait();
        BOOST_CHECK_MESSAGE(
                child.exit_code() == 0,
                "Child acquisition of '" << name << "', available=" << available
        );
    };
    check_child("process-owner", false);
    check_child("independent-name", true);
    owner->send("UNLOCK\n");
    BOOST_CHECK_EQUAL(owner->line(), "LOCK_RELEASED");
    check_child("process-owner", true);
}


BOOST_AUTO_TEST_CASE(fragmented_greeting_and_reply_wait_for_their_suffixes)
{
    // Withhold greeting and acquisition-reply suffixes from a native client. Verify it remains
    // blocked until the complete line arrives, then acquires and releases successfully.
    for (const bool greeting : {true, false}) {
        std::promise<void> prefix_sent;
        std::promise<void> release_suffix;
        const auto release = release_suffix.get_future();
        scripted_peer peer([&](scripted_peer::tcp::socket& socket) {
            if (!greeting) {
                scripted_peer::send(socket, "LOCKD\n");
                scripted_peer::expect(socket, "TRY_LOCK framing");
            }
            scripted_peer::send(socket, greeting ? "LO" : "LOCK_");
            prefix_sent.set_value();
            if (release.wait_for(std::chrono::seconds(3)) != std::future_status::ready)
                throw std::runtime_error("Test did not release the reply suffix");
            scripted_peer::send(socket, greeting ? "CKD\r\n" : "ACQUIRED\r\n");
            if (!greeting) {
                scripted_peer::expect(socket, "UNLOCK");
                scripted_peer::send(socket, "LOCK_RELEASED\n");
            }
        });
        auto client = std::async(std::launch::async, [&] {
            lld::mutex mutex("127.0.0.1", peer.port(), std::chrono::seconds(1), "framing");
            if (!greeting) {
                const auto acquired = mutex.try_lock();
                mutex.unlock(lld::mutex::tt_throw_on_failure);
                return acquired;
            }
            return mutex.connected();
        });

        // A bounded negative observation is needed here: the peer deliberately cannot send a
        // complete line until this thread releases it, and the client must not return early.
        BOOST_CHECK(
                prefix_sent.get_future().wait_for(std::chrono::seconds(3)) ==
                std::future_status::ready
        );
        BOOST_CHECK(client.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout);
        release_suffix.set_value();
        BOOST_CHECK(client.get());
        peer.finish();
    }
}


BOOST_AUTO_TEST_CASE(hostname_resolution_connects_to_a_listening_endpoint)
{
    // Connect a native mutex to a scripted listener through localhost. Verify hostname
    // resolution produces a usable connection for acquisition and release.
    // localhost may resolve to both IPv6 and IPv4. The client must walk the resolver's
    // results until it reaches this IPv4 listener, then drain its canceled connect timer.
    scripted_peer peer([](scripted_peer::tcp::socket& socket) {
        scripted_peer::send(socket, "LOCKD\n");
        scripted_peer::expect(socket, "TRY_LOCK hostname");
        scripted_peer::send(socket, "LOCK_ACQUIRED\n");
        scripted_peer::expect(socket, "UNLOCK");
        scripted_peer::send(socket, "LOCK_RELEASED\n");
    });
    lld::mutex mutex("localhost", peer.port(), std::chrono::seconds(1), "hostname");
    BOOST_REQUIRE_MESSAGE(mutex.connected(), "A resolved hostname reaches the listener");
    BOOST_CHECK_MESSAGE(mutex.try_lock(), "The connection remains usable after resolution");
    mutex.unlock(lld::mutex::tt_throw_on_failure);
    peer.finish();
}


BOOST_AUTO_TEST_CASE(hostname_interface_binds_a_loopback_listener)
{
    // Start a server with localhost as its interface. Verify it binds a loopback address and
    // serves greeting, acquisition, and release requests.
    lkd::server server("localhost", 0, 1, lkd::rr_no_report_running);
    const auto endpoint = lkd::detail::lifetime_test_access::endpoint(server);
    BOOST_REQUIRE_MESSAGE(
            endpoint.address().is_loopback(),
            "localhost binds a loopback address rather than a wildcard"
    );
    BOOST_REQUIRE_MESSAGE(endpoint.port() != 0, "The hostname listener receives a port");
    ldt::protocol_connection connection(server.io_service(), endpoint);
    BOOST_CHECK_EQUAL(connection.line(), "LOCKD");
    connection.send("TRY_LOCK hostname_listener\n");
    BOOST_CHECK_EQUAL(connection.line(), "LOCK_ACQUIRED");
    connection.send("UNLOCK\n");
    BOOST_CHECK_EQUAL(connection.line(), "LOCK_RELEASED");
    connection.close();
    server.stop();
    server.io_service().run();
}


BOOST_FIXTURE_TEST_CASE(
        connection_identifiers_are_opaque_and_shared_by_info_and_logs, ldt::network_fixture
)
{
    // Connect, acquire, release, and disconnect using the real protocol. INFO must retain the
    // same non-address identifier, and lifecycle and lock logs must use exactly that value.
    // NOLINTNEXTLINE(misc-const-correctness)
    std::ostringstream output;
    const boost::io::basic_ios_rdbuf_saver<char> restore(std::clog, output.rdbuf());
    const auto original_level = lkd::LOG_LEVEL;
    lkd::LOG_LEVEL = lkd::ll_debug;
    std::string identifier;
    try {
        const auto peer = connect();
        auto client = lifetime_test_access::connected_client(server, peer->endpoint());
        BOOST_REQUIRE_MESSAGE(client, "The accepted connection has a daemon client");
        identifier = client->identifier();
        BOOST_REQUIRE_EQUAL(identifier.size(), 13u);
        BOOST_CHECK_EQUAL(identifier.substr(0, 2), "0x");
        BOOST_CHECK_MESSAGE(
                identifier.find_first_not_of("0123456789abcdef", 2) == std::string::npos,
                "The opaque identifier uses lowercase hexadecimal without UUID hyphens"
        );
        for (int request = 0; request < 2; ++request) {
            peer->send("INFO\n");
            BOOST_CHECK_EQUAL(peer->line(), "you are client " + identifier + " at 127.0.0.1");
            BOOST_CHECK_EQUAL(peer->line(), "lock status is none (lock=<none>)");
            BOOST_CHECK_EQUAL(peer->line(), "current connected clients: 1");
        }
        peer->send("TRY_LOCK identity\nINFO\nUNLOCK\n");
        BOOST_CHECK_EQUAL(peer->line(), "LOCK_ACQUIRED");
        BOOST_CHECK_EQUAL(peer->line(), "you are client " + identifier + " at 127.0.0.1");
        BOOST_CHECK_EQUAL(peer->line(), "lock status is locked (lock=identity)");
        BOOST_CHECK_EQUAL(peer->line(), "current connected clients: 1");
        BOOST_CHECK_EQUAL(peer->line(), "LOCK_RELEASED");
        peer->close();
        client.reset();
        ldt::poll_until(server.io_service(), [&] { return server.client_count() == 0; });
    }
    catch (...) {
        lkd::LOG_LEVEL = original_level;
        throw;
    }
    lkd::LOG_LEVEL = original_level;

    const auto logs = output.str();
    for (const auto& expected :
         {"Adding client " + identifier + " to client pool.",
          "Lock acquisition request from client " + identifier,
          "Lock with name 'identity' acquired for client " + identifier,
          "Lock release request from client " + identifier,
          "Lock with name 'identity' released from client " + identifier,
          "Removing client " + identifier + " from client pool."})
        BOOST_CHECK_MESSAGE(logs.find(expected) != std::string::npos, "Missing log: " << expected);
}


BOOST_FIXTURE_TEST_CASE(new_connections_receive_distinct_identifiers, ldt::network_fixture)
{
    // Close and reconnect repeatedly so allocator reuse cannot preserve a previous identity.
    // Every INFO reply must report a fresh diagnostic label after its client has been destroyed.
    std::set<std::string> identifiers;
    for (int connection = 0; connection < 32; ++connection) {
        const auto peer = connect();
        peer->send("INFO\n");
        const auto identity_line = peer->line();
        BOOST_CHECK_MESSAGE(
                identifiers.insert(identity_line).second, "Each connection has a new ID"
        );
        BOOST_CHECK_EQUAL(peer->line(), "lock status is none (lock=<none>)");
        BOOST_CHECK_EQUAL(peer->line(), "current connected clients: 1");
        peer->close();
        ldt::poll_until(server.io_service(), [&] { return server.client_count() == 0; });
    }
}


BOOST_AUTO_TEST_CASE(coalesced_info_replies_preserve_buffered_lines_and_later_reads)
{
    // Return the INFO response lines together from a scripted peer. Verify repeated reads
    // preserve buffered lines and parse the complete client information.
    scripted_peer peer([](scripted_peer::tcp::socket& socket) {
        scripted_peer::send(socket, "LOCKD\n");
        for (int i = 0; i < 2; ++i) {
            scripted_peer::expect(socket, "INFO");
            scripted_peer::send(
                    socket,
                    "you are client 0x123 at 127.0.0.1\r\n"
                    "lock status is none (lock=<none>)\r\ncurrent connected clients: 7\r\n"
            );
        }
    });
    lld::mutex mutex("127.0.0.1", peer.port(), std::chrono::seconds(1), "info");
    for (int i = 0; i < 2; ++i) {
        const auto info = mutex.info();
        BOOST_CHECK_EQUAL(info.client_address, "0x123");
        BOOST_CHECK_EQUAL(info.client_ip, "127.0.0.1");
        BOOST_CHECK_EQUAL(info.connected_clients, 7u);
        BOOST_CHECK(info.lock_status == lld::mutex::ls_none && !info.lock_name);
    }
    peer.finish();
}


BOOST_AUTO_TEST_CASE(disconnect_before_or_during_greeting_reports_a_read_error)
{
    // Close a scripted peer before its greeting or after a partial greeting. Verify mutex
    // construction reports a read error in both cases.
    for (const auto greeting : {"", "LO"}) {
        scripted_peer peer([greeting](scripted_peer::tcp::socket& socket) {
            scripted_peer::send(socket, greeting);
        });
        BOOST_CHECK_EXCEPTION(
                lld::mutex("127.0.0.1", peer.port(), std::chrono::seconds(1), "greeting"),
                lld::exception,
                [](const lld::exception& error) {
                    return error.error_type() == lld::exception::et_read_error;
                }
        );
        peer.finish();
    }
}


BOOST_AUTO_TEST_CASE(disconnect_during_reply_or_queued_acquisition_clears_connection_state)
{
    // Close a peer during a partial reply or after acknowledging a queued acquisition. Verify a
    // read error clears connection state and later acquisitions are rejected.
    for (const bool queued : {false, true}) {
        scripted_peer peer([queued](scripted_peer::tcp::socket& socket) {
            scripted_peer::send(socket, "LOCKD\n");
            scripted_peer::expect(socket, queued ? "LOCK disconnect" : "TRY_LOCK disconnect");
            scripted_peer::send(socket, queued ? "LOCK_ACQUIRING\n" : "LOCK_");
        });
        lld::mutex mutex("127.0.0.1", peer.port(), std::chrono::seconds(1), "disconnect");
        BOOST_CHECK_EXCEPTION(
                queued ? (mutex.lock(), true) : mutex.try_lock(),
                lld::exception,
                [](const lld::exception& error) {
                    return error.error_type() == lld::exception::et_read_error;
                }
        );
        BOOST_CHECK_MESSAGE(!mutex.connected(), "EOF makes this client unusable");
        const lld::detail::lockd_test_access<lld::mutex> access(&mutex);
        BOOST_CHECK_MESSAGE(access.peer_closed(), "Peer EOF is distinguished from a read timeout");
        BOOST_CHECK_EXCEPTION(mutex.try_lock(), lld::exception, [](const lld::exception& error) {
            return error.error_type() == lld::exception::et_not_connected;
        });
        peer.finish();
    }
}


BOOST_AUTO_TEST_CASE(failed_reply_framing_closes_an_owned_connection_before_destruction)
{
    // A peer can accept ownership and then send a reply that exceeds our bounded buffer.
    // Even though the TCP connection itself is still live, a failed read must close it now:
    // retaining an unusable socket until the mutex is destroyed could retain the remote lock.
    scripted_peer peer([](scripted_peer::tcp::socket& socket) {
        scripted_peer::send(socket, "LOCKD\n");
        scripted_peer::expect(socket, "TRY_LOCK failed_reply");
        scripted_peer::send(socket, "LOCK_ACQUIRED\n");
        scripted_peer::expect(socket, "INFO");
        scripted_peer::send(socket, std::string(lld::detail::reply_buffer_limit, 'x'));
        scripted_peer::expect_disconnect(socket);
    });
    lld::mutex mutex("127.0.0.1", peer.port(), std::chrono::seconds(1), "failed_reply");
    BOOST_REQUIRE(mutex.try_lock());
    BOOST_CHECK_EXCEPTION(mutex.info(), lld::exception, [](const lld::exception& error) {
        return error.error_type() == lld::exception::et_read_error;
    });
    BOOST_CHECK(!mutex.connected());
    BOOST_CHECK_EXCEPTION(mutex.try_lock(), lld::exception, [](const lld::exception& error) {
        return error.error_type() == lld::exception::et_not_connected;
    });

    // Join while the client is still alive, so destructor cleanup cannot satisfy the check.
    BOOST_CHECK_NO_THROW(peer.finish());
}


BOOST_AUTO_TEST_CASE(failed_socket_write_closes_the_unusable_connection)
{
    // Disable the client's send direction without marking it disconnected. This forces the
    // next write to fail without relying on a peer reset arriving before a large write finishes:
    // accepting bytes into a kernel buffer does not prove that the peer has received them.
    scripted_peer peer([](scripted_peer::tcp::socket& socket) {
        scripted_peer::send(socket, "LOCKD\n");
        scripted_peer::expect_disconnect(socket);
    });
    lld::mutex mutex("127.0.0.1", peer.port(), std::chrono::seconds(1), "failed_write");
    const lld::detail::lockd_test_access<lld::mutex> access(&mutex);
    access.shutdown_send();
    BOOST_REQUIRE_MESSAGE(mutex.connected(), "No client I/O error has been recorded yet");
    BOOST_CHECK_EXCEPTION(
            access.blocking_write("TRY_LOCK failed_write\n"),
            lld::exception,
            [](const lld::exception& error) {
                return error.error_type() == lld::exception::et_write_error;
            }
    );
    BOOST_CHECK(!mutex.connected());
    BOOST_CHECK_MESSAGE(!access.socket_open(), "Write failure closes the local socket");
    BOOST_CHECK_EXCEPTION(mutex.try_lock(), lld::exception, [](const lld::exception& error) {
        return error.error_type() == lld::exception::et_not_connected;
    });
    peer.finish();
}


BOOST_AUTO_TEST_CASE(read_timeout_closes_the_connection_and_drains_the_canceled_read)
{
    // Keep a peer connected without completing the requested read. Verify the local timeout
    // closes the client and is distinguished from peer EOF or reset.
    std::promise<void> read_finished;
    const auto read_completion = read_finished.get_future().share();
    scripted_peer peer([read_completion](scripted_peer::tcp::socket& socket) {
        scripted_peer::send(socket, "LOCKD\n");

        // Keep the peer open while the client exercises its own timeout. Starting the
        // peer's three-second disconnect deadline here races a delayed test thread.
        if (read_completion.wait_for(std::chrono::seconds(30)) != std::future_status::ready)
            throw std::runtime_error("Client read did not finish before the test watchdog");
        scripted_peer::expect_disconnect(socket);
    });
    lld::mutex mutex("127.0.0.1", peer.port(), std::chrono::seconds(1), "timeout");
    const lld::detail::lockd_test_access<lld::mutex> access(&mutex);
    BOOST_CHECK_MESSAGE(!access.peer_closed(), "A connected peer has not reported EOF or reset");
    BOOST_CHECK_EXCEPTION(
            static_cast<void>(access.blocking_read_line(std::chrono::seconds(1))),
            lld::exception,
            [](const lld::exception& error) {
                return error.error_type() == lld::exception::et_read_error &&
                       std::string(error.what()).find("timeout") != std::string::npos;
            }
    );
    read_finished.set_value();
    BOOST_CHECK(!mutex.connected());
    BOOST_CHECK_MESSAGE(!access.peer_closed(), "A local read timeout is not a peer disconnect");
    BOOST_CHECK_EXCEPTION(mutex.try_lock(), lld::exception, [](const lld::exception& error) {
        return error.error_type() == lld::exception::et_not_connected;
    });
    peer.finish();
}


BOOST_AUTO_TEST_SUITE_END()

} // namespace

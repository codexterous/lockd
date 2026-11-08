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
 * File       : lifetime_test.cpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Test daemon shutdown, client lifetimes, and lock collection.
 * Start Date : 10/19/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <chrono>
#include <exception>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/io/ios_state.hpp>
#include <boost/system/error_code.hpp> // IWYU pragma: keep
#include <boost/test/framework.hpp>
#include <boost/test/tools/context.hpp>
#include <boost/test/tools/interface.hpp> // IWYU pragma: keep
#include <boost/test/unit_test.hpp>       // IWYU pragma: keep
#include <boost/test/unit_test_suite.hpp>

#ifndef WIN32
# include <thread>

# include <signal.h> // NOLINT(modernize-deprecated-headers): POSIX kill, outside <csignal>.

#else
# include <boost/system/system_error.hpp>

# include <winsock2.h>
#endif

#include <liblockd/mutex.hpp>

#include "../../../include/client.hpp"
#include "../../../include/lock_store.hpp"
#include "../../../include/mutex.hpp"
#include "../../../include/new_shared_ptr.hpp"
#ifdef WIN32
# include "../../../include/lockd.hpp"
# include "../../../include/server.hpp"
#endif
#include "../include/daemon_fixture.hpp"
#include "../include/launcher.hpp"

// Selects the supported Process v1 API across Boost versions.
#include "../include/process.hpp" // IWYU pragma: keep

using lkd::detail::lifetime_test_access;


namespace {

BOOST_AUTO_TEST_SUITE(lifetimes)


#ifndef WIN32
BOOST_AUTO_TEST_CASE(signals_drain_lock_holders_and_exit_successfully)
{
    // Send SIGINT and SIGTERM to a real daemon, with and without a held lock. Verify that
    // shutdown waits for release, exits successfully, and removes its PID file.
    namespace bp = ldt::process;
    using boost::asio::ip::tcp;
    const auto daemon_path = ldt::daemon_path().string();
    for (const auto signal : {SIGINT, SIGTERM}) {
        for (const bool hold_lock : {false, true}) {
            BOOST_TEST_CONTEXT("signal=" << signal << ", holding lock=" << hold_lock)
            {
                boost::asio::io_context service;
                tcp::acceptor port_reservation(
                        service, tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0)
                );
                const auto endpoint = port_reservation.local_endpoint();
                port_reservation.close();
                const auto artifact_base = std::filesystem::path(daemon_path).parent_path() /
                                           ("signal_" + std::to_string(endpoint.port()));
                const auto pid_file = artifact_base.string() + ".pid";
                const auto log_file = artifact_base.string() + ".log";
                bp::child daemon(
                        daemon_path,
                        "-l",
                        "info",
                        "-d",
                        "-n",
                        "-i",
                        "127.0.0.1",
                        "-p",
                        std::to_string(endpoint.port()),
                        "-t",
                        "2",
                        "--pid-file",
                        pid_file,
                        "--log-file",
                        log_file,
                        bp::std_out > bp::null,
                        bp::std_err > bp::null
                );
                try {
                    ldt::poll_until(service, [&] {
                        tcp::socket probe(service);
                        boost::system::error_code error;

                        // NOLINTNEXTLINE(bugprone-unused-return-value)
                        static_cast<void>(probe.connect(endpoint, error));
                        return !error;
                    });
                    lld::mutex mutex(
                            "127.0.0.1", endpoint.port(), std::chrono::seconds(1), "signal-holder"
                    );
                    if (hold_lock)
                        mutex.lock();
                    BOOST_CHECK_MESSAGE(
                            std::filesystem::exists(pid_file), "Running daemon owns its PID file"
                    );
                    BOOST_REQUIRE_EQUAL(::kill(daemon.id(), signal), 0);

                    // Refusal establishes that shutdown has begun before we release our lock.
                    ldt::poll_until(service, [&] {
                        tcp::socket probe(service);
                        boost::system::error_code error;

                        // NOLINTNEXTLINE(bugprone-unused-return-value)
                        static_cast<void>(probe.connect(endpoint, error));
                        return !!error;
                    });
                    if (hold_lock) {
                        BOOST_CHECK_MESSAGE(daemon.running(), "Signal waits for our lock release");
                        BOOST_CHECK_NO_THROW(mutex.unlock(lld::mutex::tt_throw_on_failure));
                    }
                    const auto deadline =
                            std::chrono::steady_clock::now() + std::chrono::seconds(3);
                    while (daemon.running() && std::chrono::steady_clock::now() < deadline)
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    const bool exited = !daemon.running();
                    BOOST_CHECK_MESSAGE(exited, "Daemon exits promptly after its holders finish");
                    if (!exited) {
                        daemon.terminate();
                        daemon.wait();
                    }
                    BOOST_CHECK_EQUAL(daemon.exit_code(), 0);
                    BOOST_CHECK_MESSAGE(
                            !std::filesystem::exists(pid_file),
                            "Graceful shutdown removes its PID file"
                    );
                }
                catch (...) {
                    if (daemon.running())
                        daemon.terminate();
                    daemon.wait();
                    std::filesystem::remove(pid_file);
                    throw;
                }
            }
        }
    }
}
#endif


BOOST_AUTO_TEST_CASE(launcher_termination_joins_and_releases_the_listening_port)
{
    // Repeat launch, termination, and join on the same port. Verify the child exits with the
    // expected status and releases its listener for the next launch.
    using boost::asio::ip::tcp;
    boost::asio::io_context service;
    tcp::acceptor acceptor(service, tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0));
    const auto endpoint = acceptor.local_endpoint();
    const auto daemon_path = ldt::daemon_path().string();
    acceptor.close();

    // Repeat on the same port to catch an unreaped child or a listener left behind. The real
    // launcher waits for --emit-running before returning, so termination cannot race startup.
    for (const auto attempt : {1, 2, 3}) {
        BOOST_TEST_CONTEXT("launch/terminate/join cycle " << attempt)
        {
            ldt::launcher launcher(daemon_path, "127.0.0.1", endpoint.port(), 1);
            BOOST_REQUIRE_MESSAGE(launcher.launch(), "Daemon reaches its running state");
            launcher.terminate();
            launcher.terminate();
            BOOST_CHECK_NO_THROW(launcher.join());
            BOOST_CHECK_NO_THROW(launcher.join());

#ifndef WIN32
            // Boost.Process terminate() sends SIGKILL on POSIX. Check the signal so a child
            // abort cannot pass unnoticed just because the parent test process survived.
            BOOST_CHECK_MESSAGE(
                    launcher.exit_code() == SIGKILL,
                    "Daemon is killed without aborting: " << launcher.exit_code()
            );
#endif
            boost::system::error_code error;
            acceptor.open(tcp::v4());

            // NOLINTNEXTLINE(bugprone-unused-return-value)
            static_cast<void>(acceptor.bind(endpoint, error));
            BOOST_CHECK_MESSAGE(!error, "Terminated daemon releases its port: " << error.message());
            acceptor.close();
        }
    }
}


BOOST_AUTO_TEST_CASE(launcher_reports_spawn_failure_without_aborting)
{
    // An executable cannot also be a directory. This fails without relying on a free port
    // or on the contents of PATH, and the worker must return its exception to this thread.
    const auto invalid_path = (ldt::daemon_path() / "not_an_executable").string();
    ldt::launcher launcher(invalid_path, "127.0.0.1", 0, 1);
    for (const auto attempt : {1, 2}) {
        BOOST_TEST_CONTEXT("failed launch " << attempt)
        {
            BOOST_CHECK_THROW(launcher.launch(), ldt::process::process_error);
            BOOST_CHECK_NO_THROW(launcher.join());
        }
    }
}


BOOST_AUTO_TEST_CASE(launcher_reports_an_occupied_port_and_can_retry_after_release)
{
    // Own the endpoint independently of lockd, first as a bound socket and then as
    // a listener. Windows requires exclusive binding for an unrelated application to
    // refuse SO_REUSEADDR; POSIX's ordinary non-reusable binding already refuses it.
    using boost::asio::ip::tcp;
    for (const bool listening : {false, true}) {
        BOOST_TEST_CONTEXT("occupied endpoint listening=" << listening)
        {
            boost::asio::io_context service;
            tcp::acceptor reservation(service, tcp::v4());
#ifdef WIN32
            const int exclusive = 1;
            BOOST_REQUIRE_MESSAGE(
                    ::setsockopt(
                            reservation.native_handle(),
                            SOL_SOCKET,
                            SO_EXCLUSIVEADDRUSE,
                            reinterpret_cast<const char*>(&exclusive),
                            sizeof(exclusive)
                    ) != SOCKET_ERROR,
                    "Reserve the endpoint exclusively before binding"
            );
#else
            reservation.set_option(tcp::acceptor::reuse_address(false));
#endif
            reservation.bind({boost::asio::ip::address_v4::loopback(), 0});
            const auto endpoint = reservation.local_endpoint();
            if (listening)
                reservation.listen();
            ldt::launcher launcher(ldt::daemon_path().string(), "127.0.0.1", endpoint.port(), 1);
            bool rejected = false;
            // The launcher's expected setup diagnostic writes through this stream's buffer.
            // NOLINTNEXTLINE(misc-const-correctness)
            std::ostringstream output;
            {
                const boost::io::basic_ios_rdbuf_saver<char> restore(std::cout, output.rdbuf());
                try {
                    static_cast<void>(launcher.launch());
                }
                catch (const boost::unit_test::framework::setup_error&) {
                    rejected = true;
                }
            }
            // Clean up even if startup unexpectedly succeeded, before asserting its result.
            launcher.terminate();
            BOOST_CHECK_NO_THROW(launcher.join());
            BOOST_REQUIRE_MESSAGE(
                    rejected, "The occupied endpoint rejects startup: " << output.str()
            );
            BOOST_CHECK(launcher.run_status() == ldt::rs_not_successful);
            BOOST_CHECK(launcher.exit_code() != 0);
            BOOST_CHECK(launcher.port() == endpoint.port());
            BOOST_CHECK(reservation.is_open());

            // A failed startup must release all of its resources and retain its configured
            // port. Retrying the same launcher succeeds once the test gives up the socket.
            reservation.close();
            BOOST_REQUIRE_MESSAGE(launcher.launch(), "The released endpoint can start normally");
            launcher.terminate();
            BOOST_CHECK_NO_THROW(launcher.join());
        }
    }
}


BOOST_AUTO_TEST_CASE(daemon_rejects_a_duplicate_listener_and_restarts_with_an_old_peer_open)
{
    // The first daemon and its replacement deliberately reuse the same port. Keep an old
    // client socket open across forced termination, so a successful restart cannot depend
    // on that peer closing or on graceful daemon shutdown finishing its TCP connections.
    using boost::asio::ip::tcp;
    boost::asio::io_context service;
    tcp::acceptor reservation(service, {boost::asio::ip::address_v4::loopback(), 0});
    const auto endpoint = reservation.local_endpoint();
    reservation.close();
    ldt::launcher first(ldt::daemon_path().string(), "127.0.0.1", endpoint.port(), 1);
    ldt::launcher contender(ldt::daemon_path().string(), "127.0.0.1", endpoint.port(), 1);
    const auto stop_children = [&] {
        first.terminate();
        contender.terminate();
        std::exception_ptr error;
        for (auto* const child : {&first, &contender}) {
            try {
                child->join();
            }
            catch (...) {
                if (!error)
                    error = std::current_exception();
            }
        }
        if (error)
            std::rethrow_exception(error);
    };
    bool rejected = false;
    try {
        static_cast<void>(first.launch());
        lld::mutex old_peer("127.0.0.1", endpoint.port(), std::chrono::seconds(3), "old_peer");
        // Capture only the expected duplicate-startup diagnostic, not any test assertions.
        // NOLINTNEXTLINE(misc-const-correctness)
        std::ostringstream output;
        {
            const boost::io::basic_ios_rdbuf_saver<char> restore(std::cout, output.rdbuf());
            try {
                static_cast<void>(contender.launch());
            }
            catch (const boost::unit_test::framework::setup_error&) {
                rejected = true;
            }
        }
        contender.terminate();
        contender.join();
        static_cast<void>(old_peer.info());
        first.terminate();
        first.join();

        static_cast<void>(first.launch());
        lld::mutex new_peer("127.0.0.1", endpoint.port(), std::chrono::seconds(3), "new_peer");
        new_peer.lock();
        new_peer.unlock(lld::mutex::tt_throw_on_failure);
        stop_children();
    }
    catch (...) {
        stop_children();
        throw;
    }
    BOOST_CHECK_MESSAGE(rejected, "A second daemon must not share the live listener");
}


#ifdef WIN32
BOOST_AUTO_TEST_CASE(windows_port_lease_covers_local_addresses_and_ephemeral_listeners)
{
    // Port zero must reserve its actual allocated port. The reservation is deliberately
    // machine-wide, so neither a different loopback address nor a wildcard bypasses it.
    lkd::server owner("127.0.0.1", 0, 1, lkd::rr_no_report_running);
    const auto endpoint = lifetime_test_access::endpoint(owner);
    BOOST_REQUIRE(endpoint.port() != 0);
    for (const std::string address : {"127.0.0.1", "127.0.0.2", "0.0.0.0"}) {
        BOOST_CHECK_EXCEPTION(
                static_cast<void>(
                        lkd::server(address, endpoint.port(), 1, lkd::rr_no_report_running)
                ),
                boost::system::system_error,
                [](const boost::system::system_error& error) {
                    return error.code() == boost::asio::error::address_in_use;
                }
        );
    }
    // A completed shutdown releases the port even if the stopped server object still exists.
    owner.stop();
    owner.io_service().poll();
    BOOST_CHECK(owner.stopping());
    BOOST_CHECK_NO_THROW(
            static_cast<void>(
                    lkd::server("127.0.0.2", endpoint.port(), 1, lkd::rr_no_report_running)
            )
    );
}
#endif


BOOST_AUTO_TEST_CASE(collection_preserves_an_outstanding_lookup)
{
    // Collect a named mutex between lookup and acquisition, then after release. Verify an
    // outstanding reference or held lock prevents replacement, while unused state can be
    // collected.
    boost::asio::io_context service;
    lkd::lock_store store(service);
    auto mutex = store.get_mutex("lookup");
    const std::weak_ptr<lkd::mutex> original(mutex);

    // Collection between lookup and try_lock must neither destroy the object nor let another
    // lookup create an independent mutex for the same name.
    BOOST_CHECK(!lifetime_test_access::collect(store, "lookup"));
    BOOST_CHECK(store.get_mutex("lookup") == mutex);
    BOOST_REQUIRE(mutex->try_lock());
    mutex.reset();
    BOOST_CHECK(!lifetime_test_access::collect(store, "lookup"));
    store.get_mutex("lookup")->unlock();
    BOOST_CHECK(lifetime_test_access::collect(store, "lookup"));
    BOOST_CHECK(original.expired());
}


BOOST_FIXTURE_TEST_CASE(
        shutdown_rejects_a_queued_grant_even_if_the_client_is_still_retained, ldt::server_fixture
)
{
    // Retain a queued client across server shutdown and unlock its mutex. Verify the late
    // handoff sends no grant and leaves the mutex available.
    const auto mutex = server.lock_store().get_mutex("late-grant");
    auto waiter = lkd::new_ptr<lkd::client>(server);
    BOOST_REQUIRE(mutex->try_lock());
    lifetime_test_access::wait(mutex, waiter, std::chrono::seconds(60));
    server.stop();
    ldt::poll_until(server.io_service(), [&] { return server.stopping(); });

    // Retaining this client simulates an in-flight callback after shutdown has begun. Queue
    // handoff must skip it rather than leaving the mutex owned without an acquisition reply.
    mutex->unlock();
    BOOST_CHECK_EQUAL(lifetime_test_access::replies(waiter, "LOCK_ACQUIRED\r\n"), 0);
    BOOST_REQUIRE_MESSAGE(mutex->try_lock(), "A refused grant does not strand mutex ownership");
    mutex->unlock();
    waiter.reset();
}


BOOST_FIXTURE_TEST_CASE(
        shutdown_releases_an_immediate_acquisition_that_raced_the_stop, ldt::server_fixture
)
{
    // Enter each acquisition path after shutdown has begun. Verify a refused grant reports
    // failure and undoes immediate mutex ownership.
    const auto client = lkd::new_ptr<lkd::client>(server);
    const auto mutex = server.lock_store().get_mutex("raced-stop");
    server.stop();
    ldt::poll_until(server.io_service(), [&] { return server.stopping(); });

    // Enter after the command's initial shutdown check, with an unlocked mutex. Each acquisition
    // path must undo ownership if shutdown wins before the grant is delivered to the client.
    for (const auto acquire :
         {lifetime_test_access::try_lock,
          lifetime_test_access::lock,
          lifetime_test_access::timed_lock}) {
        BOOST_CHECK_MESSAGE(
                !acquire(client, "raced-stop"), "A refused grant reports no acquisition"
        );
        BOOST_CHECK_MESSAGE(!mutex->locked(), "A refused grant releases immediate ownership");
    }
    server.io_service().restart();
    server.io_service().poll();
}


BOOST_FIXTURE_TEST_CASE(stale_timeout_cannot_cancel_a_later_request, ldt::server_fixture)
{
    // Save an expiry callback, complete its acquisition, then queue a new request on the same
    // client. Deliver the old callback and verify it cannot time out the new request.
    const auto mutex = server.lock_store().get_mutex("requests");
    auto client = lkd::new_ptr<lkd::client>(server);
    BOOST_REQUIRE(mutex->try_lock());
    lifetime_test_access::wait(mutex, client, std::chrono::seconds(60));
    const auto stale_timeout = lifetime_test_access::late_timeout(mutex);

    mutex->unlock();
    BOOST_REQUIRE(lifetime_test_access::status(client) == lld::mutex::ls_locked);
    BOOST_CHECK_EQUAL(lifetime_test_access::replies(client, "LOCK_ACQUIRED\r\n"), 1);
    lifetime_test_access::release(client);
    BOOST_REQUIRE(mutex->try_lock());
    lifetime_test_access::wait(mutex, client, std::chrono::seconds(60));

    // A successful expiry may already have been dispatched when cancellation happens. Replay it
    // twice, after the same client has queued a new acquisition, without relying on clock delays.
    stale_timeout();
    stale_timeout();
    BOOST_CHECK_EQUAL(lifetime_test_access::replies(client, "LOCK_TIMED_OUT\r\n"), 0);
    BOOST_CHECK(lifetime_test_access::status(client) == lld::mutex::ls_acquiring);
    mutex->unlock();
    BOOST_REQUIRE(lifetime_test_access::status(client) == lld::mutex::ls_locked);
    lifetime_test_access::release(client);
    client.reset();
    server.io_service().poll();
}


BOOST_FIXTURE_TEST_CASE(expiry_before_unlock_completes_a_request_only_once, ldt::server_fixture)
{
    // Expire the front waiter, unlock to its successor, and replay expiry. Verify exactly one
    // timeout for the expired request and exactly one grant for the survivor.
    const auto mutex = server.lock_store().get_mutex("expiry-order");
    auto expired = lkd::new_ptr<lkd::client>(server);
    auto survivor = lkd::new_ptr<lkd::client>(server);
    BOOST_REQUIRE(mutex->try_lock());
    lifetime_test_access::wait(mutex, expired, std::chrono::seconds(60));
    const auto expiry = lifetime_test_access::late_timeout(mutex);
    lifetime_test_access::wait(mutex, survivor, std::chrono::seconds(60));

    // Deliver both sides of the race in a known order, including a duplicate expiry afterward.
    expiry();
    mutex->unlock();
    expiry();
    BOOST_CHECK(lifetime_test_access::status(expired) == lld::mutex::ls_none);
    BOOST_CHECK(lifetime_test_access::status(survivor) == lld::mutex::ls_locked);
    BOOST_CHECK_EQUAL(lifetime_test_access::replies(expired, "LOCK_TIMED_OUT\r\n"), 1);
    BOOST_CHECK_EQUAL(lifetime_test_access::replies(expired, "LOCK_ACQUIRED\r\n"), 0);
    BOOST_CHECK_EQUAL(lifetime_test_access::replies(survivor, "LOCK_ACQUIRED\r\n"), 1);
    lifetime_test_access::release(survivor);
    expired.reset();
    survivor.reset();
    server.io_service().poll();
}


BOOST_FIXTURE_TEST_CASE(
        repeated_accept_failures_recover_and_pending_retry_respects_close, ldt::server_fixture
)
{
    // Inject repeated accept failures and drive retry callbacks explicitly. Verify recovery
    // accepts a real connection and a pending retry cannot reopen a closed listener.
    using boost::asio::ip::tcp;
    auto endpoint = lifetime_test_access::reopen_listener(server);
    for (int attempt = 0; attempt < 3; ++attempt) {
        lifetime_test_access::fail_accept(server, boost::asio::error::no_descriptors);
        BOOST_CHECK_EQUAL(server.client_count(), 0);

        // Deliver repeated failures without waiting for the OS clock. The final successful retry
        // below must still arm a real accept, after the canceled timer events have been drained.
        BOOST_CHECK_EQUAL(lifetime_test_access::cancel_accept_retry(server), 1);
        lifetime_test_access::retry_accept(server, boost::asio::error::operation_aborted);
        server.io_service().poll();
    }
    lifetime_test_access::fail_accept(server, boost::asio::error::connection_aborted);
    lifetime_test_access::expire_accept_retry(server);
    tcp::socket peer(server.io_service());
    peer.connect(endpoint);
    ldt::poll_until(server.io_service(), [&] { return server.client_count() == 1; });
    peer.close();
    lifetime_test_access::close_listener(server);
    ldt::poll_until(server.io_service(), [&] { return server.client_count() == 0; });

    endpoint = lifetime_test_access::reopen_listener(server);
    lifetime_test_access::fail_accept(server, boost::asio::error::no_descriptors);
    lifetime_test_access::close_listener(server);
    BOOST_CHECK_EQUAL(lifetime_test_access::cancel_accept_retry(server), 1);

    // Even a successful timer event already dispatched before close has no authority to accept.
    lifetime_test_access::retry_accept(server, {});
    server.io_service().poll();
    BOOST_CHECK_EQUAL(lifetime_test_access::cancel_accept_retry(server), 0);
    BOOST_CHECK_EQUAL(server.client_count(), 0);
}


BOOST_FIXTURE_TEST_CASE(disconnect_removes_waiters_and_cancels_their_timers, ldt::network_fixture)
{
    // Disconnect a middle waiter while the owner keeps its lock. Check immediate queue and
    // timer cleanup, live waiters' order, and that an unrelated idle connection stays usable.
    for (const bool timed : {false, true}) {
        BOOST_TEST_CONTEXT("timed waiter=" << timed)
        {
            const auto owner = connect();
            const auto idle = connect();
            owner->send("LOCK disconnect-queue\n");
            BOOST_REQUIRE_EQUAL(owner->line(), "LOCK_ACQUIRING");
            BOOST_REQUIRE_EQUAL(owner->line(), "LOCK_ACQUIRED");
            const auto first = connect();
            const auto removed = connect();
            const auto last = connect();
            first->send("LOCK disconnect-queue\n");
            BOOST_REQUIRE_EQUAL(first->line(), "LOCK_ACQUIRING");
            removed->send(
                    timed ? "TIMED_LOCK disconnect-queue 3600s\n" : "LOCK disconnect-queue\n"
            );
            BOOST_REQUIRE_EQUAL(removed->line(), "LOCK_ACQUIRING");
            last->send("LOCK disconnect-queue\n");
            BOOST_REQUIRE_EQUAL(last->line(), "LOCK_ACQUIRING");
            const auto mutex = server.lock_store().get_mutex("disconnect-queue");
            BOOST_REQUIRE_EQUAL(lifetime_test_access::queued_requests(mutex), 3);
            const auto timer = lifetime_test_access::request_timer(mutex, 1);
            auto stale_timeout = lifetime_test_access::late_timeout(mutex, 1);

            // Keep the server-side client alive to prove cleanup follows EOF, not destruction.
            const auto removed_client =
                    lifetime_test_access::connected_client(server, removed->endpoint());
            removed->close();
            ldt::poll_until(server.io_service(), [&] {
                return lifetime_test_access::queued_requests(mutex) == 2;
            });
            BOOST_CHECK_MESSAGE(mutex->locked(), "Removing a waiter preserves the owner's lock");
            BOOST_CHECK_MESSAGE(!removed_client->socket().is_open(), "EOF closes the waiter");
            if (timed) {
                const auto pending_timer = timer.lock();
                BOOST_REQUIRE_MESSAGE(pending_timer, "Saved expiry retains the request's timer");
                BOOST_CHECK_EQUAL(pending_timer->cancel(), 0);
            }

            // A successful expiry can already be dispatched when cancellation happens. Replaying
            // it must not remove a survivor; canceled callbacks must also release timer storage.
            stale_timeout();
            stale_timeout();
            stale_timeout = {};
            ldt::poll_until(server.io_service(), [&] { return timer.expired(); });
            removed_client->shutdown(true);
            BOOST_CHECK_EQUAL(lifetime_test_access::queued_requests(mutex), 2);
            idle->send("VERSION\n");
            BOOST_CHECK_MESSAGE(idle->line().find("lockd ") == 0, "Idle connection remains usable");

            owner->send("UNLOCK\n");
            BOOST_REQUIRE_EQUAL(owner->line(), "LOCK_RELEASED");
            BOOST_REQUIRE_EQUAL(first->line(), "LOCK_ACQUIRED");

            // A waiter that already received ownership must still release it on disconnect.
            first->close();
            BOOST_REQUIRE_EQUAL(last->line(), "LOCK_ACQUIRED");
            last->send("UNLOCK\n");
            BOOST_REQUIRE_EQUAL(last->line(), "LOCK_RELEASED");
            BOOST_CHECK_MESSAGE(!mutex->locked(), "The final owner releases the mutex");
            BOOST_CHECK_EQUAL(lifetime_test_access::queued_requests(mutex), 0);
        }
    }
}


BOOST_FIXTURE_TEST_CASE(handoff_can_precede_disconnected_waiter_cleanup, ldt::network_fixture)
{
    // Close a queued client, then release its owner's lock before deferred cleanup runs. Handoff
    // must skip the disconnected request and grant its successor exactly once.
    const auto owner = connect();
    const auto removed = connect();
    const auto survivor = connect();
    owner->send("LOCK disconnect-handoff\n");
    BOOST_REQUIRE_EQUAL(owner->line(), "LOCK_ACQUIRING");
    BOOST_REQUIRE_EQUAL(owner->line(), "LOCK_ACQUIRED");
    removed->send("TIMED_LOCK disconnect-handoff 3600s\n");
    BOOST_REQUIRE_EQUAL(removed->line(), "LOCK_ACQUIRING");
    survivor->send("LOCK disconnect-handoff\n");
    BOOST_REQUIRE_EQUAL(survivor->line(), "LOCK_ACQUIRING");
    const auto mutex = server.lock_store().get_mutex("disconnect-handoff");
    const auto removed_client = lifetime_test_access::connected_client(server, removed->endpoint());
    const auto owner_client = lifetime_test_access::connected_client(server, owner->endpoint());
    const auto stale_timeout = lifetime_test_access::late_timeout(mutex);
    removed_client->shutdown(true);
    lifetime_test_access::release(owner_client);
    BOOST_REQUIRE_EQUAL(lifetime_test_access::queued_requests(mutex), 0);
    stale_timeout();
    BOOST_REQUIRE_EQUAL(survivor->line(), "LOCK_ACQUIRED");
    survivor->send("UNLOCK\n");
    BOOST_REQUIRE_EQUAL(survivor->line(), "LOCK_RELEASED");
    BOOST_CHECK_MESSAGE(!mutex->locked(), "Deferred cleanup leaves no phantom ownership");
}


BOOST_FIXTURE_TEST_CASE(successful_timed_request_does_not_retain_its_client, ldt::server_fixture)
{
    // Grant a timed request, drain writes, and drop the client. Verify the canceled timer does
    // not retain the client or postpone release of its lock.
    const auto mutex = server.lock_store().get_mutex("disconnect");
    auto client = lkd::new_ptr<lkd::client>(server);
    const lkd::weak_client_ptr weak_client(client);
    BOOST_REQUIRE(mutex->try_lock());
    lifetime_test_access::wait(mutex, client, std::chrono::seconds(60));
    mutex->unlock();

    // Finish the write callbacks first so that only the caller and the canceled timer could keep
    // this client alive. Its destruction must release the acquired lock now, not in sixty seconds.
    server.io_service().poll();
    client.reset();
    BOOST_CHECK(weak_client.expired());
    BOOST_REQUIRE(mutex->try_lock());
    mutex->unlock();
}


BOOST_FIXTURE_TEST_CASE(expired_request_is_removed_before_the_next_unlock, ldt::server_fixture)
{
    // Expire a queued request before unlocking its mutex. Verify it returns to the idle state
    // and cannot receive ownership during the subsequent unlock.
    const auto mutex = server.lock_store().get_mutex("expiry");
    auto client = lkd::new_ptr<lkd::client>(server);
    BOOST_REQUIRE(mutex->try_lock());
    lifetime_test_access::wait(mutex, client, std::chrono::seconds(60));

    // Deliver expiry directly. A zero-duration OS timer still needs reactor scheduling, so a
    // single poll() is not a deterministic way to advance it on every supported platform.
    lifetime_test_access::late_timeout(mutex)();
    BOOST_CHECK(lifetime_test_access::status(client) == lld::mutex::ls_none);
    mutex->unlock();
    BOOST_REQUIRE(mutex->try_lock());
    mutex->unlock();
    client.reset();
    server.io_service().poll();
}


BOOST_FIXTURE_TEST_CASE(
        reply_queue_disconnects_before_exceeding_its_byte_budget, ldt::server_fixture
)
{
    // Fill the retained-reply budget, including the buffer used by an in-flight write. The next
    // message must close the connection without being copied into the queue or freeing that buffer.
    using boost::asio::ip::tcp;
    tcp::acceptor acceptor(
            server.io_service(), tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0)
    );
    tcp::socket peer(server.io_service());
    peer.connect(acceptor.local_endpoint());
    const auto client = lkd::new_ptr<lkd::client>(server);
    acceptor.accept(client->socket());
    const std::string block(64 * 1024, 'x');
    lifetime_test_access::send(client, block);
    lifetime_test_access::send(client, block);
    BOOST_REQUIRE_MESSAGE(client->socket().is_open(), "Exactly 128 KiB remains within the budget");
    BOOST_CHECK_EQUAL(lifetime_test_access::pending_reply_bytes(client), 128 * 1024);
    lifetime_test_access::send(client, "overflow");
    BOOST_CHECK_MESSAGE(
            !client->socket().is_open(), "A reply exceeding the byte budget disconnects"
    );
    BOOST_CHECK_EQUAL(lifetime_test_access::replies(client, "overflow"), 0);
    BOOST_CHECK_EQUAL(lifetime_test_access::pending_reply_bytes(client), 128 * 1024);
    server.io_service().poll();
    BOOST_CHECK_EQUAL(lifetime_test_access::pending_reply_bytes(client), 0);
}


BOOST_FIXTURE_TEST_CASE(async_write_owns_the_entire_message, ldt::server_fixture)
{
    // Queue a message larger than the send buffer followed by two replies. Verify asynchronous
    // writes retain all bytes and deliver the replies in enqueue order.
    using boost::asio::ip::tcp;
    auto& service = server.io_service();
    tcp::acceptor acceptor(service, tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0));
    tcp::socket peer(service);
    peer.connect(acceptor.local_endpoint());
    const auto client = lkd::new_ptr<lkd::client>(server);
    acceptor.accept(client->socket());
    // Keep the send buffer smaller than the message, but large enough for full TCP segments.
    // A 1 KiB buffer makes this test depend on the platform's delayed-ACK timing.
    client->socket().set_option(boost::asio::socket_base::send_buffer_size(16 * 1024));
    client->socket().set_option(tcp::no_delay(true));
    peer.non_blocking(true);

    std::string message(96 * 1024, 'x');
    lifetime_test_access::send(client, message);
    lifetime_test_access::send(client, "first queued reply\r\n");
    lifetime_test_access::send(client, "second queued reply\r\n");
    const std::string expected = message + "first queued reply\r\nsecond queued reply\r\n";

    // Keep the message within the reply budget but larger than the socket send buffer. The
    // asynchronous writes must retain it even after the caller changes the original string.
    message.assign(message.size(), 'z');
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::string received;
    while (received.size() < expected.size() && std::chrono::steady_clock::now() < deadline) {
        service.poll();
        char buffer[16384];
        boost::system::error_code error;
        const auto count = peer.read_some(boost::asio::buffer(buffer), error);
        if (error != boost::asio::error::would_block && error != boost::asio::error::try_again)
            BOOST_REQUIRE_MESSAGE(!error, error.message());
        received.append(buffer, count);
    }
    BOOST_REQUIRE_EQUAL(received.size(), expected.size());
    BOOST_CHECK_MESSAGE(
            received == expected,
            "Queued replies must own their buffers and arrive in enqueue order"
    );
    service.poll();
}

BOOST_FIXTURE_TEST_CASE(accept_failure_recovers_and_cancellation_stays_closed, ldt::server_fixture)
{
    // Inject an accept failure and trigger its retry, then close the listener. Verify recovery
    // delivers a real greeting and cancellation leaves the listener closed.
    using boost::asio::ip::tcp;
    const auto endpoint = lifetime_test_access::reopen_listener(server);
    lifetime_test_access::fail_accept(server, boost::asio::error::no_descriptors);
    BOOST_CHECK_EQUAL(server.client_count(), 0);
    lifetime_test_access::expire_accept_retry(server);

    // The recovery must arm a real accept. Connecting through the loopback socket proves that
    // processing resumes; checking a retry counter alone would miss a disconnected accept chain.
    tcp::socket peer(server.io_service());
    peer.connect(endpoint);
    peer.non_blocking(true);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::string greeting;
    while (greeting.find('\n') == std::string::npos &&
           std::chrono::steady_clock::now() < deadline) {
        server.io_service().poll();
        char buffer[64];
        boost::system::error_code error;
        const auto count = peer.read_some(boost::asio::buffer(buffer), error);
        BOOST_REQUIRE(
                !error || error == boost::asio::error::would_block ||
                error == boost::asio::error::try_again
        );
        greeting.append(buffer, count);
    }
    BOOST_CHECK_EQUAL(greeting, "LOCKD\r\n");
    BOOST_CHECK_EQUAL(server.client_count(), 1);
    peer.close();
    lifetime_test_access::close_listener(server);

    // Closing the peer does not make its EOF handler immediately ready on every system.
    // Wait for the server to observe the disconnect before checking cancellation behavior.
    ldt::poll_until(server.io_service(), [&] { return server.client_count() == 0; });
    BOOST_CHECK_EQUAL(server.client_count(), 0);

    // A shutdown cancellation must neither schedule retries nor reopen the acceptor.
    lifetime_test_access::fail_accept(server, boost::asio::error::operation_aborted);
    BOOST_CHECK_EQUAL(server.client_count(), 0);
}


BOOST_AUTO_TEST_SUITE_END()

} // namespace

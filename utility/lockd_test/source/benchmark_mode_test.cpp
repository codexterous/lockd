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
 * File       : benchmark_mode_test.cpp
 * Author     : Brett Gmoser
 * Part Of    : lockd_test, a unit test suite for lockd and liblockd.
 * Purpose    : Verify independent benchmark mode and load measurements.
 * Start Date : 11/01/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/system/error_code.hpp> // IWYU pragma: keep
#include <boost/system/system_error.hpp>
#include <boost/test/tools/context.hpp>
#include <boost/test/unit_test.hpp> // IWYU pragma: keep
#include <boost/test/unit_test_suite.hpp>

#include <liblockd/mutex.hpp>

#include "../../../include/lock_store.hpp"
#include "../include/daemon_fixture.hpp"
#include "../include/detail/async_benchmark_detail.hpp"
#include "../include/detail/benchmark_detail.hpp"
#include "../include/lockd_test.hpp"
#include "../include/process.hpp"
#include "../include/scripted_peer.hpp"

void ldt::detail::async_benchmark_test_access::expire_deadline(
        const char* const phase, const bool stale
)
{
    benchmark_options options;
    options.connections = 2;
    benchmark_result result;
    result.tcp_connected = 1;
    std::atomic<bool> failed{false};
    async_benchmark_worker worker(options, result, failed, 0);
    worker._clients.push_back(std::make_unique<async_benchmark_client>(worker, "diagnostic", 1));
    auto& client = *worker._clients.front();
    client._arm(std::chrono::seconds(10), phase);
    const auto generation = client._generation;
    BOOST_REQUIRE_EQUAL(client._timer.cancel(), 1);
    if (stale)
        client._arm(std::chrono::seconds(10), "next operation");
    client.handle_timeout({}, generation);
}


std::pair<std::size_t, std::uint64_t>
ldt::detail::async_benchmark_test_access::setup_progress(async_benchmark_worker& worker)
{
    // prepare() owns this state on its event-loop thread. Post the observation there, rather
    // than racing socket inspection or counters against connection and greeting callbacks.
    const auto reply = std::make_shared<std::promise<std::pair<std::size_t, std::uint64_t>>>();
    auto answer = reply->get_future();
    boost::asio::post(worker._service, [&worker, reply] {
        const auto opened = std::count_if(
                worker._clients.begin(), worker._clients.end(), [](const auto& client) {
                    return client->_socket.is_open();
                }
        );
        reply->set_value({static_cast<std::size_t>(opened), worker._result.greetings_received});
    });
    if (answer.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
        worker._failed = true;
        throw std::runtime_error("Timed out observing asynchronous benchmark setup");
    }
    return answer.get();
}

namespace {

boost::asio::ip::tcp::socket
benchmark_accept(boost::asio::io_context& service, boost::asio::ip::tcp::acceptor& listener)
{
    boost::asio::ip::tcp::socket socket(service);
    ldt::poll_until(service, [&] {
        boost::system::error_code error;

        // NOLINTNEXTLINE(bugprone-unused-return-value)
        static_cast<void>(listener.accept(socket, error));
        if (error && error != boost::asio::error::would_block &&
            error != boost::asio::error::try_again)
            throw boost::system::system_error(error);
        return !error;
    });
    socket.non_blocking(true);
    return socket;
}

std::pair<int, std::string> benchmark_child(const std::vector<std::string>& arguments)
{
    ldt::process::ipstream captured;
    ldt::process::child child(
            ldt::master_test_suite_argv()[0],
            ldt::process::args(arguments),
            (ldt::process::std_out & ldt::process::std_err) > captured
    );
    auto reader = std::async(std::launch::async, [&captured] {
        std::ostringstream output;
        output << captured.rdbuf();
        return output.str();
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (child.running() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool finished = !child.running();
    if (!finished)
        child.terminate();
    child.wait();
    const std::string output = reader.get();
    BOOST_REQUIRE_MESSAGE(finished, "Benchmark child exits within its bound: " << output);
    return {child.exit_code(), output};
}

BOOST_AUTO_TEST_SUITE(lifetimes)

BOOST_AUTO_TEST_CASE(benchmark_help_bypasses_normal_tests_and_rejects_test_options)
{
    // Both mode spellings show dedicated help without touching a daemon or entering Boost.Test.
    // Normal test selectors and launcher options must fail, rather than silently changing mode.
    for (const std::vector<std::string>& arguments :
         {std::vector<std::string>{"--help"}, {"--", "--help"}}) {
        const auto [status, output] = benchmark_child(arguments);
        BOOST_CHECK_MESSAGE(status == 0, "General help succeeds: " << output);
        BOOST_CHECK_MESSAGE(
                output.find("-- -B") != std::string::npos,
                "Both help paths point to the runner's benchmark mode: " << output
        );
        if (arguments.size() == 2)
            BOOST_CHECK_MESSAGE(
                    output.find("--benchmark-test") != std::string::npos,
                    "Runner help lists the full benchmark option, not just a usage hint: " << output
            );
        BOOST_CHECK_MESSAGE(
                output.find("Running ") == std::string::npos,
                "General help never starts normal tests"
        );
    }
    for (const std::string mode : {"--benchmark-test", "-B"}) {
        const auto [status, output] = benchmark_child({"--", mode, "--help"});
        BOOST_CHECK_MESSAGE(status == 0, "Dedicated help succeeds: " << output);
        BOOST_CHECK_MESSAGE(
                output.find("Benchmark options") != std::string::npos,
                "Dedicated help describes load generation"
        );
        BOOST_CHECK_MESSAGE(
                output.find("Running ") == std::string::npos,
                "Help never announces a normal test run"
        );
        BOOST_CHECK_MESSAGE(
                output.find(std::to_string(LOCKD_TEST_PORT)) != std::string::npos,
                "Load mode uses the configured runner port default"
        );
    }
    for (const std::vector<std::string>& arguments :
         {std::vector<std::string>{"--", "-B", "--run_test=protocol"},
          {"--run_test=lifetimes", "--", "--benchmark-test"},
          {"-B", "--help"},
          {"--", "--bench"},
          {"--", "-B", "--lockd-path=unused"},
          {"--", "-B", "--", "--help"},
          {"--", "-B", "--log_level=all"},
          {"--", "-B", "-p", "0"},
          {"--", "-B", "-t", "0"},
          {"--", "-B", "--duration=-1"},
          {"--", "-B", "--warmup=-1"},
          {"--", "-B", "--connections=0"},
          {"--", "-B", "--workload=unknown"},
          {"--", "-B", "--engine=unknown"},
          {"--", "-B", "--lock-count=2"},
          {"--", "-B", "--workload=idle", "--hold-ms=1"},
          {"--", "-B", "--workload=idle", "--require-all-clients"},
          {"--", "-B", "--lock-prefix=invalid name"}}) {
        const auto [status, output] = benchmark_child(arguments);
        BOOST_CHECK_MESSAGE(status != 0, "Invalid or mixed options fail: " << output);
        BOOST_CHECK_MESSAGE(
                output.find("Running ") == std::string::npos,
                "Rejected load commands never start normal tests"
        );
        BOOST_CHECK_MESSAGE(
                output.find("Connecting clients") == std::string::npos,
                "Invalid options are rejected before starting any load: " << output
        );
    }
}

BOOST_AUTO_TEST_CASE(benchmark_histograms_merge_counts_without_averaging_percentiles)
{
    // Merge bounded histograms and verify percentile ranks and upward rounding, including
    // an empty result. Cross-machine percentiles must come from counts, not averaged p99s.
    ldt::detail::benchmark_result first;
    ldt::detail::benchmark_result second;
    BOOST_CHECK_MESSAGE(first.percentile(99) == 0, "An empty run has no latency samples");
    for (unsigned int value = 1; value <= 100; ++value) {
        auto& result = value <= 50 ? first : second;
        result.record(std::chrono::microseconds(value));
    }
    first.merge(second);
    BOOST_CHECK_MESSAGE(first.cycles == 100, "Each successful cycle contributes one sample");
    for (const unsigned int percent : {50U, 95U, 99U, 100U}) {
        BOOST_CHECK_MESSAGE(
                first.percentile(percent) >= percent && first.percentile(percent) <= percent + 6,
                "Percentiles are upper bounds with bounded rounding"
        );
    }
}

BOOST_FIXTURE_TEST_CASE(
        benchmark_workloads_use_external_server_and_release_clients, ldt::running_server_fixture
)
{
    // Exercise every workload through the executable against one existing daemon. Verify CSV
    // output, completed protocol cycles, and that the daemon still serves requests afterward.
    const auto endpoint = lkd::detail::lifetime_test_access::endpoint(server);
    const auto path =
            std::filesystem::current_path() /
            ("benchmark_" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".csv");
    for (const std::string engine : {"sync", "async"})
        for (const std::string workload : {"independent", "contended", "idle", "churn"}) {
            BOOST_TEST_CONTEXT("Benchmark engine: " << engine << "; workload: " << workload)
            {
                const auto [status, output] = benchmark_child(
                        {"--",
                         "-B",
                         "-i",
                         endpoint.address().to_string(),
                         "-p",
                         std::to_string(endpoint.port()),
                         "-t",
                         "2",
                         "--connections=2",
                         "--duration=1",
                         "--warmup=0",
                         "--workload=" + workload,
                         "--engine=" + engine,
                         "--results=" + path.string()}
                );
                BOOST_REQUIRE_MESSAGE(status == 0, "External-daemon load succeeds: " << output);
                BOOST_CHECK_MESSAGE(
                        output.find("Running ") == std::string::npos,
                        "Load mode never starts the normal suites"
                );
                const std::ifstream file(path);
                std::ostringstream contents;
                contents << file.rdbuf();
                BOOST_CHECK_MESSAGE(
                        contents.str().find("metric,worker,value,count\n") == 0,
                        "Results use the documented CSV schema"
                );
                BOOST_CHECK_MESSAGE(
                        contents.str().find("errors,0,0,0") != std::string::npos &&
                                contents.str().find("errors,1,0,0") != std::string::npos,
                        "Both workers complete without protocol errors"
                );
                BOOST_CHECK_EQUAL(
                        contents.str().find("tcp_connected,0,2,0") != std::string::npos,
                        engine == "async"
                );
                BOOST_CHECK_EQUAL(
                        contents.str().find("greetings_received,0,2,0") != std::string::npos,
                        engine == "async"
                );
                BOOST_CHECK(
                        contents.str().find(
                                std::string("setup_handshakes_per_worker,all,") +
                                (engine == "async" ? "2,0" : "1,0")
                        ) != std::string::npos
                );
                BOOST_CHECK_MESSAGE(
                        (contents.str().find("acquisition_us,") != std::string::npos) ==
                                (workload != "idle"),
                        "Active workloads record acquisitions; idle workloads do not"
                );
            }
        }
    BOOST_CHECK_MESSAGE(std::filesystem::remove(path), "Remove the temporary benchmark result");
    lld::mutex probe(
            endpoint.address().to_string(),
            endpoint.port(),
            std::chrono::seconds(1),
            "benchmark_server_survives"
    );
    BOOST_CHECK_MESSAGE(probe.try_lock(), "Load mode leaves the external daemon running");
}

BOOST_FIXTURE_TEST_CASE(
        benchmark_client_coverage_requires_success_on_every_connection, ldt::running_server_fixture
)
{
    const auto endpoint = lkd::detail::lifetime_test_access::endpoint(server);
    const auto path =
            std::filesystem::current_path() /
            ("coverage_" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".csv");
    for (const std::string engine : {"sync", "async"}) {
        std::vector<std::string> arguments{
                "--",
                "-B",
                "-i",
                endpoint.address().to_string(),
                "-p",
                std::to_string(endpoint.port()),
                "-t",
                "2",
                "--connections=3",
                "--engine=" + engine,
                "--duration=1",
                "--warmup=0",
                "--require-all-clients",
                "--results=" + path.string()
        };

        // Repeated successful cycles on each connection count that connection only once.
        const auto [status, output] = benchmark_child(arguments);
        BOOST_REQUIRE_MESSAGE(status == 0, "All clients participate: " << output);
        const std::ifstream file(path);
        std::ostringstream contents;
        contents << file.rdbuf();
        BOOST_CHECK_MESSAGE(
                contents.str().find("clients_completed,0,3,0") != std::string::npos &&
                        contents.str().find("clients_completed,1,3,0") != std::string::npos,
                "Both engines record distinct measured clients, not total acquisitions"
        );

        // Connected clients that only time out never satisfy measured participation. Their
        // result file must survive failure so the controller can archive the incomplete run.
        lld::mutex owner(
                endpoint.address().to_string(),
                endpoint.port(),
                std::chrono::seconds(1),
                "coverage_held_0"
        );
        const std::unique_lock<lld::mutex> held(owner);
        arguments.insert(
                arguments.end(),
                {"--workload=contended",
                 "--lock-count=1",
                 "--lock-prefix=coverage_held",
                 "--acquire-timeout=50"}
        );
        const auto [failed, failed_output] = benchmark_child(arguments);
        BOOST_CHECK_MESSAGE(failed != 0, "Incomplete coverage fails: " << failed_output);
        BOOST_CHECK_MESSAGE(
                failed_output.find("Measured clients completed: 0/6") != std::string::npos &&
                        failed_output.find("Client coverage incomplete") != std::string::npos,
                "A connected population without measured grants isn't a complete run"
        );
    }
    BOOST_CHECK_MESSAGE(std::filesystem::remove(path), "Remove client coverage CSV");
}

BOOST_AUTO_TEST_CASE(benchmark_release_errors_abort_without_counting_a_cycle)
{
    for (const std::string engine : {"sync", "async"}) {

        // A grant followed by a rejected release must fail the benchmark, close the connection,
        // and leave its cycle count at zero. Never report failed cleanup as useful throughput.
        ldt::scripted_peer peer([](ldt::scripted_peer::tcp::socket& socket) {
            ldt::scripted_peer::send(socket, "LOCKD\n");
            const std::string request = ldt::scripted_peer::line(socket);
            if (request.compare(0, 11, "TIMED_LOCK ") != 0)
                throw std::runtime_error("Expected bounded lock acquisition");
            ldt::scripted_peer::send(socket, "LOCK_ACQUIRING\nLOCK_ACQUIRED\n");
            ldt::scripted_peer::expect(socket, "UNLOCK");
            ldt::scripted_peer::send(socket, "INVALID_COMMAND\n");
            ldt::scripted_peer::expect_disconnect(socket);
        });
        const auto [status, output] = benchmark_child(
                {"--",
                 "-B",
                 "-i",
                 "127.0.0.1",
                 "-p",
                 std::to_string(peer.port()),
                 "-t",
                 "1",
                 "--engine=" + engine,
                 "--duration=1",
                 "--warmup=0"}
        );
        peer.finish();
        BOOST_CHECK_MESSAGE(status != 0, "Release failure returns failure: " << output);
        if (engine == "async")
            BOOST_CHECK_MESSAGE(
                    output.find("phase=unlock reply") != std::string::npos &&
                            output.find("initial TCP connected=1/1; greetings=1/1") !=
                                    std::string::npos,
                    "Release diagnostics retain the failed protocol phase: " << output
            );
        BOOST_CHECK_MESSAGE(
                output.find("Completed cycles: 0") != std::string::npos,
                "Failed release does not contribute a completed cycle"
        );
        BOOST_CHECK_MESSAGE(
                output.find("Run aborted") != std::string::npos &&
                        output.find("cycles/s:") == std::string::npos,
                "Partial results cannot be mistaken for a throughput measurement: " << output
        );
    }
}

BOOST_FIXTURE_TEST_CASE(
        benchmark_timeouts_preserve_an_external_lock_owner, ldt::running_server_fixture
)
{
    for (const std::string engine : {"sync", "async"}) {

        // A second process repeatedly times out on a held shared name. Its timeouts are load
        // results, not client errors, and it must neither acquire nor release the owner's lock.
        const auto endpoint = lkd::detail::lifetime_test_access::endpoint(server);
        lld::mutex owner(
                endpoint.address().to_string(),
                endpoint.port(),
                std::chrono::seconds(1),
                "benchmark_held_0"
        );
        const std::unique_lock<lld::mutex> held(owner);
        const auto [status, output] = benchmark_child(
                {"--",
                 "-B",
                 "-i",
                 endpoint.address().to_string(),
                 "-p",
                 std::to_string(endpoint.port()),
                 "-t",
                 "1",
                 "--workload=contended",
                 "--lock-prefix=benchmark_held",
                 "--engine=" + engine,
                 "--duration=1",
                 "--warmup=0",
                 "--acquire-timeout=50"}
        );
        BOOST_CHECK_MESSAGE(status == 0, "Contention timeouts are measured outcomes: " << output);
        BOOST_CHECK_MESSAGE(
                output.find("Completed cycles: 0") != std::string::npos &&
                        output.find("timeouts: 0;") == std::string::npos,
                "The child records timeouts without acquiring the held lock"
        );
        lld::mutex observer(
                endpoint.address().to_string(),
                endpoint.port(),
                std::chrono::seconds(1),
                "benchmark_held_0"
        );
        BOOST_CHECK_MESSAGE(!observer.try_lock(), "The external owner's lock remains held");
    }
}

BOOST_FIXTURE_TEST_CASE(
        async_benchmark_queues_all_connections_and_cancels_waiters, ldt::running_server_fixture
)
{
    // Hold one name until every connection from ONE worker has a pending acquisition. This
    // observes concurrency directly instead of guessing it from throughput or sleep duration.
    const auto endpoint = lkd::detail::lifetime_test_access::endpoint(server);
    ldt::detail::benchmark_options options;
    options.interface = endpoint.address().to_string();
    options.port = endpoint.port();
    options.workload = "contended";
    options.prefix = "async_parallel";
    options.connections = 8;
    options.acquire_timeout = 60000;
    lld::mutex owner(options.interface, options.port, std::chrono::seconds(1), "async_parallel_0");
    const std::unique_lock<lld::mutex> held(owner);
    const auto mutex = server.lock_store().get_mutex("async_parallel_0");
    std::atomic<bool> failed{false};
    ldt::detail::benchmark_result result;
    ldt::detail::async_benchmark_worker worker(options, result, failed, 0);
    worker.prepare(std::vector<std::string>(8, "async_parallel_0"));
    auto run = std::async(std::launch::async, [&] {
        const auto now = std::chrono::steady_clock::now();
        worker.run(now, now + std::chrono::seconds(60));
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (lkd::detail::lifetime_test_access::queued_requests(mutex) != 8 &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    const auto queued = lkd::detail::lifetime_test_access::queued_requests(mutex);
    failed = true;
    const auto stopped = run.wait_for(std::chrono::seconds(3));
    BOOST_REQUIRE_MESSAGE(stopped == std::future_status::ready, "Abort interrupts pending waits");
    run.get();
    BOOST_CHECK_EQUAL(queued, 8);
    BOOST_CHECK_EQUAL(result.cycles, 0);
    BOOST_CHECK_EQUAL(result.timeouts, 0);
    const auto cleanup = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (lkd::detail::lifetime_test_access::queued_requests(mutex) != 0 &&
           std::chrono::steady_clock::now() < cleanup)
        std::this_thread::yield();
    BOOST_CHECK_EQUAL(lkd::detail::lifetime_test_access::queued_requests(mutex), 0);
    BOOST_CHECK(held.owns_lock());
}

BOOST_AUTO_TEST_CASE(async_benchmark_drains_fragmented_grants_and_releases_at_window_end)
{
    // The hold extends past measurement end. Still release a granted lock, consume the full
    // fragmented acknowledgement, and exclude that boundary-crossing cycle from the histogram.
    ldt::scripted_peer peer([](ldt::scripted_peer::tcp::socket& socket) {
        ldt::scripted_peer::send(socket, "LOC");
        ldt::scripted_peer::send(socket, "KD\r\n");
        const auto command = ldt::scripted_peer::line(socket);
        if (command.compare(0, 11, "TIMED_LOCK ") != 0)
            throw std::runtime_error("Expected a timed acquisition");
        ldt::scripted_peer::send(socket, "LOCK_ACQUIRING\r\nLOCK_ACQ");
        ldt::scripted_peer::send(socket, "UIRED\n");
        ldt::scripted_peer::expect(socket, "UNLOCK");
        ldt::scripted_peer::send(socket, "LOCK_RELEA");
        ldt::scripted_peer::send(socket, "SED\n");
        ldt::scripted_peer::expect_disconnect(socket);
    });
    ldt::detail::benchmark_options options;
    options.interface = "127.0.0.1";
    options.port = peer.port();
    options.hold_ms = 60000;
    std::atomic<bool> failed{false};
    ldt::detail::benchmark_result result;
    ldt::detail::async_benchmark_worker worker(options, result, failed, 0);
    worker.prepare({"async_boundary"});
    const auto now = std::chrono::steady_clock::now();
    worker.run(now, now + std::chrono::milliseconds(100));
    peer.finish();
    BOOST_CHECK_EQUAL(result.cycles, 0);
    BOOST_CHECK_EQUAL(result.errors, 0);
    BOOST_CHECK(!failed);
}

BOOST_AUTO_TEST_CASE(async_benchmark_rejects_invalid_and_unbounded_replies)
{
    // Untrusted framing and unexpected replies must abort, close the connection, and never
    // contribute cycles. The stream buffer's bound also applies to an unfinished greeting.
    for (const std::string& reply : std::vector<std::string>{"INVALID\n", std::string(4096, 'x')}) {
        ldt::scripted_peer peer([&reply](ldt::scripted_peer::tcp::socket& socket) {
            ldt::scripted_peer::send(socket, reply);
            ldt::scripted_peer::expect_disconnect(socket);
        });
        const auto [status, output] = benchmark_child(
                {"--",
                 "-B",
                 "--engine=async",
                 "-i",
                 "127.0.0.1",
                 "-p",
                 std::to_string(peer.port()),
                 "-t",
                 "1",
                 "--duration=1",
                 "--warmup=0"}
        );
        peer.finish();
        BOOST_CHECK_MESSAGE(status != 0, "Invalid server replies abort: " << output);
        BOOST_CHECK(output.find("Completed cycles: 0") != std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(async_benchmark_setup_slots_wait_for_complete_greetings)
{
    // TCP connect alone doesn't prove the daemon has accepted the connection. Hold all four
    // greetings partway through a line: no replacement may start until a full LOCKD arrives.
    // Then finish greetings one at a time, proving each frees exactly one setup slot and that
    // every requested connection stays open before measurement. This uses events, not sleeps.
    typedef ldt::scripted_peer::tcp tcp;
    boost::asio::io_context service;
    tcp::acceptor listener(service, tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0));
    listener.non_blocking(true);
    std::vector<tcp::socket> sockets;
    ldt::detail::benchmark_options options;
    options.interface = "127.0.0.1";
    options.port = listener.local_endpoint().port();
    options.connections = 11;
    std::atomic<bool> failed{false};
    ldt::detail::benchmark_result result;
    auto worker = std::make_unique<ldt::detail::async_benchmark_worker>(options, result, failed, 0);
    auto preparing = std::async(std::launch::async, [&] {
        worker->prepare(std::vector<std::string>(options.connections, "setup_pacing"));
    });
    for (std::size_t index = 0; index < 4; ++index) {
        sockets.push_back(benchmark_accept(service, listener));
        ldt::scripted_peer::send(sockets.back(), "LOC");
    }
    auto progress = ldt::detail::async_benchmark_test_access::setup_progress(*worker);
    BOOST_CHECK_EQUAL(progress.first, 4);
    BOOST_CHECK_EQUAL(progress.second, 0);

    for (std::size_t index = 0; index < options.connections; ++index) {
        ldt::scripted_peer::send(sockets[index], index < 4 ? "KD\r\n" : "LOCKD\n");
        const auto opened = std::min<std::size_t>(4 + index + 1, options.connections);
        if (sockets.size() < opened)
            sockets.push_back(benchmark_accept(service, listener));
        if (index + 1 != options.connections) {
            ldt::poll_until(service, [&] {
                progress = ldt::detail::async_benchmark_test_access::setup_progress(*worker);
                return progress.second >= index + 1;
            });
            BOOST_CHECK_EQUAL(progress.first, opened);
            BOOST_CHECK_EQUAL(progress.second, index + 1);
        }
    }
    BOOST_REQUIRE(preparing.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
    preparing.get();
    BOOST_CHECK(!failed);
    BOOST_CHECK_EQUAL(result.tcp_connected, options.connections);
    BOOST_CHECK_EQUAL(result.greetings_received, options.connections);
    BOOST_CHECK_EQUAL(result.cycles, 0);
    for (auto& socket : sockets) {
        char byte;
        boost::system::error_code error;

        // No load commands, EOF, or teardown may appear before measurement starts.
        const auto count = socket.read_some(boost::asio::buffer(&byte, 1), error);
        BOOST_CHECK_EQUAL(count, 0);
        BOOST_CHECK(
                error == boost::asio::error::would_block || error == boost::asio::error::try_again
        );
    }
    worker.reset();
    for (auto& socket : sockets)
        ldt::scripted_peer::expect_disconnect(socket);
}

BOOST_AUTO_TEST_CASE(async_benchmark_setup_cancellation_leaves_queued_connections_unopened)
{
    // An error on a different native worker aborts this one too. Unstarted clients must not
    // create new TCP connections while the four outstanding greetings are being canceled.
    typedef ldt::scripted_peer::tcp tcp;
    boost::asio::io_context service;
    tcp::acceptor listener(service, tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0));
    listener.non_blocking(true);
    std::vector<tcp::socket> sockets;
    ldt::detail::benchmark_options options;
    options.interface = "127.0.0.1";
    options.port = listener.local_endpoint().port();
    options.connections = 11;
    std::atomic<bool> failed{false};
    ldt::detail::benchmark_result result;
    ldt::detail::async_benchmark_worker worker(options, result, failed, 0);
    auto preparing = std::async(std::launch::async, [&] {
        worker.prepare(std::vector<std::string>(options.connections, "setup_cancel"));
    });
    for (std::size_t index = 0; index < 4; ++index) {
        sockets.push_back(benchmark_accept(service, listener));
        ldt::scripted_peer::send(sockets.back(), "LOCK");
    }
    const auto progress = ldt::detail::async_benchmark_test_access::setup_progress(worker);
    BOOST_CHECK_EQUAL(progress.first, 4);
    BOOST_CHECK_EQUAL(progress.second, 0);
    failed = true;
    BOOST_REQUIRE(preparing.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
    preparing.get();
    BOOST_CHECK_EQUAL(result.greetings_received, 0);
    for (auto& socket : sockets)
        ldt::scripted_peer::expect_disconnect(socket);
    tcp::socket unexpected(service);
    boost::system::error_code error;

    // An empty accept queue proves queued clients weren't connected and then quietly closed.
    // NOLINTNEXTLINE(bugprone-unused-return-value)
    static_cast<void>(listener.accept(unexpected, error));
    BOOST_CHECK(error == boost::asio::error::would_block || error == boost::asio::error::try_again);
}

BOOST_AUTO_TEST_CASE(async_benchmark_setup_failure_drains_other_connections)
{
    // One invalid greeting throws while three sockets await their greeting suffix and seven
    // more clients are queued. Teardown must drain ALL reads before freeing client buffers,
    // and cancellation callbacks must not open queued connections while unwinding the error.
    typedef ldt::scripted_peer::tcp tcp;
    boost::asio::io_context service;
    tcp::acceptor listener(service, tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0));
    listener.non_blocking(true);
    const auto port = listener.local_endpoint().port();
    auto script = std::async(std::launch::async, [&] {
        std::vector<tcp::socket> sockets;
        for (std::size_t index = 0; index < 4; ++index) {
            sockets.emplace_back(service);
            ldt::poll_until(service, [&] {
                boost::system::error_code error;

                // NOLINTNEXTLINE(bugprone-unused-return-value)
                static_cast<void>(listener.accept(sockets.back(), error));
                if (error && error != boost::asio::error::would_block &&
                    error != boost::asio::error::try_again)
                    throw boost::system::system_error(error);
                return !error;
            });
            sockets.back().non_blocking(true);
        }
        for (std::size_t index = 1; index < sockets.size(); ++index)
            ldt::scripted_peer::send(sockets[index], "LOCK");
        ldt::scripted_peer::send(sockets.front(), "INVALID\n");
        for (auto& socket : sockets)
            ldt::scripted_peer::expect_disconnect(socket);
    });
    const auto prepare = [&] {
        ldt::detail::benchmark_options options;
        options.interface = "127.0.0.1";
        options.port = port;
        options.connections = 11;
        std::atomic<bool> failed{false};
        ldt::detail::benchmark_result result;
        ldt::detail::async_benchmark_worker worker(options, result, failed, 0);
        worker.prepare(std::vector<std::string>(options.connections, "setup_failure"));
    };
    BOOST_CHECK_THROW(prepare(), std::runtime_error);
    script.get();
    tcp::socket unexpected(service);
    boost::system::error_code error;

    // NOLINTNEXTLINE(bugprone-unused-return-value)
    static_cast<void>(listener.accept(unexpected, error));
    BOOST_CHECK(error == boost::asio::error::would_block || error == boost::asio::error::try_again);
}

BOOST_AUTO_TEST_CASE(async_benchmark_timeout_reports_phase_and_handshake_progress)
{
    // Deliver real deadline handlers directly so stage classification doesn't cost ten seconds
    // per case in CI. A canceled generation must remain powerless after another phase starts.
    for (const char* const phase :
         {"TCP connect",
          "server greeting",
          "lock acknowledgement",
          "lock result",
          "unlock write",
          "unlock reply"}) {
        BOOST_CHECK_EXCEPTION(
                ldt::detail::async_benchmark_test_access::expire_deadline(phase, false),
                std::runtime_error,
                [phase](const std::runtime_error& error) {
                    const std::string message(error.what());
                    return message.find(std::string("phase=") + phase) != std::string::npos &&
                           message.find("connection=1") != std::string::npos &&
                           message.find("budget_ms=10000") != std::string::npos &&
                           message.find("initial TCP connected=1/2; greetings=0/2") !=
                                   std::string::npos;
                }
        );
        BOOST_CHECK_NO_THROW(
                ldt::detail::async_benchmark_test_access::expire_deadline(phase, true)
        );
    }
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace

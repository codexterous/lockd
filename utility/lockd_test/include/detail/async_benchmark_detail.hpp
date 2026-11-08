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
 * File       : async_benchmark_detail.hpp
 * Author     : Brett Gmoser
 * Part Of    : lockd_test, a unit test suite for lockd and liblockd.
 * Purpose    : Drive concurrent benchmark connections with asynchronous I/O.
 * Start Date : 11/01/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#if !defined(LOCKD_TEST_DETAIL_ASYNC_BENCHMARK_DETAIL_HPP_INCLUDED_)
# define LOCKD_TEST_DETAIL_ASYNC_BENCHMARK_DETAIL_HPP_INCLUDED_

# include <atomic>
# include <chrono>
# include <cstddef>
# include <cstdint>
# include <memory>
# include <string>
# include <utility>
# include <vector>

# include <boost/asio/io_context.hpp>
# include <boost/asio/ip/tcp.hpp>
# include <boost/asio/steady_timer.hpp>
# include <boost/asio/streambuf.hpp>
# include <boost/system/error_code.hpp>

# include "benchmark_detail.hpp"

namespace ldt::detail {

class async_benchmark_client;
class async_benchmark_test_access;

// Limit initial TCP-connect-plus-greeting handshakes PER native worker, not retained clients.
// Opening 32000 sockets at once overflowed the daemon VM's 4096-entry listen backlog: clients
// reported successful TCP connects, but thousands never received LOCKD. A successful connect
// doesn't prove the server has accepted it. Keep the slot until the complete greeting arrives.
// Four slots allow progress around a slow greeting without reproducing the full launch burst.
// The fleet-wide bound still scales with generator count and native workers: 20 * 16 * 4 = 1280.
// This isn't a universal kernel-safe limit; larger fleets or smaller backlogs may need tuning.
inline constexpr std::size_t async_setup_concurrency = 4;

// This is a daemon load generator, not an asynchronous public liblockd API. Each worker runs
// its own context on exactly one thread. Its connections can wait independently without locks
// around their state or histograms. The runner still supplies the common measurement gate.
// prepare() establishes EVERY requested connection before that gate opens. Only initial setup
// is paced; all retained connections can drive cycles concurrently during measurement, and
// churn reconnects remain part of the measured workload. No sleeps or additional threads pace
// setup. Each completed initial greeting admits exactly one queued connection on this context.
// Callbacks borrow clients owned by the worker. On errors or abort, close every socket and
// drain its completions before freeing clients, including buffers viewed by Windows IOCP.
class async_benchmark_worker
{
    /* construction and destruction */
public:
    async_benchmark_worker(
            const benchmark_options& options,
            benchmark_result& result,
            std::atomic<bool>& failed,
            const std::size_t worker
    );
    async_benchmark_worker(const async_benchmark_worker&) = delete;
    async_benchmark_worker& operator=(const async_benchmark_worker&) = delete;
    virtual ~async_benchmark_worker();

    /* public methods */
public:
    void prepare(const std::vector<std::string>& names);
    void
    run(const std::chrono::steady_clock::time_point& start,
        const std::chrono::steady_clock::time_point& end);

    /* private methods */
private:
    friend class async_benchmark_client;
    friend class async_benchmark_test_access;
    void _connectNext();
    void _watch(const bool preparing);

    /* private data members */
private:
    const benchmark_options& _options;
    benchmark_result& _result;
    std::atomic<bool>& _failed;
    const std::size_t _worker;
    std::size_t _iteration = 0;
    boost::asio::io_context _service;
    boost::asio::steady_timer _watchTimer;
    boost::asio::ip::tcp::resolver::results_type _endpoints;
    std::vector<std::unique_ptr<async_benchmark_client>> _clients;
    std::size_t _nextConnect = 0;
    std::chrono::steady_clock::time_point _start;
    std::chrono::steady_clock::time_point _end;
};

// One request at a time per connection: TIMED_LOCK has an acknowledgement and a terminal reply.
// No subsequent request is sent until LOCK_RELEASED, or a server timeout, has been consumed.
// Measurement end stops new cycles, but grants in flight must still be released and confirmed.
// Local transport deadlines abort the run, rather than masquerading as server lock timeouts.
// Diagnostics retain the active phase without an extra clock read in the measured path.
// Initial handshake counters exclude churn reconnects and survive aborted setup in the CSV.
// Measured client coverage is recorded once per logical client and survives churn reconnects.
// A grant contributes only after its release is confirmed wholly inside the measurement window.
class async_benchmark_client
{
    /* enumerations */
private:
    typedef enum
    {
        rs_greeting,
        rs_acknowledgement,
        rs_grant,
        rs_release
    } reply_stage_t;

    /* construction and destruction */
public:
    async_benchmark_client(
            async_benchmark_worker& worker, std::string name, const std::size_t connection
    );
    async_benchmark_client(const async_benchmark_client&) = delete;
    async_benchmark_client& operator=(const async_benchmark_client&) = delete;
    virtual ~async_benchmark_client() = default;

    /* public methods */
public:
    void connect();
    void start();
    void close();
    [[nodiscard]] bool ready() const;
    [[nodiscard]] bool finished() const;

    /* private methods */
private:
    friend class async_benchmark_test_access;
    void _disconnect();
    void _cycle();
    void _acquire();
    void _release();
    void _write(std::string command, const reply_stage_t stage);
    void _read(const reply_stage_t stage);
    void _reply(const std::string& line, const reply_stage_t stage);
    void _arm(const std::chrono::steady_clock::duration& timeout, const char* const phase);
    void handle_timeout(const boost::system::error_code& error, const std::uint64_t generation);
    [[nodiscard]] std::string _diagnostic(const std::string& reason) const;
    [[nodiscard]] static const char* _replyPhase(const reply_stage_t stage);
    void _cancelDeadline();

    /* private data members */
private:
    async_benchmark_worker& _worker;
    const std::string _name;
    const std::size_t _connection;
    boost::asio::ip::tcp::socket _socket;
    boost::asio::streambuf _buffer;
    boost::asio::steady_timer _timer;
    std::string _outgoing;
    std::uint64_t _generation = 0;
    const char* _phase = "not started";
    std::chrono::steady_clock::duration _phaseTimeout{};
    bool _ready = false;
    bool _finished = false;
    bool _measured = false;
    std::chrono::steady_clock::time_point _started;
    std::chrono::steady_clock::time_point _acquired;
    std::chrono::milliseconds _budget{0};
};

// Exercise deadline classification without waiting ten seconds, and inspect setup on its own
// event-loop thread. Tests control the remote endpoint rather than replacing admission logic.
class async_benchmark_test_access
{
    /* construction and destruction */
public:
    virtual ~async_benchmark_test_access() = default;

    /* public methods */
public:
    static void expire_deadline(const char* const phase, const bool stale);
    [[nodiscard]] static std::pair<std::size_t, std::uint64_t>
    setup_progress(async_benchmark_worker& worker);
};

} // namespace ldt::detail

#endif // !defined(LOCKD_TEST_DETAIL_ASYNC_BENCHMARK_DETAIL_HPP_INCLUDED_)

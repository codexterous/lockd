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
 * File       : async_benchmark_detail.cpp
 * Author     : Brett Gmoser
 * Part Of    : lockd_test, a unit test suite for lockd and liblockd.
 * Purpose    : Drive concurrent benchmark connections with asynchronous I/O.
 * Start Date : 11/01/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <boost/version.hpp>

// Older Asio resolver results trigger GCC bounds diagnostics when inlined. Load the header
// before ip/tcp.hpp includes it, using the same scoped exception as the daemon and C++ client.
// Boost 1.90 bounds the address copy by family and no longer needs this exception.
#if defined(__GNUC__) && !defined(__clang__) && BOOST_VERSION < 109000
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Warray-bounds"
# pragma GCC diagnostic ignored "-Wstringop-overflow"
# include <boost/asio/ip/basic_resolver_results.hpp>

# pragma GCC diagnostic pop
#endif

#include <boost/asio/buffer.hpp>

// These public headers supply composed-operation templates whose declarations include-cleaner
// attributes to Asio implementation headers. Keep the supported entry points for all Boost versions.
#include <boost/asio/connect.hpp> // IWYU pragma: keep
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read_until.hpp>   // IWYU pragma: keep
#include <boost/asio/write.hpp>        // IWYU pragma: keep
#include <boost/system/error_code.hpp> // IWYU pragma: keep
#include <boost/system/system_error.hpp>

#include <liblockd/detail/client_contract.hpp>

#include "../../include/detail/async_benchmark_detail.hpp"
#include "../../include/detail/benchmark_detail.hpp"

ldt::detail::async_benchmark_worker::async_benchmark_worker(
        const benchmark_options& options,
        benchmark_result& result,
        std::atomic<bool>& failed,
        const std::size_t worker
)
        : _options(options)
        , _result(result)
        , _failed(failed)
        , _worker(worker)
        , _watchTimer(_service)
{
}


ldt::detail::async_benchmark_worker::~async_benchmark_worker()
{
    // A handler can throw with other connections' I/O still outstanding. Cancel and drain it
    // before destroying ANY client buffers. Finished clients never schedule another operation.
    for (const auto& client : _clients)
        client->close();

    // NOLINTNEXTLINE(bugprone-unused-return-value)
    static_cast<void>(_watchTimer.cancel());
    _service.restart();
    _service.run();
}


void ldt::detail::async_benchmark_worker::prepare(const std::vector<std::string>& names)
{
    // Resolve once per worker, outside the measured window, and allocate every retained client
    // before handlers can run. Queued clients don't open sockets or start transport deadlines
    // until admitted; time spent waiting for a setup slot isn't a failed connection attempt.
    boost::asio::ip::tcp::resolver resolver(_service);
    _endpoints = resolver.resolve(_options.interface, std::to_string(_options.port));
    for (std::size_t index = 0; index < names.size(); ++index)
        _clients.push_back(std::make_unique<async_benchmark_client>(*this, names[index], index));
    // Seed a bounded number of independent chains. Only a complete initial greeting replaces
    // a chain's current connection, so TCP connects waiting for server acceptance still count
    // against the bound. Starting every socket here overwhelmed Linux's listen backlog even
    // though steady-state memory, descriptor limits and CPU all had plenty of headroom.
    for (std::size_t slot = 0; slot < async_setup_concurrency; ++slot)
        _connectNext();
    _watch(true);
    _service.run();
}


void ldt::detail::async_benchmark_worker::_connectNext()
{
    // All admissions run on this worker's context, or before it starts, so no new lock is
    // needed. A failure on another worker must not admit queued clients while abort drains
    // outstanding operations. Advance before scheduling; an exception aborts the whole run.
    if (_failed || _nextConnect == _clients.size())
        return;
    _clients[_nextConnect++]->connect();
}


void ldt::detail::async_benchmark_worker::run(
        const std::chrono::steady_clock::time_point& start,
        const std::chrono::steady_clock::time_point& end
)
{
    _start = start;
    _end = end;
    _service.restart();
    for (const auto& client : _clients)
        client->start();
    _watch(false);
    _service.run();
}


void ldt::detail::async_benchmark_worker::_watch(const bool preparing)
{
    // A failure on another worker must interrupt outstanding I/O here too. This low-frequency
    // worker-wide check avoids both cross-thread socket access and a polling timer per client.
    if (_failed) {
        for (const auto& client : _clients)
            client->close();
        return;
    }
    if (!preparing && _options.workload == "idle" && std::chrono::steady_clock::now() >= _end)
        for (const auto& client : _clients)
            client->close();
    if (std::all_of(_clients.begin(), _clients.end(), [preparing](const auto& client) {
            return client->finished() || (preparing && client->ready());
        }))
        return;
    _watchTimer.expires_after(std::chrono::milliseconds(50));
    _watchTimer.async_wait([this, preparing](const boost::system::error_code& error) {
        if (!error)
            _watch(preparing);
    });
}


ldt::detail::async_benchmark_client::async_benchmark_client(
        async_benchmark_worker& worker, std::string name, const std::size_t connection
)
        : _worker(worker)
        , _name(std::move(name))
        , _connection(connection)
        , _socket(worker._service)
        , _buffer(lld::detail::reply_buffer_limit)
        , _timer(worker._service)
{
}


bool ldt::detail::async_benchmark_client::ready() const
{
    return _ready;
}


bool ldt::detail::async_benchmark_client::finished() const
{
    return _finished;
}


void ldt::detail::async_benchmark_client::_cancelDeadline()
{
    // An already queued expiry can arrive after cancel(). Its generation must not affect the
    // next write, read, hold, or reconnect using this same timer and connection object.
    ++_generation;

    // NOLINTNEXTLINE(bugprone-unused-return-value)
    static_cast<void>(_timer.cancel());
}


void ldt::detail::async_benchmark_client::_arm(
        const std::chrono::steady_clock::duration& timeout, const char* const phase
)
{
    _phase = phase;
    _phaseTimeout = timeout;
    const auto generation = ++_generation;
    _timer.expires_after(timeout);
    _timer.async_wait([this, generation](const boost::system::error_code& error) {
        handle_timeout(error, generation);
    });
}


void ldt::detail::async_benchmark_client::handle_timeout(
        const boost::system::error_code& error, const std::uint64_t generation
)
{
    if (!error && generation == _generation && !_finished)
        throw std::runtime_error(_diagnostic("Asynchronous benchmark timed out"));
}


std::string ldt::detail::async_benchmark_client::_diagnostic(const std::string& reason) const
{
    const auto elapsed = std::chrono::steady_clock::now() - (_timer.expiry() - _phaseTimeout);
    std::ostringstream message;
    message << reason << "; phase=" << _phase << "; connection=" << _connection << "; budget_ms="
            << std::chrono::duration_cast<std::chrono::milliseconds>(_phaseTimeout).count()
            << "; elapsed_ms="
            << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
            << "; initial TCP connected=" << _worker._result.tcp_connected << '/'
            << _worker._options.connections << "; greetings=" << _worker._result.greetings_received
            << '/' << _worker._options.connections;
    return message.str();
}


const char* ldt::detail::async_benchmark_client::_replyPhase(const reply_stage_t stage)
{
    switch (stage) {
    case rs_greeting: return "server greeting";
    case rs_acknowledgement: return "lock acknowledgement";
    case rs_grant: return "lock result";
    case rs_release: return "unlock reply";
    }
    return "unknown reply";
}


void ldt::detail::async_benchmark_client::_disconnect()
{
    _cancelDeadline();
    boost::system::error_code ignored;

    // NOLINTNEXTLINE(bugprone-unused-return-value)
    static_cast<void>(_socket.close(ignored));
}


void ldt::detail::async_benchmark_client::close()
{
    _finished = true;
    _disconnect();
}


void ldt::detail::async_benchmark_client::connect()
{
    _buffer.consume(_buffer.size());
    _arm(std::chrono::seconds(_worker._options.connect_timeout), "TCP connect");
    boost::asio::async_connect(
            _socket,
            _worker._endpoints,
            [this](const boost::system::error_code& error,
                   const boost::asio::ip::tcp::endpoint& /*endpoint*/) {
                if (_finished)
                    return;
                if (error)
                    throw boost::system::system_error(error, _diagnostic("Connection failed"));
                if (!_ready)
                    ++_worker._result.tcp_connected;
                _cancelDeadline();
                _read(rs_greeting);
            }
    );
}


void ldt::detail::async_benchmark_client::start()
{
    if (_worker._options.workload != "idle")
        _cycle();
}


void ldt::detail::async_benchmark_client::_cycle()
{
    if (_worker._failed || std::chrono::steady_clock::now() >= _worker._end) {
        close();
        return;
    }
    if (_worker._options.workload == "churn") {
        _disconnect();
        connect();
        return;
    }
    _acquire();
}


void ldt::detail::async_benchmark_client::_acquire()
{
    _started = std::chrono::steady_clock::now();
    if (_worker._failed || _started >= _worker._end) {
        close();
        return;
    }
    _budget = std::chrono::ceil<std::chrono::milliseconds>(std::min(
            _worker._end - _started,
            std::chrono::steady_clock::duration(
                    std::chrono::milliseconds(_worker._options.acquire_timeout)
            )
    ));
    const auto name = _worker._options.workload == "contended"
                              ? _worker._options.prefix + '_' +
                                        std::to_string(
                                                (_worker._worker + _worker._iteration++) %
                                                _worker._options.lock_count
                                        )
                              : _name;
    _write("TIMED_LOCK " + name + ' ' + std::to_string(_budget.count()) + "ms\n",
           rs_acknowledgement);
}


void ldt::detail::async_benchmark_client::_release()
{
    _write("UNLOCK\n", rs_release);
}


void ldt::detail::async_benchmark_client::_write(std::string command, const reply_stage_t stage)
{
    _outgoing = std::move(command);
    _arm(lld::detail::command_timeout, stage == rs_release ? "unlock write" : "acquisition write");
    boost::asio::async_write(
            _socket,
            boost::asio::buffer(_outgoing),
            [this, stage](const boost::system::error_code& error, const std::size_t /*bytes*/) {
                if (_finished)
                    return;
                if (error)
                    throw boost::system::system_error(error, _diagnostic("Write failed"));
                _cancelDeadline();
                _read(stage);
            }
    );
}


void ldt::detail::async_benchmark_client::_read(const reply_stage_t stage)
{
    const auto timeout = stage == rs_greeting          ? lld::detail::greeting_timeout
                         : stage == rs_acknowledgement ? lld::detail::timed_ack_timeout
                                                       : lld::detail::command_timeout;

    // A terminal acquisition reply includes the requested server wait. A missing reply is a
    // transport failure; only an actual LOCK_TIMED_OUT response increments the timeout counter.
    _arm(std::chrono::duration_cast<std::chrono::milliseconds>(timeout) +
                 (stage == rs_grant ? _budget : std::chrono::milliseconds::zero()),
         _replyPhase(stage));
    boost::asio::async_read_until(
            _socket,
            _buffer,
            '\n',
            [this, stage](const boost::system::error_code& error, const std::size_t /*bytes*/) {
                if (_finished)
                    return;
                if (error)
                    throw boost::system::system_error(error, _diagnostic("Read failed"));
                _cancelDeadline();
                std::istream input(&_buffer);
                std::string line;
                std::getline(input, line);
                lld::detail::trim_reply(line);
                _reply(line, stage);
            }
    );
}


void ldt::detail::async_benchmark_client::_reply(const std::string& line, const reply_stage_t stage)
{
    switch (stage) {
    case rs_greeting:
        if (line != "LOCKD")
            break;
        if (_ready)
            _acquire();
        else {
            _ready = true;
            ++_worker._result.greetings_received;

            // TCP completion alone is too early to release a setup slot. This full LOCKD line
            // confirms application acceptance. Admit one replacement, keeping this connection
            // open for measurement. Reconnects during churn take the _ready branch above and
            // never re-enter initial admission or change its counters.
            _worker._connectNext();
        }
        return;
    case rs_acknowledgement:
        if (line != "LOCK_ACQUIRING")
            break;
        _read(rs_grant);
        return;
    case rs_grant:
        if (line == "LOCK_TIMED_OUT") {
            if (_started >= _worker._start && std::chrono::steady_clock::now() <= _worker._end)
                ++_worker._result.timeouts;
            _cycle();
            return;
        }
        if (line != "LOCK_ACQUIRED")
            break;
        _acquired = std::chrono::steady_clock::now();
        if (_worker._options.hold_ms == 0) {
            _release();
            return;
        }

        // Timer-based simulated work must not stop unrelated connections on this worker.
        _timer.expires_at(
                std::min(
                        _worker._end,
                        _acquired + std::chrono::milliseconds(_worker._options.hold_ms)
                )
        );
        _timer.async_wait([this](const boost::system::error_code& error) {
            if (!error && !_finished)
                _release();
        });
        return;
    case rs_release:
        if (line != "LOCK_RELEASED")
            break;
        if (_started >= _worker._start && std::chrono::steady_clock::now() <= _worker._end) {
            _worker._result.record(_acquired - _started);
            if (!_measured) {
                _measured = true;
                ++_worker._result.clients_completed;
            }
        }
        _cycle();
        return;
    }
    throw std::runtime_error(_diagnostic("Unexpected asynchronous benchmark reply: " + line));
}

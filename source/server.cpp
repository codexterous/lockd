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
 * File       : server.cpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Accept connections, run workers, and coordinate daemon shutdown.
 * Start Date : 03/26/2012
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#if defined(SIGUSR1) && !defined(WIN32)
# include <exception>
#endif
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <boost/version.hpp>

// Older Asio resolver results trigger GCC bounds diagnostics when inlined.
// Load this header before server.hpp includes Asio, as in the client resolver.
// Boost 1.90 bounds the address copy by family and no longer needs this suppression.
#if defined(__GNUC__) && !defined(__clang__) && BOOST_VERSION < 109000
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Warray-bounds"
# pragma GCC diagnostic ignored "-Wstringop-overflow"
# include <boost/asio/ip/basic_resolver_results.hpp>

# pragma GCC diagnostic pop
#endif

#include <boost/asio/error.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>

// The public Format header supplies the template definitions used by free_funcs.hpp.
#include <boost/format.hpp> // IWYU pragma: keep
#include <boost/format/free_funcs.hpp>
#include <boost/system/error_code.hpp> // IWYU pragma: keep

#include "../include/client.hpp"
#include "../include/lockd.hpp"
#include "../include/logging.hpp"
#include "../include/new_shared_ptr.hpp"
#include "../include/server.hpp"

#ifdef WIN32
# include <memory>

# include "../include/platform/win32/detail/listener_guard.hpp"
#endif

// ASIO is namespace hell. This makes things a bit easier.
using namespace boost::asio::ip;

namespace {

// Deadlines protect bounded connection buffers from indefinite retention. They need no precise
// per-write wake-up: check every five seconds instead of scheduling/canceling at protocol speed.
constexpr auto CLIENT_MAINTENANCE_INTERVAL = std::chrono::seconds(5);

} // namespace


lkd::server::server(
        const std::string& listen_ip,
        const std::uint16_t port,
        const unsigned short threads,
        const lkd::report_running_t report_running
)
        : _listenIP(resolve_interface(listen_ip))
        , _threads(threads)
        , _reportRunning(report_running)
        , _endpoint(_listenIP, port)
        , _acceptor(_ioService)
        , _acceptRetry(_ioService)
        , _shutdownTimer(_ioService)
        , _maintenanceTimer(_ioService)
        , _signals(_ioService)
        , _stopping(false)
        , _connectedClients(0)
        , _lockStore(_ioService)
        , _permanentWork(_ioService.get_executor())
{
    _openListener();

    // Create the first client, and set up our acceptor to accept them.
    _createNextClient();
    _scheduleMaintenance();
}


lkd::server::~server() = default;


void lkd::server::_openListener()
{
    _acceptor.open(_endpoint.protocol());

    // POSIX reuse allows immediate restart without allowing a second live listener.
    // Windows reuse also permits duplicate listeners, so acquire a kernel-owned lease
    // before binding there. SO_EXCLUSIVEADDRUSE would delay restart behind old peers.
    _acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
#ifdef WIN32
    _listenerGuard.reset();
    if (_endpoint.port() != 0)
        _listenerGuard =
                std::make_unique<platform::win32::detail::listener_guard>(_endpoint.port());
#endif
    _acceptor.bind(_endpoint);
#ifdef WIN32
    // Port zero asks Windows to allocate a port. Reserve the actual result before listen(),
    // so a later explicit-port daemon cannot share this otherwise anonymous listener.
    if (!_listenerGuard)
        _listenerGuard = std::make_unique<platform::win32::detail::listener_guard>(
                _acceptor.local_endpoint().port()
        );
#endif
    _acceptor.listen();
}


boost::asio::ip::address lkd::server::resolve_interface(const std::string& listen_ip)
{
    boost::system::error_code error;
    auto address = make_address(listen_ip, error);
    if (!error)
        return address;

    // Keep one listener, using the system resolver's first address for hostnames.
    // Numeric addresses bypass resolution, including wildcard and scoped IPv6 addresses.
    boost::asio::io_context service;
    tcp::resolver resolver(service);
    const auto endpoints = resolver.resolve(listen_ip, "0", tcp::resolver::numeric_service);
    return endpoints.begin()->endpoint().address();
}


void lkd::server::_scheduleMaintenance()
{
    // Construction is single-threaded; the handler holds _mutex before scheduling another pass.
    _maintenanceTimer.expires_after(CLIENT_MAINTENANCE_INTERVAL);
    _maintenanceTimer.async_wait([this](const boost::system::error_code& error) {
        handle_maintenance(error);
    });
}


void lkd::server::handle_maintenance(const boost::system::error_code& error)
{
    if (error)
        return;
    _checkClientDeadlines(std::chrono::steady_clock::now());

    // Shutdown may begin during the sweep. Serialize rearming with cancellation so a completed
    // timer callback can never restart maintenance after shutdown has canceled it.
    const std::lock_guard<std::recursive_mutex> lock(_mutex);
    if (!_stopping)
        _scheduleMaintenance();
}


void lkd::server::_checkClientDeadlines(const std::chrono::steady_clock::time_point now) const
{
    std::vector<client_ptr> clients;
    {
        const std::lock_guard<std::recursive_mutex> lock(_mutex);
        if (_stopping)
            return;
        clients.reserve(_clients.size());
        for (const auto& entry : _clients)
            if (const auto client = entry.second.lock())
                clients.push_back(client);
    }

    // As during shutdown, never take a client mutex while holding the server mutex. A client
    // completion can notify the server. Temporary strong handles keep this snapshot valid if
    // sockets close concurrently, and disappear before the next sweep is scheduled.
    for (const auto& client : clients)
        client->check_deadlines(now);
}


void lkd::server::_createNextClient()
{
    const client_ptr client = lkd::new_ptr<lkd::client>(*this);

    _acceptor.async_accept(
            client->socket(), [this, client](const boost::system::error_code& error) {
                this->handle_accept(client, error);
            }
    );
}


size_t lkd::server::run()
{
    // Asio delivers signals on an I/O worker. Logging, timers and socket operations must not
    // run in a C signal handler, where they are not async-signal-safe.
    _signals.add(SIGINT);
    _signals.add(SIGTERM);
#if defined(SIGUSR1) && !defined(WIN32)
    _signals.add(SIGUSR1);
#endif
    _waitForSignal();

    // Create io service threads.
    std::vector<std::thread> threads;
    threads.reserve(_threads);

    try {
        for (std::size_t i = 0; i < _threads; ++i)
            threads.emplace_back([io_service = &_ioService] { io_service->run(); });

        // If asked, report the server as running. Normally this message would be pretty extraneous,
        // but, it is used so that `lockd_test` knows when we are fully started up and running. Take
        // care not to change this message unless you also change `lockd_test`. The launcher uses
        // info logging so this readiness message is visible.
        if (rr_report_running == _reportRunning)
            console_print(
                    cp_stdlog, ll_info, "lockd is running\n", lkd::ct_fg_default, lkd::ct_bg_default
            );
    }
    catch (...) {
        // A failed worker creation must not destroy joinable threads or leave them running
        // against a server whose construction/launch is being unwound.
        _ioService.stop();
        for (auto& thread : threads)
            thread.join();
        throw;
    }

    // Wait for all threads in the pool to exit.
    for (auto& thread : threads)
        thread.join();

    return 0;
}


void lkd::server::client_closed(const lkd::client& cref)
{
    console_print(
            cp_stdlog,
            ll_debug,
            str(boost::format(" - Removing client %1% from client pool.\n") % cref.identifier())
    );
    const std::lock_guard<std::recursive_mutex> lock(_mutex);
    --_connectedClients;
    _clients.erase(&cref);
    if (_stopping && _connectedClients == 0)
        _finishShutdown();
}


std::uintmax_t lkd::server::client_count() const
{
    const std::lock_guard<std::recursive_mutex> lock(_mutex);
    return _connectedClients;
}


void lkd::server::handle_accept(const client_ptr& cptr, const boost::system::error_code& error)
{
    const std::lock_guard<std::recursive_mutex> lock(_mutex);
    if (_stopping)
        return;
    if (!error) {
        console_print(
                cp_stdlog,
                ll_debug,
                str(boost::format(" - Adding client %1% to client pool.\n") % cptr->identifier())
        );

        // We have successfully connected this client (cptr).
        ++_connectedClients;
        _clients.emplace(cptr.get(), cptr);

        // Notify the client object to begin processing commands.
        cptr->start();

        // Create a new client which will handle the next accept we make. Set up our acceptor to
        // accept the next client connection.
        _createNextClient();
    }
    else if (error != boost::asio::error::operation_aborted && _acceptor.is_open()) {
        console_print(
                cp_stdlog,
                ll_error,
                str(boost::format(" - Error handling accept: %1% (%2%)\n") % error %
                    error.message())
        );

        // A failed accept consumes the outstanding operation too. Retry after a short pause so
        // descriptor exhaustion or another persistent failure cannot spin the worker threads.
        // Cancellation while shutting down must never reopen the listener.
        _acceptRetry.expires_after(std::chrono::milliseconds(100));
        _acceptRetry.async_wait([this](const boost::system::error_code& retry_error) {
            handle_accept_retry(retry_error);
        });
    }
}


void lkd::server::handle_accept_retry(const boost::system::error_code& error)
{
    const std::lock_guard<std::recursive_mutex> lock(_mutex);
    if (!error && !_stopping && _acceptor.is_open())
        _createNextClient();
}


bool lkd::server::stopping() const
{
    const std::lock_guard<std::recursive_mutex> lock(_mutex);
    return _stopping;
}


void lkd::server::stop()
{
    // Service controls can arrive on an external thread. Serialize the listener and timer
    // operations with our handlers, and never wait here for a client to finish.
    boost::asio::post(_ioService, [this] { _beginShutdown(); });
}


void lkd::server::_waitForSignal()
{
    // Serialize registration with shutdown's signal cancellation. A reopen must rearm the
    // wait so repeated USR1 signals do not prevent a later SIGTERM from stopping the daemon.
    const std::lock_guard<std::recursive_mutex> lock(_mutex);
    if (!_stopping)
        _signals.async_wait([this](const boost::system::error_code& error, const int signal) {
            handle_signal(error, signal);
        });
}


void lkd::server::handle_signal(const boost::system::error_code& error, const int signal)
{
    if (error)
        return;
#if defined(SIGUSR1) && !defined(WIN32)
    if (signal == SIGUSR1) {
        try {
            logging::reopen_log_file();
            console_print(cp_stdlog, ll_info, "Reopened log file\n");
        }
        catch (const std::exception& failure) {
            console_print(
                    cp_stderr,
                    ll_fatal,
                    std::string("Unable to reopen log file: ") + failure.what() + "\n"
            );
        }
        _waitForSignal();
        return;
    }
#else
    static_cast<void>(signal);
#endif
    stop();
}


void lkd::server::_beginShutdown()
{
    std::vector<client_ptr> clients;
    {
        const std::lock_guard<std::recursive_mutex> lock(_mutex);
        if (_stopping)
            return;
        _stopping = true;

        boost::system::error_code ignored;
        _acceptRetry.cancel();
        _maintenanceTimer.cancel();

        // NOLINTNEXTLINE(bugprone-unused-return-value)
        static_cast<void>(_acceptor.close(ignored));
        _shutdownTimer.expires_after(SHUTDOWN_GRACE_PERIOD);
        _shutdownTimer.async_wait([this](const boost::system::error_code& error) {
            handle_shutdown_timeout(error);
        });
        for (const auto& [fst, snd] : _clients)
            if (const auto client = snd.lock())
                clients.push_back(client);
        if (_connectedClients == 0)
            _finishShutdown();
    }

    console_print(
            cp_stdlog,
            ll_info,
            str(boost::format(
                        "Stopping lockd; allowing lock holders up to %1% seconds to release\n"
                ) %
                SHUTDOWN_GRACE_PERIOD.count())
    );

    // A client can notify the server while holding its own mutex. Drop the server mutex
    // before entering clients to avoid reversing that lock order during shutdown.
    for (const auto& client : clients)
        client->shutdown();
}


void lkd::server::handle_shutdown_timeout(const boost::system::error_code& error) const
{
    if (error)
        return;
    std::vector<client_ptr> clients;
    {
        const std::lock_guard<std::recursive_mutex> lock(_mutex);
        for (const auto& entry : _clients)
            if (const auto client = entry.second.lock())
                clients.push_back(client);
    }

    console_print(
            cp_stdlog,
            ll_warning,
            "Shutdown deadline reached; closing remaining client connections\n"
    );
    for (const auto& client : clients)
        client->shutdown(true);
}


void lkd::server::_finishShutdown()
{
    // All accepted clients have destructed and released their locks. Collection timers need
    // not keep the daemon alive; their handlers are discarded when the server is destroyed.
    boost::system::error_code ignored;
    _shutdownTimer.cancel();

#ifdef WIN32
    // Keep the reservation while accepted clients drain, even though the listener is closed.
    // Releasing it earlier could let a replacement grant locks still held in this daemon.
    // Once all clients are gone, a stopped server must no longer reserve its TCP port.
    _listenerGuard.reset();
#endif

    // NOLINTNEXTLINE(bugprone-unused-return-value)
    static_cast<void>(_signals.cancel(ignored));
    _ioService.stop();
}

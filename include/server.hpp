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
 * File       : server.hpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Accept connections, run workers, and coordinate daemon shutdown.
 * Start Date : 03/26/2012
 *
 * Description: One five-second maintenance timer checks connection deadlines. It snapshots
 *              strong client handles under the server mutex, then releases that mutex before
 *              entering clients. Client I/O can notify the server, so reversing this lock order
 *              would deadlock. The snapshot lasts only for the sweep; it never retains clients
 *              between passes. Each client checks its current timestamps under its own mutex.
 *              Thirty-second stalls therefore expire on a later pass, normally within 30-35
 *              seconds, subject to worker scheduling. Shutdown cancels maintenance; acquisition
 *              timeouts and the shutdown grace timer remain independent.
 *
 *              Shutdown closes the acceptor and idle or queued clients before draining current
 *              owners. One 30-second timer bounds the entire drain; repeated stop requests do
 *              not extend it. Owners may finish UNLOCK replies before their connections close.
 *              Once owners are gone, or the timer expires, the server stops its I/O context.
 *              POSIX signal handling rearms after SIGUSR1 so reopening logs cannot stop service.
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#if !defined(LOCKD_LOCKD_SERVER_HPP_INCLUDED_)
# define LOCKD_LOCKD_SERVER_HPP_INCLUDED_

# if defined(_MSC_VER) && (_MSC_VER >= 1020)
#  pragma once
# endif

# include <chrono>
# include <cstdint>
# include <map>
# include <mutex>
# include <string>

# ifdef WIN32
#  include <memory>
# endif

# include <boost/asio.hpp>
# include <boost/asio/steady_timer.hpp>

# include "../include/client.hpp"
# include "../include/lock_store.hpp"
# include "../include/lockd.hpp"

# ifdef WIN32
namespace lkd::platform::win32::detail {
class listener_guard;
}
# endif

namespace lkd {

// forward declarations
class lock_store;

class server
{
    /* construction and destruction */
public:
    server(const server&) = delete;
    server& operator=(const server&) = delete;
    server(const std::string& listen_ip,
           const std::uint16_t port,
           const unsigned short threads,
           const report_running_t report_running);
    virtual ~server();

    /* public methods */
public:
    [[nodiscard]] static boost::asio::ip::address resolve_interface(const std::string& listen_ip);
    size_t run();
    // Stop accepting work and allow current lock holders at most 30 seconds to release.
    void stop();
    lkd::lock_store& lock_store() { return _lockStore; }
    const lkd::lock_store& lock_store() const { return _lockStore; }
    boost::asio::io_context& io_service() { return _ioService; }

    /* public methods called by (mostly) clients to notify us of things */
public:
    void client_closed(const client& cref);

    /* public methods (data examination) */
public:
    std::uintmax_t client_count() const;
    bool stopping() const;

    /* private handler methods */
private:
    void handle_accept(const client_ptr& cptr, const boost::system::error_code& error);
    void handle_accept_retry(const boost::system::error_code& error);
    void handle_maintenance(const boost::system::error_code& error);
    void handle_shutdown_timeout(const boost::system::error_code& error) const;
    void handle_signal(const boost::system::error_code& error, const int signal);

    /* private methods */
private:
    friend class detail::lifetime_test_access;
    void _openListener();
    void _createNextClient();
    void _scheduleMaintenance();
    void _checkClientDeadlines(const std::chrono::steady_clock::time_point now) const;
    void _waitForSignal();
    void _beginShutdown();
    void _finishShutdown();

    /* private data members */
private:
    const boost::asio::ip::address _listenIP;
    const unsigned short _threads;
    const report_running_t _reportRunning;
    boost::asio::io_context _ioService;
    boost::asio::ip::tcp::endpoint _endpoint;
# ifdef WIN32
    // Declared before the acceptor so the listener closes before its reservation is released.
    std::unique_ptr<platform::win32::detail::listener_guard> _listenerGuard;
# endif
    boost::asio::ip::tcp::acceptor _acceptor;
    boost::asio::steady_timer _acceptRetry;
    boost::asio::steady_timer _shutdownTimer;
    boost::asio::steady_timer _maintenanceTimer;
    boost::asio::signal_set _signals;
    std::map<const client*, weak_client_ptr> _clients;
    bool _stopping;
    std::uintmax_t _connectedClients;
    lkd::lock_store _lockStore;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> _permanentWork;
    mutable std::recursive_mutex _mutex;

}; // class server

} // namespace lkd

#endif // !defined(LOCKD_LOCKD_SERVER_HPP_INCLUDED_)

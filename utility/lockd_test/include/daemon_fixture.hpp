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
 * File       : daemon_fixture.hpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Shared daemon probes and loopback connections for regression tests.
 * Start Date : 10/19/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#if !defined(LOCKD_TEST_DAEMON_FIXTURE_HPP_INCLUDED_)
# define LOCKD_TEST_DAEMON_FIXTURE_HPP_INCLUDED_

# if defined(_MSC_VER) && (_MSC_VER >= 1020)
#  pragma once
# endif

# include <chrono>
# include <cstddef>
# include <functional>
# include <memory>
# include <stdexcept>
# include <string>
# include <thread>
# include <vector>

# include <boost/asio/io_context.hpp>
# include <boost/asio/ip/tcp.hpp>
# include <boost/asio/steady_timer.hpp>
# include <boost/system/error_code.hpp> // IWYU pragma: keep

# include <liblockd/mutex.hpp>

# include "../../../include/server.hpp"

namespace lkd::detail {

// Keep the probes here, not in the daemon's public interface. Tests use real production methods;
// this access delivers timer events, maintenance times and collection attempts at controlled points.
class lifetime_test_access
{
    /* construction and destruction */
public:
    virtual ~lifetime_test_access() = default;

    /* public methods */
public:
    static bool collect(lock_store& store, const std::string& name);
    static void close_listener(server& server);
    static boost::asio::ip::tcp::endpoint reopen_listener(server& server);
    static void fail_accept(server& server, const boost::system::error_code& error);
    static void expire_accept_retry(server& server);
    static boost::asio::ip::tcp::endpoint endpoint(const server& server);
    static size_t cancel_accept_retry(server& server);
    static void retry_accept(server& server, const boost::system::error_code& error);
    static std::chrono::steady_clock::time_point shutdown_deadline(const server& server);
    static void expire_shutdown(server& server);
    static void canceled_shutdown(const server& server);
    static client_ptr
    connected_client(const server& server, const boost::asio::ip::tcp::endpoint& peer);
    static std::size_t command_bytes(const client_ptr& client);
    static std::chrono::steady_clock::time_point command_deadline(const client_ptr& client);
    static void expire_command_read(const client_ptr& client);
    static void
    check_deadlines(const client_ptr& client, const std::chrono::steady_clock::time_point now);
    static void
    check_clients(const server& server, const std::chrono::steady_clock::time_point now);
    static std::chrono::steady_clock::time_point maintenance_deadline(const server& server);
    static void expire_maintenance(server& server);
    static std::size_t cancel_maintenance(server& server);
    static void maintenance(server& server, const boost::system::error_code& error);
    static std::size_t pending_reply_bytes(const client_ptr& client);
    static bool reads_paused(const client_ptr& client);
    static bool read_pending(const client_ptr& client);
    static std::chrono::steady_clock::time_point write_deadline(const client_ptr& client);
    static void expire_write_progress(const client_ptr& client);
    static size_t replies(const client_ptr& client, const std::string& message);
    static void send(const client_ptr& client, const std::string& message);
    static void
    wait(const std::shared_ptr<mutex>& mutex,
         const client_ptr& client,
         const std::chrono::seconds& timeout);
    static lld::mutex::lock_status_t status(const client_ptr& client);
    static void release(const client_ptr& client);
    static bool try_lock(const client_ptr& client, const std::string& name);
    static bool lock(const client_ptr& client, const std::string& name);
    static bool timed_lock(const client_ptr& client, const std::string& name);
    static std::size_t queued_requests(const std::shared_ptr<mutex>& mutex);
    static std::weak_ptr<boost::asio::steady_timer>
    request_timer(const std::shared_ptr<mutex>& mutex, const std::size_t index);
    static std::function<void()>
    late_timeout(const std::shared_ptr<mutex>& mutex, const std::size_t index = 0);
};

} // namespace lkd::detail

namespace ldt {

std::size_t fill_unread_replies(const lkd::client_ptr& client, boost::asio::io_context& service);

// Poll the real daemon on the test thread. Every socket read and state transition has a bound;
// tests can establish queue order by reading acknowledgements instead of sleeping between sends.
template <typename Predicate>
void poll_until(
        boost::asio::io_context& service,
        const Predicate& ready,
        const char* const operation = "loopback test operation"
)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!ready()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error(std::string("Timed out waiting for ") + operation);
        service.poll();
        std::this_thread::yield();
    }
}

class protocol_connection
{
    /* construction and destruction */
public:
    protocol_connection(
            boost::asio::io_context& service,
            const boost::asio::ip::tcp::endpoint& endpoint,
            const int receive_buffer_size = 0
    );
    virtual ~protocol_connection();

    /* public methods */
public:
    void close();
    [[nodiscard]] boost::asio::ip::tcp::endpoint endpoint() const
    {
        return _socket.local_endpoint();
    }
    void send(const std::string& message);
    std::string line();

    /* private methods */
private:
    void _readAvailable();

    /* private data members */
private:
    boost::asio::io_context& _service;
    boost::asio::ip::tcp::socket _socket;
    std::string _received;
};

struct network_fixture
{
    /* construction and destruction */
public:
    network_fixture();
    virtual ~network_fixture();

    /* public methods */
public:
    std::shared_ptr<protocol_connection> connect(const int receive_buffer_size = 0);

    /* public data members */
public:
    lkd::server server;
    std::vector<std::shared_ptr<protocol_connection>> connections;
};

struct server_fixture
{
    /* construction and destruction */
public:
    server_fixture();
    virtual ~server_fixture();

    /* public data members */
public:
    lkd::server server;
};

struct running_server_fixture : ldt::network_fixture
{
    /* construction and destruction */
public:
    running_server_fixture();
    virtual ~running_server_fixture() override;

    /* public data members */
public:
    std::thread thread;
};

} // namespace ldt

#endif // !defined(LOCKD_TEST_DAEMON_FIXTURE_HPP_INCLUDED_)

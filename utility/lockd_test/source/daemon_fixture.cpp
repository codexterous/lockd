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
 * File       : daemon_fixture.cpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Shared daemon probes and loopback connections for regression tests.
 * Start Date : 10/21/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <ostream>
#include <stdexcept>
#include <string>

#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/error_code.hpp> // IWYU pragma: keep
#include <boost/system/system_error.hpp>
#include <boost/test/tools/interface.hpp> // IWYU pragma: keep
#include <boost/test/unit_test.hpp>       // IWYU pragma: keep

#include <liblockd/mutex.hpp>

#include "../../../include/client.hpp"
#include "../../../include/lock_store.hpp"
#include "../../../include/lockd.hpp"
#include "../../../include/mutex.hpp"
#include "../../../include/new_shared_ptr.hpp"
#include "../../../include/server.hpp"
#include "../include/daemon_fixture.hpp"

namespace lkd::detail {

bool lifetime_test_access::collect(lock_store& store, const std::string& name)
{
    return store._collectMutex(name);
}


void lifetime_test_access::close_listener(server& server)
{
    server._acceptor.close();
    server.io_service().poll();
}


boost::asio::ip::tcp::endpoint lifetime_test_access::reopen_listener(server& server)
{
    server._openListener();
    return server._acceptor.local_endpoint();
}


void lifetime_test_access::fail_accept(server& server, const boost::system::error_code& error)
{
    // Use the handler's log stream so this label stays beside the deliberately injected error.
    if (error != boost::asio::error::operation_aborted)
        std::clog << "[expected test error] Injecting accept failure: " << error << " ("
                  << error.message() << ')' << std::endl;

    server.handle_accept(lkd::new_ptr<lkd::client>(server), error);
}


void lifetime_test_access::expire_accept_retry(server& server)
{
    // Deliver the timer event explicitly, then discard the OS wait. No clock delay is needed.
    BOOST_CHECK_EQUAL(server._acceptRetry.cancel(), 1);
    server.handle_accept_retry({});
}


boost::asio::ip::tcp::endpoint lifetime_test_access::endpoint(const server& server)
{
    return server._acceptor.local_endpoint();
}


size_t lifetime_test_access::cancel_accept_retry(server& server)
{
    return server._acceptRetry.cancel();
}


void lifetime_test_access::retry_accept(server& server, const boost::system::error_code& error)
{
    server.handle_accept_retry(error);
}


size_t lifetime_test_access::replies(const client_ptr& client, const std::string& message)
{
    return static_cast<size_t>(
            std::count(client->_pendingWrites.begin(), client->_pendingWrites.end(), message)
    );
}


std::chrono::steady_clock::time_point lifetime_test_access::shutdown_deadline(const server& server)
{
    return server._shutdownTimer.expiry();
}


void lifetime_test_access::expire_shutdown(server& server)
{
    BOOST_REQUIRE_EQUAL(server._shutdownTimer.cancel(), 1);
    server.handle_shutdown_timeout({});
}


void lifetime_test_access::canceled_shutdown(const server& server)
{
    server.handle_shutdown_timeout(boost::asio::error::operation_aborted);
}


client_ptr lifetime_test_access::connected_client(
        const server& server, const boost::asio::ip::tcp::endpoint& peer
)
{
    for (const auto& entry : server._clients)
        if (const auto client = entry.second.lock()) {
            boost::system::error_code error;
            const auto endpoint = client->_socket.remote_endpoint(error);
            if (!error && endpoint == peer)
                return client;
        }
    BOOST_FAIL("No accepted client for the loopback connection");
    return {};
}


std::size_t lifetime_test_access::command_bytes(const client_ptr& client)
{
    return client->_buffer.size();
}


std::chrono::steady_clock::time_point
lifetime_test_access::command_deadline(const client_ptr& client)
{
    return client->_commandDeadline;
}


void lifetime_test_access::expire_command_read(const client_ptr& client)
{
    const auto deadline = command_deadline(client);
    BOOST_REQUIRE(deadline != std::chrono::steady_clock::time_point::max());
    check_deadlines(client, deadline);
}


void lifetime_test_access::check_deadlines(
        const client_ptr& client, const std::chrono::steady_clock::time_point now
)
{
    // Supply a monotonic observation time to the production check, without changing its policy.
    client->check_deadlines(now);
}


void lifetime_test_access::check_clients(
        const server& server, const std::chrono::steady_clock::time_point now
)
{
    server._checkClientDeadlines(now);
}


std::chrono::steady_clock::time_point
lifetime_test_access::maintenance_deadline(const server& server)
{
    return server._maintenanceTimer.expiry();
}


void lifetime_test_access::expire_maintenance(server& server)
{
    // Deliver one registered timer event without a five-second delay. The canceled OS callback
    // is still delivered by Asio and must not rearm a second maintenance wait.
    BOOST_REQUIRE_EQUAL(server._maintenanceTimer.cancel(), 1);
    server.handle_maintenance({});
}


std::size_t lifetime_test_access::cancel_maintenance(server& server)
{
    return server._maintenanceTimer.cancel();
}


void lifetime_test_access::maintenance(server& server, const boost::system::error_code& error)
{
    server.handle_maintenance(error);
}


std::size_t lifetime_test_access::pending_reply_bytes(const client_ptr& client)
{
    std::size_t retained = 0;
    for (const auto& message : client->_pendingWrites)
        retained += message.size();
    if (client->_pendingWriteBytes != retained)
        BOOST_FAIL(
                "Reply byte counter is " << client->_pendingWriteBytes << ", retained " << retained
        );
    return retained;
}


bool lifetime_test_access::reads_paused(const client_ptr& client)
{
    return client->_readsPaused;
}


bool lifetime_test_access::read_pending(const client_ptr& client)
{
    return client->_readPending;
}


std::chrono::steady_clock::time_point lifetime_test_access::write_deadline(const client_ptr& client)
{
    return client->_writeDeadline;
}


void lifetime_test_access::expire_write_progress(const client_ptr& client)
{
    const auto deadline = write_deadline(client);
    BOOST_REQUIRE(deadline != std::chrono::steady_clock::time_point::max());
    check_deadlines(client, deadline);
}


void lifetime_test_access::send(const client_ptr& client, const std::string& message)
{
    client->_scheduleWrite(message);
}


void lifetime_test_access::wait(
        const std::shared_ptr<mutex>& mutex,
        const client_ptr& client,
        const std::chrono::seconds& timeout
)
{
    client->_lockStatus = lld::mutex::ls_acquiring;
    BOOST_REQUIRE(!mutex->timed_lock(client, timeout));
}


lld::mutex::lock_status_t lifetime_test_access::status(const client_ptr& client)
{
    return client->_lockStatus;
}


void lifetime_test_access::release(const client_ptr& client)
{
    client->_releaseLock();
}


std::size_t lifetime_test_access::queued_requests(const std::shared_ptr<mutex>& mutex)
{
    const std::lock_guard<std::recursive_mutex> lock(mutex->_mutex);
    return mutex->_queue.size();
}


std::weak_ptr<boost::asio::steady_timer>
lifetime_test_access::request_timer(const std::shared_ptr<mutex>& mutex, const std::size_t index)
{
    const std::lock_guard<std::recursive_mutex> lock(mutex->_mutex);
    return mutex->_queue.at(index)->timer;
}


std::function<void()>
lifetime_test_access::late_timeout(const std::shared_ptr<mutex>& mutex, const std::size_t index)
{
    const auto request = mutex->_queue.at(index);
    return [mutex, request] { mutex->_lockTimeout(request); };
}


bool lifetime_test_access::try_lock(const client_ptr& client, const std::string& name)
{
    return client->_tryLock(name);
}


bool lifetime_test_access::lock(const client_ptr& client, const std::string& name)
{
    return client->_acquireLock(name);
}


bool lifetime_test_access::timed_lock(const client_ptr& client, const std::string& name)
{
    return client->_timedAcquireLock(name, std::chrono::seconds(1));
}

} // namespace lkd::detail

namespace ldt {

std::size_t fill_unread_replies(const lkd::client_ptr& client, boost::asio::io_context& service)
{
    // Windows can accept a whole 96 KiB reply despite a small configured socket buffer. Fill
    // the kernel's actual capacity with bounded replies, stopping once 96 KiB remains queued.
    const std::string reply = std::string(16 * 1024 - 1, 'x') + "\n";
    std::size_t count = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (lkd::detail::lifetime_test_access::pending_reply_bytes(client) < 96 * 1024) {
        if (++count > 1024 || std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("Could not fill the loopback socket within the test bound");
        lkd::detail::lifetime_test_access::send(client, reply);
        service.poll();
        if (!client->socket().is_open())
            throw std::runtime_error("Connection closed while filling the loopback socket");
    }
    return count;
}


protocol_connection::protocol_connection(
        boost::asio::io_context& service,
        const boost::asio::ip::tcp::endpoint& endpoint,
        const int receive_buffer_size
)
        : _service(service)
        , _socket(service)
{
    // Set a small receive window before connecting when a test must hold a server write pending.
    // Changing it after connect can leave the peer using the larger window already advertised.
    if (receive_buffer_size != 0) {
        _socket.open(endpoint.protocol());
        _socket.set_option(boost::asio::socket_base::receive_buffer_size(receive_buffer_size));
    }
    _socket.connect(endpoint);
    _socket.non_blocking(true);
}


protocol_connection::~protocol_connection()
{
    close();
}


void protocol_connection::close()
{
    boost::system::error_code ignored;

    // NOLINTNEXTLINE(bugprone-unused-return-value)
    static_cast<void>(_socket.close(ignored));
}


void protocol_connection::send(const std::string& message)
{
    size_t sent = 0;
    poll_until(
            _service,
            [&] {
                boost::system::error_code error;
                sent += _socket.write_some(
                        boost::asio::buffer(message.data() + sent, message.size() - sent), error
                );
                if (error && error != boost::asio::error::would_block &&
                    error != boost::asio::error::try_again)
                    throw boost::system::system_error(error);
                return sent == message.size();
            },
            "loopback command send"
    );
}


std::string protocol_connection::line()
{
    poll_until(
            _service,
            [&] {
                if (_received.find('\n') != std::string::npos)
                    return true;
                _readAvailable();
                return _received.find('\n') != std::string::npos;
            },
            "loopback reply line"
    );
    const auto end = _received.find('\n');
    auto result = _received.substr(0, end);
    _received.erase(0, end + 1);
    if (!result.empty() && result.back() == '\r')
        result.pop_back();
    return result;
}


void protocol_connection::_readAvailable()
{
    char buffer[4096];
    boost::system::error_code error;
    const auto count = _socket.read_some(boost::asio::buffer(buffer), error);
    if (error && error != boost::asio::error::would_block && error != boost::asio::error::try_again)
        throw boost::system::system_error(error);
    _received.append(buffer, count);
}

} // namespace ldt

namespace ldt {

network_fixture::network_fixture()
        : server("127.0.0.1", 0, 1, lkd::rr_no_report_running)
{
}


network_fixture::~network_fixture()
{
    // Destroy all accepted clients while the server and its locks still exist. In particular,
    // io_service destruction is too late for a client's destructor to notify its server.
    for (const auto& connection : connections)
        connection->close();
    lkd::detail::lifetime_test_access::cancel_accept_retry(server);
    lkd::detail::lifetime_test_access::close_listener(server);
    try {
        poll_until(server.io_service(), [&] { return server.client_count() == 0; });
        server.io_service().poll();
    }
    catch (const std::exception& error) {
        BOOST_ERROR("Failed to drain test daemon: " << error.what());
    }
}


std::shared_ptr<protocol_connection> network_fixture::connect(const int receive_buffer_size)
{
    // Preserve move semantics and don't make this const, even though it should be const.
    auto connection = std::make_shared<protocol_connection>(
            server.io_service(),
            lkd::detail::lifetime_test_access::endpoint(server),
            receive_buffer_size
    );
    connections.push_back(connection);
    BOOST_REQUIRE_MESSAGE(connection->line() == "LOCKD", "Daemon sends its greeting");
    return connection;
}

} // namespace ldt

namespace ldt {

server_fixture::server_fixture()
        : server("127.0.0.1", 0, 1, lkd::rr_no_report_running)
{
    lkd::detail::lifetime_test_access::close_listener(server);
}


server_fixture::~server_fixture()
{
    // Also drain failed writes if an assertion unwinds before a test's explicit cleanup.
    server.io_service().poll();
}

} // namespace ldt

namespace ldt {

running_server_fixture::running_server_fixture()
        : thread([this] { server.io_service().run(); })
{
}


running_server_fixture::~running_server_fixture()
{
    // All DLL handles in the test body have already gone. Wait for their EOF callbacks before
    // stopping the worker; the base fixture then cancels and drains the unaccepted client.
    try {
        ldt::poll_until(server.io_service(), [&] { return server.client_count() == 0; });
    }
    catch (const std::exception& error) {
        BOOST_ERROR("DLL connections did not close: " << error.what());
    }
    server.io_service().stop();
    thread.join();
    server.io_service().restart();
}

} // namespace ldt

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
 * File       : scripted_peer.cpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Provide a scripted loopback peer for client protocol tests.
 * Start Date : 10/21/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>

#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/system/error_code.hpp> // IWYU pragma: keep
#include <boost/system/system_error.hpp>

#include "../include/scripted_peer.hpp"

namespace ldt {

template <typename Predicate> void scripted_peer::_wait(const Predicate& ready)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!ready()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("Timed out waiting for scripted peer operation");
        std::this_thread::yield();
    }
}


scripted_peer::scripted_peer(const std::function<void(tcp::socket&)>& script)
        : _acceptor(_service, tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0))
        , _port(_acceptor.local_endpoint().port())
        , _thread([this, script] {
            try {
                tcp::socket socket(_service);
                _acceptor.non_blocking(true);
                _wait([&] {
                    boost::system::error_code error;

                    // NOLINTNEXTLINE(bugprone-unused-return-value)
                    static_cast<void>(_acceptor.accept(socket, error));
                    _check(error);
                    return !error;
                });
                socket.non_blocking(true);
                socket.set_option(tcp::no_delay(true));
                script(socket);
            }
            catch (...) {
                _error = std::current_exception();
            }
        })
{
}


scripted_peer::~scripted_peer()
{
    if (_thread.joinable())
        _thread.join();
}


std::uint16_t scripted_peer::port() const
{
    return _port;
}


void scripted_peer::finish()
{
    _thread.join();
    if (_error)
        std::rethrow_exception(_error);
}


void scripted_peer::send(tcp::socket& socket, const std::string& message)
{
    size_t sent = 0;
    _wait([&] {
        boost::system::error_code error;
        sent += socket.write_some(
                boost::asio::buffer(message.data() + sent, message.size() - sent), error
        );
        _check(error);
        return sent == message.size();
    });
}


std::string scripted_peer::line(tcp::socket& socket)
{
    std::string result;
    _wait([&] {
        char byte;
        boost::system::error_code error;
        if (socket.read_some(boost::asio::buffer(&byte, 1), error))
            result += byte;
        _check(error);
        return !result.empty() && result.back() == '\n';
    });
    result.pop_back();
    if (!result.empty() && result.back() == '\r')
        result.pop_back();
    return result;
}


void scripted_peer::expect(tcp::socket& socket, const std::string& command)
{
    const auto received = line(socket);
    if (received != command)
        throw std::runtime_error("Expected '" + command + "', received '" + received + "'");
}


void scripted_peer::expect_disconnect(tcp::socket& socket)
{
    _wait([&] {
        char byte;
        boost::system::error_code error;
        const auto count = socket.read_some(boost::asio::buffer(&byte, 1), error);
        if (error == boost::asio::error::eof || error == boost::asio::error::connection_reset)
            return true;
        _check(error);
        if (count)
            throw std::runtime_error("Unexpected command before disconnect");
        return false;
    });
}


void scripted_peer::_check(const boost::system::error_code& error)
{
    if (error && error != boost::asio::error::would_block && error != boost::asio::error::try_again)
        throw boost::system::system_error(error);
}

} // namespace ldt

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
 * File       : scripted_peer.hpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Provide a scripted loopback peer for client protocol tests.
 * Start Date : 10/19/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#if !defined(LOCKD_TEST_SCRIPTED_PEER_HPP_INCLUDED_)
# define LOCKD_TEST_SCRIPTED_PEER_HPP_INCLUDED_

# if defined(_MSC_VER) && (_MSC_VER >= 1020)
#  pragma once
# endif

# include <cstdint>
# include <exception>
# include <functional>
# include <string>
# include <thread>

# include <boost/asio.hpp>

namespace ldt {

// A small mock of the remote endpoint, not of client decisions. All accepts, reads, and writes
// are bounded, including cleanup after a failed assertion on the test thread. Exceptions from
// the script are reported by finish(), rather than using Boost.Test assertions on a worker.
class scripted_peer
{
    /* typedefs */
public:
    typedef boost::asio::ip::tcp tcp;

    /* construction and destruction */
public:
    explicit scripted_peer(const std::function<void(tcp::socket&)>& script);
    virtual ~scripted_peer();

    /* public methods */
public:
    [[nodiscard]] std::uint16_t port() const;
    void finish();
    static void send(tcp::socket& socket, const std::string& message);
    static std::string line(tcp::socket& socket);
    static void expect(tcp::socket& socket, const std::string& command);
    static void expect_disconnect(tcp::socket& socket);

    /* private methods */
private:
    static void _check(const boost::system::error_code& error);
    template <typename Predicate> static void _wait(const Predicate& ready);

    /* private data members */
private:
    boost::asio::io_context _service;
    tcp::acceptor _acceptor;
    const std::uint16_t _port;
    std::exception_ptr _error;
    std::thread _thread;
};

} // namespace ldt

#endif // !defined(LOCKD_TEST_SCRIPTED_PEER_HPP_INCLUDED_)

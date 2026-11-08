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
 * File       : lockd_test_access.cpp
 * Author     : Brett Gmoser
 * Part Of    : liblockd, a client library for interfacing with lockd.
 * Purpose    : Expose protected C++ client operations to the test suite.
 * Start Date : 08/07/2023
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <boost/asio/error.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <liblockd/mutex.hpp>

#include "../../include/liblockd/detail/lockd_test_access.hpp"

bool lld::detail::lockd_test_access<lld::mutex>::peer_closed() const
{
    // Asio's header-only categories have separate instances in Windows DLLs. Compare inside
    // liblockd, where the read recorded its error, so EOF keeps its category identity.
    return error() == boost::asio::error::eof || error() == boost::asio::error::connection_reset;
}


void lld::detail::lockd_test_access<lld::mutex>::shutdown_send() const
{
    // Leave the descriptor open and the client's error state unchanged. The next real socket
    // write must fail at the OS boundary, independently of peer timing and kernel buffer sizes.
    _mutexPtr->_socket.shutdown(boost::asio::ip::tcp::socket::shutdown_send);
}

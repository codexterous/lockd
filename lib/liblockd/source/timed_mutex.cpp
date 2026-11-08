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
 * File       : timed_mutex.cpp
 * Author     : Brett Gmoser
 * Part Of    : liblockd, a client library for interfacing with lockd.
 * Purpose    : Provide a network-backed C++ mutex with timed acquisition.
 * Start Date : 11/19/2012
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <chrono>
#include <cstdint>
#include <string>

// The public Format header supplies the template definitions used by free_funcs.hpp.
#include <boost/format.hpp> // IWYU pragma: keep
#include <boost/format/free_funcs.hpp>

#include <liblockd/detail/source_path.hpp>
#include <liblockd/mutex.hpp>

#include "../include/liblockd/detail/client_contract.hpp"
#include "../include/liblockd/exceptions.hpp"
#include "../include/liblockd/timed_mutex.hpp"

lld::timed_mutex::timed_mutex(
        const std::string& hostname,
        const std::uint16_t port,
        const std::chrono::seconds& connect_timeout,
        const std::string& lock_name
)
        : lld::mutex(hostname, port, connect_timeout, lock_name)
{
}


bool lld::timed_mutex::_timedLock(const std::string& time_spec)
{
    // Attempt to lock the mutex in a blocking fashion, blocking forever until the server grants us
    // the lock or sends us a timeout message. First, write our command.
    blocking_write(str(boost::format("TIMED_LOCK %1% %2%\n") % _lockName % time_spec));

    // The first reply we'll get from the server is "LOCK_ACQUIRING", acknowledging the request. Use
    // a reasonable 10-second timeout for this read - server should reply right away.
    const auto first_reply = blocking_read_line(lld::detail::timed_ack_timeout);

    if (first_reply != "LOCK_ACQUIRING") {
        // This reply isn't at all what we expected.
        throw lld::exception(
                lld::exception::et_unexpected,
                LOCKD_SOURCE_FILE,
                __LINE__,
                __FUNCTION__,
                str(boost::format("lockd returned unexpected response (%1%)") % first_reply)
        );
    }

    // The daemon enforces the requested lock-wait budget and replies with a grant or timeout.
    // This read has no additional client deadline; transport failures still close the connection.
    const auto second_reply = blocking_read_line();
    if (second_reply == "LOCK_ACQUIRED") {
        // We're now locked.
        _locked = true;
        return true;
    }

    if (second_reply == "LOCK_TIMED_OUT") {
        // The lock attempt timed out!
        _locked = false;
        return false;
    }

    // This reply isn't at all what we expected.
    throw lld::exception(
            lld::exception::et_unexpected,
            LOCKD_SOURCE_FILE,
            __LINE__,
            __FUNCTION__,
            str(boost::format("lockd returned unexpected response (%1%)") % second_reply)
    );
}

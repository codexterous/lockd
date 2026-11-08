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
 * File       : client.hpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Per-connection command parsing, lock ownership, and reply delivery.
 * Start Date : 03/26/2012
 *
 * Description: The read buffer caps each command at 1,024 wire bytes before line editing.
 *              Immediate LOCK and TIMED_LOCK successes enqueue their acknowledgement and grant
 *              in one owned reply buffer. Queued requests acknowledge immediately after queue
 *              insertion, then receive only the grant on handoff. TRY_LOCK has no acknowledgement.
 *              Acquisition decisions run with the client mutex held, so a concurrent handoff or
 *              expiry cannot enqueue a terminal reply before the queued acknowledgement.
 *              Batching uses the existing write queue, byte caps, partial-write handling and
 *              progress deadline. It changes no reply text, framing, socket options or timeout policy.
 *              A 30-second command deadline starts at the first byte and is not extended by
 *              trickle input. Idle connections and complete lock requests have no read deadline.
 *              Already buffered complete commands bypass read deadlines and composed reads.
 *              Complete commands use bounded batches of 32 before yielding; _readPending
 *              covers that continuation so write callbacks cannot start an overlapping read.
 *
 *              Reply accounting retains each whole queued string, including the active write,
 *              until that string completes. Reads pause at 64 KiB and resume at 32 KiB, below the
 *              128 KiB hard cap so an in-flight read or lock notification still has room. Writes
 *              submit at most 16 KiB at once; only written bytes restart the 30-second progress
 *              deadline. These are timestamps, checked under the client mutex by the server
 *              every five seconds. Normal replies never arm or cancel a per-client timer.
 *              A check reads current deadlines, so completed work cannot expire a later command
 *              or write. No pending work means no deadline, including between lock scopes.
 *
 *              Each connection stores one immutable diagnostic ID shared by INFO and logs.
 *              A fresh Boost.UUID random generator supplies OS-backed entropy; the first 44
 *              random bits precede the UUID's version and variant fields and fit 11 hex digits.
 *              Generation failures propagate instead of falling back to addresses or timestamps.
 *              CMake verifies UUID support and links bcrypt on Windows. Labels can collide and
 *              must never be used for authorization or ownership decisions.
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#if !defined(LOCKD_LOCKD_CLIENT_HPP_INCLUDED_)
# define LOCKD_LOCKD_CLIENT_HPP_INCLUDED_

# if defined(_MSC_VER) && (_MSC_VER >= 1020)
#  pragma once
# endif

# include <chrono>
# include <cstddef>
# include <deque>
# include <memory>
# include <mutex>
# include <optional>
# include <string>
# include <utility>

# include <boost/asio.hpp>
# include <boost/system/error_code.hpp> // IWYU pragma: keep

# include <liblockd/mutex.hpp>

# include "../include/new_shared_ptr.hpp"

namespace lkd {

// forward declarations
class server;
class mutex;
namespace detail {

class lifetime_test_access;

class match_char
{
    /* construction and destruction */
public:
    explicit match_char(std::string characters);
    virtual ~match_char();

    /* public methods */
public:
    template <typename Iterator>
    std::pair<Iterator, bool> operator()(const Iterator& begin, const Iterator& end) const;

    /* private data members */
private:
    const std::string _characters;
};

} // namespace detail

class client : public std::enable_shared_from_this<client>
{
    /* enumerations */
public:
    typedef enum
    {
        cs_waiting = 0,
        cs_gone,
        cs_connected
    } client_status_t;

    /* discarded methods */
public:
    client(const client&) = delete;
    client& operator=(const client&) = delete;

    /* construction and destruction, private so can only be accessed by new_ptr */
private:
    explicit client(server& sref);
    virtual ~client();
    friend class lkd::new_ptr_access;

    /* public methods */
public:
    void start();
    void shutdown(const bool force = false);
    void check_deadlines(const std::chrono::steady_clock::time_point now);
    const std::string& identifier() const;
    boost::asio::ip::tcp::socket& socket() { return _socket; }
    const boost::asio::ip::tcp::socket& socket() const { return _socket; }

    // Return false during shutdown so immediate acquisitions release ownership and queue handoff
    // skips this client instead of granting a lock that it can no longer use.
    // Only immediate LOCK/TIMED_LOCK callers request a combined acknowledgement. Queue handoff
    // and TRY_LOCK retain the default grant-only reply; repeating an acknowledgement breaks framing.
    bool lock_acquired(const std::string& lock_name, const bool acknowledge = false);

    // Called by a mutex when the timer to scheduled interval for lock timeout has passed. We return
    // true if we have acquired the lock, so that the mutex can take us out of its queue.
    bool lock_timeout();

    /* private methods */
private:
    friend class detail::lifetime_test_access;
    static std::string _createIdentifier();
    void _scheduleRead();
    void _readCommand();
    void _processCommand(const std::size_t bytes_transferred);
    void _closeConnection();
    void _cancelPendingLock();
    void _scheduleWrite(const std::string& message);
    void _writeNext();
    bool _tryLock(const std::string& lock_name);
    bool _acquireLock(const std::string& lock_name);
    template <typename D> bool _timedAcquireLock(const std::string& lock_name, const D& wait_time);
    void _releaseLock();

    /* private data members */
private:
    server& _server;
    const std::string _identifier;
    boost::asio::ip::tcp::socket _socket;
    boost::asio::streambuf _buffer;
    std::chrono::steady_clock::time_point _commandDeadline;
    bool _readPending;
    std::deque<std::string> _pendingWrites;
    std::size_t _pendingWriteBytes;
    std::size_t _writeOffset;
    std::chrono::steady_clock::time_point _writeDeadline;
    bool _readsPaused;
    lld::mutex::lock_status_t _lockStatus;
    std::optional<std::string> _currentLock;
    std::weak_ptr<mutex> _pendingMutex;
    bool _destructing;
    bool _started;
    bool _draining;
    mutable std::recursive_mutex _mutex;
}; // class client

typedef std::shared_ptr<client> client_ptr;
typedef std::weak_ptr<client> weak_client_ptr;

} // namespace lkd

#endif // !defined(LOCKD_LOCKD_CLIENT_HPP_INCLUDED_)

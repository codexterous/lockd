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
 * File       : client.cpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Per-connection command parsing, lock ownership, and reply delivery.
 * Start Date : 03/26/2012
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <exception>
#include <ios>
#include <istream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/buffers_iterator.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read_until.hpp> // IWYU pragma: keep

// The public Format header supplies the template definitions used by free_funcs.hpp.
#include <boost/format.hpp> // IWYU pragma: keep
#include <boost/format/free_funcs.hpp>
#include <boost/system/error_code.hpp> // IWYU pragma: keep
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid.hpp>

// The grammar uses X3 operators and directives supplied by this public umbrella.
#include <boost/spirit/home/x3.hpp> // IWYU pragma: keep
#include <boost/spirit/home/x3/auxiliary/eoi.hpp>
#include <boost/spirit/home/x3/char/char.hpp>
#include <boost/spirit/home/x3/char/char_class.hpp>
#include <boost/spirit/home/x3/core/parse.hpp>
#include <boost/spirit/home/x3/directive/no_case.hpp>
#include <boost/spirit/home/x3/directive/omit.hpp>
#include <boost/spirit/home/x3/directive/raw.hpp>
#include <boost/spirit/home/x3/numeric/uint.hpp>
#include <boost/spirit/home/x3/string/literal_string.hpp>

#include <liblockd/detail/lock_name.hpp>
#include <liblockd/mutex.hpp>

#include "../include/client.hpp"
#include "../include/logging.hpp"
#include "../include/server.hpp"
#include "../include/version.hpp"

namespace {

// Count wire bytes, including whitespace, editing bytes, and the delimiter. Canonical commands
// fit comfortably; names still have their separate 256-byte limit.
constexpr std::size_t MAX_COMMAND_BYTES = 1024;

// A pipelined connection must yield to other I/O rather than process an unbounded batch in
// one callback. Complete commands need no deadline, including across these continuations.
constexpr std::size_t MAX_BUFFERED_COMMANDS_PER_DISPATCH = 32;

// Start with the first byte, not connection acceptance. Trickle input must not extend this
// deadline, but idle connections and clients holding or waiting for locks may live indefinitely.
constexpr auto COMMAND_READ_TIMEOUT = std::chrono::seconds(30);

// Pause before the hard cap so a command already being read, or an asynchronous lock notification,
// still has room for its replies. Count whole strings until their active write has completed.
constexpr std::size_t PAUSE_READS_REPLY_BYTES = 64 * 1024;
constexpr std::size_t RESUME_READS_REPLY_BYTES = 32 * 1024;
constexpr std::size_t MAX_PENDING_REPLY_BYTES = 128 * 1024;

// Some socket implementations complete the whole submitted buffer at once. Keep submissions
// small so a large retained reply can report progress before the entire string has been sent.
constexpr std::size_t MAX_WRITE_CHUNK_BYTES = 16 * 1024;

// This is a lack-of-progress deadline. Only bytes actually written restart it; enqueuing more
// replies must not keep an unread connection and its lock ownership alive indefinitely.
constexpr auto WRITE_PROGRESS_TIMEOUT = std::chrono::seconds(30);

// Keep connection labels within the existing 0x + 11 hexadecimal digit display width.
constexpr std::size_t CLIENT_IDENTIFIER_HEX_DIGITS = 11;

} // namespace


lkd::client::client(lkd::server& sref)
        : _server(sref)
        , _identifier(_createIdentifier())
        , _socket(_server.io_service())
        , _buffer(MAX_COMMAND_BYTES)
        , _commandDeadline(std::chrono::steady_clock::time_point::max())
        , _readPending(false)
        , _pendingWriteBytes(0)
        , _writeOffset(0)
        , _writeDeadline(std::chrono::steady_clock::time_point::max())
        , _readsPaused(false)
        , _lockStatus(lld::mutex::ls_none)
        , _destructing(false)
        , _started(false)
        , _draining(false)
{
}


lkd::client::~client()
{
    // Set this flag so that _releaseLock knows not to do anything which would use
    // shared_from_this() (like schedule a write).
    _destructing = true;

    // A queued acquisition has no owned lock to release, but its request still needs removal.
    _cancelPendingLock();

    // Client may destruct while holding lock.
    _releaseLock();

    // Notify the server that we've destructed, so it can keep track of statistics.
    if (_started)
        _server.client_closed(*this);
}


std::string lkd::client::_createIdentifier()
{
    // Use a fresh generator for each connection so there is no shared generator state to lock.
    // Entropy failures propagate; never fall back to an address, timestamp, or weaker generator.
    const boost::uuids::uuid uuid = boost::uuids::random_generator()();
    constexpr char digits[] = "0123456789abcdef";
    std::string identifier = "0x";
    identifier.reserve(2 + CLIENT_IDENTIFIER_HEX_DIGITS);

    // The first 44 UUID bits are random; version and variant bits occur later. Keep the
    // historical display width. These diagnostic labels may collide and confer no authority.
    for (const auto byte : uuid) {
        identifier += digits[byte >> 4];
        if (identifier.size() == 2 + CLIENT_IDENTIFIER_HEX_DIGITS)
            break;
        identifier += digits[byte & 0x0f];
    }
    return identifier;
}


const std::string& lkd::client::identifier() const
{
    // INFO and logs share this immutable value, including during destruction. Keeping it
    // independent of the address also gives a new identity when an allocator reuses storage.
    return _identifier;
}


bool lkd::client::lock_timeout()
{
    const std::lock_guard<std::recursive_mutex> our_lock(_mutex);
    if (_lockStatus == lld::mutex::ls_acquiring) {
        // If we're still acquiring, and the timeout has hit, then we're not going to get it in
        // time.
        _lockStatus = lld::mutex::ls_none;
        _pendingMutex.reset();
        _scheduleWrite("LOCK_TIMED_OUT\r\n");
        return false;
    }

    // Let the mutex know that we no longer require the timeout services of it.
    return true;
}


void lkd::client::start()
{
    // Failed accepts also destroy client objects, but they were never counted by the server.
    _started = true;

    // Send a welcome message.
    _scheduleWrite("LOCKD\r\n");

    // Schedule our first read.
    _scheduleRead();
}


void lkd::client::shutdown(const bool force)
{
    const std::lock_guard<std::recursive_mutex> lock(_mutex);
    _draining = true;
    if (force || _lockStatus != lld::mutex::ls_locked)
        _closeConnection();
}


lkd::detail::match_char::match_char(std::string characters)
        : _characters(std::move(characters))
{
}


lkd::detail::match_char::~match_char() = default;


template <typename Iterator>
std::pair<Iterator, bool>
lkd::detail::match_char::operator()(const Iterator& begin, const Iterator& end) const
{
    Iterator i = begin;
    while (i != end) {
        for (const auto& pos : _characters)
            if (pos == *i)
                return std::make_pair(++i, true);
        ++i;
    }
    return std::make_pair(i, false);
}

template <> struct boost::asio::is_match_condition<lkd::detail::match_char> : public std::true_type
{
}; // namespace boost::asio


void lkd::client::_scheduleRead()
{
    const std::lock_guard<std::recursive_mutex> lock(_mutex);
    if (_readPending || _readsPaused || !_started || !_socket.is_open() ||
        (_draining && _lockStatus != lld::mutex::ls_locked))
        return;

    // Write completions can resume reads independently of the previous read's completion. Keep
    // this flag set through both read stages so those paths cannot start overlapping operations.
    _readPending = true;

    // Wait without a deadline until a command starts. Buffered bytes may belong to the next
    // command in a batch; don't read over them or give them an unlimited wait for their suffix.
    if (_buffer.size() != 0) {
        _readCommand();
        return;
    }

    _socket.async_read_some(
            _buffer.prepare(MAX_COMMAND_BYTES),
            [client_ptr = shared_from_this()](
                    const boost::system::error_code& error, const std::size_t bytes_transferred
            ) {
                const std::lock_guard<std::recursive_mutex> lock(client_ptr->_mutex);
                client_ptr->_buffer.commit(bytes_transferred);
                if (error || !client_ptr->_socket.is_open()) {
                    client_ptr->_readPending = false;
                    client_ptr->_closeConnection();
                    return;
                }
                client_ptr->_readCommand();
            }
    );
}


void lkd::client::_readCommand()
{
    if (!_socket.is_open() || (_draining && _lockStatus != lld::mutex::ls_locked)) {
        _readPending = false;
        return;
    }

    // The first read often already contains the delimiter. Avoid creating a deadline and an
    // immediately completed read_until operation for those commands. Reuse the slow path's
    // exact delimiter matcher so framing and the wire-byte limit remain the same.
    const detail::match_char match("\n\x04\x1a");
    for (std::size_t processed = 0;; ++processed) {
        const auto data = _buffer.data();
        const auto begin = boost::asio::buffers_begin(data);
        const auto found = match(begin, boost::asio::buffers_end(data));
        if (!found.second)
            break;
        if (processed == MAX_BUFFERED_COMMANDS_PER_DISPATCH) {
            // Keep _readPending set while yielding. A write completion that resumes reads must
            // not start a second operation against the same buffered input.
            boost::asio::post(_server.io_service(), [client_ptr = shared_from_this()] {
                const std::lock_guard<std::recursive_mutex> lock(client_ptr->_mutex);
                client_ptr->_readCommand();
            });
            return;
        }
        _processCommand(static_cast<std::size_t>(std::distance(begin, found.first)));
        if (_readsPaused || !_socket.is_open() ||
            (_draining && _lockStatus != lld::mutex::ls_locked)) {
            _readPending = false;
            return;
        }
    }
    if (_buffer.size() == 0) {
        _readPending = false;
        _scheduleRead();
        return;
    }

    // Only an unfinished command needs further socket input and a first-byte deadline.
    // Reaching a partial tail after the batch limit also starts its deadline here, rather
    // than deferring it behind a posted continuation intended only for complete commands.
    _commandDeadline = std::chrono::steady_clock::now() + COMMAND_READ_TIMEOUT;

    // Read until \n, \x04 (CTRL+D), or \x1a (CTRL+Z).
    boost::asio::async_read_until(
            _socket,
            _buffer,
            detail::match_char("\n\x04\x1a"),
            [client_ptr = shared_from_this()](
                    const boost::system::error_code& error, const std::size_t bytes_transferred
            ) {
                const std::lock_guard<std::recursive_mutex> lock(client_ptr->_mutex);
                client_ptr->_readPending = false;
                if (error || !client_ptr->_socket.is_open()) {
                    if (error == boost::asio::error::not_found)
                        console_print(
                                cp_stdlog,
                                ll_error,
                                str(boost::format(
                                            "Command from client %1% exceeds the %2%-byte "
                                            "wire limit; closing connection\n"
                                    ) %
                                    client_ptr->identifier() % MAX_COMMAND_BYTES)
                        );
                    client_ptr->_closeConnection();
                    return;
                }

                // Maintenance observes this timestamp under the same mutex. Clear it before
                // processing the command, so a completed request never expires during idle work.
                client_ptr->_commandDeadline = std::chrono::steady_clock::time_point::max();

                client_ptr->_processCommand(bytes_transferred);
                client_ptr->_scheduleRead();
            }
    );
}


void lkd::client::check_deadlines(const std::chrono::steady_clock::time_point now)
{
    const std::lock_guard<std::recursive_mutex> lock(_mutex);
    if (!_socket.is_open())
        return;

    // Inspect current state while holding the I/O handlers' mutex. A sweep can overlap a
    // completion, but cannot act on a deadline that completion has already cleared or renewed.
    // time_point::max() means disabled, including for indefinitely idle owners and waiters.
    const auto no_deadline = std::chrono::steady_clock::time_point::max();
    if (_commandDeadline != no_deadline && now >= _commandDeadline) {
        console_print(
                cp_stdlog,
                ll_error,
                str(boost::format(
                            "Incomplete command from client %1% exceeded the %2%-second "
                            "read deadline; closing connection\n"
                    ) %
                    identifier() % COMMAND_READ_TIMEOUT.count())
        );
        _closeConnection();
        return;
    }
    if (!_pendingWrites.empty() && _writeDeadline != no_deadline && now >= _writeDeadline) {
        console_print(
                cp_stdlog,
                ll_error,
                str(boost::format(
                            "Replies to client %1% made no write progress for %2% seconds; "
                            "closing connection\n"
                    ) %
                    identifier() % WRITE_PROGRESS_TIMEOUT.count())
        );
        _closeConnection();
    }
}


void lkd::client::_scheduleWrite(const std::string& message)
{
    const std::lock_guard<std::recursive_mutex> lock(_mutex);
    if (message.empty() || (_draining && !_socket.is_open()))
        return;

    // The active write still views its queue entry. Enforce the cap before copying another
    // string, and leave all queued storage alive until that operation's completion callback.
    if (message.size() > MAX_PENDING_REPLY_BYTES - _pendingWriteBytes) {
        console_print(
                cp_stdlog,
                ll_error,
                str(boost::format(
                            "Replies to client %1% exceed the %2%-byte queue limit; "
                            "closing connection\n"
                    ) %
                    identifier() % MAX_PENDING_REPLY_BYTES)
        );
        _closeConnection();
        return;
    }

    const bool write_in_progress = !_pendingWrites.empty();
    _pendingWrites.push_back(message);
    _pendingWriteBytes += message.size();
    if (_pendingWriteBytes >= PAUSE_READS_REPLY_BYTES)
        _readsPaused = true;

    // Asio keeps a view of this data until completion. The caller's message can be a temporary;
    // our queue owns the string until the completion callback removes it.
    if (!write_in_progress)
        _writeNext();
}


void lkd::client::_writeNext()
{
    // Both callers hold _mutex. Keep exactly one write active so replies remain ordered and
    // nothing removes the buffer while Asio still uses it. Partial completions let the deadline
    // follow real socket progress, rather than treating a slowly delivered reply as stalled.
    // This runs only for the first queued reply or after a successful partial write. Appending
    // behind an active write never reaches here, so queued replies cannot renew stalled output.
    _writeDeadline = std::chrono::steady_clock::now() + WRITE_PROGRESS_TIMEOUT;
    const auto& message = _pendingWrites.front();
    _socket.async_write_some(
            boost::asio::buffer(
                    message.data() + _writeOffset,
                    std::min(message.size() - _writeOffset, MAX_WRITE_CHUNK_BYTES)
            ),
            [client_ptr = shared_from_this()](
                    const boost::system::error_code& error, const std::size_t bytes_transferred
            ) {
                const std::lock_guard<std::recursive_mutex> lock(client_ptr->_mutex);
                if (error || bytes_transferred == 0 || !client_ptr->_socket.is_open()) {
                    client_ptr->_closeConnection();

                    // The operation has completed, so Asio no longer views the active string.
                    client_ptr->_pendingWrites.clear();
                    client_ptr->_pendingWriteBytes = 0;
                    client_ptr->_writeOffset = 0;
                    return;
                }

                client_ptr->_writeOffset += bytes_transferred;
                if (client_ptr->_writeOffset == client_ptr->_pendingWrites.front().size()) {
                    client_ptr->_pendingWriteBytes -= client_ptr->_pendingWrites.front().size();
                    client_ptr->_pendingWrites.pop_front();
                    client_ptr->_writeOffset = 0;
                }
                if (!client_ptr->_pendingWrites.empty())
                    client_ptr->_writeNext();
                else {
                    client_ptr->_writeDeadline = std::chrono::steady_clock::time_point::max();
                    if (client_ptr->_draining && client_ptr->_lockStatus != lld::mutex::ls_locked)
                        client_ptr->_closeConnection();
                }

                if (client_ptr->_readsPaused &&
                    client_ptr->_pendingWriteBytes <= RESUME_READS_REPLY_BYTES) {
                    client_ptr->_readsPaused = false;
                    client_ptr->_scheduleRead();
                }
            }
    );
}


namespace {

typedef enum
{
    ct_lock,
    ct_try_lock,
    ct_timed_lock,
    ct_unlock,
    ct_version,
    ct_info,
    ct_quit,
    ct_invalid
} command_type_t;


} // namespace

void lkd::client::_closeConnection()
{
    // Reject a queued grant while canceled I/O still retains this object. Disable both deadlines
    // so repeated maintenance checks have no work left. An active write still views the queue's
    // first string; its completion owns cleanup after socket cancellation has finished.
    _draining = true;
    _cancelPendingLock();
    _commandDeadline = std::chrono::steady_clock::time_point::max();
    _writeDeadline = std::chrono::steady_clock::time_point::max();

    // http://www.boost.org/doc/libs/1_43_0/doc/html/boost_asio/reference/basic_stream_socket/close/overload2.html
    // Remarks
    //
    // For portable behavior with respect to graceful closure of a connected
    // socket, call shutdown() before closing the socket.
    boost::system::error_code ec;

    // NOLINTNEXTLINE(bugprone-unused-return-value)
    static_cast<void>(_socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec));

    // NOLINTNEXTLINE(bugprone-unused-return-value)
    static_cast<void>(_socket.close(ec));
}


void lkd::client::_cancelPendingLock()
{
    const auto mutex = _pendingMutex.lock();
    _pendingMutex.reset();
    if (!mutex)
        return;

    // Handoff takes the queue lock before the client lock. Close is called with the client
    // lock held, so defer removal rather than taking those locks in reverse order. Weak owner
    // identity still matches after destruction, without retaining this client or using its address.
    boost::asio::post(_server.io_service(), [mutex, client = weak_from_this()] {
        mutex->cancel(client);
    });
}


void lkd::client::_processCommand(const std::size_t bytes_transferred)
{
    namespace x3 = boost::spirit::x3;
    using x3::char_;
    using x3::eoi;
    using x3::no_case;
    using x3::omit;
    using x3::space;
    using x3::string;
    using x3::ulong_;

    try {
        std::istream is(&_buffer);
        std::string line(bytes_transferred, '\0');

        // read_until may have buffered later commands too. Consume only this command,
        // including its delimiter, so an edit can never reach back across a completed line.
        is.read(line.data(), static_cast<std::streamsize>(bytes_transferred));

        // Handle BS and DEL before trimming so spaces can be erased too, and ignore them at
        // the start of the line. Leave terminal echo to the client; escape sequences here
        // would become unsolicited protocol replies.
        std::size_t edited_size = 0;
        for (const auto character : line) {
            if (character == '\b' || character == '\x7f') {
                if (edited_size != 0)
                    --edited_size;
            }
            else
                line[edited_size++] = character;
        }
        line.resize(edited_size);

        // Trim left/right leading whitespace.
        boost::algorithm::trim_if(line, boost::algorithm::is_any_of("\r\n\t "));

        // Command arguments are optional so we can report which required field is missing.
        std::optional<std::string> lock_name;
        std::optional<unsigned long> time;
        std::optional<std::string> time_type;
        command_type_t command = ct_invalid;
        auto beg = line.begin();
        const auto end = line.end();

        const auto set_command = [&command](const command_type_t value) {
            return [&command, value](const auto&) { command = value; };
        };
        const auto capture_text = [](std::optional<std::string>& destination) {
            return [&destination](const auto& context) {
                const auto& text = x3::_attr(context);
                destination = std::string(text.begin(), text.end());
            };
        };
        const auto capture_time = [&time](const auto& context) { time = x3::_attr(context); };

        // Capture fields separately so X3 doesn't treat optional strings as containers on
        // older Boost versions. raw[] preserves the original spelling inside no_case[].

        // @formatter:off
        const bool r = x3::parse(
                beg,
                end,

                // A CTRL+D or CTRL+Z anywhere in the message is interpreted to mean an immediate
                // quit.
                omit[*~char_("\x04\x1a") >> char_("\x04\x1a")[set_command(ct_quit)] >> *char_ >>
                     eoi] |
                        no_case[(omit[(string("LOCK")[set_command(ct_lock)] |
                                       string("TRY_LOCK")[set_command(ct_try_lock)] |
                                       string("TIMED_LOCK")[set_command(ct_timed_lock)])]
                                 // Require the whole name token. Otherwise the timeout-unit
                                 // fallback below could consume a forbidden name suffix.
                                 >>
                                 -(omit[+space] >> x3::raw[+lld::detail::lock_name_characters >>
                                                           &(space | eoi)][capture_text(lock_name)])
                                 // lock time (for TIMED_LOCK)
                                 >> -(omit[+space] >> ulong_[capture_time])
                                 // lock time type for TIMED_LOCK (s/ms/m/h).
                                 >>
                                 -x3::raw
                                         [string("s") | string("ms") | string("m") | string("h") |
                                          +(char_ - (space | eoi)) // this is invalid
        ][capture_text(time_type)]) |
                                omit[(string("UNLOCK") >> eoi)[set_command(ct_unlock)] |
                                     (string("VERSION") >> eoi)[set_command(ct_version)] |
                                     (string("INFO") >> eoi)[set_command(ct_info)] |
                                     (string("QUIT") >> eoi)[set_command(ct_quit)]]] >>
                                eoi |
                        x3::eoi[set_command(ct_quit)]
        );
        // @formatter:on

        if (r) {
            if (ct_lock == command || ct_try_lock == command || ct_timed_lock == command) {
                if (_draining || _server.stopping())
                    _closeConnection();
                else if (!lock_name)
                    _scheduleWrite("MISSING_LOCK_NAME\r\n");
                else if (ct_timed_lock == command && !time)
                    _scheduleWrite("MISSING_LOCK_TIME\r\n");
                else if (time && ct_timed_lock != command)
                    _scheduleWrite("INVALID_ARGS\r\n");

                // Parse the ASCII alphabet above, then report the length separately so
                // oversized names keep their existing LOCK_NAME_TOO_LONG response.
                else if (lock_name.value().length() > lld::detail::max_lock_name_length)
                    _scheduleWrite("LOCK_NAME_TOO_LONG\r\n");
                else {
                    if (_currentLock || lld::mutex::ls_none != _lockStatus) {
                        // Only one lock allowed at a time.
                        _scheduleWrite("UNLOCK_FIRST\r\n");
                    }
                    else {
                        lkd::console_print(
                                lkd::cp_stdlog,
                                lkd::ll_debug,
                                str(boost::format(
                                            " - Lock acquisition request from client "
                                            "%1% for lock with name '%2%'.\n"
                                    ) %
                                    identifier() % lock_name.value())
                        );

                        // Blocking acquisition attempt.
                        if (ct_lock == command) {
                            _lockStatus = lld::mutex::ls_acquiring;
                            _acquireLock(lock_name.value());
                        }

                        // Non-blocking attempt to acquire if immediately available.
                        else if (ct_try_lock == command) {
                            if (!_tryLock(lock_name.value()))
                                _scheduleWrite("LOCK_UNAVAILABLE\r\n");
                        }

                        // Timed acquisition attempt. Waits an amount of seconds for a lock to
                        // become available, eventually failing if it can't get it in time.
                        else if (ct_timed_lock == command) {
                            if (!time_type)
                                time_type = "s";
                            if (boost::algorithm::iequals(time_type.value(), "s"))
                                _timedAcquireLock(
                                        lock_name.value(), std::chrono::seconds(time.value())
                                );
                            else if (boost::algorithm::iequals(time_type.value(), "ms"))
                                _timedAcquireLock(
                                        lock_name.value(), std::chrono::milliseconds(time.value())
                                );
                            else if (boost::algorithm::iequals(time_type.value(), "m"))
                                _timedAcquireLock(
                                        lock_name.value(), std::chrono::minutes(time.value())
                                );
                            else if (boost::algorithm::iequals(time_type.value(), "h"))
                                _timedAcquireLock(
                                        lock_name.value(), std::chrono::hours(time.value())
                                );

                            else
                                _scheduleWrite("INVALID_TIME_TYPE\r\n");
                        }
                    }
                }
            }

            else if (ct_unlock == command) {
                if (lld::mutex::ls_locked != _lockStatus) {
                    // Only one lock allowed at a time.
                    _scheduleWrite("LOCK_FIRST\r\n");
                }
                else {
                    lkd::console_print(
                            lkd::cp_stdlog,
                            lkd::ll_debug,
                            str(boost::format(
                                        " - Lock release request from client %1% for "
                                        "lock with name '%2%'.\n"
                                ) %
                                identifier() % *_currentLock)
                    );

                    // We can unlock immediately.
                    _releaseLock();
                }
            }
            else if (ct_version == command) {
                _scheduleWrite(
                        str(boost::format("lockd %1% built on liblockd %2%\r\n") %
                            lkd::version_string() % lkd::version_string())
                );
            }
            else if (ct_info == command) {
                // NOTE: Do not change this output unless you also change lld::mutex::info to
                //       match the new specification.
                _scheduleWrite(
                        str(boost::format("you are client %1% at %2%\r\n") % identifier() %
                            _socket.remote_endpoint().address().to_string())
                );
                std::string ls;
                switch (_lockStatus) {
                case lld::mutex::ls_none: ls = "none"; break;
                case lld::mutex::ls_acquiring: ls = "acquiring"; break;
                case lld::mutex::ls_locked: ls = "locked"; break;
                default: ls = "unknown";
                }
                _scheduleWrite(
                        str(boost::format("lock status is %1% (lock=%2%)\r\n") % ls %
                            _currentLock.value_or("<none>"))
                );
                _scheduleWrite(str(
                        boost::format("current connected clients: %1%\r\n") % _server.client_count()
                ));
            }

            // Is it a quit command?
            else if (ct_quit == command) {
                _closeConnection();
            }
        }
        else
            _scheduleWrite("INVALID_COMMAND\r\n");
    }
    catch (const std::exception& e) {
        lkd::console_print(
                lkd::cp_stderr,
                lkd::ll_error,
                str(boost::format(" * ERROR in read process for client %1%: %2%\n") % identifier() %
                    e.what())
        );
    }
    catch (...) {
        lkd::console_print(
                lkd::cp_stderr,
                lkd::ll_error,
                str(boost::format(
                            " * ERROR in read process for client %1%: Generic "
                            "exception caught.\n"
                    ) %
                    identifier())
        );
    }
}


bool lkd::client::lock_acquired(const std::string& lock_name, const bool acknowledge)
{
    // Called either by us when we are able to immediately acquire a lock, or by the mutex when the
    // lock is available for us.
    const std::lock_guard<std::recursive_mutex> lock(_mutex);
    if (_draining || _server.stopping())
        return false;
    assert(lld::mutex::ls_locked != _lockStatus);
    console_print(
            cp_stdlog,
            ll_debug,
            str(boost::format(" - Lock with name '%1%' acquired for client %2%\n") % lock_name %
                identifier())
    );
    _currentLock = lock_name;
    _pendingMutex.reset();
    _lockStatus = lld::mutex::ls_locked;

    // Both lines are already known for an immediate acquisition. Own them in one queue entry
    // so they share the first write submission and its watchdog. Don't wait for a queued grant
    // to batch: those clients have already received LOCK_ACQUIRING and need only this result.
    _scheduleWrite(acknowledge ? "LOCK_ACQUIRING\r\nLOCK_ACQUIRED\r\n" : "LOCK_ACQUIRED\r\n");
    return true;
}


bool lkd::client::_tryLock(const std::string& lock_name)
{
    assert(lld::mutex::ls_locked != _lockStatus);

    // Get the mutex we'll be using.
    const auto m = _server.lock_store().get_mutex(lock_name);

    // Attempt to lock it.
    const bool res = m->try_lock();
    if (res && !lock_acquired(lock_name)) {
        m->unlock();
        return false;
    }

    return res;
}


bool lkd::client::_acquireLock(const std::string& lock_name)
{
    assert(lld::mutex::ls_locked != _lockStatus);

    // Get the mutex we'll be using.
    const auto m = _server.lock_store().get_mutex(lock_name);

    // Attempt to lock it.
    const bool res = m->lock(shared_from_this());
    if (!res) {
        _pendingMutex = m;

        // The command handler holds this client's mutex through queue insertion and reply
        // enqueueing. A concurrent handoff also takes it, so its grant cannot overtake the ACK.
        _scheduleWrite("LOCK_ACQUIRING\r\n");
    }
    if (res && !lock_acquired(lock_name, true)) {
        m->unlock();
        return false;
    }
    return res;
}


template <typename D>
bool lkd::client::_timedAcquireLock(const std::string& lock_name, const D& wait_time)
{
    assert(lld::mutex::ls_locked != _lockStatus);

    // Protocol integers may fit their stated unit yet overflow conversion to the timer's
    // finer resolution or addition to its current clock value. Reject them before changing
    // ownership or queuing a request. Cast the remaining range down to the requested unit,
    // so the comparison itself never multiplies an untrusted count.
    const auto remaining =
            std::chrono::steady_clock::time_point::max() - std::chrono::steady_clock::now();
    if (wait_time < D::zero() || wait_time > std::chrono::duration_cast<D>(remaining)) {
        _scheduleWrite("INVALID_ARGS\r\n");
        return false;
    }

    _lockStatus = lld::mutex::ls_acquiring;

    // Get the mutex we'll be using.
    const auto m = _server.lock_store().get_mutex(lock_name);

    // Attempt to lock it within the timeframe.
    const bool res = m->timed_lock(shared_from_this(), wait_time);
    if (!res) {
        _pendingMutex = m;

        // As for LOCK, acknowledge as soon as the request is queued, without waiting for the
        // server timeout or owner release. The client mutex orders an expiry behind this ACK.
        _scheduleWrite("LOCK_ACQUIRING\r\n");
    }
    if (res && !lock_acquired(lock_name, true)) {
        m->unlock();
        return false;
    }
    return res;
}


// The test fixture calls this specialization from another translation unit, where
// the template definition is unavailable. Emit it even when local calls are inlined.
template bool lkd::client::_timedAcquireLock<std::chrono::seconds>(
        const std::string& lock_name, const std::chrono::seconds& wait_time
);


void lkd::client::_releaseLock()
{
    // No-op if no lock exists.
    if (lld::mutex::ls_locked == _lockStatus) {
        const auto m = _server.lock_store().get_mutex(*_currentLock);
        m->unlock();
        console_print(
                cp_stdlog,
                ll_debug,
                str(boost::format(" - Lock with name '%1%' released from client %2%\n") %
                    *_currentLock % identifier())
        );

        _lockStatus = lld::mutex::ls_none;
        _currentLock.reset();

        // If the client is still here, write them a message about it.
        if (!_destructing)
            _scheduleWrite("LOCK_RELEASED\r\n");
    }
}

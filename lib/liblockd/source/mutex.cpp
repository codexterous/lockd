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
 * File       : mutex.cpp
 * Author     : Brett Gmoser
 * Part Of    : liblockd, a client library for interfacing with lockd.
 * Purpose    : Provide a network-backed C++ mutex for interprocess locking.
 * Start Date : 03/26/2012
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <istream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/version.hpp>

// Older Asio resolver results trigger GCC's array-bounds and stringop-overflow
// diagnostics when inlined.
// Load that header under the exception before mutex.hpp includes the rest of Asio.
// Boost 1.90 bounds the address copy by family and no longer needs this suppression.
#if defined(__GNUC__) && !defined(__clang__) && BOOST_VERSION < 109000
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Warray-bounds"
# pragma GCC diagnostic ignored "-Wstringop-overflow"
# include <boost/asio/ip/basic_resolver_results.hpp>

# pragma GCC diagnostic pop
#endif

#include <boost/asio/buffer.hpp>
#include <boost/asio/completion_condition.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read_until.hpp> // IWYU pragma: keep
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp> // IWYU pragma: keep

// The public Format header supplies the template definitions used by free_funcs.hpp.
#include <boost/format.hpp> // IWYU pragma: keep
#include <boost/format/free_funcs.hpp>

// Spirit needs Fusion's std::tuple adaptation to write the INFO fields through std::tie.
#include <boost/fusion/adapted/std_tuple.hpp> // IWYU pragma: keep
#include <boost/system/error_code.hpp>        // IWYU pragma: keep
#include <boost/system/system_error.hpp>


// The grammar uses X3 operators and directives supplied by this public umbrella.
#include <boost/spirit/home/x3.hpp> // IWYU pragma: keep
#include <boost/spirit/home/x3/auxiliary/eoi.hpp>
#include <boost/spirit/home/x3/char/char.hpp>
#include <boost/spirit/home/x3/char/char_class.hpp>
#include <boost/spirit/home/x3/core/parse.hpp>
#include <boost/spirit/home/x3/directive/omit.hpp>
#include <boost/spirit/home/x3/directive/skip.hpp>
#include <boost/spirit/home/x3/numeric/uint.hpp>
#include <boost/spirit/home/x3/string/literal_string.hpp>


#include "../include/liblockd/detail/client_contract.hpp"
#include "../include/liblockd/detail/source_path.hpp"
#include "../include/liblockd/exceptions.hpp"
#include "../include/liblockd/mutex.hpp"

lld::mutex::client_info lld::mutex::info()
{
    namespace x3 = boost::spirit::x3;
    using x3::char_;
    using x3::eoi;
    using x3::omit;
    using x3::skip;
    using x3::space;
    using x3::string;
    using x3::uint_;

    // Write the INFO command.
    blocking_write("INFO\n");

    lld::mutex::client_info info;
    std::vector<std::string> responses;
    responses.push_back(blocking_read_line(std::chrono::seconds(10)));
    responses.push_back(blocking_read_line(std::chrono::seconds(10)));
    responses.push_back(blocking_read_line(std::chrono::seconds(10)));

    const auto address_fields = std::tie(info.client_address, info.client_ip);

    // Parse the opaque connection identifier and its network address.
    // @formatter:off
    bool parse_client_info = x3::parse(
            responses[0].begin(),
            responses[0].end(),
            omit[string("you are client")] >> omit[+space] >> +(char_ - space) >>
                    omit[+space >> "at" >> +space] >> +(char_ - (space | eoi)) >> eoi,
            address_fields
    );
    // @formatter:on

    // Next, parse the lock status and lock name.
    if (parse_client_info) {
        std::string lock_name;
        const auto set_status = [&info](const lock_status_t value) {
            return [&info, value](const auto&) { info.lock_status = value; };
        };

        // @formatter:off
        parse_client_info = x3::parse(
                responses[1].begin(),
                responses[1].end(),
                omit[string("lock status is") >> +space >>
                     (string("none")[set_status(ls_none)] |
                      string("acquiring")[set_status(ls_acquiring)] |
                      string("locked")[set_status(ls_locked)]

                      // The "unknown" status here isn't actually a valid lock status, but, the server could
                      // return it.
                      | string("unknown")[set_status(ls_none)]) >>
                     +space >> omit[string("(lock=")]] >>
                        (omit["<none>"] | +(char_ - ")")) >> omit[string(")")] >> eoi,
                lock_name
        );
        // @formatter:on

        // Spirit can't take a std::optional<> because it uses insert(), so, only set the lock name
        // if we got one.
        if (!lock_name.empty())
            info.lock_name = lock_name;
    }

    if (parse_client_info)
        parse_client_info = x3::parse(
                responses[2].begin(),
                responses[2].end(),
                omit[string("current connected clients:")] >> omit[+space] >> uint_ >> eoi,
                info.connected_clients
        );

    if (parse_client_info) {
        return info;
    }

    // If we get here, one of the parses failed. This is quite unexpected.
    throw lld::exception(
            lld::exception::et_unexpected,
            LOCKD_SOURCE_FILE,
            __LINE__,
            __FUNCTION__,
            "lockd sent an unrecognizable response to the INFO command"
    );
}


boost::system::error_code lld::mutex::_startConnect(
        const boost::asio::ip::tcp::resolver::results_type& endpoints,
        const std::chrono::seconds& connect_timeout
)
{
    _error = boost::asio::error::host_not_found;

    for (const auto& endpoint : endpoints) {
        bool complete = false;
        bool timed_out = false;
        boost::asio::steady_timer timer(_ioService, connect_timeout);
        timer.async_wait([&](const boost::system::error_code& error) {
            if (!error && !complete) {
                timed_out = true;
                boost::system::error_code ignored;

                // NOLINTNEXTLINE(bugprone-unused-return-value)
                static_cast<void>(_socket.close(ignored));
            }
        });
        _socket.async_connect(endpoint.endpoint(), [&](const boost::system::error_code& error) {
            complete = true;
            _error = error;
            timer.cancel();
        });

        // Drain both completions before retrying. A canceled timer or connect must not change
        // the result of the next endpoint, and neither callback may outlive these local variables.
        _ioService.restart();
        _ioService.run();
        if (timed_out)
            _error = boost::system::errc::make_error_code(boost::system::errc::timed_out);
        if (!_error)
            return {};

        boost::system::error_code ignored;

        // NOLINTNEXTLINE(bugprone-unused-return-value)
        static_cast<void>(_socket.close(ignored));
    }
    return _error;
}

lld::mutex::mutex(
        const std::string& hostname,
        const std::uint16_t port,
        const std::chrono::seconds& connect_timeout,
        std::string lock_name
)
        : _hostName(hostname)
        , _port(port)
        , _socket(_ioService)
        , _buffer(lld::detail::reply_buffer_limit)
        , _lockName(std::move(lock_name))
        , _locked(false)
{
    using boost::asio::ip::tcp;

    // A lock name is one protocol token. Reject delimiters and editing controls
    // before connecting, so invalid construction cannot send an injected command.
    _validateLockName(_lockName);

    lld::detail::validate_connect_timeout(connect_timeout);

    // Create a client. First thing we do is connect to the server.
    try {
        const auto str_port = std::to_string(_port);
        tcp::resolver resolver(_ioService);
        const auto endpoints = resolver.resolve(_hostName, str_port);

        // Try each endpoint until we successfully establish a connection.
        boost::system::error_code error;
        if ((error = _startConnect(endpoints, connect_timeout))) {
            // The connection failed. Throw the last error message.
            throw lld::exception(
                    lld::exception::et_lockd_unavailable,
                    LOCKD_SOURCE_FILE,
                    __LINE__,
                    __FUNCTION__,
                    str(boost::format("Unable to connect to lockd at %1% on port %2%. %3% (%4%)") %
                        hostname % port % error % error.message())
            );
        }

        // Read the server's greeting. Give it ten seconds to respond, if it doesn't, it's too busy
        // and can't handle our request. We'll treat it just like a failed connect
        // (blocking_read_line will throw in this case).
        const auto greeting = blocking_read_line(lld::detail::greeting_timeout);
        if (greeting != "LOCKD")
            throw lld::exception(
                    lld::exception::et_unexpected,
                    LOCKD_SOURCE_FILE,
                    __LINE__,
                    __FUNCTION__,
                    str(boost::format("lockd returned unexpected response (%1%)") % greeting)
            );
    }

    // Re-throw our own exceptions.
    catch (const lld::exception&) {
        throw;
    }

    // Asio's throwing overloads report system_error; error_code is a returned status.
    catch (const boost::system::system_error& error) {
        throw lld::exception(
                lld::exception::et_lockd_unavailable,
                LOCKD_SOURCE_FILE,
                __LINE__,
                __FUNCTION__,
                error.what()
        );
    }

    catch (const std::exception& e) {
        throw lld::exception(
                lld::exception::et_unknown_exception,
                LOCKD_SOURCE_FILE,
                __LINE__,
                __FUNCTION__,
                e.what()
        );
    }

    // Catch unknown exceptions and re-throw them as our exceptions.
    catch (...) {
        throw lld::exception(
                lld::exception::et_unknown_exception,
                LOCKD_SOURCE_FILE,
                __LINE__,
                __FUNCTION__,
                "An unknown exception was caught."
        );
    }
}


lld::mutex::~mutex()
{
    // Close our socket if it is open.
    try {
        // Derived state is already destroyed; release the remote lock through the base class.
        if (_locked && _socket.is_open())
            mutex::unlock(tt_no_throw);

        _socket.close();
    }

    // Exceptions in dtor == bad!
    catch (...) {
    }
}


void lld::mutex::lock()
{
    // Match the ordinary mutex contract: wait until ownership is granted. The acknowledgement
    // has a bounded read below, but contention itself has no acquisition deadline.
    blocking_write(str(boost::format("LOCK %1%\n") % _lockName));

    // The first reply we'll get from the server is "LOCK_ACQUIRING", acknowledging the request.
    // Use a reasonable 30-second timeout for this read - server should reply right away.
    const auto first_reply = blocking_read_line(lld::detail::command_timeout);

    if (first_reply != "LOCK_ACQUIRING") {
        // This reply isn't what we expected.
        throw lld::exception(
                lld::exception::et_unexpected,
                LOCKD_SOURCE_FILE,
                __LINE__,
                __FUNCTION__,
                str(boost::format("lockd returned unexpected response (%1%)") % first_reply)
        );
    }

    // The second reply we'll get is "LOCK_ACQUIRED". Don't use a timeout for this, wait forever.
    const auto second_reply = blocking_read_line();
    if (second_reply != "LOCK_ACQUIRED") {
        // This reply isn't what we expected.
        throw lld::exception(
                lld::exception::et_unexpected,
                LOCKD_SOURCE_FILE,
                __LINE__,
                __FUNCTION__,
                str(boost::format("lockd returned unexpected response (%1%)") % second_reply)
        );
    }

    // If we get here, we're now locked.
    _locked = true;
}


bool lld::mutex::try_lock()
{
    // Attempt to lock the mutex in a non-blocking (relative to the locking operation anyway)
    // fashion, returning false if the lock was not available.
    blocking_write(str(boost::format("TRY_LOCK %1%\n") % _lockName));

    // Wait for the reply, which is either LOCK_UNAVAILABLE or LOCK_ACQUIRED. Use a reasonable
    // 30-second timeout for this read - server should reply right away.
    const auto reply = blocking_read_line(lld::detail::command_timeout);

    if (reply == "LOCK_UNAVAILABLE")
        return false;

    if (reply == "LOCK_ACQUIRED") {
        _locked = true;
        return true;
    }

    // This reply isn't what we expected.
    throw lld::exception(
            lld::exception::et_unexpected,
            LOCKD_SOURCE_FILE,
            __LINE__,
            __FUNCTION__,
            str(boost::format("lockd returned unexpected response (%1%)") % reply)
    );
}


void lld::mutex::unlock(const lld::mutex::unlock_throw_t throw_on_failure)
{
    try {
        // A duplicate release is a local usage error. It does not make an otherwise healthy
        // connection uncertain, and must not disconnect a client that can acquire again.
        if (!_locked)
            throw lld::exception(
                    lld::exception::et_logic_error,
                    LOCKD_SOURCE_FILE,
                    __LINE__,
                    __FUNCTION__,
                    "This mutex does not own the lock."
            );

        // Attempt to unlock the mutex, which should always succeed providing we are currently
        // holding a lock.
        blocking_write("UNLOCK\n");

        // Wait for the reply, which is going to be LOCK_RELEASED. Use a reasonable 30-second
        // timeout for this read - server should reply right away.
        const auto reply = blocking_read_line(lld::detail::command_timeout);

        if (reply != "LOCK_RELEASED") {
            // This reply isn't what we expected.
            throw lld::exception(
                    lld::exception::et_unexpected,
                    LOCKD_SOURCE_FILE,
                    __LINE__,
                    __FUNCTION__,
                    str(boost::format("lockd returned unexpected response (%1%)") % reply)
            );
        }

        // If we get here, we're now unlocked.
        _locked = false;
    }
    catch (...) {
        if (_locked) {
            _locked = false;
            _closeConnection();
        }

        // Standard lock guards call unlock() during destruction, so cleanup must not throw.
        // Callers can explicitly request an exception when checking a remote release.
        if (tt_throw_on_failure == throw_on_failure)
            throw;
    }
}


bool lld::mutex::connected() const
{
    return _socket.is_open() && !_error;
}


const std::string& lld::mutex::lock_name() const
{
    return _lockName;
}


void lld::mutex::lock_name(const std::string& name)
{
    if (_locked)
        throw lld::exception(
                lld::exception::et_logic_error,
                LOCKD_SOURCE_FILE,
                __LINE__,
                __FUNCTION__,
                "Cannot change lock name while locked."
        );

    _validateLockName(name);
    _lockName = name;
}


void lld::mutex::_validateLockName(const std::string& name)
{
    lld::detail::validate_lock_name(name);
}


void lld::mutex::_closeConnection()
{
    // After a transport or framing failure, command delivery and ownership are uncertain.
    // Close immediately so the daemon can release or cancel the request, rather than keeping
    // an unusable connection alive until destruction. Preserve _error for diagnostics.
    boost::system::error_code ignored;

    // Shut down first so cancellation also gives the peer an orderly EOF where possible.
    // NOLINTNEXTLINE(bugprone-unused-return-value)
    static_cast<void>(_socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored));

    // NOLINTNEXTLINE(bugprone-unused-return-value)
    static_cast<void>(_socket.close(ignored));
}


void lld::mutex::blocking_write(const std::string& message)
{
    // All public commands pass through here. Check once at the I/O boundary, including
    // retries after an earlier failure, rather than repeating the check in every operation.
    // Base destruction intentionally uses base state; live objects retain virtual dispatch.
    // NOLINTNEXTLINE(clang-analyzer-optin.cplusplus.VirtualCall)
    if (!connected())
        throw lld::exception(
                lld::exception::et_not_connected,
                LOCKD_SOURCE_FILE,
                __LINE__,
                __FUNCTION__,
                "Not connected to lockd."
        );

    // Use the non-throwing version of write because I like it that way.
    boost::system::error_code error;
    boost::asio::write(
            _socket,
            boost::asio::buffer(message.data(), message.length()),
            boost::asio::transfer_all(),
            error
    );

    if (error) {
        _error = error;
        _closeConnection();
        const std::string error_mesg =
                str(boost::format("Socket write error. %1% (%2%)") % error % error.message());
        throw lld::exception(
                lld::exception::et_write_error,
                LOCKD_SOURCE_FILE,
                __LINE__,
                __FUNCTION__,
                error_mesg
        );
    }
}


std::string lld::mutex::blocking_read_line(const std::chrono::seconds& timeout)
{
    bool complete = false;
    bool timed_out = false;
    boost::asio::steady_timer timer(_ioService);
    _error.clear();

    // async_read_until is a composed operation. A packet can run an intermediate handler without
    // completing a line, so we must wait for its completion rather than calling run_one once.
    boost::asio::async_read_until(
            _socket,
            _buffer,
            '\n',
            [&](const boost::system::error_code& error, const std::size_t /*bytes_transferred*/) {
                complete = true;
                _error = error;
                timer.cancel();
            }
    );
    if (timeout != std::chrono::seconds(0)) {
        timer.expires_after(timeout);
        timer.async_wait([&](const boost::system::error_code& error) {
            if (!error && !complete) {
                timed_out = true;

                // A timed-out connection is no longer usable. Closing also completes the pending
                // read; drain that callback before any of the captured variables leave scope.
                _closeConnection();
            }
        });
    }

    // This io_context belongs to this connection. run() waits for the read and any canceled timer
    // handler, including when a complete line was already buffered by the preceding read.
    _ioService.restart();
    _ioService.run();
    if (timed_out)
        _error = boost::system::errc::make_error_code(boost::system::errc::timed_out);
    if (_error) {
        _closeConnection();
        throw lld::exception(
                lld::exception::et_read_error,
                LOCKD_SOURCE_FILE,
                __LINE__,
                __FUNCTION__,
                str(boost::format("Socket read %1%error. %2% (%3%)") %
                    (timed_out ? "timeout " : "") % _error % _error.message())
        );
    }

    std::istream is(&_buffer);
    std::string line;
    std::getline(is, line);
    lld::detail::trim_reply(line);
    return line;
}

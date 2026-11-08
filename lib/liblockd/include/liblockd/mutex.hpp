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
 * File       : mutex.hpp
 * Author     : Brett Gmoser
 * Part Of    : liblockd, a client library for interfacing with lockd.
 * Purpose    : Provide a network-backed C++ mutex for interprocess locking.
 * Start Date : 03/26/2012
 *
 * Description: Each client owns its socket, input stream buffer, I/O context, and last error.
 *              connected() checks the local open socket and that error; it sends no liveness
 *              probe. A disconnect may therefore be discovered only by the next read or write.
 *              Lock state belongs to this connection, so separate instances with the same name
 *              still contend remotely. Cleanup uses nonthrowing unlock unless the caller asks
 *              for tt_throw_on_failure explicitly. Instances are not safe for concurrent use.
 *              Connection timeouts must be nonnegative and fit the steady clock's range.
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#if !defined(LOCKD_LIBLOCKD_MUTEX_HPP_INCLUDED_)
# define LOCKD_LIBLOCKD_MUTEX_HPP_INCLUDED_

# if defined(_MSC_VER) && (_MSC_VER >= 1020)
#  pragma once
# endif

# include <chrono>
# include <cstdint>
# include <istream>
# include <mutex>
# include <optional>
# include <ostream>
# include <stdexcept>
# include <string>

# include <boost/asio.hpp>

# include "api.hpp"

namespace lld {

namespace detail {
template <typename MutexT> class lockd_test_access;
template <typename MutexT> class _lockdTestAccessBase;
} // namespace detail


class mutex
{
    /* typedefs */
public:
    typedef std::unique_lock<mutex> scoped_timed_lock;
    typedef scoped_timed_lock scoped_lock;

    typedef enum
    {
        ls_none = 0,
        ls_acquiring,
        ls_locked
    } lock_status_t;

    struct client_info
    {
        client_info()
                : lock_status(ls_none)
                , connected_clients(0)
        {
        }

        // Historical field name: the daemon supplies an opaque connection ID, not an address.
        std::string client_address;
        std::string client_ip;
        lock_status_t lock_status;
        std::optional<std::string> lock_name;
        unsigned int connected_clients;
    };

    /* enumerations */
public:
    typedef enum
    {
        tt_throw_on_failure,
        tt_no_throw
    } unlock_throw_t;

    /* construction and destruction */
public:
    mutex(const mutex&) = delete;
    mutex& operator=(const mutex&) = delete;
    LIBLOCKD_API
    mutex(const std::string& hostname,
          const std::uint16_t port,
          const std::chrono::seconds& connect_timeout,
          std::string lock_name);
    virtual LIBLOCKD_API ~mutex();

    /* public methods */
public:
    virtual LIBLOCKD_API void lock();
    virtual LIBLOCKD_API bool try_lock();
    virtual LIBLOCKD_API void unlock(const unlock_throw_t throw_on_failure = tt_no_throw);
    virtual LIBLOCKD_API client_info info();

    /* public methods, observers */
public:
    // Reports local socket/error state. A remote close is recorded when I/O observes it;
    // this observer does not probe the peer or promise that the next operation will succeed.
    virtual LIBLOCKD_API bool connected() const;
    virtual LIBLOCKD_API const std::string& lock_name() const;

    /* public methods, setters */
public:
    virtual LIBLOCKD_API void lock_name(const std::string& name);

    /* private methods */
private:
    static void _validateLockName(const std::string& name);
    void _closeConnection();
    boost::system::error_code _startConnect(
            const boost::asio::ip::tcp::resolver::results_type& endpoints,
            const std::chrono::seconds& connect_timeout
    );

    /* protected methods */
protected:
    friend class lld::detail::lockd_test_access<mutex>;
    friend class lld::detail::_lockdTestAccessBase<mutex>;
    LIBLOCKD_API std::string
    blocking_read_line(const std::chrono::seconds& timeout = std::chrono::seconds(0));
    LIBLOCKD_API void blocking_write(const std::string& message);

    /* private data members */
private:
    const std::string _hostName;
    const std::uint16_t _port;

    boost::asio::io_context _ioService;
    boost::asio::ip::tcp::socket _socket;
    boost::asio::streambuf _buffer;
    boost::system::error_code _error;

    /* protected data members */
protected:
    std::string _lockName;
    bool _locked;
    mutable std::mutex _mutex;
}; // class mutex

} // namespace lld


inline std::ostream& operator<<(std::ostream& strm, const lld::mutex::lock_status_t lock_status)
{
    switch (lock_status) {
    case lld::mutex::ls_none: strm << "none"; break;
    case lld::mutex::ls_acquiring: strm << "acquiring"; break;
    case lld::mutex::ls_locked: strm << "locked"; break;
    }
    return strm;
}


inline std::istream& operator>>(std::istream& strm, lld::mutex::lock_status_t& lock_status)
{
    std::string token;
    strm >> token;
    if (token == "none")
        lock_status = lld::mutex::ls_none;
    else if (token == "acquiring")
        lock_status = lld::mutex::ls_acquiring;
    else if (token == "locked")
        lock_status = lld::mutex::ls_locked;
    else
        throw std::invalid_argument(token);
    return strm;
}


#endif // !defined(LOCKD_LIBLOCKD_MUTEX_HPP_INCLUDED_)

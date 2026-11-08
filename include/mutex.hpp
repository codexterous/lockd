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
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Remote lock ownership and queued timed or untimed acquisition requests.
 * Start Date : 04/11/2012
 *
 * Description: Requests retain weak client references so the queue cannot keep a disconnected
 *              connection alive. Timed requests own their timers; cancellation removes the exact
 *              request and cancels its timer before a later handoff can grant it. The client also
 *              releases its owned lock on disconnect. Timer callbacks must not act on a newer
 *              request by the same client or retain a mutex after garbage collection.
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#if !defined(LOCKD_LOCKD_MUTEX_HPP_INCLUDED_)
# define LOCKD_LOCKD_MUTEX_HPP_INCLUDED_

# if defined(_MSC_VER) && (_MSC_VER >= 1020)
#  pragma once
# endif

# include <deque>
# include <memory>
# include <mutex>
# include <string>

# include <boost/asio.hpp>

# include "../include/client.hpp"

namespace lkd {

namespace detail {
class lifetime_test_access;
}

class mutex : public std::enable_shared_from_this<mutex>
{
    /* private types */
private:
    struct lock_request
    {
        /* construction and destruction */
    public:
        explicit lock_request(const client_ptr& cptr)
                : client(cptr)
        {
        }
        virtual ~lock_request() = default;

        /* public data members */
    public:
        const weak_client_ptr client;
        std::shared_ptr<boost::asio::steady_timer> timer;
    };
    typedef std::shared_ptr<lock_request> lock_request_ptr;

    /* construction and destruction */
public:
    mutex(const mutex&) = delete;
    mutex& operator=(const mutex&) = delete;
    mutex(boost::asio::io_context& io_service, std::string name);
    virtual ~mutex() = default;

    /* public methods */
public:
    bool locked() const;
    bool try_lock();
    bool operator<(const mutex& rhs) const;

    // A "blocking" lock. Adds client to the queue. Returns true if acquired immediately.
    bool lock(const client_ptr& cptr);
    void cancel(const weak_client_ptr& cptr);

    // Tells the caller if we have a queue for this mutex.
    bool have_queue() const;

    // Same as lock, but gives up after the specified timeframe.
    template <typename D> bool timed_lock(const client_ptr& cptr, const D& time)
    {
        const std::lock_guard<std::recursive_mutex> lock(_mutex);
        if (!_locked) {
            _locked = true;
            return true;
        }

        const auto request = std::make_shared<lock_request>(cptr);
        request->timer = std::make_shared<boost::asio::steady_timer>(_ioService, time);
        const std::weak_ptr<mutex> mutex_ptr(shared_from_this());
        request->timer->async_wait([mutex_ptr, request](const boost::system::error_code& error) {
            // Neither a timer nor a canceled callback should keep a disconnected client
            // alive. The request itself identifies this acquisition, not just its client.
            if (!error)
                if (const auto strong = mutex_ptr.lock())
                    strong->_lockTimeout(request);
        });
        _queue.push_back(request);
        return false;
    }

    // Unlocks the mutex.
    void unlock();

    /* private methods */
private:
    friend class detail::lifetime_test_access;
    void _lockTimeout(const lock_request_ptr& request);

    /* private data members */
private:
    boost::asio::io_context& _ioService;
    mutable std::recursive_mutex _mutex;
    const std::string _name;
    bool _locked;
    std::deque<lock_request_ptr> _queue;
};

} // namespace lkd

#endif // !defined(LOCKD_LOCKD_MUTEX_HPP_INCLUDED_)

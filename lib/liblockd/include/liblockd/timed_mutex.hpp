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
 * File       : timed_mutex.hpp
 * Author     : Brett Gmoser
 * Part Of    : liblockd, a client library for interfacing with lockd.
 * Purpose    : Provide a network-backed C++ mutex with timed acquisition.
 * Start Date : 11/19/2012
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#if !defined(LOCKD_LIBLOCKD_TIMED_MUTEX_HPP_INCLUDED_)
# define LOCKD_LIBLOCKD_TIMED_MUTEX_HPP_INCLUDED_

# if defined(_MSC_VER) && (_MSC_VER >= 1020)
#  pragma once
# endif

# include <chrono>
# include <cstdint>
# include <mutex>
# include <ratio>
# include <string>

# include <boost/format.hpp>

# include "../../include/liblockd/detail/client_contract.hpp"
# include "../../include/liblockd/mutex.hpp"

namespace lld {

namespace detail {
template <typename MutexT> class _lockdTestAccessBase;
template <typename MutexT> class lockd_test_access;
} // namespace detail

// Timed operations accept standard chrono clocks. Relative durations must have
// millisecond or coarser resolution; clock-native deadline precision is converted to milliseconds.
// Expired deadlines and nonpositive durations make an immediate remote try_lock attempt, which
// still requires a network round trip. Instances require external synchronization between threads.
class timed_mutex : public lld::mutex
{
    /* typedefs */
public:
    typedef std::unique_lock<timed_mutex> scoped_timed_lock;
    typedef scoped_timed_lock scoped_lock;

    /* construction and destruction */
public:
    LIBLOCKD_API timed_mutex(
            const std::string& hostname,
            const std::uint16_t port,
            const std::chrono::seconds& connect_timeout,
            const std::string& lock_name
    );
    virtual ~timed_mutex() override = default;

    /* public methods */
public:
    template <class Rep, class Period>
    bool try_lock_for(const std::chrono::duration<Rep, Period>& rel_time)
    {
        static_assert(
                std::ratio_greater_equal_v<Period, std::milli>,
                "timed_lock cannot be used with this resolution. use milliseconds instead."
        );
        const auto timeout = lld::detail::lock_wait_milliseconds(
                std::chrono::duration<double, std::milli>(rel_time).count()
        );
        if (timeout == std::chrono::milliseconds::zero())
            return try_lock();
        return _timedLock(boost::str(boost::format("%1%ms") % timeout.count()));
    }
    template <class Clock, class Duration>
    bool try_lock_until(const std::chrono::time_point<Clock, Duration>& t)
    {
        const auto now = Clock::now();
        if (t <= now)
            return try_lock();

        // Subtract in the caller's clock domain before converting to the protocol's resolution.
        return try_lock_for(
                lld::detail::lock_wait_milliseconds(
                        std::chrono::duration<double, std::milli>(t - now).count()
                )
        );
    }
    template <class Rep, class Period>
    bool timed_lock(const std::chrono::duration<Rep, Period>& rel_time)
    {
        return try_lock_for(rel_time);
    }
    template <class Clock, class Duration>
    bool timed_lock(const std::chrono::time_point<Clock, Duration>& abs_time)
    {
        return try_lock_until(abs_time);
    }

    /* protected methods */
protected:
    friend class lld::detail::lockd_test_access<timed_mutex>;
    friend class lld::detail::_lockdTestAccessBase<timed_mutex>;

    LIBLOCKD_API bool _timedLock(const std::string& time_spec);

    template <typename TimeDuration>
    static std::string duration_to_time_spec(const TimeDuration& duration)
    {
        return boost::str(boost::format("%1%ms") % duration);
    }
    static std::string duration_to_time_spec(const unsigned int& duration)
    {
        return boost::str(boost::format("%1%s") % duration);
    }
    static std::string duration_to_time_spec(const std::chrono::milliseconds& duration)
    {
        return boost::str(boost::format("%1%ms") % duration.count());
    }
    static std::string duration_to_time_spec(const std::chrono::seconds& duration)
    {
        return boost::str(boost::format("%1%s") % duration.count());
    }
    static std::string duration_to_time_spec(const std::chrono::minutes& duration)
    {
        return boost::str(boost::format("%1%m") % duration.count());
    }
    static std::string duration_to_time_spec(const std::chrono::hours& duration)
    {
        return boost::str(boost::format("%1%h") % duration.count());
    }

}; // class timed_mutex

} // namespace lld

#endif // !defined(LOCKD_LIBLOCKD_TIMED_MUTEX_HPP_INCLUDED_)

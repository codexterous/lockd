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
 * File       : recursive_mutex.hpp
 * Author     : Brett Gmoser
 * Part Of    : liblockd, a client library for interfacing with lockd.
 * Purpose    : Provide a thread-recursive C++ mutex for remote locks.
 * Start Date : 10/22/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#if !defined(LOCKD_LIBLOCKD_RECURSIVE_MUTEX_HPP_INCLUDED_)
# define LOCKD_LIBLOCKD_RECURSIVE_MUTEX_HPP_INCLUDED_

# if defined(_MSC_VER) && (_MSC_VER >= 1020)
#  pragma once
# endif

# include <chrono>
# include <cstdint>
# include <memory>
# include <mutex>
# include <string>
# include <vector>


# include "mutex.hpp"

namespace lld {

// Recursion belongs to this client instance and the owning thread. Separate instances with
// the same remote name still compete. Each successful lock requires an unlock on that thread.
class recursive_mutex : public mutex
{
    /* private types */
private:
    // Only the remote guard bypasses virtual dispatch. Public base references
    // must retain recursive ownership; its first and final network calls must not reenter it.
    class remote_mutex
    {
        /* construction and destruction */
    public:
        explicit remote_mutex(mutex& remote);
        virtual ~remote_mutex() = default;

        /* public methods */
    public:
        void lock() const;
        [[nodiscard]] bool try_lock() const;
        void unlock() const;

        /* private data members */
    private:
        mutex& _remote;
    };

    typedef std::unique_lock<remote_mutex> remote_lock;

    /* typedefs */
public:
    typedef std::unique_lock<recursive_mutex> scoped_lock;
    typedef scoped_lock scoped_timed_lock;

    /* construction and destruction */
public:
    LIBLOCKD_API recursive_mutex(
            const std::string& hostname,
            const std::uint16_t port,
            const std::chrono::seconds& connect_timeout,
            const std::string& lock_name
    );
    virtual LIBLOCKD_API ~recursive_mutex() override;

    /* public methods */
public:
    LIBLOCKD_API void lock() override;
    LIBLOCKD_API bool try_lock() override;
    LIBLOCKD_API void unlock(const unlock_throw_t throw_on_failure = tt_no_throw) override;
    LIBLOCKD_API client_info info() override;
    LIBLOCKD_API bool connected() const override;
    LIBLOCKD_API const std::string& lock_name() const override;
    LIBLOCKD_API void lock_name(const std::string& name) override;

    /* private methods */
private:
    bool _acquire(const bool try_only);

    /* private data members */
private:
    mutable std::recursive_mutex _recursiveMutex;
    remote_mutex _remoteMutex;

    // The native mutex tracks thread recursion. Shared guards retain one remote acquisition
    // until the final record is removed; their reference count controls its lifetime.
    // Destroy records before the native gate and before the base connection.
    std::vector<std::shared_ptr<remote_lock>> _acquisitions;
};

} // namespace lld

#endif // !defined(LOCKD_LIBLOCKD_RECURSIVE_MUTEX_HPP_INCLUDED_)

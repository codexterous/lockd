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
 * File         : sync_detail.hpp
 * Part Of      : pylockd - Python integration for liblockd.
 * Purpose      : Register synchronous Python lock types and exceptions.
 * Start Date   : 2026-10-21
 *
 * Description  : Blocking native calls release the GIL through an RAII guard, restoring it
 *                before Boost.Python handles a return value or native exception. Copied wrappers
 *                share the client, lock state, and operation mutex. Acquire that mutex after
 *                releasing the GIL and release it before restoring the GIL to avoid a lock-order
 *                deadlock. Ownership misuse is checked before calling std::unique_lock so it
 *                raises lld::exception directly, rather than translating std::system_error.
 *                Python context exits and explicit unlock use the native throwing release mode.
 *                Destructors retain nonthrowing release. If context cleanup fails while a body
 *                exception is active, keep that exception and attach cleanup failure as its cause.
 *                Python usage examples belong in PYTHON.md.
 */

#if !defined(LOCKD_PYLOCKD_SYNC_DETAIL_HPP_INCLUDED_)
# define LOCKD_PYLOCKD_SYNC_DETAIL_HPP_INCLUDED_

# if defined(_MSC_VER) && (_MSC_VER >= 1020)
#  pragma once
# endif

// Python's feature macros must precede standard headers; keep Boost's Python ABI wrapper first.
# include <boost/python.hpp>

# include <cstdint>
# include <memory>
# include <mutex>
# include <string>


// Python enters these wrappers with the GIL held. Native connection, acquisition, and release
// can block, so the guard restores Python's thread state even when a native operation throws.
class gil_release
{
    /* construction and destruction */
public:
    gil_release(const gil_release&) = delete;
    gil_release& operator=(const gil_release&) = delete;
    gil_release();
    virtual ~gil_release();

    /* private data members */
private:
    PyThreadState* const _threadState;
};

namespace lld {
class mutex;
} // namespace lld

class sync_scoped_lock;

// Keep the native mutex hierarchy visible to Boost.Python without binding its blocking methods
// directly. Scopes retain this shared client after the Python mutex wrapper is dropped. Copies
// also share the operation mutex because the underlying nonrecursive client is not thread-safe.
class sync_mutex
{
    /* construction and destruction */
public:
    sync_mutex(
            const std::string& hostname,
            const std::uint16_t port,
            const std::int64_t connect_timeout,
            const std::string& lock_name
    );
    sync_mutex(const sync_mutex&) = default;
    virtual ~sync_mutex();
protected:
    explicit sync_mutex(std::shared_ptr<lld::mutex> mutex);

    /* private data members */
private:
    friend class sync_scoped_lock;
    std::shared_ptr<lld::mutex> _mutexPtr;
    const std::shared_ptr<std::mutex> _operationMutex;
};

class sync_timed_mutex : public sync_mutex
{
    /* construction and destruction */
public:
    sync_timed_mutex(
            const std::string& hostname,
            const std::uint16_t port,
            const std::int64_t connect_timeout,
            const std::string& lock_name
    );
    virtual ~sync_timed_mutex() override;
};

class sync_recursive_mutex : public sync_mutex
{
    /* construction and destruction */
public:
    sync_recursive_mutex(
            const std::string& hostname,
            const std::uint16_t port,
            const std::int64_t connect_timeout,
            const std::string& lock_name
    );
    virtual ~sync_recursive_mutex() override;
};

// std::unique_lock supplies C++ ownership and destructor cleanup; this wrapper supplies the
// Python context-manager contract. Construction defers acquisition until __enter__ or lock().
// Copied Python wrappers share one unique_lock, so copying cannot create a second remote owner
// or release the same acquisition twice. Explicit unlock reports errors; destruction cannot.
class sync_scoped_lock
{
    /* construction and destruction */
public:
    explicit sync_scoped_lock(const sync_mutex& mutex);
    sync_scoped_lock(const sync_scoped_lock&) = default;
    virtual ~sync_scoped_lock();

    /* public methods */
public:
    [[nodiscard]] const sync_scoped_lock& enter() const;
    void
    exit(const boost::python::object&,
         const boost::python::object& error,
         const boost::python::object&) const;
    [[nodiscard]] bool owns_lock() const;
    void lock() const;
    void unlock() const;
    [[nodiscard]] bool try_lock() const;

    /* protected methods */
protected:
    // Check under _operationMutex before entering std::unique_lock. Usage errors must have the
    // same lld::exception category as async operations, rather than std::system_error.
    void _checkOwnership(const bool expected) const;

    /* protected data members */
protected:
    // Recursive guards use separate operation mutexes: the native recursive mutex already
    // coordinates threads, and serializing all guards would let a blocked contender prevent
    // the owning thread from reaching unlock(). Copies of one guard still share its mutex.
    std::shared_ptr<lld::mutex> _mutexPtr;
    const std::shared_ptr<std::mutex> _operationMutex;
    std::shared_ptr<std::unique_lock<lld::mutex>> _lockPtr;
};

// A successful native timed acquisition must be adopted by the existing unique_lock. Creating
// a normally locking unique_lock afterward would send another LOCK instead of recording ownership.
class sync_scoped_timed_lock : public sync_scoped_lock
{
    /* construction and destruction */
public:
    explicit sync_scoped_timed_lock(const sync_timed_mutex& mutex);
    virtual ~sync_scoped_timed_lock() override;

    /* public methods */
public:
    [[nodiscard]] bool try_lock_until(const double abs_time) const;
    [[nodiscard]] bool try_lock_for(const double ms) const;
};

class sync_scoped_recursive_lock : public sync_scoped_lock
{
    /* construction and destruction */
public:
    explicit sync_scoped_recursive_lock(const sync_recursive_mutex& mutex);
    virtual ~sync_scoped_recursive_lock() override;
};

#endif // !defined(LOCKD_PYLOCKD_SYNC_DETAIL_HPP_INCLUDED_)

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
 * File       : lockd_test_access.hpp
 * Author     : Brett Gmoser
 * Part Of    : liblockd, a client library for interfacing with lockd.
 * Purpose    : Expose protected C++ client operations to the test suite.
 * Start Date : 08/07/2023
 *
 * Description: Derived helpers expose protected operations without redefining C++ access
 *              keywords, preserving access control and MSVC symbol names.
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#if !defined(LOCKD_LIBLOCKD_DETAIL_LOCKD_TEST_ACCESS_HPP_INCLUDED_)
# define LOCKD_LIBLOCKD_DETAIL_LOCKD_TEST_ACCESS_HPP_INCLUDED_

# include <chrono>
# include <string>

# include "../../../include/liblockd/mutex.hpp"
# include "../../../include/liblockd/timed_mutex.hpp"

namespace lld::detail {

template <typename MutexT> class _lockdTestAccessBase
{
    /* construction and destruction */
public:
    _lockdTestAccessBase(const _lockdTestAccessBase&) = delete;
    _lockdTestAccessBase& operator=(const _lockdTestAccessBase&) = delete;
    explicit _lockdTestAccessBase(MutexT* const mutex_ptr)
            : _mutexPtr(mutex_ptr)
    {
    }
    virtual ~_lockdTestAccessBase() = default;

    /* public methods */
public:
    [[nodiscard]] std::string blocking_read_line(const std::chrono::seconds& seconds) const
    {
        return _mutexPtr->blocking_read_line(seconds);
    }
    void blocking_write(const std::string& message) const
    {
        return _mutexPtr->blocking_write(message);
    }
    [[nodiscard]] const boost::system::error_code& error() const { return _mutexPtr->_error; }
    [[nodiscard]] bool socket_open() const { return _mutexPtr->_socket.is_open(); }

    /* private data members */
protected:
    MutexT* const _mutexPtr;
};


template <typename MutexT> class lockd_test_access : public _lockdTestAccessBase<MutexT>
{
    /* construction and destruction */
public:
    explicit lockd_test_access(MutexT* const mutex_ptr)
            : _lockdTestAccessBase<MutexT>(mutex_ptr)
    {
    }
};


template <> class lockd_test_access<lld::mutex> : public _lockdTestAccessBase<lld::mutex>
{
    /* construction and destruction */
public:
    explicit lockd_test_access(lld::mutex* const mutex_ptr)
            : _lockdTestAccessBase(mutex_ptr)
    {
    }
    virtual ~lockd_test_access() override = default;

    /* public methods */
public:
    [[nodiscard]] LIBLOCKD_API bool peer_closed() const;
    LIBLOCKD_API void shutdown_send() const;
};


template <>
class lockd_test_access<lld::timed_mutex> : public _lockdTestAccessBase<lld::timed_mutex>
{
    /* construction and destruction */
public:
    explicit lockd_test_access(lld::timed_mutex* const mutex_ptr)
            : _lockdTestAccessBase(mutex_ptr)
    {
    }

    /* public methods */
public:
    template <typename TimeDurationT>
    [[nodiscard]] std::string duration_to_time_spec(const TimeDurationT& duration) const
    {
        return lld::timed_mutex::duration_to_time_spec(duration);
    }
};

} // namespace lld::detail

#endif // !defined(LOCKD_LIBLOCKD_DETAIL_LOCKD_TEST_ACCESS_HPP_INCLUDED_)

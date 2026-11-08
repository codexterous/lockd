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
 * File       : lock_store.hpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Keep one shared mutex per lock name and reclaim unused entries.
 * Start Date : 03/26/2012
 *
 * Description: Lookup copies a shared handle while holding the store mutex. Collection checks
 *              sole map ownership under that same mutex before erasing an entry, so a caller
 *              cannot receive an old mutex while a second one is created for the same name.
 *              Queued requests and owned locks prevent collection. Each name is checked after
 *              five minutes and rechecked at that interval while still in use; the callback
 *              retains its timer until completion.
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#if !defined(LOCKD_LOCKD_LOCK_STORE_HPP_INCLUDED_)
# define LOCKD_LOCKD_LOCK_STORE_HPP_INCLUDED_

# if defined(_MSC_VER) && (_MSC_VER >= 1020)
#  pragma once
# endif

# include <memory>
# include <mutex>
# include <string>
# include <unordered_map>

# include <boost/asio.hpp>

# include "../include/mutex.hpp"

namespace lkd {

namespace detail {
class lifetime_test_access;
}

class lock_store
{
    /* construction and destruction */
public:
    lock_store(const lock_store&) = delete;
    lock_store& operator=(const lock_store&) = delete;
    explicit lock_store(boost::asio::io_context& io_service);
    virtual ~lock_store() = default;

    /* public methods */
public:
    // Keep this handle until the operation completes. Collection must not replace a named mutex
    // while a caller still has it, even if it hasn't acquired the mutex yet.
    std::shared_ptr<lkd::mutex> get_mutex(const std::string& lock_name);

    /* private methods */
private:
    friend class detail::lifetime_test_access;
    bool _collectMutex(const std::string& lock_name);
    void _scheduleGarbageCollection(const std::string& lock_name);

    /* private data members */
private:
    std::unordered_map<std::string, std::shared_ptr<lkd::mutex>> _mutexes;
    mutable std::mutex _dataMutex;
    boost::asio::io_context& _ioService;
};

} // namespace lkd

#endif // !defined(LOCKD_LOCKD_LOCK_STORE_HPP_INCLUDED_)

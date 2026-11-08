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
 * File       : lock_store.cpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Keep one shared mutex per lock name and reclaim unused entries.
 * Start Date : 03/26/2012
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <chrono>
#include <memory>
#include <mutex>
#include <string>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

// The public Format header supplies the template definitions used by free_funcs.hpp.
#include <boost/format.hpp> // IWYU pragma: keep
#include <boost/format/free_funcs.hpp>
#include <boost/system/error_code.hpp> // IWYU pragma: keep

#include "../include/lock_store.hpp"
#include "../include/logging.hpp"
#include "../include/mutex.hpp"

lkd::lock_store::lock_store(boost::asio::io_context& io_service)
        : _ioService(io_service)
{
}


std::shared_ptr<lkd::mutex> lkd::lock_store::get_mutex(const std::string& lock_name)
{
    // Copy the shared handle under the map lock, before collection can inspect its ownership.
    const std::lock_guard<std::mutex> our_lock(_dataMutex);

    // Does the lock map have a lock by this name?
    const auto pos = _mutexes.find(lock_name);
    if (_mutexes.end() == pos) {
        // Preserve move semantics and don't make this const, even though it should be const.
        auto mutex_ptr = std::make_shared<lkd::mutex>(_ioService, lock_name);
        _mutexes[lock_name] = mutex_ptr;

        _scheduleGarbageCollection(lock_name);

        // Return the mutex.
        return mutex_ptr;
    }

    // Return the one that we found.
    return pos->second;
}


bool lkd::lock_store::_collectMutex(const std::string& lock_name)
{
    const std::lock_guard<std::mutex> our_lock(_dataMutex);

    // Attempt to find the mutex.
    const auto pos = _mutexes.find(lock_name);
    if (_mutexes.end() != pos) {
        // A lookup copies the shared pointer under this same store lock. Checking sole ownership
        // here closes the gap between that lookup and the caller's eventual acquisition. Checking
        // only have_queue() would still allow collection in that gap, creating two mutexes with
        // the same name. Locked mutexes and queued requests also keep their map entry.
        if (pos->second.use_count() == 1 && !pos->second->have_queue()) {
            _mutexes.erase(pos);
            lkd::console_print(
                    lkd::cp_stdlog,
                    lkd::ll_trace,
                    str(boost::format(" - Garbage collecting mutex for lock with name '%1%'.\n") %
                        lock_name)
            );
        }
        else
            return false;
    }
    return true;
}


void lkd::lock_store::_scheduleGarbageCollection(const std::string& lock_name)
{
    const auto timer =
            std::make_shared<boost::asio::steady_timer>(_ioService, std::chrono::seconds(300));
    timer->async_wait([this, lock_name, timer](const boost::system::error_code& error) {
        (void)timer; // Own the timer until its completion handler has been destroyed.
        if (!error && !_collectMutex(lock_name))
            _scheduleGarbageCollection(lock_name);
    });
}

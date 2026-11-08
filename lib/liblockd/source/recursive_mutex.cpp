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
 * File       : recursive_mutex.cpp
 * Author     : Brett Gmoser
 * Part Of    : liblockd, a client library for interfacing with lockd.
 * Purpose    : Provide a thread-recursive C++ mutex for remote locks.
 * Start Date : 10/22/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include <liblockd/detail/source_path.hpp>
#include <liblockd/mutex.hpp>

#include "../include/liblockd/exceptions.hpp"
#include "../include/liblockd/recursive_mutex.hpp"

lld::recursive_mutex::remote_mutex::remote_mutex(mutex& remote)
        : _remote(remote)
{
}


void lld::recursive_mutex::remote_mutex::lock() const
{
    _remote.mutex::lock();
}


bool lld::recursive_mutex::remote_mutex::try_lock() const
{
    return _remote.mutex::try_lock();
}


void lld::recursive_mutex::remote_mutex::unlock() const
{
    _remote.mutex::unlock();
}


lld::recursive_mutex::recursive_mutex(
        const std::string& hostname,
        const std::uint16_t port,
        const std::chrono::seconds& connect_timeout,
        const std::string& lock_name
)
        : mutex(hostname, port, connect_timeout, lock_name)
        , _remoteMutex(*this)
{
}


lld::recursive_mutex::~recursive_mutex() = default;


bool lld::recursive_mutex::_acquire(const bool try_only)
{
    std::unique_lock<std::recursive_mutex> local(_recursiveMutex, std::defer_lock);
    if (try_only) {
        if (!local.try_lock())
            return false;
    }
    else
        local.lock();

    if (_acquisitions.empty()) {
        const auto remote = std::make_shared<remote_lock>(_remoteMutex, std::defer_lock);
        if (try_only) {
            if (!remote->try_lock())
                return false;
        }
        else
            remote->lock();
        _acquisitions.push_back(remote);
    }
    else
        _acquisitions.push_back(_acquisitions.back());

    // Transfer this native recursion level to unlock(). If acquisition or allocation throws,
    // local and the temporary remote guard unwind without leaving an extra level held.
    local.release();
    return true;
}


void lld::recursive_mutex::lock()
{
    (void)_acquire(false);
}


bool lld::recursive_mutex::try_lock()
{
    return _acquire(true);
}


void lld::recursive_mutex::unlock(const unlock_throw_t throw_on_failure)
{
    try {
        const std::unique_lock<std::recursive_mutex> local(_recursiveMutex, std::try_to_lock);
        if (!local.owns_lock() || _acquisitions.empty())
            throw lld::exception(
                    lld::exception::et_logic_error,
                    LOCKD_SOURCE_FILE,
                    __LINE__,
                    __FUNCTION__,
                    "This thread does not own the recursive mutex."
            );

        // Adopt the original native level too, so a failed remote release still unwinds both
        // levels. The shared guard count is inspected only while the native gate is held.
        const std::unique_lock<std::recursive_mutex> acquired(_recursiveMutex, std::adopt_lock);
        const bool final = _acquisitions.back().use_count() == 1;
        if (final)
            _acquisitions.back()->release();
        _acquisitions.pop_back();
        if (final)
            mutex::unlock(throw_on_failure);
    }
    catch (...) {
        if (throw_on_failure == tt_throw_on_failure)
            throw;
    }
}


lld::mutex::client_info lld::recursive_mutex::info()
{
    const std::lock_guard<std::recursive_mutex> lock(_recursiveMutex);
    return mutex::info();
}


bool lld::recursive_mutex::connected() const
{
    const std::lock_guard<std::recursive_mutex> lock(_recursiveMutex);
    return mutex::connected();
}


const std::string& lld::recursive_mutex::lock_name() const
{
    return mutex::lock_name();
}


void lld::recursive_mutex::lock_name(const std::string& name)
{
    const std::lock_guard<std::recursive_mutex> lock(_recursiveMutex);
    mutex::lock_name(name);
}

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
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Remote lock ownership and queued timed or untimed acquisition requests.
 * Start Date : 04/11/2012
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <algorithm>
#include <cassert>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include <boost/asio/io_context.hpp>

#include "../include/client.hpp"
#include "../include/mutex.hpp"

lkd::mutex::mutex(boost::asio::io_context& io_service, std::string name)
        : _ioService(io_service)
        , _name(std::move(name))
        , _locked(false)
{
}


bool lkd::mutex::locked() const
{
    const std::lock_guard<std::recursive_mutex> lock(_mutex);
    return _locked;
}


bool lkd::mutex::have_queue() const
{
    const std::lock_guard<std::recursive_mutex> lock(_mutex);
    return _locked || !_queue.empty();
}


bool lkd::mutex::try_lock()
{
    const std::lock_guard<std::recursive_mutex> lock(_mutex);
    if (_locked)
        return false;
    _locked = true;
    return _locked;
}


void lkd::mutex::_lockTimeout(const lock_request_ptr& request)
{
    const std::lock_guard<std::recursive_mutex> lock(_mutex);

    // Cancellation may race with an already dispatched expiry. Once this particular request has
    // left the queue, its callback has no authority over any later request by the same client.
    const auto pos = std::find(_queue.begin(), _queue.end(), request);
    if (pos == _queue.end())
        return;
    _queue.erase(pos);
    if (const auto strong = request->client.lock())
        strong->lock_timeout();
}


bool lkd::mutex::lock(const lkd::client_ptr& cptr)
{
    const std::lock_guard<std::recursive_mutex> lock(_mutex);
    if (!_locked) {
        _locked = true;
        return true;
    }

    // Add this client to the queue then.
    _queue.push_back(std::make_shared<lock_request>(cptr));
    return false;
}


void lkd::mutex::cancel(const lkd::weak_client_ptr& cptr)
{
    const std::lock_guard<std::recursive_mutex> lock(_mutex);

    // Each connection can queue only one acquisition. Match shared ownership even if the client
    // has already died; a new connection can reuse its address but cannot share its control block.
    const auto pos = std::find_if(_queue.begin(), _queue.end(), [&cptr](const auto& request) {
        return !request->client.owner_before(cptr) && !cptr.owner_before(request->client);
    });
    if (pos == _queue.end())
        return;
    if ((*pos)->timer)
        (*pos)->timer->cancel();
    _queue.erase(pos);
}


void lkd::mutex::unlock()
{
    const std::lock_guard<std::recursive_mutex> lock(_mutex);
    assert(_locked);
    _locked = false;

    // Are there others waiting? We can notify them that the lock is available if there are.
    while (!_queue.empty()) {
        const auto request = _queue.front();
        _queue.pop_front();
        if (request->timer) {
            request->timer->cancel();
        }
        const client_ptr strong = request->client.lock();
        if (strong && strong->lock_acquired(_name)) {
            _locked = true;
            return;
        }
    }
}


bool lkd::mutex::operator<(const lkd::mutex& rhs) const
{
    return _name < rhs._name;
}

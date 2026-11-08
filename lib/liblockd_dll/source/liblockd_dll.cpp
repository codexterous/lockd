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
 * File       : liblockd_dll.cpp
 * Author     : Brett Gmoser
 * Part Of    : liblockd_dll, a C client library DLL for interfacing with lockd.
 * Purpose    : C ABI for mutex handles, lock guards, errors, and version strings.
 * Start Date : 03/26/2012
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <mutex>
#include <sstream>
#include <string>

#include <liblockd/mutex.hpp>
#include <liblockd/version.hpp>

#include "../include/liblockd_dll/liblockd_dll.h"
#include "../include/liblockd_dll/version.hpp"

namespace detail {
namespace {
std::string& last_error()
{
    // Only threads using the error API need this storage. Namespace-level TLS also initializes
    // on unrelated MSVC async pool workers, which can outlive debug CRT leak reporting.
    thread_local std::string error;
    return error;
}

void set_last_error(const std::string& error_message)
{
    last_error() = error_message;
}

bool copy_string(char* const buf, const std::size_t buflen, const std::string& value)
{
    if (!buf || !buflen)
        return false;

    // buflen is the caller's capacity, including the terminator. Even a truncated result must
    // remain a C string; sizeof(buf) here would only give us the size of a pointer.
    const auto copied = std::min(value.size(), buflen - 1);
    std::memcpy(buf, value.data(), copied);
    buf[copied] = '\0';
    return value.size() < buflen;
}
} // namespace
} // namespace detail


LLDAPI void* create_mutex(
        const char* const server,
        const std::uint16_t port,
        const std::int64_t timeout,
        const char* const lock_name
)
{
    try {
        auto* const mptr = new lld::mutex(server, port, std::chrono::seconds(timeout), lock_name);
        return mptr;
    }
    catch (const std::exception& e) {
        detail::set_last_error(e.what());
        return nullptr;
    }
}


LLDAPI void destroy_mutex(void* const mptr)
{
    delete static_cast<lld::mutex*>(mptr);
}


LLDAPI void* create_lock(void* const mptr)
{
    try {
        auto* const lptr =
                new std::unique_lock<lld::mutex>(*static_cast<lld::mutex*>(mptr), std::defer_lock);

        return lptr;
    }
    catch (const std::exception& e) {
        detail::set_last_error(e.what());
        return nullptr;
    }
}


LLDAPI void destroy_lock(void* const lptr)
{
    delete static_cast<std::unique_lock<lld::mutex>*>(lptr);
}


LLDAPI bool lock(void* const lptr)
{
    try {
        static_cast<std::unique_lock<lld::mutex>*>(lptr)->lock();
        return true;
    }
    catch (const std::exception& e) {
        detail::set_last_error(e.what());
        return false;
    }
}


LLDAPI std::int32_t try_lock(void* const lptr)
{
    try {
        return static_cast<std::unique_lock<lld::mutex>*>(lptr)->try_lock() ? 0 : 1;
    }
    catch (const std::exception& e) {
        detail::set_last_error(e.what());
        return 2;
    }
}


LLDAPI bool unlock(void* const lptr)
{
    try {
        static_cast<std::unique_lock<lld::mutex>*>(lptr)->unlock();
        return true;
    }
    catch (const std::exception& e) {
        detail::set_last_error(e.what());
        return false;
    }
}


LLDAPI bool get_last_error(char* const buf, const std::size_t buflen)
{
    const auto& error = detail::last_error();
    if (error.empty()) {
        detail::copy_string(buf, buflen, "");
        return false;
    }
    return detail::copy_string(buf, buflen, error);
}

LLDAPI bool liblockd_version(char* const buf, const std::size_t buflen)
{
    const std::string lld_ver = lld::version_string();
    const std::string ldd_ver = ldd::version_string();
    std::stringstream ss;
    ss << "liblockd_dll " << ldd_ver << " built on liblockd " << lld_ver;
    const std::string final = ss.str();

    return detail::copy_string(buf, buflen, final);
}

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
 * File       : listener_guard.cpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Reserve one Windows TCP port for a daemon instance.
 * Start Date : 11/06/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <cstdint>
#include <string>

#include <boost/asio/error.hpp>
#include <boost/system/system_error.hpp>

#include "../../../../include/platform/win32/detail/listener_guard.hpp"

lkd::platform::win32::detail::listener_guard::listener_guard(const std::uint16_t port)
        : _handle(_reservePort(port), ::CloseHandle)
{
}


HANDLE lkd::platform::win32::detail::listener_guard::_reservePort(const std::uint16_t port)
{
    std::wstring name = L"Global\\lockd.tcp.port.";
    name += std::to_wstring(port);
    ::SetLastError(ERROR_SUCCESS);
    const auto handle = ::CreateMutexExW(nullptr, name.c_str(), 0, SYNCHRONIZE);
    const DWORD error = ::GetLastError();
    if (!handle)
        throw boost::system::system_error(
                static_cast<int>(error),
                boost::system::system_category(),
                "Cannot reserve TCP port " + std::to_string(port) + " for lockd"
        );
    if (error == ERROR_ALREADY_EXISTS) {
        ::CloseHandle(handle);
        throw boost::system::system_error(
                boost::asio::error::address_in_use,
                "Another lockd instance already reserves TCP port " + std::to_string(port)
        );
    }

    // This is an object-lifetime lease, not a thread-owned mutex lock: there is no
    // WaitForSingleObject/ReleaseMutex pair or thread-affinity requirement. CloseHandle
    // destroys the name when our last handle closes, including on process termination.
    // A rejected contender closes its temporary handle immediately above. No handle is
    // inherited by child processes, and no PID file or stale owner record can survive us.
    return handle;
}

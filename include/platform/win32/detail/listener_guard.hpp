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
 * File       : listener_guard.hpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Reserve one Windows TCP port for a daemon instance.
 * Start Date : 11/06/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#if !defined(LOCKD_PLATFORM_WIN32_DETAIL_LISTENER_GUARD_HPP_INCLUDED_)
# define LOCKD_PLATFORM_WIN32_DETAIL_LISTENER_GUARD_HPP_INCLUDED_

# if defined(_MSC_VER) && (_MSC_VER >= 1020)
#  pragma once
# endif

# include <cstdint>
# include <memory>

# if !defined(WIN32_LEAN_AND_MEAN)
#  define WIN32_LEAN_AND_MEAN
# endif
# include <winsock2.h> // IWYU pragma: keep
# include <windows.h>  // IWYU pragma: keep

namespace lkd::platform::win32::detail {

// Windows SO_REUSEADDR permits competing listeners. Keep one non-inheritable named
// kernel object per TCP port, across addresses, IP families, architectures and sessions.
// This coordinates lockd instances, not unrelated applications using Winsock port reuse.
class listener_guard
{
    /* construction and destruction */
public:
    explicit listener_guard(const std::uint16_t port);
    virtual ~listener_guard() = default;
    listener_guard(const listener_guard&) = delete;
    listener_guard& operator=(const listener_guard&) = delete;

    /* private methods */
private:
    [[nodiscard]] static HANDLE _reservePort(const std::uint16_t port);

    /* private data members */
private:
    const std::unique_ptr<void, decltype(&::CloseHandle)> _handle;
};

} // namespace lkd::platform::win32::detail

#endif // !defined(LOCKD_PLATFORM_WIN32_DETAIL_LISTENER_GUARD_HPP_INCLUDED_)

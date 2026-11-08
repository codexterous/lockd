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
 * File       : service_elevation.hpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Elevate Windows service management operations.
 * Start Date : 10/23/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#if !defined(LOCKD_PLATFORM_WIN32_SERVICE_ELEVATION_HPP_INCLUDED_)
# define LOCKD_PLATFORM_WIN32_SERVICE_ELEVATION_HPP_INCLUDED_

# if defined(_MSC_VER) && (_MSC_VER >= 1020)
#  pragma once
# endif

# include <optional>
# include <string>

namespace lkd::platform::win32 {

// Only service installation and removal call this helper. An empty result means the current
// process is elevated; otherwise the caller must return the child's exit status. Elevation
// failures throw and are reported as fatal by main.
class service_elevation
{
    /* construction and destruction */
public:
    virtual ~service_elevation() = default;

    /* public methods */
public:
    [[nodiscard]] std::optional<int> run() const;
    [[nodiscard]] static std::wstring quote_argument(const std::wstring& argument);
    [[nodiscard]] static std::wstring executable_path();

    /* protected methods */
protected:
    [[nodiscard]] virtual bool elevated() const;
    [[nodiscard]] virtual int relaunch() const;
};

} // namespace lkd::platform::win32

#endif // !defined(LOCKD_PLATFORM_WIN32_SERVICE_ELEVATION_HPP_INCLUDED_)

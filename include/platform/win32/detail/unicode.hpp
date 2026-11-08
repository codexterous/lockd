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
 * File       : unicode.hpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Convert Windows Unicode arguments, paths, and console output.
 * Start Date : 11/07/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#if !defined(LOCKD_PLATFORM_WIN32_DETAIL_UNICODE_HPP_INCLUDED_)
# define LOCKD_PLATFORM_WIN32_DETAIL_UNICODE_HPP_INCLUDED_

# if defined(_MSC_VER) && (_MSC_VER >= 1020)
#  pragma once
# endif

# include <iosfwd>
# include <string>
# include <string_view>
# include <vector>

namespace lkd::platform::win32::detail {

// Shared daemon text is UTF-8. Convert only at the native Windows UTF-16 boundary;
// neither the user's ANSI code page nor the console code page can represent every path.
[[nodiscard]] std::string to_utf8(const std::wstring& text);

// Invalid Unicode is rejected for arguments and settings. Diagnostic text may replace
// invalid third-party bytes so reporting an error does not itself fail conversion.
[[nodiscard]] std::wstring
from_utf8(const std::string_view text, const bool replace_invalid = false);
[[nodiscard]] std::vector<std::string> command_line_arguments();
void write_console(std::ostream& output, const std::string_view text);

} // namespace lkd::platform::win32::detail

#endif // !defined(LOCKD_PLATFORM_WIN32_DETAIL_UNICODE_HPP_INCLUDED_)

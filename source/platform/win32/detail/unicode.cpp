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
 * File       : unicode.cpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Convert Windows Unicode arguments, paths, and console output.
 * Start Date : 11/07/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#ifdef WIN32

# define WIN32_LEAN_AND_MEAN

# include <algorithm>
# include <cstddef>
# include <iostream>
# include <limits>
# include <memory>
# include <stdexcept>
# include <string>
# include <string_view>
# include <vector>

# include <boost/system/error_code.hpp> // IWYU pragma: keep
# include <boost/system/system_error.hpp>

// The Windows umbrella supplies prerequisites for the Shell and console APIs.
# include <windows.h> // IWYU pragma: keep
# include <consoleapi.h>
# include <shellapi.h>
# include <stringapiset.h>
# include <winnls.h>

# include "../../../../include/platform/win32/detail/unicode.hpp"

namespace {

// Preserve stream redirection through rdbuf(), including the test suite's captures.
// Only the original standard streams may bypass their byte buffers for WriteConsoleW.
std::streambuf* const STDOUT_BUFFER = std::cout.rdbuf();
std::streambuf* const STDERR_BUFFER = std::cerr.rdbuf();
std::streambuf* const STDLOG_BUFFER = std::clog.rdbuf();

int conversion_size(const std::size_t size)
{
    if (size > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
        throw std::length_error("Unicode text exceeds the Windows conversion limit");
    return static_cast<int>(size);
}

void conversion_failed()
{
    throw boost::system::system_error(
            static_cast<int>(::GetLastError()),
            boost::system::system_category(),
            "Cannot convert Windows Unicode text"
    );
}

} // namespace


std::string lkd::platform::win32::detail::to_utf8(const std::wstring& text)
{
    if (text.empty())
        return {};
    const auto length = conversion_size(text.size());
    const int size = ::WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length, nullptr, 0, nullptr, nullptr
    );
    if (!size)
        conversion_failed();
    std::string result(static_cast<std::size_t>(size), '\0');
    if (!::WideCharToMultiByte(
                CP_UTF8,
                WC_ERR_INVALID_CHARS,
                text.data(),
                length,
                result.data(),
                size,
                nullptr,
                nullptr
        ))
        conversion_failed();
    return result;
}


namespace {

std::wstring decode_utf8(const std::string_view text, const DWORD flags)
{
    if (text.empty())
        return {};
    const auto length = conversion_size(text.size());
    const int size = ::MultiByteToWideChar(CP_UTF8, flags, text.data(), length, nullptr, 0);
    if (!size)
        conversion_failed();
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (!::MultiByteToWideChar(CP_UTF8, flags, text.data(), length, result.data(), size))
        conversion_failed();
    return result;
}


} // namespace


std::wstring
lkd::platform::win32::detail::from_utf8(const std::string_view text, const bool replace_invalid)
{
    return decode_utf8(text, replace_invalid ? 0 : MB_ERR_INVALID_CHARS);
}


std::vector<std::string> lkd::platform::win32::detail::command_line_arguments()
{
    int count = 0;
    auto* const arguments = ::CommandLineToArgvW(::GetCommandLineW(), &count);
    if (!arguments)
        conversion_failed();
    const std::unique_ptr<void, decltype(&::LocalFree)> guard(arguments, ::LocalFree);
    std::vector<std::string> result;
    result.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index)
        result.push_back(to_utf8(arguments[index]));
    return result;
}


void lkd::platform::win32::detail::write_console(std::ostream& output, const std::string_view text)
{
    const bool stdout_stream = &output == &std::cout && output.rdbuf() == STDOUT_BUFFER;
    const bool stderr_stream = (&output == &std::cerr && output.rdbuf() == STDERR_BUFFER) ||
                               (&output == &std::clog && output.rdbuf() == STDLOG_BUFFER);
    const auto handle = ::GetStdHandle(stdout_stream ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
    DWORD mode = 0;
    if ((!stdout_stream && !stderr_stream) || !::GetConsoleMode(handle, &mode)) {
        // Files, pipes, and captured streams retain UTF-8 bytes, without a BOM.
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        return;
    }
    if (text.empty())
        return;

    // Flush preceding color sequences before using the native Unicode console API.
    // Do not change a shell's code page or the process-wide C/C++ locale.
    output.flush();

    // A third-party diagnostic may contain invalid UTF-8. Replace those bytes for display
    // instead of throwing another exception while reporting the original failure. Arguments
    // and service settings still use the strict conversion above.
    const auto wide = decode_utf8(text, 0);
    for (std::size_t offset = 0; offset < wide.size();) {
        DWORD written = 0;
        auto count = static_cast<DWORD>((std::min)(wide.size() - offset, std::size_t{16384}));

        // Bound native writes without splitting a supplementary character's surrogate pair.
        if (offset + count < wide.size() && wide[offset + count - 1] >= 0xd800 &&
            wide[offset + count - 1] <= 0xdbff)
            --count;
        if (!::WriteConsoleW(handle, wide.data() + offset, count, &written, nullptr) || !written) {
            output.setstate(std::ios::badbit);
            return;
        }
        offset += written;
    }
}

#endif // WIN32

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
 * File       : service_elevation.cpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Elevate Windows service management operations.
 * Start Date : 10/23/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#ifdef WIN32

# if !defined(WIN32_LEAN_AND_MEAN)
#  define WIN32_LEAN_AND_MEAN
# endif

# include <cstddef>
# include <filesystem>
# include <memory>
# include <optional>
# include <string>

# include <boost/system/error_code.hpp> // IWYU pragma: keep
# include <boost/system/system_error.hpp>

// Winsock2 precedes windows.h; the Shell and COM APIs need the Windows declarations.
// Winsock2 must be initialized before windows.h to avoid the older Winsock declarations.
# include <winsock2.h> // IWYU pragma: keep
// Windows SDK declarations require the public umbrella and its prerequisite definitions.
# include <windows.h> // IWYU pragma: keep
# include <objbase.h>
# include <shellapi.h>

# include "../../../include/logging.hpp"
# include "../../../include/platform/win32/service_elevation.hpp"


std::optional<int> lkd::platform::win32::service_elevation::run() const
{
    if (elevated())
        return std::nullopt;

    // Elevation failures are system failures, including cancellation. Let main report the
    // exception as fatal; do not log twice or retry the UAC prompt.
    return relaunch();
}


std::wstring lkd::platform::win32::service_elevation::quote_argument(const std::wstring& argument)
{
    // Windows argument parsing treats backslashes specially only before a quote. Double those
    // before embedded quotes and the closing quote, preserving empty arguments and trailing slashes.
    std::wstring result(1, L'"');
    std::size_t backslashes = 0;
    for (const auto character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        result.append(character == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
        result += character;
        backslashes = 0;
    }
    result.append(backslashes * 2, L'\\');
    result += L'"';
    return result;
}


bool lkd::platform::win32::service_elevation::elevated() const
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
        throw boost::system::system_error(
                static_cast<int>(::GetLastError()),
                boost::system::system_category(),
                "Cannot query service installation privileges"
        );
    const std::unique_ptr<void, decltype(&::CloseHandle)> token_guard(token, ::CloseHandle);
    TOKEN_ELEVATION elevation = {};
    DWORD size = 0;
    if (!::GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size))
        throw boost::system::system_error(
                static_cast<int>(::GetLastError()),
                boost::system::system_category(),
                "Cannot determine whether service installation is elevated"
        );
    return elevation.TokenIsElevated != 0;
}


std::wstring lkd::platform::win32::service_elevation::executable_path()
{
    std::wstring executable(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = ::GetModuleFileNameW(
                nullptr, executable.data(), static_cast<DWORD>(executable.size())
        );
        if (length == 0)
            throw boost::system::system_error(
                    static_cast<int>(::GetLastError()),
                    boost::system::system_category(),
                    "Cannot locate lockd for service elevation"
            );
        if (length < executable.size()) {
            executable.resize(length);
            break;
        }
        executable.resize(executable.size() * 2);
    }

    return executable;
}


int lkd::platform::win32::service_elevation::relaunch() const
{
    // Resolve our actual executable, not argv[0] or PATH, and preserve the native Unicode command
    // line. This also avoids a shell interpreting metacharacters in a service name or interface.
    const auto executable = executable_path();
    const auto working_directory = std::filesystem::current_path().wstring();

    int count = 0;
    auto* const arguments = ::CommandLineToArgvW(::GetCommandLineW(), &count);
    if (!arguments)
        throw boost::system::system_error(
                static_cast<int>(::GetLastError()),
                boost::system::system_category(),
                "Cannot read arguments for service elevation"
        );
    const std::unique_ptr<void, decltype(&::LocalFree)> argument_guard(arguments, ::LocalFree);
    std::wstring parameters;
    for (int index = 1; index < count; ++index) {
        if (index > 1)
            parameters += L' ';
        parameters += quote_argument(arguments[index]);
    }

    SHELLEXECUTEINFOW launch = {};
    launch.cbSize = sizeof(launch);
    launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    launch.lpVerb = L"runas";
    launch.lpFile = executable.c_str();
    launch.lpParameters = parameters.c_str();

    // Relative log paths and the default must resolve identically after elevation.
    launch.lpDirectory = working_directory.c_str();
    launch.nShow = SW_HIDE;

    // Shell extensions can need COM. Balance successful initialization, including S_FALSE,
    // while leaving an existing apartment alone if another subsystem initialized it differently.
    const HRESULT initialized =
            ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
        throw boost::system::system_error(
                static_cast<int>(initialized),
                boost::system::system_category(),
                "Cannot initialize service elevation"
        );
    const BOOL launched = ::ShellExecuteExW(&launch);
    const DWORD launch_error = launched ? ERROR_SUCCESS : ::GetLastError();
    if (SUCCEEDED(initialized))
        ::CoUninitialize();
    if (!launched)
        throw boost::system::system_error(
                static_cast<int>(launch_error),
                boost::system::system_category(),
                launch_error == ERROR_CANCELLED
                        ? "Service elevation was canceled"
                        : "Cannot elevate service operation; try an administrator terminal"
        );

    const std::unique_ptr<void, decltype(&::CloseHandle)> process(launch.hProcess, ::CloseHandle);
    if (!process)
        throw boost::system::system_error(
                ERROR_INVALID_HANDLE,
                boost::system::system_category(),
                "Service elevation did not return a process handle"
        );
    if (::WaitForSingleObject(process.get(), INFINITE) != WAIT_OBJECT_0)
        throw boost::system::system_error(
                static_cast<int>(::GetLastError()),
                boost::system::system_category(),
                "Cannot wait for elevated service operation"
        );
    DWORD exit_code = 0;
    if (!::GetExitCodeProcess(process.get(), &exit_code))
        throw boost::system::system_error(
                static_cast<int>(::GetLastError()),
                boost::system::system_category(),
                "Cannot read elevated service operation result"
        );

    // The elevated child has a separate console. Report its result in the original terminal;
    // detailed failures can be inspected by running the same command in an administrator terminal.
    console_print(
            exit_code == 0 ? cp_stdout : cp_stderr,
            exit_code == 0                     ? ll_info
            : exit_code == 7 || exit_code == 8 ? ll_error
                                               : ll_fatal,
            exit_code == 0
                    ? "Service operation completed successfully.\n"
                    : "Service operation failed (exit code " + std::to_string(exit_code) +
                              "). Run the command in an administrator terminal for details.\n"
    );
    return static_cast<int>(exit_code);
}

#endif // WIN32

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
 * File       : service.hpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Install, remove, and run the Windows service.
 * Start Date : 07/31/2023
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#if !defined(LOCKD_LOCKD_SERVICE_HPP_INCLUDED_)
# define LOCKD_LOCKD_SERVICE_HPP_INCLUDED_

# if defined(_MSC_VER) && (_MSC_VER >= 1020)
#  pragma once
# endif

# ifdef WIN32

#  include <cstdint>
#  include <filesystem>
#  include <string>

// Keep the optional COM headers from redefining our interface parameter names.
#  if !defined(WIN32_LEAN_AND_MEAN)
#   define WIN32_LEAN_AND_MEAN
#  endif

// Winsock2 must precede windows.h so this header can be used with Asio.
#  include <winsock2.h>
#  include <windows.h>

#  include "../../logging.hpp"

namespace lkd::platform::win32 {

class service_installer
{
    /* construction and destruction */
public:
    service_installer();
    virtual ~service_installer();

    /* public methods */
public:
    static std::string executable_path();
    [[nodiscard]] static bool
    owns_command_line(const std::wstring& command, const std::filesystem::path& executable);
    [[nodiscard]] static std::string command_line(
            const std::string& executable,
            const std::string& service_name,
            const std::string& listen_interface,
            const std::uint16_t port,
            const unsigned short threads,
            const log_level_t level,
            const std::filesystem::path& log_file,
            const bool event_log,
            const unsigned int rotate_size_mib = logging::DEFAULT_LOG_ROTATE_SIZE_MIB,
            const unsigned int rotate_keep = logging::DEFAULT_LOG_ROTATE_KEEP
    );
    [[nodiscard]] static std::string description(
            const std::string& listen_interface,
            const std::uint16_t port,
            const unsigned short threads,
            const log_level_t level,
            const bool event_log
    );
    bool install_service(
            const std::string& service_name,
            const std::string& listen_interface,
            const std::uint16_t port,
            const unsigned short threads,
            const log_level_t level,
            const std::filesystem::path& log_file,
            const bool event_log,
            const unsigned int rotate_size_mib = logging::DEFAULT_LOG_ROTATE_SIZE_MIB,
            const unsigned int rotate_keep = logging::DEFAULT_LOG_ROTATE_KEEP
    );
    [[nodiscard]] bool uninstall_service() const;

    // Keep the handle constant, not the Windows object behind it.
    static bool get_service_executable_path(
            // NOLINTNEXTLINE(misc-misplaced-const)
            const SC_HANDLE service,
            std::string* const executable_path
    );

    /* private methods */
private:
    [[nodiscard]] bool _removeExistingService(const std::string& service_name) const;

    /* private data members */
private:
    SC_HANDLE _schSCManager;
    SC_HANDLE _schService;
    std::string _executablePath;
};


class service_runner
{
    /* construction and destruction */
public:
    service_runner(
            std::string service_name,
            std::string listen_interface,
            const std::uint16_t port,
            const unsigned short threads
    );
    virtual ~service_runner() = default;

    /* public methods, observers */
public:
    [[nodiscard]] const std::string& service_name() const { return _serviceName; }
    [[nodiscard]] const std::string& interface() const { return _interface; }
    [[nodiscard]] std::uint16_t port() const { return _port; }
    [[nodiscard]] unsigned short threads() const { return _threads; }

    /* public methods */
public:
    void report_service_event(const char* const function, const DWORD error) const;
    static void
    report_service_status(const DWORD current_state, const DWORD exit_code, const DWORD wait_hint);
    void service_init() const;

    // Windows calls these through WINAPI function pointers. The calling convention matters
    // on x86 even though an ordinary C++ function appears to work on x64.
    static void WINAPI service_control_handler(const DWORD dwCtrl);
    static void WINAPI service_main(const DWORD argument_count, LPWSTR* const arguments);

    /* private data members */
private:
    static SERVICE_STATUS _serviceStatus;
    static SERVICE_STATUS_HANDLE _serviceStatusHandle;
    static HANDLE _serviceStopEvent;
    const std::string _serviceName;
    const std::string _interface;
    const std::uint16_t _port;
    const unsigned short _threads;
};


// Startup elevates before calling this function and validates the log path for installation.
int manage_service(
        const bool install,
        const std::string& service_name,
        const std::string& listen_interface,
        const std::uint16_t port,
        const unsigned short threads,
        const log_level_t level,
        const std::filesystem::path& log_file,
        const bool event_log,
        const unsigned int rotate_size_mib = logging::DEFAULT_LOG_ROTATE_SIZE_MIB,
        const unsigned int rotate_keep = logging::DEFAULT_LOG_ROTATE_KEEP
);
int run_service(
        const std::string& service_name,
        const std::string& listen_interface,
        const std::uint16_t port,
        const unsigned short threads,
        const bool event_log
);

} // namespace lkd::platform::win32

# endif // WIN32

#endif // !defined(LOCKD_LOCKD_SERVICE_HPP_INCLUDED_)

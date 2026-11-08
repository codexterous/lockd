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
 * File       : service.cpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Install, remove, and run the Windows service.
 * Start Date : 07/31/2023
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#ifdef WIN32

# define WIN32_LEAN_AND_MEAN

# include <chrono>
# include <cstdint>
# include <cstdlib>
# include <exception>
# include <filesystem>
# include <memory>
# include <string>
# include <system_error>
# include <thread>
# include <type_traits>
# include <utility>

// The public Format header supplies the template definitions used by free_funcs.hpp.
# include <boost/format.hpp> // IWYU pragma: keep
# include <boost/format/free_funcs.hpp>
# include <boost/system/error_code.hpp> // IWYU pragma: keep
# include <boost/system/system_error.hpp>

// Winsock2 must be initialized before windows.h to avoid the older Winsock declarations.
# include <winsock2.h> // IWYU pragma: keep
// Windows SDK declarations require the public umbrella and its prerequisite definitions.
# include <windows.h> // IWYU pragma: keep
# include <shellapi.h>
# include <stringapiset.h>
# include <winnls.h>

# include <liblockd/detail/source_path.hpp>
# include <liblockd/exceptions.hpp>

# include "../../../include/lockd.hpp"
# include "../../../include/logging.hpp"
# include "../../../include/platform/win32/detail/unicode.hpp"
# include "../../../include/platform/win32/event_log.hpp"
# include "../../../include/platform/win32/service.hpp"
# include "../../../include/platform/win32/service_elevation.hpp"
# include "../../../include/server.hpp"

bool lkd::platform::win32::service_installer::owns_command_line(
        const std::wstring& command, const std::filesystem::path& executable
)
{
    // Parse with Windows, rather than matching a prefix which may include another
    // executable or quoted arguments. File identity also handles case and path aliases.
    if (command.empty())
        return false;
    int count = 0;
    auto* const arguments = ::CommandLineToArgvW(command.c_str(), &count);
    if (!arguments)
        throw boost::system::system_error(
                static_cast<int>(::GetLastError()),
                boost::system::system_category(),
                "Cannot parse the installed service command"
        );
    const std::unique_ptr<void, decltype(&::LocalFree)> guard(arguments, ::LocalFree);
    if (!count || !arguments[0][0])
        return false;
    std::error_code error;
    const bool owned = std::filesystem::equivalent(arguments[0], executable, error);
    if (error)
        throw boost::system::system_error(
                error.value(),
                boost::system::system_category(),
                "Cannot verify the service executable"
        );
    return owned;
}


std::string lkd::platform::win32::service_installer::command_line(
        const std::string& executable,
        const std::string& service_name,
        const std::string& listen_interface,
        const std::uint16_t port,
        const unsigned short threads,
        const log_level_t level,
        const std::filesystem::path& log_file,
        const bool event_log,
        const unsigned int rotate_size_mib,
        const unsigned int rotate_keep
)
{
    // The SCM does not use the installer's working directory. Preserve the selected file,
    // including the default, as an absolute path alongside the other installed settings.
    auto command = str(
            boost::format(
                    R"(%1% --as-service -i "%2%" -p %3% -t %4% -n "%5%" -l %6% --log-file "%7%"%8%)"
            ) %
            executable % listen_interface % port % threads % service_name % log_level_input(level) %
            std::filesystem::absolute(log_file).u8string() % (event_log ? "" : " --no-event-log")
    );
    if (rotate_size_mib != logging::DEFAULT_LOG_ROTATE_SIZE_MIB)
        command += " --log-rotate-size " + std::to_string(rotate_size_mib);
    if (rotate_keep != logging::DEFAULT_LOG_ROTATE_KEEP)
        command += " --log-rotate-keep " + std::to_string(rotate_keep);
    return command;
}


std::string lkd::platform::win32::service_installer::description(
        const std::string& listen_interface,
        const std::uint16_t port,
        const unsigned short threads,
        const log_level_t level,
        const bool event_log
)
{
    return str(
            boost::format(
                    "lockd, an interprocess locking daemon, running on '%1%' port %2% with "
                    "%3% acquisition threads and log level '%4%'.%5%"
            ) %
            listen_interface % port % threads % log_level_input(level) %
            (event_log ? "" : " Windows Event Log output is disabled.")
    );
}


int lkd::platform::win32::manage_service(
        const bool install,
        const std::string& service_name,
        const std::string& listen_interface,
        const std::uint16_t port,
        const unsigned short threads,
        const log_level_t level,
        const std::filesystem::path& log_file,
        const bool event_log,
        const unsigned int rotate_size_mib,
        const unsigned int rotate_keep
)
{
    const int failure = install ? 7 : 8;
    service_installer installer;
    const bool result = install ? installer.install_service(
                                          service_name,
                                          listen_interface,
                                          port,
                                          threads,
                                          level,
                                          log_file,
                                          event_log,
                                          rotate_size_mib,
                                          rotate_keep
                                  )
                                : installer.uninstall_service();
    return result ? EXIT_SUCCESS : failure;
}


namespace lkd::platform::win32 {
namespace {

std::string get_error_description(const DWORD error_code)
{
    // Take Windows error codes that are returned for functions that we use, and genericize them
    // to give the user a better understanding of what may be happening.
    switch (error_code) {
    case ERROR_ACCESS_DENIED: return "The request was denied.";
    case ERROR_INVALID_HANDLE: return "The specified handle is invalid.";
    case ERROR_SERVICE_MARKED_FOR_DELETE:
        return "The specified service has been marked for deletion.";
    case ERROR_MORE_DATA: return "The buffer is too small, try again with a larger buffer.";
    case ERROR_INVALID_PARAMETER: return "An illegal parameter value was used.";
    case ERROR_INVALID_LEVEL: return "A parameter contains an unsupported value.";
    case ERROR_SHUTDOWN_IN_PROGRESS:
        return "The system is shutting down; this function cannot be called.";
    case ERROR_INVALID_NAME: return "The specified service name is invalid.";
    case ERROR_SERVICE_DOES_NOT_EXIST: return "The specified service does not exist.";
    case ERROR_INSUFFICIENT_BUFFER:
        return "Insufficient buffer space was available. Try again with a larger buffer.";
    case ERROR_DATABASE_DOES_NOT_EXIST: return "The specified database does not exist.";
    case ERROR_CIRCULAR_DEPENDENCY: return "A circular service dependency was specified.";
    case ERROR_DUPLICATE_SERVICE_NAME:
        return "The display name already exists in the service control manager database either as "
               "a service name or as another display name.";
    case ERROR_INVALID_SERVICE_ACCOUNT:
        return "The user account name specified in the lpServiceStartName parameter does not "
               "exist.";
    case ERROR_SERVICE_EXISTS: return "The specified service already exists in this database.";
    default: return "Unknown error";
    }
}

} // namespace
} // namespace lkd::platform::win32


lkd::platform::win32::service_installer::service_installer()
        : _schSCManager(nullptr)
        , _schService(nullptr)
{
    // Get a handle to the SCM database. We'll need this for both installing and uninstalling.
    this->_schSCManager = ::OpenSCManagerW(
            nullptr, // local computer
            nullptr, // ServicesActive database
            SC_MANAGER_ALL_ACCESS
    ); // full access rights

    if (nullptr == _schSCManager) {
        const DWORD last_error = ::GetLastError();
        throw lld::exception(
                lld::exception::et_unknown_exception,
                LOCKD_SOURCE_FILE,
                __LINE__,
                __FUNCTION__,
                str(boost::format("Service operation failed; OpenSCManager (%1$#x): %2%") %
                    last_error % get_error_description(last_error))
        );
    }

    // Determine our full executable path. This function can throw.
    _executablePath = executable_path();
}


lkd::platform::win32::service_installer::~service_installer()
{
    if (this->_schService) {
        ::CloseServiceHandle(this->_schService);
    }
    if (this->_schSCManager) {
        ::CloseServiceHandle(this->_schSCManager);
    }
}


std::string lkd::platform::win32::service_installer::executable_path()
{
    // Reuse the native path reader, which also handles paths longer than MAX_PATH.
    return "\"" + detail::to_utf8(service_elevation::executable_path()) + "\"";
}


bool lkd::platform::win32::service_installer::get_service_executable_path(
        // NOLINTNEXTLINE(misc-misplaced-const): The handle itself is constant.
        const SC_HANDLE service,
        std::string* const executable_path
)
{
    // Get the service configuration information. The process is quite arduous - we first have to
    // use QueryServiceConfig to determine the number of bytes we need to store the information.
    // https://learn.microsoft.com/en-us/windows/win32/services/querying-a-service-s-configuration
    DWORD additional_needed = 0;
    if (!::QueryServiceConfigW(service, nullptr, 0, &additional_needed)) {
        const DWORD last_error = ::GetLastError();
        if (ERROR_INSUFFICIENT_BUFFER == last_error) {
            // The byte count includes the configuration structure and its trailing strings.
            const auto buffer = std::make_unique<unsigned char[]>(additional_needed);
            auto* const config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.get());
            bool result = false;
            if (::QueryServiceConfigW(service, config, additional_needed, &additional_needed)) {
                // Copy the path while the buffer still owns its string storage.
                result = true;
                *executable_path = detail::to_utf8(config->lpBinaryPathName);
            }

            // If we were not successful, simply ignore this service.
            return result;
        }

        // Some other failure.
        return false;
    }

    // QueryServiceConfig should never return TRUE on the first run.
    return false;
}


bool lkd::platform::win32::service_installer::uninstall_service() const
{
    // Uninstall the lockd service, if it exists. While we could allow the user to specify the name
    // of any arbitrary service on the command line and just delete that one, that doesn't feel
    // right - we're not a service deletion tool. Instead, we'll enumerate all services on the
    // machine, and find the one that has the same exact executable path as our own executable. If
    // we find one, we'll delete it.
    bool got_all = false;
    const auto native_executable = detail::from_utf8(_executablePath);
    const auto executable_length = static_cast<int>(native_executable.size());

    // Begin the arduous task of enumerating services by starting with a reasonable buffer size of
    // 1024 services. We'll increase that if necessary.
    DWORD buffer_size = 1024;
    while (!got_all) {
        DWORD additional_needed = 0;
        DWORD cnt = 0;
        DWORD resume = 0;
        const auto services = std::unique_ptr<ENUM_SERVICE_STATUS_PROCESSW[]>{
                new ENUM_SERVICE_STATUS_PROCESSW[buffer_size]
        };

        if (::EnumServicesStatusExW(
                    _schSCManager,
                    SC_ENUM_PROCESS_INFO,      // Influences 5th parameter!
                    SERVICE_WIN32_OWN_PROCESS, // Service type (SERVICE_WIN32_OWN_PROCESS =
                                               // services that run in their own process)
                    SERVICE_STATE_ALL,         // Service state (ALL = active and inactive
                                               // ones)
                    reinterpret_cast<LPBYTE>(services.get()),
                    sizeof(services[0]) * buffer_size,
                    &additional_needed,
                    &cnt,
                    &resume,
                    nullptr
            )) // Group name
        {
            got_all = true;
            for (DWORD i = 0; i < cnt; i++) {
                // Keep the handle constant, not the Windows object behind it.
                // NOLINTNEXTLINE(misc-misplaced-const)
                const SC_HANDLE service = ::OpenServiceW(
                        _schSCManager, services[i].lpServiceName, SC_MANAGER_ALL_ACCESS
                );
                if (service) {
                    std::string executable_path;
                    if (get_service_executable_path(service, &executable_path)) {
                        const auto native_command = detail::from_utf8(executable_path);

                        // Preserve the quoted executable-prefix match, using Windows case
                        // comparison rather than folding individual bytes of UTF-8 text.
                        if (native_command.size() >= native_executable.size() &&
                            ::CompareStringOrdinal(
                                    native_command.data(),
                                    executable_length,
                                    native_executable.data(),
                                    executable_length,
                                    TRUE
                            ) == CSTR_EQUAL) {
                            bool success = false;
                            if (!::DeleteService(service)) {
                                // We found us!
                                const DWORD last_error = ::GetLastError();
                                lkd::console_print(
                                        lkd::cp_stderr,
                                        lkd::ll_error,
                                        str(boost::format(
                                                    "Service deletion failed; "
                                                    "DeleteService (%1$#x): %2%\n"
                                            ) %
                                            last_error % get_error_description(last_error))
                                );
                                success = false;
                            }
                            else {
                                lkd::console_print(
                                        lkd::cp_stdout,
                                        lkd::ll_info,
                                        str(boost::format(
                                                    "Service '%1%' was "
                                                    "uninstalled successfully\n"
                                            ) %
                                            detail::to_utf8(services[i].lpServiceName))
                                );
                                success = true;
                            }
                            ::CloseServiceHandle(service);
                            return success;
                        }
                    }
                    ::CloseServiceHandle(service);
                }
            }
        }
        else {
            const DWORD last_error = ::GetLastError();

            // According to the documentation:
            // https://learn.microsoft.com/en-us/windows/win32/api/winsvc/nf-winsvc-enumservicesstatusexa
            // ERROR_MORE_DATA should be returned by ::GetLastError() if additional room is needed.
            // Occasionally, on my machine, a different error code is returned, but with
            // additional_needed being non-zero. So, we check for that, too.
            if (additional_needed || ERROR_MORE_DATA == last_error) {
                // EnumServicesStatusEx indicated that we don't have enough room in our buffer to
                // store all the services on this machine. Increase our buffer and try again. The
                // additional_needed argument is sized in bytes.
                buffer_size += additional_needed / sizeof(services[0]);
            }
            else {
                lkd::console_print(
                        lkd::cp_stderr,
                        lkd::ll_error,
                        str(boost::format(
                                    "Service deletion failed; EnumServicStatusEx (%1$#x): "
                                    "%2%\n"
                            ) %
                            last_error % get_error_description(last_error))
                );
                return false;
            }
        }
    }

    // If we get to this point, we didn't find a service with our executable name.
    lkd::console_print(
            lkd::cp_stderr,
            lkd::ll_error,
            str(boost::format(
                        "Service deletion failed; did not find a service with a binary "
                        "path name matching this executable (%1%). If the service still "
                        "exists, you may need to delete it manually.\n"
                ) %
                _executablePath)
    );
    return false;
}


bool lkd::platform::win32::service_installer::_removeExistingService(
        const std::string& service_name
) const
{
    const auto native_name = detail::from_utf8(service_name);
    const auto service = ::OpenServiceW(
            _schSCManager,
            native_name.c_str(),
            SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS | SERVICE_STOP | DELETE
    );
    if (!service)
        throw boost::system::system_error(
                static_cast<int>(::GetLastError()),
                boost::system::system_category(),
                "Cannot open the existing service for replacement"
        );
    std::unique_ptr<std::remove_pointer_t<SC_HANDLE>, decltype(&::CloseServiceHandle)> guard(
            service, ::CloseServiceHandle
    );

    // Query the named service, not the first service found for this executable.
    // Use the wide command line so the ownership check sees the actual Windows path.
    DWORD bytes = 0;
    const bool sized = ::QueryServiceConfigW(service, nullptr, 0, &bytes) != FALSE;
    const DWORD size_error = sized ? ERROR_INVALID_DATA : ::GetLastError();
    if (size_error != ERROR_INSUFFICIENT_BUFFER)
        throw boost::system::system_error(
                static_cast<int>(size_error),
                boost::system::system_category(),
                "Cannot size the existing service configuration"
        );
    const auto buffer = std::make_unique<unsigned char[]>(bytes);
    auto* const configuration = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.get());
    if (!::QueryServiceConfigW(service, configuration, bytes, &bytes))
        throw boost::system::system_error(
                static_cast<int>(::GetLastError()),
                boost::system::system_category(),
                "Cannot read the existing service configuration"
        );
    if (!owns_command_line(configuration->lpBinaryPathName, service_elevation::executable_path())) {
        console_print(
                cp_stderr,
                ll_error,
                "Service '" + service_name +
                        "' belongs to another executable; it was not changed.\n"
        );
        return false;
    }

    console_print(cp_stdout, ll_info, "Replacing existing service '" + service_name + "'.\n");

    // Allow the daemon's 30-second graceful shutdown to finish. Never delete the
    // registration while its old process can still own sockets or client locks.
    constexpr auto operation_timeout = std::chrono::seconds(45);
    const auto stop_deadline = std::chrono::steady_clock::now() + operation_timeout;
    bool stop_requested = false;
    for (;;) {
        SERVICE_STATUS status = {};
        if (!::QueryServiceStatus(service, &status))
            throw boost::system::system_error(
                    static_cast<int>(::GetLastError()),
                    boost::system::system_category(),
                    "Cannot query the service during replacement"
            );
        if (status.dwCurrentState == SERVICE_STOPPED)
            break;
        if (!stop_requested &&
            (status.dwCurrentState == SERVICE_RUNNING || status.dwCurrentState == SERVICE_PAUSED)) {
            if (!::ControlService(service, SERVICE_CONTROL_STOP, &status) &&
                ::GetLastError() != ERROR_SERVICE_NOT_ACTIVE)
                throw boost::system::system_error(
                        static_cast<int>(::GetLastError()),
                        boost::system::system_category(),
                        "Cannot stop the existing service"
                );
            stop_requested = true;
        }
        if (std::chrono::steady_clock::now() >= stop_deadline)
            throw boost::system::system_error(
                    ERROR_SERVICE_REQUEST_TIMEOUT,
                    boost::system::system_category(),
                    "Timed out stopping the existing service; its registration was not removed"
            );
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!::DeleteService(service))
        throw boost::system::system_error(
                static_cast<int>(::GetLastError()),
                boost::system::system_category(),
                "Cannot remove the existing service"
        );
    guard.reset();

    // DeleteService only marks the record for deletion. Close our own handle first,
    // then wait for any other service handles to be released before creating its successor.
    const auto delete_deadline = std::chrono::steady_clock::now() + operation_timeout;
    for (;;) {
        const auto remaining =
                ::OpenServiceW(_schSCManager, native_name.c_str(), SERVICE_QUERY_STATUS);
        if (remaining) {
            ::CloseServiceHandle(remaining);
        }
        else {
            const DWORD error = ::GetLastError();
            if (error == ERROR_SERVICE_DOES_NOT_EXIST)
                return true;
            if (error != ERROR_SERVICE_MARKED_FOR_DELETE)
                throw boost::system::system_error(
                        static_cast<int>(error),
                        boost::system::system_category(),
                        "Cannot confirm deletion of the existing service"
                );
        }
        if (std::chrono::steady_clock::now() >= delete_deadline)
            throw boost::system::system_error(
                    ERROR_SERVICE_REQUEST_TIMEOUT,
                    boost::system::system_category(),
                    "Service deletion is still pending; close service-management windows and retry"
            );
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}


bool lkd::platform::win32::service_installer::install_service(
        const std::string& service_name,
        const std::string& listen_interface,
        const std::uint16_t port,
        const unsigned short threads,
        const log_level_t level,
        const std::filesystem::path& log_file,
        const bool event_log,
        const unsigned int rotate_size_mib,
        const unsigned int rotate_keep
)
{
    const auto native_name = detail::from_utf8(service_name);
    const auto command = detail::from_utf8(command_line(
            _executablePath,
            service_name,
            listen_interface,
            port,
            threads,
            level,
            log_file,
            event_log,
            rotate_size_mib,
            rotate_keep
    ));

    // Retry creation only after removing the verified, same-executable service.
    // If another installer takes the name meanwhile, do not remove its new record.
    bool replaced = false;
    for (;;) {
        this->_schService = ::CreateServiceW(
                this->_schSCManager,       // SCM database
                native_name.c_str(),       // name of service
                native_name.c_str(),       // service name to display
                SERVICE_ALL_ACCESS,        // desired access
                SERVICE_WIN32_OWN_PROCESS, // service type
                SERVICE_DEMAND_START,      // start type
                SERVICE_ERROR_NORMAL,      // error control type
                command.c_str(),           // path to service's binary
                nullptr,                   // no load ordering group
                nullptr,                   // no tag identifier
                nullptr,                   // no dependencies
                nullptr,                   // LocalSystem account
                nullptr                    // no password
        );

        if (this->_schService != nullptr)
            break;
        const DWORD last_error = ::GetLastError();
        if (last_error == ERROR_SERVICE_EXISTS && !replaced) {
            if (!_removeExistingService(service_name))
                return false;
            replaced = true;
            continue;
        }
        lkd::console_print(
                lkd::cp_stderr,
                lkd::ll_error,
                str(boost::format("Service creation failed; CreateService (%1$#x): %2%\n") %
                    last_error % get_error_description(last_error))
        );
        return false;
    }

    // The service was successfully installed, let's give it a description.
    auto native_description =
            detail::from_utf8(description(listen_interface, port, threads, level, event_log));
    SERVICE_DESCRIPTIONW service_description;
    service_description.lpDescription = native_description.data();

    if (!::ChangeServiceConfig2W(
                this->_schService, SERVICE_CONFIG_DESCRIPTION, &service_description
        )) {
        // The documentation for this function does not list possible values for ::GetLastError.
        // https://learn.microsoft.com/en-us/windows/win32/api/winsvc/nf-winsvc-changeserviceconfig2a
        lkd::console_print(
                lkd::cp_stderr,
                lkd::ll_warning,
                str(boost::format("WARNING: Unable to set service description (%1$#x)\n") %
                    ::GetLastError())
        );
    }

    lkd::console_print(
            lkd::cp_stdout,
            lkd::ll_info,
            str(boost::format("Service '%1%' was installed successfully\n") % service_name)
    );

    return true;
}


SERVICE_STATUS lkd::platform::win32::service_runner::_serviceStatus;
SERVICE_STATUS_HANDLE lkd::platform::win32::service_runner::_serviceStatusHandle = nullptr;
HANDLE lkd::platform::win32::service_runner::_serviceStopEvent = nullptr;
namespace lkd::platform::win32 {
namespace {

service_runner* SERVICE_RUNNER = nullptr;

} // namespace
} // namespace lkd::platform::win32


lkd::platform::win32::service_runner::service_runner(
        std::string service_name,
        std::string listen_interface,
        const std::uint16_t port,
        const unsigned short threads
)
        : _serviceName(std::move(service_name))
        , _interface(std::move(listen_interface))
        , _port(port)
        , _threads(threads)
{
}


void lkd::platform::win32::service_runner::report_service_event(
        const char* const function, const DWORD error
) const
{
    const boost::system::error_code code(static_cast<int>(error), boost::system::system_category());
    console_print(
            cp_stderr,
            ll_fatal,
            str(boost::format("Service '%1%': %2% failed (%3%): %4%\n") % _serviceName % function %
                error % code.message())
    );
}


//
// Purpose:
//   Sets the current service status and reports it to the SCM.
//
// Parameters:
//   current_state - The current state (see SERVICE_STATUS)
//   exit_code - The system error code
//   wait_hint - Estimated time for pending operation, in milliseconds
//
// Return value:
//   None
//
void lkd::platform::win32::service_runner::report_service_status(
        const DWORD current_state, const DWORD exit_code, const DWORD wait_hint
)
{
    static DWORD dwCheckPoint = 1;

    // Fill in the SERVICE_STATUS structure.
    _serviceStatus.dwCurrentState = current_state;
    _serviceStatus.dwWin32ExitCode = exit_code;
    _serviceStatus.dwWaitHint = wait_hint;
    _serviceStatus.dwControlsAccepted = current_state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP : 0;

    if (current_state == SERVICE_RUNNING || current_state == SERVICE_STOPPED)
        _serviceStatus.dwCheckPoint = 0;

    else
        _serviceStatus.dwCheckPoint = dwCheckPoint++;

    // Report the status of the service to the SCM.
    ::SetServiceStatus(_serviceStatusHandle, &_serviceStatus);
}

//
// Purpose:
//   The service code
//
// Return value:
//   None
//
void lkd::platform::win32::service_runner::service_init() const
{
    // Be sure to periodically call report_service_status() with SERVICE_START_PENDING. If
    // initialization fails, call report_service_status with SERVICE_STOPPED.
    service_event(str(boost::format("Service '%1%' starting.") % _serviceName));

    // Create an event. The control handler function, service_control_handler, signals this event
    // when it receives the stop control code.
    _serviceStopEvent = ::CreateEvent(
            nullptr, // default security attributes
            TRUE,    // manual reset event
            FALSE,   // not signaled
            nullptr
    ); // no name

    if (_serviceStopEvent == nullptr) {
        const DWORD error = ::GetLastError();
        report_service_event("CreateEvent", error);
        service_event(
                str(boost::format("Service '%1%' stopped after startup failure.") % _serviceName)
        );
        report_service_status(SERVICE_STOPPED, error, 0);
        return;
    }

    try {
        lkd::server server(_interface, _port, _threads, lkd::rr_no_report_running);
        std::exception_ptr failure;
        std::thread server_thread([&server, &failure] {
            try {
                server.run();
            }
            catch (...) {
                failure = std::current_exception();
            }
            ::SetEvent(_serviceStopEvent);
        });

        // No logging or wait failure may unwind past a joinable worker thread.
        try {
            report_service_status(SERVICE_RUNNING, NO_ERROR, 0);
            service_event(str(boost::format("Service '%1%' started.") % _serviceName));
            if (::WaitForSingleObject(_serviceStopEvent, INFINITE) != WAIT_OBJECT_0)
                throw boost::system::system_error(
                        static_cast<int>(::GetLastError()),
                        boost::system::system_category(),
                        "Waiting for service stop failed"
                );
        }
        catch (...) {
            server.stop();
            server_thread.join();
            throw;
        }

        // The control callback only wakes us. Keep the server alive and join its workers before
        // reporting STOPPED, so the service uses the same bounded drain as signal shutdown.
        constexpr auto wait_hint =
                static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                           SHUTDOWN_GRACE_PERIOD + std::chrono::seconds(1)
                )
                                           .count());
        report_service_status(SERVICE_STOP_PENDING, NO_ERROR, wait_hint);
        server.stop();
        server_thread.join();
        if (failure)
            std::rethrow_exception(failure);
        service_event(str(boost::format("Service '%1%' stopped.") % _serviceName));
        report_service_status(SERVICE_STOPPED, NO_ERROR, 0);
    }
    catch (const std::exception& error) {
        console_print(
                cp_stderr,
                ll_fatal,
                str(boost::format("Service '%1%' failed: %2%\n") % _serviceName % error.what())
        );
        service_event(str(boost::format("Service '%1%' stopped after failure.") % _serviceName));
        _serviceStatus.dwServiceSpecificExitCode = EXIT_FAILURE;
        report_service_status(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, 0);
    }
    ::CloseHandle(_serviceStopEvent);
    _serviceStopEvent = nullptr;
}


//
// Purpose:
//   Called by SCM whenever a control code is sent to the service using the ControlService function.
//
// Parameters:
//   dwCtrl - control code
//
// Return value:
//   None
//
void WINAPI lkd::platform::win32::service_runner::service_control_handler(const DWORD dwCtrl)
{
    // Handle the requested control code.
    if (SERVICE_CONTROL_STOP == dwCtrl) {
        // Signal the service to stop.
        SetEvent(_serviceStopEvent);
    }
}


void WINAPI lkd::platform::win32::service_runner::
        service_main(const DWORD /*argument_count*/, LPWSTR* const /*arguments*/)
{
    // Register the handler function for the service
    const auto native_name = detail::from_utf8(SERVICE_RUNNER->service_name());
    _serviceStatusHandle =
            ::RegisterServiceCtrlHandlerW(native_name.c_str(), service_control_handler);

    if (!_serviceStatusHandle) {
        SERVICE_RUNNER->report_service_event("RegisterServiceCtrlHandler", ::GetLastError());
        return;
    }

    // These SERVICE_STATUS members remain as set here
    _serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    _serviceStatus.dwServiceSpecificExitCode = 0;

    // Report initial status to the SCM
    report_service_status(SERVICE_START_PENDING, NO_ERROR, 3000);

    // Perform service-specific initialization and work.
    SERVICE_RUNNER->service_init();
}


int lkd::platform::win32::run_service(
        const std::string& service_name,
        const std::string& listen_interface,
        const std::uint16_t port,
        const unsigned short threads,
        const bool event_log
)
{
    if (event_log)
        setup_event_log(service_name, LOG_LEVEL);
    SERVICE_RUNNER =
            new lkd::platform::win32::service_runner(service_name, listen_interface, port, threads);

    auto native_name = detail::from_utf8(service_name);
    const SERVICE_TABLE_ENTRYW dispatch_table[] = {
            {native_name.data(), lkd::platform::win32::service_runner::service_main},
            {nullptr, nullptr}
    };

    // This call returns when the service has stopped. The process should simply terminate when
    // the call returns.
    const bool dispatched = ::StartServiceCtrlDispatcherW(dispatch_table) != FALSE;
    if (!dispatched)
        SERVICE_RUNNER->report_service_event("StartServiceCtrlDispatcher", ::GetLastError());

    delete SERVICE_RUNNER;

    SERVICE_RUNNER = nullptr;
    return dispatched ? EXIT_SUCCESS : EXIT_FAILURE;
}

#endif // WIN32

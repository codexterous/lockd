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
 * File       : win32.cpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Windows daemon startup and service options.
 * Start Date : 03/26/2012
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

// The public Format header supplies the template definitions used by free_funcs.hpp.
#include <boost/format.hpp> // IWYU pragma: keep
#include <boost/format/free_funcs.hpp>
#include <boost/program_options/errors.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/program_options/value_semantic.hpp>
#include <boost/program_options/variables_map.hpp>
#include <boost/system/system_error.hpp>

// Initialize Winsock before windows.h without the older Winsock declarations.
#if !defined(WIN32_LEAN_AND_MEAN)
# define WIN32_LEAN_AND_MEAN
#endif
// Winsock2 must be initialized before windows.h to avoid the older Winsock declarations.
#include <winsock2.h> // IWYU pragma: keep
// Windows SDK declarations require the public umbrella and its prerequisite definitions.
#include <windows.h> // IWYU pragma: keep
#include <knownfolders.h>
#include <shlobj_core.h>
// The shell headers bring in COM's interface macro, which conflicts with the daemon's accessor.
#undef interface

#include "../../../include/lockd.hpp"
#include "../../../include/logging.hpp"
#include "../../../include/platform/win32/service.hpp"
#include "../../../include/platform/win32/service_elevation.hpp"
#include "../../../include/platform/win32/win32.hpp"


bool lkd::IS_FORKED = false;


unsigned int lkd::platform::win32::terminal_width()
{
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info))
        return info.dwSize.X;
    return 80;
}


lkd::platform::win32::startup::startup(
        std::filesystem::path current_path,
        const unsigned int width,
        const unsigned int description_width
)
        : _currentPath(std::move(current_path))
        , _serviceName("lockd")
        , _options("", width, description_width)
{
    namespace po = boost::program_options;

    // Service related options
    _options.add_options()(
            "service-name,n",
            po::value<std::string>(&_serviceName)->default_value(_serviceName),
            "WIN32 service name to install")(
            "install-service,S",
            str(boost::format(
                        "install or replace %1% as a WIN32 service with the given service name "
                        "and exit. the service will run with the provided allowed options") %
                lkd::platform::win32::service_installer::executable_path())
                    .c_str())(
            "no-event-log,E",
            "disable Windows Event Log output for the installed service (requires -S)")(
            "uninstall-service,U",
            str(boost::format("uninstall %1% as a WIN32 service and exit") %
                lkd::platform::win32::service_installer::executable_path())
                    .c_str());
}


std::filesystem::path lkd::platform::win32::startup::default_log_file() const
{
    return _currentPath / "lockd.log";
}


std::filesystem::path lkd::platform::win32::startup::default_log_file(
        const boost::program_options::parsed_options& options
) const
{
    const bool service =
            std::any_of(options.options.begin(), options.options.end(), [](const auto& option) {
                return option.string_key == "install-service" || option.string_key == "as-service";
            });
    if (!service)
        return default_log_file();

    // Ask Windows for the configured common-data folder instead of assuming C:\ProgramData
    // or inheriting an environment override. Merely displaying help must not create anything.
    wchar_t* folder = nullptr;
    const auto result =
            ::SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DONT_VERIFY, nullptr, &folder);
    const std::unique_ptr<wchar_t, decltype(&::CoTaskMemFree)> guard(folder, ::CoTaskMemFree);
    if (FAILED(result))
        throw boost::system::system_error(
                static_cast<int>(result),
                boost::system::system_category(),
                "Cannot determine the Windows service log directory"
        );
    return std::filesystem::path(folder) / "lockd" / "lockd.log";
}


std::string
lkd::platform::win32::startup::options_help(const unsigned int option_column_width) const
{
    std::ostringstream help;
    help << "Service installation options:\n"
            "  Installation saves the supplied daemon options for subsequent service starts.\n\n";
    _options.print(help, option_column_width);
    return help.str();
}


void lkd::platform::win32::startup::add_options(
        boost::program_options::options_description& visible,
        boost::program_options::options_description& hidden
) const
{
    visible.add(_options);
    hidden.add_options()("as-service,A", "run as a service");
}


void lkd::platform::win32::startup::validate_options(
        const boost::program_options::variables_map& options
)
{
    // Installation, removal and the SCM entry point are separate operations. Reject mixed
    // requests before file validation or elevation can produce any side effects.
    const auto service_actions = options.count("install-service") +
                                 options.count("uninstall-service") + options.count("as-service");
    if (service_actions > 1)
        throw boost::program_options::error(
                "--install-service (-S), --uninstall-service (-U), and --as-service (-A) "
                "cannot be combined"
        );

    // --as-service is the internal SCM entry point for the persisted install option.
    if (options.count("no-event-log") &&
        (options.count("uninstall-service") ||
         (!options.count("install-service") && !options.count("as-service"))))
        throw boost::program_options::error(
                "--no-event-log requires --install-service (-S) or --as-service"
        );
}


std::optional<int> lkd::platform::win32::startup::prepare(
        const boost::program_options::variables_map& options,
        const std::string& /*listen_ip*/,
        const std::uint16_t port,
        const unsigned short threads,
        const std::filesystem::path& log_file,
        const lkd::log_level_t log_level
) const
{
    const bool install = options.count("install-service") != 0;
    const bool uninstall = options.count("uninstall-service") != 0;
    if (install || uninstall) {
        // The first installation may need to create a protected ProgramData directory.
        // Elevate before touching the log, and propagate the child's status to the parent.
        if (const auto result = service_elevation().run())
            return result;
    }
    if (!uninstall) {
        if ((install || options.count("as-service")) && options["log-file"].defaulted()) {
            // Only the service default gets an automatically created parent. An explicit
            // log path retains the existing contract that its directory must already exist.
            std::error_code error;
            std::filesystem::create_directories(log_file.parent_path(), error);
            if (error)
                throw boost::system::system_error(
                        error.value(),
                        boost::system::system_category(),
                        "Cannot create the Windows service log directory"
                );
        }
        lkd::logging::validate_log_file(log_file, true);
    }

    // Registration persists the user's configuration, not this process's DNS result.
    // Keep the requested hostname in the command and description; the service resolves
    // it when it starts, while the running server continues to use the resolved address.
    if (install || uninstall)
        return lkd::platform::win32::manage_service(
                install,
                _serviceName,
                options["interface"].as<std::string>(),
                port,
                threads,
                log_level,
                log_file,
                options.count("no-event-log") == 0,
                static_cast<unsigned int>(options["log-rotate-size"].as<int>()),
                static_cast<unsigned int>(options["log-rotate-keep"].as<int>())
        );
    return std::nullopt;
}


std::optional<int> lkd::platform::win32::startup::run_service(
        const boost::program_options::variables_map& options,
        const std::string& listen_ip,
        const std::uint16_t port,
        const unsigned short threads
) const
{
    if (options.count("as-service"))
        return lkd::platform::win32::run_service(
                _serviceName, listen_ip, port, threads, options.count("no-event-log") == 0
        );
    return std::nullopt;
}


void lkd::platform::win32::startup::ready()
{
}

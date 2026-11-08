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
 * File       : service_test.cpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Test Windows service elevation, configuration, and logging.
 * Start Date : 10/23/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#ifdef WIN32

# if !defined(WIN32_LEAN_AND_MEAN)
#  define WIN32_LEAN_AND_MEAN
# endif

# include <algorithm>
# include <array>
# include <chrono>
# include <cstdarg>
# include <cstddef>
# include <filesystem>
# include <fstream>
# include <future>
# include <initializer_list>
# include <ios>
# include <iterator>
# include <memory>
# include <sstream>
# include <string>
# include <system_error>
# include <thread>
# include <vector>

// Supplies the expression operators used by Boost.Log formatters and filters.
# include <boost/log/expressions.hpp> // IWYU pragma: keep
# include <boost/log/expressions/formatters/stream.hpp>
# include <boost/log/expressions/message.hpp>
# include <boost/log/sinks/event_log_backend.hpp>
# include <boost/log/sinks/event_log_constants.hpp>
# include <boost/log/sources/severity_feature.hpp>

# include <boost/system/error_code.hpp> // IWYU pragma: keep

# include <boost/log/sinks/sync_frontend.hpp>
# include <boost/log/sinks/text_ostream_backend.hpp>
# include <boost/log/sources/record_ostream.hpp>
# include <boost/log/sources/severity_logger.hpp>
# include <boost/test/tools/interface.hpp> // IWYU pragma: keep
# include <boost/test/unit_test_suite.hpp>

# include <boost/make_shared.hpp> // IWYU pragma: keep
# include <boost/program_options/options_description.hpp>
# include <boost/program_options/parsers.hpp>
# include <boost/program_options/value_semantic.hpp>
# include <boost/program_options/variables_map.hpp>
# include <boost/system/system_error.hpp>

# include <boost/test/unit_test.hpp> // IWYU pragma: keep

// Winsock2 precedes windows.h; shellapi.h needs the Windows declarations.
// Winsock2 must be initialized before windows.h to avoid the older Winsock declarations.
# include <winsock2.h> // IWYU pragma: keep
// Windows SDK declarations require the public umbrella and its prerequisite definitions.
# include <windows.h> // IWYU pragma: keep
# include <shellapi.h>

# include "../../../include/logging.hpp"
# include "../../../include/platform/win32/detail/unicode.hpp"
# include "../../../include/platform/win32/service_elevation.hpp"
# include "../../../include/platform/win32/event_log.hpp"
# include "../../../include/platform/win32/service.hpp"
# include "../../../include/platform/win32/win32.hpp"
# include "../../../include/server.hpp"
# include "../include/launcher.hpp"
// Selects the supported Process v1 API across Boost versions.
# include "../include/process.hpp" // IWYU pragma: keep
# include "../include/service_fixture.hpp"

bool ldt::service_elevation_fixture::elevated() const
{
    if (token_error != 0)
        throw boost::system::system_error(token_error, boost::system::system_category());
    return token_elevated;
}


int ldt::service_elevation_fixture::relaunch() const
{
    ++launches;
    if (launch_error != 0)
        throw boost::system::system_error(launch_error, boost::system::system_category());
    return child_result;
}


namespace {

BOOST_AUTO_TEST_SUITE(lifetimes)

BOOST_AUTO_TEST_CASE(windows_unicode_conversions_preserve_arguments)
{
    // Include a supplementary character so UTF-16 surrogate pairs are exercised, too.
    namespace unicode = lkd::platform::win32::detail;
    const std::wstring wide = L"service_\u798f_\u00e9_\U0001F512";
    const std::string utf8 = u8"service_\u798f_\u00e9_\U0001F512";
    BOOST_CHECK(unicode::to_utf8(wide) == utf8);
    BOOST_CHECK(unicode::from_utf8(utf8) == wide);
    BOOST_CHECK(unicode::to_utf8(L"").empty());
    BOOST_CHECK(unicode::from_utf8("").empty());

    // Reject invalid Unicode instead of silently saving a different service name or path.
    BOOST_CHECK_THROW(
            static_cast<void>(unicode::from_utf8(std::string("\xc0\xaf", 2))),
            boost::system::system_error
    );
    BOOST_CHECK_THROW(
            static_cast<void>(unicode::to_utf8(std::wstring(1, wchar_t(0xd800)))),
            boost::system::system_error
    );
    std::ostringstream output;
    unicode::write_console(output, utf8);
    BOOST_CHECK(output.str() == utf8);
}


BOOST_AUTO_TEST_CASE(daemon_help_and_validation_preserve_unicode_paths)
{
    // Copy only the daemon and its runtime DLLs. Help must work from a Unicode working
    // directory, retain the service command preview, and report an invalid log path without
    // losing its Unicode argument. A missing log directory prevents the daemon from starting.
    const auto directory =
            std::filesystem::current_path() /
            (L"unicode_\u798f_\U0001F512_" +
             std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()));
    BOOST_REQUIRE(std::filesystem::create_directory(directory));
    const auto source = ldt::daemon_path();
    const auto executable = directory / L"lockd_\u00e9.exe";
    std::filesystem::copy_file(source, executable);
    for (const auto& entry : std::filesystem::directory_iterator(source.parent_path())) {
        if (entry.path().extension() == L".dll")
            std::filesystem::copy_file(entry.path(), directory / entry.path().filename());
    }
    for (const auto& arguments :
         {std::vector<std::wstring>{L"--help"},
          std::vector<std::wstring>{
                  L"--interface",
                  L"127.0.0.1",
                  L"--log-file",
                  L"missing_\u798f\\log_\u00e9_\U0001F512.log"
          }}) {
        ldt::process::ipstream output;
        ldt::process::child child(
                executable.wstring(),
                ldt::process::args(arguments),
                ldt::process::start_dir = directory.wstring(),
                (ldt::process::std_out & ldt::process::std_err) > output
        );
        auto reader = std::async(std::launch::async, [&output] {
            return std::string(
                    std::istreambuf_iterator<char>(output), std::istreambuf_iterator<char>()
            );
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (child.running() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const bool exited = !child.running();
        if (!exited)
            child.terminate();
        child.wait();
        const auto text = reader.get();
        BOOST_REQUIRE_MESSAGE(exited, text);
        BOOST_CHECK_MESSAGE(child.exit_code() == (arguments.size() == 1 ? 0 : 1), text);
        if (arguments.size() == 1) {
            // Program_options wraps long defaults; compare without its layout whitespace.
            const auto compact = [](std::string value) {
                value.erase(
                        std::remove_if(
                                value.begin(),
                                value.end(),
                                [](const char character) {
                                    return character == ' ' || character == '\r' ||
                                           character == '\n';
                                }
                        ),
                        value.end()
                );
                return value;
            };
            BOOST_CHECK_MESSAGE(
                    compact(text).find(compact((directory / L"lockd.log").u8string())) !=
                            std::string::npos,
                    text
            );
            BOOST_CHECK_MESSAGE(text.find(executable.u8string()) != std::string::npos, text);
            BOOST_CHECK_MESSAGE(
                    compact(text).find(compact("\"" + executable.u8string() + "\"")) !=
                            std::string::npos,
                    text
            );
        }
        else {
            BOOST_CHECK_MESSAGE(
                    text.find(lkd::platform::win32::detail::to_utf8(arguments.back())) !=
                            std::string::npos,
                    text
            );
        }
    }

    // Deleting a just-executed image can fail transiently on Windows. Retry access/sharing
    // failures for a bounded interval, while still reporting other cleanup failures directly.
    const auto cleanup_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    const std::error_code sharing_violation(ERROR_SHARING_VIOLATION, std::system_category());
    std::error_code error;
    do {
        std::filesystem::remove_all(directory, error);
        if (!error || (error != std::errc::permission_denied && error != sharing_violation))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < cleanup_deadline);
    BOOST_REQUIRE_MESSAGE(
            !error, "Remove the copied daemon and its directory: " << error.message()
    );
}


BOOST_AUTO_TEST_CASE(service_elevation_skips_relaunch_when_already_elevated)
{
    // Simulate an already elevated process. Verify the operation proceeds locally without
    // launching another child or prompting again.
    ldt::service_elevation_fixture elevation;
    elevation.token_elevated = true;
    BOOST_CHECK_MESSAGE(!elevation.run().has_value(), "Elevated service operations run locally");
    BOOST_CHECK_MESSAGE(elevation.launches == 0, "An elevated child never prompts again");
}


BOOST_AUTO_TEST_CASE(service_elevation_preserves_child_exit_status)
{
    // Simulate elevated children returning different exit codes. Verify exactly one relaunch and
    // propagation of the child status to the parent.
    for (const int result : {0, 1, 7, 8}) {
        ldt::service_elevation_fixture elevation;
        elevation.child_result = result;
        const auto returned = elevation.run();
        BOOST_REQUIRE_MESSAGE(returned.has_value(), "The parent must exit after relaunching");
        BOOST_CHECK_MESSAGE(*returned == result, "Installation and removal retain exit codes");
        BOOST_CHECK_MESSAGE(elevation.launches == 1, "Only one elevated child is launched");
    }
}


BOOST_AUTO_TEST_CASE(service_elevation_does_not_retry_cancellation_or_launch_failure)
{
    // Simulate canceled elevation and launch failure. Verify the error propagates and neither
    // condition triggers another elevation attempt.
    for (const int error : {ERROR_CANCELLED, ERROR_ACCESS_DENIED, ERROR_FILE_NOT_FOUND}) {
        ldt::service_elevation_fixture elevation;
        elevation.launch_error = error;
        BOOST_CHECK_EXCEPTION(
                static_cast<void>(elevation.run()),
                boost::system::system_error,
                [error](const boost::system::system_error& exception) {
                    return exception.code().value() == error;
                }
        );
        BOOST_CHECK_MESSAGE(elevation.launches == 1, "Failure does not cause another UAC prompt");
    }
}


BOOST_AUTO_TEST_CASE(service_elevation_token_failure_does_not_launch)
{
    // Fail the privilege-token query. Verify the unknown privilege state raises an error without
    // attempting to launch an elevated child.
    ldt::service_elevation_fixture elevation;
    elevation.token_error = ERROR_ACCESS_DENIED;
    BOOST_CHECK_EXCEPTION(
            static_cast<void>(elevation.run()),
            boost::system::system_error,
            [](const boost::system::system_error& exception) {
                return exception.code().value() == ERROR_ACCESS_DENIED;
            }
    );
    BOOST_CHECK_MESSAGE(elevation.launches == 0, "An unknown privilege state fails explicitly");
}


BOOST_AUTO_TEST_CASE(service_elevation_arguments_round_trip_through_windows_parser)
{
    // Quote empty, Unicode, whitespace, quote, and backslash-containing arguments. Parse them
    // with Windows and verify every argument and boundary survives unchanged.
    const std::vector<std::wstring> arguments = {
            L"--install-service",
            L"--uninstall-service",
            L"--service-name",
            L"",
            L"name with spaces",
            L"name\twith\ttabs",
            L"embedded\"quote",
            L"trailing\\",
            L"backslashes\\\\\"before quote",
            L"&|<>^%",
            L"service-\u03bb-\u4e2d",
            L"--interface",
            L"127.0.0.1",
            L"--port",
            L"7198"
    };
    std::wstring command_line = L"lockd.exe";
    for (const auto& argument : arguments)
        command_line += L" " + lkd::platform::win32::service_elevation::quote_argument(argument);

    int count = 0;
    auto* const parsed = ::CommandLineToArgvW(command_line.c_str(), &count);
    BOOST_REQUIRE_MESSAGE(parsed != nullptr, "Windows accepts the elevated command line");
    const std::unique_ptr<void, decltype(&::LocalFree)> parsed_guard(parsed, ::LocalFree);
    BOOST_REQUIRE_MESSAGE(
            static_cast<std::size_t>(count) == arguments.size() + 1,
            "Quoting preserves empty arguments and argument boundaries"
    );
    for (std::size_t index = 0; index < arguments.size(); ++index)
        BOOST_CHECK_MESSAGE(
                arguments[index] == parsed[index + 1], "Elevation preserves argument " << index
        );
}


BOOST_AUTO_TEST_CASE(service_ownership_checks_the_parsed_executable)
{
    // Exercise the actual Windows parser and filesystem identity check without opening
    // the SCM. An executable mentioned only in another service's arguments is not its owner.
    const std::filesystem::path executable =
            lkd::platform::win32::service_elevation::executable_path();
    const auto quoted =
            lkd::platform::win32::service_elevation::quote_argument(executable.wstring());
    BOOST_CHECK(lkd::platform::win32::service_installer::owns_command_line(quoted, executable));
    BOOST_CHECK(
            lkd::platform::win32::service_installer::owns_command_line(
                    quoted + L" --as-service --port 7198", executable
            )
    );
    const auto alias = executable.parent_path() / L"." / executable.filename();
    BOOST_CHECK(
            lkd::platform::win32::service_installer::owns_command_line(
                    lkd::platform::win32::service_elevation::quote_argument(alias.wstring()),
                    executable
            )
    );
    const auto other =
            lkd::platform::win32::service_elevation::quote_argument(ldt::daemon_path().wstring());
    std::wstring foreign_command = other;
    foreign_command += L" --argument ";
    foreign_command += quoted;
    BOOST_CHECK(
            !lkd::platform::win32::service_installer::owns_command_line(foreign_command, executable)
    );
    BOOST_CHECK(!lkd::platform::win32::service_installer::owns_command_line(L"", executable));
    BOOST_CHECK(!lkd::platform::win32::service_installer::owns_command_line(L"\"\"", executable));
}


BOOST_AUTO_TEST_CASE(service_log_defaults_follow_the_parsed_startup_mode)
{
    // Resolve aliases through Program_options before choosing defaults. A service flag
    // selects common application data, while option values containing that text do not.
    // These checks only parse options; they never create a directory or open the SCM.
    namespace po = boost::program_options;
    const auto current_path = std::filesystem::current_path();
    const lkd::platform::win32::startup startup(current_path, 100, 50);
    for (const std::vector<std::string>& arguments :
         {std::vector<std::string>{"--help"},
          {"--as-service", "--help"},
          {"-A", "--help"},
          {"--install-service", "--help"},
          {"-S", "--help"},
          {"--help", "--log-file=--as-service"},
          {"--as-service", "--help", "--log-file=custom.log"},
          {"--install-service", "--help", "--log-file=custom.log"}}) {
        po::options_description visible;
        po::options_description hidden;
        auto* const log_option = po::value<std::string>();
        visible.add_options()("help,h", "help")("log-file,f", log_option, "log file");
        startup.add_options(visible, hidden);
        po::options_description all;
        all.add(visible).add(hidden);
        const auto parsed = po::command_line_parser(arguments).options(all).run();
        const auto default_file = startup.default_log_file(parsed);
        log_option->default_value(default_file.u8string());
        po::variables_map options;
        po::store(parsed, options);
        po::notify(options);
        const bool service = options.count("as-service") || options.count("install-service");
        if (service) {
            BOOST_CHECK(default_file.is_absolute());
            BOOST_CHECK(default_file.filename() == "lockd.log");
            BOOST_CHECK(default_file.parent_path().filename() == "lockd");
            BOOST_CHECK(default_file != current_path / "lockd.log");
        }
        else
            BOOST_CHECK(default_file == current_path / "lockd.log");

        const auto& value = options["log-file"];
        const std::string expected = value.defaulted() ? default_file.u8string()
                                     : service         ? "custom.log"
                                                       : "--as-service";
        BOOST_CHECK(value.as<std::string>() == expected);
        std::ostringstream help;
        help << visible;
        BOOST_CHECK(help.str().find(default_file.u8string()) != std::string::npos);
    }
}


BOOST_AUTO_TEST_CASE(service_installation_preserves_log_rotation)
{
    // Build a service command with custom rotation settings. Verify installation carries
    // both settings to the SCM entry point without installing or changing a service.
    const auto command = lkd::platform::win32::service_installer::command_line(
            "lockd.exe", "lockd", "127.0.0.1", 7198, 2, lkd::ll_info, "lockd.log", true, 25, 7
    );
    BOOST_CHECK_MESSAGE(
            command.find(" --log-rotate-size 25 --log-rotate-keep 7") != std::string::npos,
            "The installed service keeps the selected size in MiB and archive count"
    );
    const auto external = lkd::platform::win32::service_installer::command_line(
            "lockd.exe", "lockd", "127.0.0.1", 7198, 2, lkd::ll_info, "lockd.log", true, 0, 5
    );
    BOOST_CHECK_MESSAGE(
            external.find(" --log-rotate-size 0") != std::string::npos,
            "Windows service installation preserves explicitly disabled file rotation"
    );
}


BOOST_AUTO_TEST_CASE(service_installation_preserves_logging_level)
{
    // Build service commands and descriptions for every severity. Verify network settings,
    // selected verbosity, and Event Log configuration are represented correctly.
    for (const auto level :
         {lkd::ll_trace,
          lkd::ll_debug,
          lkd::ll_info,
          lkd::ll_warning,
          lkd::ll_error,
          lkd::ll_fatal}) {
        std::ostringstream expected;
        expected << "\"C:\\Program Files\\lockd\\lockd.exe\" --as-service -i \"127.0.0.1\""
                    " -p 7198 -t 2 -n \"test service\" -l "
                 << lkd::log_level_input(level) << " --log-file \""
                 << std::filesystem::absolute("lockd.log").string() << '"';
        BOOST_CHECK_MESSAGE(
                lkd::platform::win32::service_installer::command_line(
                        "\"C:\\Program Files\\lockd\\lockd.exe\"",
                        "test service",
                        "127.0.0.1",
                        7198,
                        2,
                        level,
                        "lockd.log",
                        true
                ) == expected.str(),
                "The installed service retains its network settings and selected verbosity"
        );

        std::ostringstream expected_description;
        expected_description << "lockd, an interprocess locking daemon, running on '127.0.0.1' "
                                "port 7198 with 16 acquisition threads and log level '"
                             << lkd::log_level_input(level) << "'.";
        BOOST_CHECK_MESSAGE(
                lkd::platform::win32::service_installer::description(
                        "127.0.0.1", 7198, 16, level, true
                ) == expected_description.str(),
                "The service description includes the configured logging level"
        );
        BOOST_CHECK_MESSAGE(
                lkd::platform::win32::service_installer::description(
                        "127.0.0.1", 7198, 16, level, false
                ) == expected_description.str() + " Windows Event Log output is disabled.",
                "The description identifies services configured for file logging only"
        );
    }
}


BOOST_AUTO_TEST_CASE(service_installation_preserves_requested_interface_text)
{
    // Main resolves the bound option variable before Windows startup. The variables_map must
    // still retain the user's spelling for registration, including hostnames and expanded IPv6.
    // Exercise parsing, resolution, and the real service formatters without touching the SCM.
    namespace po = boost::program_options;
    for (const std::string requested : {"localhost", "LoCaLhOsT", "0:0:0:0:0:0:0:1"}) {
        std::string listen_ip;
        po::options_description options("Interface options");
        options.add_options()("interface,i", po::value<std::string>(&listen_ip));
        po::variables_map values;
        po::store(
                po::command_line_parser({"--interface", requested}).options(options).run(), values
        );
        po::notify(values);
        listen_ip = lkd::server::resolve_interface(listen_ip).to_string();
        BOOST_REQUIRE_MESSAGE(
                listen_ip != requested, "The binding address differs from the requested spelling"
        );
        const auto& configured = values["interface"].as<std::string>();
        BOOST_CHECK_MESSAGE(configured == requested, "Resolution preserves the parsed option");
        const auto description = lkd::platform::win32::service_installer::description(
                configured, 7198, 2, lkd::ll_info, false
        );
        BOOST_CHECK_MESSAGE(
                description ==
                        "lockd, an interprocess locking daemon, running on '" + requested +
                                "' port 7198 with 2 acquisition threads and log level 'info'. "
                                "Windows Event Log output is disabled.",
                "The service description preserves the user's interface text: " << description
        );
        const auto command = lkd::platform::win32::service_installer::command_line(
                "lockd.exe", "lockd", configured, 7198, 2, lkd::ll_info, "lockd.log", false
        );
        BOOST_CHECK_MESSAGE(
                command.find(" -i \"" + requested + "\" -p 7198") != std::string::npos,
                "The service resolves the saved interface again when it starts: " << command
        );
    }
}


BOOST_AUTO_TEST_CASE(service_installation_preserves_file_logging_without_event_logging)
{
    // Build service commands with default and spaced log paths and Event Log disabled. Parse
    // them with Windows to verify absolute paths and the opt-out remain separate intact
    // arguments.
    for (const auto& log_file :
         {std::filesystem::path("lockd.log"),
          std::filesystem::path("logs with spaces") / "service.log",
          std::filesystem::path(L"logs_\u798f_\U0001F512") / L"service_\u00e9.log"}) {
        const auto expected_path = std::filesystem::absolute(log_file);
        const auto command = lkd::platform::win32::service_installer::command_line(
                "lockd.exe",
                u8"lockd_\u798f_\U0001F512",
                "127.0.0.1",
                7198,
                16,
                lkd::ll_warning,
                log_file,
                false
        );
        const auto wide_command = lkd::platform::win32::detail::from_utf8(command);
        int count = 0;
        auto* const parsed = ::CommandLineToArgvW(wide_command.c_str(), &count);
        BOOST_REQUIRE_MESSAGE(parsed != nullptr, "Windows parses the installed service command");
        const std::unique_ptr<void, decltype(&::LocalFree)> guard(parsed, ::LocalFree);
        BOOST_REQUIRE_MESSAGE(count == 15, "The file path stays a single argument");
        BOOST_CHECK(std::wstring(parsed[9]) == L"lockd_\u798f_\U0001F512");
        BOOST_CHECK_MESSAGE(
                std::wstring(parsed[12]) == L"--log-file" &&
                        std::filesystem::path(parsed[13]) == expected_path,
                "Default and explicit files retain their absolute installation path"
        );
        BOOST_CHECK_MESSAGE(
                std::wstring(parsed[14]) == L"--no-event-log",
                "The service runtime receives the installer's Event Log opt-out"
        );
    }
}


BOOST_AUTO_TEST_CASE(service_event_types_match_severity)
{
    // Map each logging severity to a Windows event type. Verify informational, warning, and
    // error categories retain their intended grouping.
    namespace events = boost::log::sinks::event_log;
    for (const auto level : {lkd::ll_trace, lkd::ll_debug, lkd::ll_info})
        BOOST_CHECK_MESSAGE(
                lkd::platform::win32::event_log_type(level) == events::info,
                "Trace, debug and info use Information events"
        );
    BOOST_CHECK_MESSAGE(
            lkd::platform::win32::event_log_type(lkd::ll_warning) == events::warning,
            "Warnings use Warning events"
    );
    for (const auto level : {lkd::ll_error, lkd::ll_fatal})
        BOOST_CHECK_MESSAGE(
                lkd::platform::win32::event_log_type(level) == events::error,
                "Errors and fatal failures use Error events"
        );
}


BOOST_AUTO_TEST_CASE(service_lifecycle_bypasses_event_threshold)
{
    // Emit lifecycle records and ordinary messages at every threshold into a test sink. Verify
    // only lifecycle records bypass severity filtering.
    namespace logging = boost::log;
    typedef logging::sinks::synchronous_sink<logging::sinks::text_ostream_backend> test_sink;

    // Substitute only the destination; use the production filter and lifecycle emitter.
    // No Event Log source is registered and nothing is written to the Windows Event Log.
    const auto output = boost::make_shared<std::ostringstream>();
    const auto sink = boost::make_shared<test_sink>();
    sink->locked_backend()->add_stream(output);
    sink->set_formatter(logging::expressions::stream << logging::expressions::smessage);
    logging::core::get()->add_sink(sink);
    const auto remove_sink = [sink](const void*) { logging::core::get()->remove_sink(sink); };
    const std::unique_ptr<const void, decltype(remove_sink)> guard(sink.get(), remove_sink);
    logging::sources::severity_logger<lkd::log_level_t> logger;

    constexpr std::array<lkd::log_level_t, 6> thresholds = {
            lkd::ll_trace,
            lkd::ll_debug,
            lkd::ll_info,
            lkd::ll_warning,
            lkd::ll_error,
            lkd::ll_fatal
    };
    constexpr std::array<const char*, 6> expected_records = {
            "starting\ntrace\ndebug\ninfo\nwarning\nerror\nfatal\nstopped\n",
            "starting\ndebug\ninfo\nwarning\nerror\nfatal\nstopped\n",
            "starting\ninfo\nwarning\nerror\nfatal\nstopped\n",
            "starting\nwarning\nerror\nfatal\nstopped\n",
            "starting\nerror\nfatal\nstopped\n",
            "starting\nfatal\nstopped\n"
    };
    for (std::size_t index = 0; index < thresholds.size(); ++index) {
        output->str("");
        sink->set_filter(lkd::platform::win32::event_log_filter(thresholds[index]));
        lkd::platform::win32::service_event("starting");
        BOOST_LOG_SEV(logger, lkd::ll_trace) << "trace";
        BOOST_LOG_SEV(logger, lkd::ll_debug) << "debug";
        BOOST_LOG_SEV(logger, lkd::ll_info) << "info";
        BOOST_LOG_SEV(logger, lkd::ll_warning) << "warning";
        BOOST_LOG_SEV(logger, lkd::ll_error) << "error";
        BOOST_LOG_SEV(logger, lkd::ll_fatal) << "fatal";
        lkd::platform::win32::service_event("stopped");
        sink->flush();
        BOOST_CHECK_MESSAGE(
                output->str() == expected_records[index],
                "Only lifecycle records bypass the configured severity threshold; got: "
                        << output->str()
        );
    }
}


BOOST_AUTO_TEST_CASE(service_event_message_resource_formats_insertions)
{
    // Compose an event message and load the daemon message resource. Verify the insertion text
    // and message ID render the complete message expected by Event Viewer.
    boost::log::sources::severity_logger<lkd::log_level_t> logger;
    auto record = logger.open_record();
    BOOST_REQUIRE_MESSAGE(record, "A log record is available for event composition");
    {
        boost::log::record_ostream stream(record);
        stream << u8"Service \u798f \U0001F512 started.";
        stream.flush();
    }
    const auto composer = lkd::platform::win32::event_log_composer();
    boost::log::sinks::event_log::wevent_composer::insertion_list insertions;
    const auto event_id = composer(record.lock(), insertions);
    BOOST_REQUIRE_MESSAGE(
            insertions.size() == 1,
            "The event composer must supply the message for the %1 placeholder"
    );
    BOOST_CHECK_MESSAGE(
            insertions.front() == L"Service \u798f \U0001F512 started.",
            "The insertion contains the actual log message"
    );
    const auto executable = ldt::daemon_path();
    const auto module = ::LoadLibraryExW(executable.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE);
    BOOST_REQUIRE_MESSAGE(module != nullptr, "The daemon's message resources can be loaded");
    const auto free_module = [](HMODULE const value) { ::FreeLibrary(value); };
    const std::unique_ptr<HINSTANCE__, decltype(free_module)> guard(module, free_module);
    wchar_t output[256] = {};
    const DWORD_PTR arguments[] = {reinterpret_cast<DWORD_PTR>(insertions.front().c_str())};
    const DWORD length = ::FormatMessageW(
            FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_ARGUMENT_ARRAY,
            module,
            event_id,
            0,
            output,
            256,
            reinterpret_cast<va_list*>(const_cast<DWORD_PTR*>(arguments))
    );
    BOOST_REQUIRE_MESSAGE(length != 0, "Event Viewer can resolve the daemon's message ID");
    BOOST_CHECK_MESSAGE(
            std::wstring(output, length) == L"Service \u798f \U0001F512 started.",
            "The embedded message table renders the complete log message"
    );
}


BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(service_logging)


BOOST_AUTO_TEST_CASE(file_and_event_destinations_filter_independently)
{
    // Run file and event logging in an isolated child with different filtering behavior. Verify
    // lifecycle bypass applies only to events and buffered errors retain their severity.
    // File logging is initialized once per process. Isolate it here so this test can run
    // with every other test, whether launched directly, through CTest, or from an IDE.
    ldt::process::wenvironment environment = boost::this_process::wenvironment();
    if (environment.count(L"LOCKD_SERVICE_LOGGING_TEST_CHILD") == 0) {
        environment[L"LOCKD_SERVICE_LOGGING_TEST_CHILD"] = L"1";
        ldt::process::ipstream output;
        ldt::process::child child(
                lkd::platform::win32::service_elevation::executable_path(),
                ldt::process::args(
                        std::vector<std::wstring>{
                                L"--run_test=service_logging/"
                                L"file_and_event_destinations_filter_independently",
                                L"--report_level=short"
                        }
                ),
                environment,
                (ldt::process::std_out & ldt::process::std_err) > output
        );
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (child.running() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const bool exited = !child.running();
        if (!exited)
            child.terminate();
        child.wait();
        const std::string diagnostics(
                (std::istreambuf_iterator<char>(output)), std::istreambuf_iterator<char>()
        );
        BOOST_REQUIRE_MESSAGE(exited, "The isolated logging test finishes within ten seconds");
        BOOST_CHECK_MESSAGE(child.exit_code() == 0, "Isolated logging assertions: " << diagnostics);
        return;
    }

    namespace logging = boost::log;
    typedef logging::sinks::synchronous_sink<logging::sinks::text_ostream_backend> test_sink;
    const auto log_file = std::filesystem::current_path() / "service_logging_test.log";
    std::ofstream(log_file, std::ios::trunc).close();
    lkd::logging::setup_logging(log_file, lkd::ll_warning);

    const auto output = boost::make_shared<std::ostringstream>();
    const auto sink = boost::make_shared<test_sink>();
    sink->locked_backend()->add_stream(output);
    sink->set_formatter(logging::expressions::stream << logging::expressions::smessage);
    sink->set_filter(lkd::platform::win32::event_log_filter(lkd::ll_warning));
    logging::core::get()->add_sink(sink);
    const auto remove_sink = [sink](const void*) { logging::core::get()->remove_sink(sink); };
    const std::unique_ptr<const void, decltype(remove_sink)> guard(sink.get(), remove_sink);

    lkd::platform::win32::service_event("starting");
    lkd::console_print(lkd::cp_stdlog, lkd::ll_info, "ordinary info\n");
    lkd::console_print(lkd::cp_stdlog, lkd::ll_error, "buffered error");
    lkd::console_print(lkd::cp_stdlog, lkd::ll_info, "\n");
    BOOST_CHECK_MESSAGE(
            output->str() == "starting\n", "A suppressed newline does not flush a buffered error"
    );
    lkd::console_print(lkd::cp_stdlog, lkd::ll_error, "\n");
    lkd::console_print(lkd::cp_stdlog, lkd::ll_warning, "ordinary warning\n");
    lkd::platform::win32::service_event("stopped");
    logging::core::get()->flush();

    BOOST_CHECK_MESSAGE(
            output->str() == "starting\nbuffered error\nordinary warning\nstopped\n",
            "The event destination sees lifecycle events and the buffered error"
    );
    std::ifstream input(log_file);
    const std::string file_output(
            (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>()
    );
    BOOST_CHECK_MESSAGE(
            file_output.find("<error> buffered error") != std::string::npos,
            "An accepted newline completes the buffered error without downgrading it"
    );
    BOOST_CHECK_MESSAGE(
            file_output.find("<warning> ordinary warning") != std::string::npos,
            "The file includes messages at its threshold"
    );
    BOOST_CHECK_MESSAGE(
            file_output.find("starting") == std::string::npos &&
                    file_output.find("stopped") == std::string::npos &&
                    file_output.find("ordinary info") == std::string::npos,
            "The file threshold still excludes all Information records"
    );
}


BOOST_AUTO_TEST_SUITE_END()

} // namespace

#endif // WIN32

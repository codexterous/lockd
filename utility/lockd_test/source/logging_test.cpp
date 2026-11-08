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
 * File       : logging_test.cpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Test log output, rotation, severity filtering, and command-line validation.
 * Start Date : 10/22/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <boost/io/ios_state.hpp>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/value_semantic.hpp>
#include <boost/system/error_code.hpp> // IWYU pragma: keep
#include <boost/system/system_error.hpp>
#include <boost/test/tools/context.hpp>
#include <boost/test/tools/interface.hpp> // IWYU pragma: keep
#include <boost/test/unit_test.hpp>       // IWYU pragma: keep
#include <boost/test/unit_test_suite.hpp>

#include "../../../include/lockd.hpp"
#include "../../../include/logging.hpp"
#ifdef WIN32
# include "../../../include/platform/win32/detail/unicode.hpp"
# include "../../../include/platform/win32/service_elevation.hpp"
#endif
#include "../include/daemon_fixture.hpp"
#include "../include/launcher.hpp"
#include "../include/lockd_test.hpp"

// Selects the supported Process v1 API across Boost versions.
#include "../include/process.hpp" // IWYU pragma: keep

namespace {

std::string capture_console_output(
        const std::string& message,
        const lkd::log_level_t level,
        const lkd::console_print_t destination = lkd::cp_stderr,
        const std::optional<lkd::console_color_code_t> foreground = std::nullopt,
        const std::optional<lkd::console_color_code_t> background = std::nullopt,
        const lkd::log_level_t threshold = lkd::ll_info
)
{
    // The selected console stream writes into this stream through its rdbuf().
    // NOLINTNEXTLINE(misc-const-correctness)
    std::ostringstream output;
    std::ostream& stream = destination == lkd::cp_stdout   ? std::cout
                           : destination == lkd::cp_stdlog ? std::clog
                                                           : std::cerr;
    const boost::io::basic_ios_rdbuf_saver<char> restore(stream, output.rdbuf());
    const auto original_level = lkd::LOG_LEVEL;
    lkd::LOG_LEVEL = threshold;
    try {
        lkd::console_print(destination, level, message, foreground, background);
    }
    catch (...) {
        lkd::LOG_LEVEL = original_level;
        throw;
    }
    lkd::LOG_LEVEL = original_level;
    return output.str();
}


std::string read_log_file(const std::filesystem::path& path)
{
    // Read after the child has exited so its sink has released the file on every platform.
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}


std::vector<std::filesystem::path> rotated_log_files(const std::filesystem::path& directory)
{
    // Only numbered archives belong to this log; similarly named unrelated files must survive.
    std::vector<std::filesystem::path> files;
    const std::string prefix = "custom.log.";
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        const auto name = entry.path().filename().string();
        if (name.compare(0, prefix.size(), prefix) == 0 && name.size() > prefix.size() &&
            name.find_first_not_of("0123456789", prefix.size()) == std::string::npos)
            files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    return files;
}


void run_log_rotation_child(const std::filesystem::path& directory, const std::string& stage)
{
    // File logging initializes once per process. Isolate each restart while exercising the
    // production setup method, rather than resetting its globals for the test.
#ifdef WIN32
    // Use the native environment and argument types: carrying a UTF-8 path through an
    // ANSI child environment would make this check depend on the user's code page.
    ldt::process::wenvironment environment = boost::this_process::wenvironment();
    environment[L"LOCKD_LOG_ROTATION_TEST_DIRECTORY"] = directory.wstring();
    environment[L"LOCKD_LOG_ROTATION_TEST_STAGE"] = lkd::platform::win32::detail::from_utf8(stage);
    const auto executable = lkd::platform::win32::service_elevation::executable_path();
    const std::vector<std::wstring> arguments{
            L"--run_test=lifetimes/file_rotation_preserves_restarts_and_bounds_archives",
            L"--report_level=short"
    };
#else
    ldt::process::environment environment = ldt::process::native_environment();
    environment["LOCKD_LOG_ROTATION_TEST_DIRECTORY"] = directory.native();
    environment["LOCKD_LOG_ROTATION_TEST_STAGE"] = stage;
    const auto executable = ldt::master_test_suite_argv()[0];
    const std::vector<std::string> arguments{
            "--run_test=lifetimes/file_rotation_preserves_restarts_and_bounds_archives",
            "--report_level=short"
    };
#endif
    ldt::process::ipstream output;
    ldt::process::child child(
            executable,
            ldt::process::args(arguments),
            ldt::process::env = environment,
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
    BOOST_REQUIRE_MESSAGE(exited, "Log rotation child exits within ten seconds");
    BOOST_REQUIRE_MESSAGE(
            child.exit_code() == 0, "Log rotation stage " << stage << ": " << diagnostics
    );
}


std::string check_options_child(
        const std::string& executable,
        const std::vector<std::string>& arguments,
        const bool succeeds,
        const std::string& expected_output
)
{
    // These probes either request help or fail validation. Neither path may start a daemon,
    // install a service, or prompt for elevation, even if validation regresses.
    ldt::process::ipstream output;
    ldt::process::child child(
            executable,
            ldt::process::args(arguments),
            (ldt::process::std_out & ldt::process::std_err) > output
    );

    // Debug CRT reports on help exits can exceed the pipe's capacity. Drain while
    // the child runs so writing diagnostics cannot prevent it from exiting.
    auto reader = std::async(std::launch::async, [&output] {
        std::ostringstream text;
        text << output.rdbuf();
        return text.str();
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (child.running() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool exited = !child.running();
    if (!exited)
        child.terminate();
    child.wait();
    const std::string diagnostics = reader.get();
    BOOST_REQUIRE_MESSAGE(exited, "Option validation exits promptly: " << diagnostics);
    BOOST_CHECK_MESSAGE(
            (child.exit_code() == 0) == succeeds,
            "Option validation preserves its success or failure status: " << diagnostics
    );
    BOOST_CHECK_MESSAGE(
            diagnostics.find(expected_output) != std::string::npos,
            "Option validation explains the result: " << diagnostics
    );
    return diagnostics;
}

} // namespace


namespace {

BOOST_AUTO_TEST_SUITE(lifetimes)


BOOST_AUTO_TEST_CASE(file_rotation_preserves_restarts_and_bounds_archives)
{
    // Restart real file sinks around a small rotation threshold. Verify append behavior,
    // retention of discovered archives, unrelated-file preservation, and default error filtering.
#ifdef WIN32
    const ldt::process::wenvironment environment = boost::this_process::wenvironment();
    const auto* const directory_key = L"LOCKD_LOG_ROTATION_TEST_DIRECTORY";
    const auto* const stage_key = L"LOCKD_LOG_ROTATION_TEST_STAGE";
#else
    const ldt::process::environment environment = ldt::process::native_environment();
    const auto* const directory_key = "LOCKD_LOG_ROTATION_TEST_DIRECTORY";
    const auto* const stage_key = "LOCKD_LOG_ROTATION_TEST_STAGE";
#endif
    const auto directory_setting = environment.find(directory_key);
    if (directory_setting != environment.end()) {
        const auto stage_setting = environment.find(stage_key);
        BOOST_REQUIRE(stage_setting != environment.end());
        const std::filesystem::path directory = directory_setting->to_string();
#ifdef WIN32
        const auto stage = lkd::platform::win32::detail::to_utf8(stage_setting->to_string());
#else
        const auto stage = stage_setting->to_string();
#endif
        lkd::logging::setup_logging(
                directory / "custom.log", lkd::ll_info, stage == "disabled" ? 0 : 1, 2
        );

        // Suppress console delivery of large records; the production file sink remains active.
        lkd::IS_FORKED = true;
        lkd::console_print(lkd::cp_stdlog, lkd::ll_debug, "rotation debug marker\n");
        lkd::console_print(lkd::cp_stdlog, lkd::ll_debug, "rotation suppressed fragment");
        lkd::console_print(lkd::cp_stdlog, lkd::ll_info, "rotation stage " + stage + "\n");
        if (stage == "first") {
            ldt::network_fixture fixture;
            const auto connection = fixture.connect();
            connection->send("NOT_A_COMMAND\n");
            BOOST_REQUIRE_MESSAGE(
                    connection->line() == "INVALID_COMMAND", "Invalid commands receive a reply"
            );
            connection->send(std::string(1024, 'x'));
            ldt::poll_until(fixture.server.io_service(), [&fixture] {
                return fixture.server.client_count() == 0;
            });
        }
        else if (stage == "rotate" || stage == "disabled") {
            const std::string record = "rotation payload " + std::string(65536, 'x') + '\n';
            for (unsigned int index = 0; index < 64; ++index)
                lkd::console_print(lkd::cp_stdlog, lkd::ll_info, record);
        }
        lkd::console_print(lkd::cp_stdlog, lkd::ll_info, "rotation complete " + stage + "\n");
        return;
    }

    // Construct the path from UTF-16 on Windows and UTF-8 on POSIX. This also accepts
    // C++20's char8_t literal without u8path(), which is deprecated in that mode.
#ifdef WIN32
    const auto* const prefix = L"log rotation \u798f_\U0001F512 ";
#else
    const auto* const prefix = u8"log rotation \u798f_\U0001F512 ";
#endif
    auto directory = std::filesystem::current_path() / prefix;
    directory += std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    BOOST_REQUIRE_MESSAGE(
            std::filesystem::create_directory(directory), "Use a fresh log directory"
    );
    const auto log_file = directory / "custom.log";
    std::ofstream(log_file) << "previous run\n";
    run_log_rotation_child(directory, "first");
    const auto first = read_log_file(log_file);
    BOOST_CHECK_MESSAGE(first.find("previous run\n") == 0, "Restart appends to the active path");
    BOOST_CHECK_MESSAGE(
            first.find("<error> Command from client ") != std::string::npos,
            "Oversized requests are logged at the default info threshold"
    );
    BOOST_CHECK_MESSAGE(
            first.find("rotation debug marker") == std::string::npos &&
                    first.find("INVALID_COMMAND") == std::string::npos &&
                    first.find("NOT_A_COMMAND") == std::string::npos,
            "Debug messages and ordinary invalid requests do not add file entries"
    );
    BOOST_CHECK_MESSAGE(
            first.find("rotation suppressed fragment") == std::string::npos,
            "Suppressed fragments never join a later accepted file record"
    );
    BOOST_CHECK_MESSAGE(rotated_log_files(directory).empty(), "Shutdown keeps a small active file");

    // Discover older archives on restart instead of forgetting them when applying retention.
    std::ofstream(directory / "custom.log.1") << "old archive one\n";
    std::ofstream(directory / "custom.log.2") << "old archive two\n";
    std::ofstream(directory / "custom.log.notes") << "unrelated notes\n";
    std::ofstream(directory / "other.log.1") << "another log\n";
    run_log_rotation_child(directory, "rotate");
    const auto archives = rotated_log_files(directory);
    BOOST_REQUIRE_MESSAGE(archives.size() == 2, "The configured two-archive limit applies");
    for (const auto& archive : archives) {
        const auto contents = read_log_file(archive);
        BOOST_CHECK_MESSAGE(
                contents.find("rotation payload") != std::string::npos,
                "Retention replaces discovered old archives with current records"
        );
        BOOST_CHECK_MESSAGE(
                std::filesystem::file_size(archive) <= 1024 * 1024 + 65536,
                "The configured one-MiB threshold applies between records"
        );
    }
    BOOST_CHECK_MESSAGE(
            read_log_file(directory / "custom.log.notes") == "unrelated notes\n" &&
                    read_log_file(directory / "other.log.1") == "another log\n",
            "Retention does not remove unrelated files"
    );

    run_log_rotation_child(directory, "restart");
    const auto restarted = read_log_file(log_file);
    BOOST_CHECK_MESSAGE(
            restarted.find("rotation complete rotate") != std::string::npos &&
                    restarted.find("rotation complete restart") != std::string::npos,
            "Restart preserves and appends to the active file after rotations"
    );
    BOOST_CHECK_MESSAGE(
            rotated_log_files(directory) == archives,
            "A small restart does not rename or overwrite existing archives"
    );

    // Size zero must disable both rotation and collection, including removal of older files.
    const auto external_directory = directory / "external";
    std::filesystem::create_directory(external_directory);
    for (unsigned int index = 1; index <= 3; ++index)
        std::ofstream(external_directory / ("custom.log." + std::to_string(index))) << "preserve\n";
    run_log_rotation_child(external_directory, "disabled");
    BOOST_CHECK_MESSAGE(
            std::filesystem::file_size(external_directory / "custom.log") > 4 * 1024 * 1024,
            "Size zero permits the active file to exceed the automatic rotation threshold"
    );
    const auto external_archives = rotated_log_files(external_directory);
    BOOST_REQUIRE_MESSAGE(
            external_archives.size() == 3, "External mode ignores the configured archive count"
    );
    for (const auto& archive : external_archives)
        BOOST_CHECK_MESSAGE(
                read_log_file(archive) == "preserve\n", "External archives remain unchanged"
        );
    std::filesystem::remove_all(directory);
}


BOOST_AUTO_TEST_CASE(zero_log_rotation_size_is_an_accepted_program_option)
{
    // Parse zero through the actual daemon CLI on every platform. Help must advertise the
    // disabled mode and exit successfully without starting a daemon or service.
    ldt::process::ipstream output;
    ldt::process::child child(
            ldt::daemon_path().string(),
            "--log-rotate-size=0",
            "--help",
            (ldt::process::std_out & ldt::process::std_err) > output
    );
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (child.running() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool exited = !child.running();
    if (!exited)
        child.terminate();
    child.wait();
    const std::string help(
            (std::istreambuf_iterator<char>(output)), std::istreambuf_iterator<char>()
    );
    BOOST_REQUIRE_MESSAGE(exited, "Help exits promptly with rotation disabled");
    BOOST_CHECK_MESSAGE(child.exit_code() == 0, "Size zero is accepted");
    BOOST_CHECK_MESSAGE(help.find('\033') == std::string::npos, "Redirected help is plain text");
    const auto logging_heading = help.find("Logging options:");
    BOOST_REQUIRE_MESSAGE(logging_heading != std::string::npos, "Logging has its own category");
    BOOST_CHECK_MESSAGE(
            help.find("Allowed options:") < logging_heading,
            "Logging follows the general daemon options"
    );
    for (const char* const name :
         {"--log-level", "--log-file", "--log-rotate-size", "--log-rotate-keep"}) {
        const auto option = help.find(name);
        BOOST_CHECK_MESSAGE(
                option != std::string::npos && option > logging_heading,
                name << " appears under Logging options"
        );
    }
    BOOST_CHECK_MESSAGE(
            help.find("--log-rotate-size") != std::string::npos &&
                    help.find("0 ") != std::string::npos,
            "Help describes the zero-size mode"
    );
    BOOST_CHECK_MESSAGE(
            help.find("(positive integer)") == std::string::npos,
            "Log option help leaves numeric validation to invalid-input diagnostics"
    );
}


BOOST_AUTO_TEST_CASE(help_styling_preserves_boost_wrapping_and_plain_descriptions)
{
    // Keep Boost's exact text and spacing at several terminal widths. Long defaults,
    // short-only options and paragraphs exercise both same-line and wrapped descriptions;
    // mentioning --help in prose must not turn it into another highlighted option.
    // Usage paths and section headings reset their yellow emphasis before other text.
    namespace po = boost::program_options;
    for (const unsigned int width : {48U, 80U, 120U}) {
        po::options_description options("All options", width, width / 2);
        po::options_description information("Informational options", width, width / 2);
        information.add_options()("help,h", "Print --help without changing description styling");
        options.add(information);
        options.add_options()(",v", "Print the version");
        options.add_options()(
                "log-file,f",
                po::value<std::string>()->default_value("a long directory/with spaces/lockd.log"),
                "Write messages to this file.\nKeep existing contents when the daemon restarts."
        );
        options.add_options()("threads,t", po::value<int>()->default_value(8), "Worker count");
        std::ostringstream boost_help;
        boost_help << options;
        const std::string executable = "directory with spaces/lockd";
        const std::string text = "\nUsage: " + executable + " [options]\n\n" + boost_help.str() +
                                 "\nTail without a newline";

        std::ostringstream styled;
        lkd::logging::print_help(styled, text, options, executable);
        std::string stripped = styled.str();
        BOOST_CHECK_MESSAGE(
                stripped.find(
                        "Usage: \033[33m\033[1m" + executable + "\033[22m\033[39m [options]"
                ) != std::string::npos,
                "The complete executable path is bold yellow, without coloring usage arguments"
        );
        BOOST_CHECK_MESSAGE(
                stripped.find("\033[97m\033[1mAll options:\033[22m\033[39m\n") != std::string::npos,
                "The overall heading uses the same bright white and bold as option defaults"
        );
        BOOST_CHECK_MESSAGE(
                stripped.find("\033[33m\033[1mInformational options:\033[22m\033[39m\n") !=
                        std::string::npos,
                "Nested option categories use the same heading styling"
        );
        for (const auto& option : options.options()) {
            std::string label = "\033[36m\033[1m";
            label += option->format_name();
            label += "\033[97m ";
            label += option->format_parameter();
            label += "\033[22m\033[39m";
            BOOST_CHECK_MESSAGE(
                    stripped.find(label) != std::string::npos,
                    "Only the API-generated name and parameter receive bold styling"
            );
        }
        std::size_t bold_count = 0;
        for (std::string::size_type offset = 0;
             (offset = stripped.find("\033[1m", offset)) != std::string::npos;
             ++offset)
            ++bold_count;
        BOOST_CHECK_MESSAGE(
                bold_count == options.options().size() + 3, "Description text never becomes bold"
        );
        for (const char* const code :
             {"\033[1m", "\033[22m", "\033[33m", "\033[36m", "\033[97m", "\033[39m"}) {
            for (auto offset = stripped.find(code); offset != std::string::npos;
                 offset = stripped.find(code))
                stripped.erase(offset, std::char_traits<char>::length(code));
        }
        BOOST_CHECK_MESSAGE(stripped == text, "Color does not change Boost's wrapping or spacing");

        std::ostringstream plain;
        lkd::logging::print_help(plain, text, options, executable, false);
        BOOST_CHECK_MESSAGE(plain.str() == text, "Disabling color preserves the original help");
        std::ostringstream empty;
        lkd::logging::print_help(empty, "", options, executable);
        BOOST_CHECK_MESSAGE(empty.str().empty(), "Empty help emits no terminal controls");
    }
}


BOOST_AUTO_TEST_CASE(standard_and_boost_system_errors_share_startup_reporting)
{
    // Report equivalent standard and Boost system errors. Verify matching native exit
    // codes, category messages, and fatal colors for bind and other startup failures.
    for (const auto condition : {std::errc::address_not_available, std::errc::permission_denied}) {
        const auto code = std::make_error_code(condition);
        const std::vector<std::exception_ptr> exceptions = {
                std::make_exception_ptr(std::system_error(code)),
                std::make_exception_ptr(
                        boost::system::system_error(code.value(), boost::system::generic_category())
                )
        };
        for (const auto& exception : exceptions) {
            // std::cerr writes into this stream through its rdbuf().
            // NOLINTNEXTLINE(misc-const-correctness)
            std::ostringstream output;
            std::optional<int> result;
            {
                const boost::io::basic_ios_rdbuf_saver<char> restore(std::cerr, output.rdbuf());
                try {
                    std::rethrow_exception(exception);
                }
                catch (const std::exception& error) {
                    result = lkd::logging::report_server_error(error);
                }
            }
            BOOST_REQUIRE_MESSAGE(result.has_value(), "Both system-error types are recognized");
            BOOST_CHECK_MESSAGE(
                    *result == -code.value(), "The native error exit code is preserved"
            );
            const std::string prefix =
                    condition == std::errc::address_not_available
                            ? "bind: Could not bind to the specified address or port ("
                            : "lockd: Could not create server (";
            BOOST_CHECK_MESSAGE(
                    output.str() == "\033[37m\033[41m" + prefix + std::to_string(code.value()) +
                                            "): " + code.message() + "\033[39m\033[49m\n",
                    "Both exception types retain the diagnostic, category message and "
                    "fatal colors"
            );
        }
    }
}


BOOST_AUTO_TEST_CASE(unrelated_exceptions_are_not_reported_as_system_errors)
{
    // Pass an unrelated runtime error to the system-error reporter. Verify it declines
    // the exception without output so the general handler can report it once.
    // std::cerr writes into this stream through its rdbuf().
    // NOLINTNEXTLINE(misc-const-correctness)
    std::ostringstream output;
    {
        const boost::io::basic_ios_rdbuf_saver<char> restore(std::cerr, output.rdbuf());
        const std::runtime_error error("unrelated failure");
        BOOST_CHECK_MESSAGE(
                !lkd::logging::report_server_error(error).has_value(),
                "The caller can rethrow an unrelated exception to its general handler"
        );
    }
    BOOST_CHECK_MESSAGE(output.str().empty(), "Unrecognized exceptions are not logged twice");
}


BOOST_AUTO_TEST_CASE(startup_banner_and_errors_are_complete_separate_lines)
{
    // Launch version, parsing-failure, and server-failure scenarios. Check exit status,
    // banner presence, diagnostic severity, and complete lines without extra separators.
    const auto executable = ldt::daemon_path();
    const auto log_file = std::filesystem::current_path() / "startup formatting.log";
    const std::vector<std::vector<std::string>> scenarios = {
            {"--version"},
            {"--unknown-lockd-option"},
            {"--interface",
             "invalid address",
             "--log-file",
             log_file.string()
#if LOCKD_HAS_FORK
                     ,
             "--no-drop"
#endif
            }};
    for (std::size_t index = 0; index < scenarios.size(); ++index) {
        ldt::process::ipstream standard, errors;
        ldt::process::child child(
                executable.string(),
                ldt::process::args(scenarios[index]),
                ldt::process::std_out > standard,
                ldt::process::std_err > errors
        );
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (child.running() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const bool exited = !child.running();
        if (!exited)
            child.terminate();
        child.wait();
        BOOST_REQUIRE_MESSAGE(exited, "Version and invalid startup requests exit promptly");
        const std::string banner(
                (std::istreambuf_iterator<char>(standard)), std::istreambuf_iterator<char>()
        );
        const std::string diagnostic(
                (std::istreambuf_iterator<char>(errors)), std::istreambuf_iterator<char>()
        );
        if (index == 1) {
            BOOST_CHECK_MESSAGE(banner.empty(), "Parsing failure does not print a startup banner");
            BOOST_CHECK_MESSAGE(
                    diagnostic.find("\033[31minvalid program options: ") == 0,
                    "Option errors begin directly with their error-colored message"
            );
        }
        else {
            BOOST_CHECK_MESSAGE(
                    banner.find("lockd ") == 0, "The banner starts with the daemon version"
            );
            BOOST_REQUIRE_MESSAGE(!banner.empty(), "The banner is present");
            BOOST_CHECK_MESSAGE(
                    banner.find('\n') == banner.size() - 1,
                    "The uncolored banner is exactly one complete line"
            );
            BOOST_CHECK_MESSAGE(
                    banner.find('\033') == std::string::npos,
                    "The banner does not inherit diagnostic colors"
            );
        }
        if (index == 0) {
            BOOST_CHECK_MESSAGE(
                    child.exit_code() == 0 && diagnostic.empty(),
                    "Version output succeeds without extra diagnostics or blank lines"
            );
        }
        else {
            BOOST_CHECK_MESSAGE(child.exit_code() != 0, "Invalid requests report failure");
            BOOST_REQUIRE_MESSAGE(!diagnostic.empty(), "Failure supplies a diagnostic");
            BOOST_CHECK_MESSAGE(
                    diagnostic.front() != '\n' && diagnostic.back() == '\n',
                    "Diagnostics have no leading blank line and end with a newline"
            );
            if (index == 2)
                BOOST_CHECK_MESSAGE(
                        diagnostic.find("\033[37m\033[41mlockd: Could not create server (") == 0,
                        "Startup exceptions begin with the fatal message, not a separator: "
                                << diagnostic
                );
        }
    }
    std::filesystem::remove(log_file);
}


BOOST_AUTO_TEST_CASE(invalid_program_options_are_errors_not_fatal_failures)
{
    // Launch invalid option requests and capture stderr. Verify prompt failure with
    // program-options diagnostics and error colors rather than fatal colors.
    const auto executable = ldt::daemon_path();
    const std::vector<std::vector<std::string>> invalid_arguments = {
            {"--unknown-lockd-option"},
            {"--port", "not-a-number"},
            {"--port", "-1"},
            {"-p", "-65535"},
            {"--port", "65536"},
            {"--threads", "-1"},
            {"-t", "0"},
            {"--threads", "65536"},
            {"--threads", "2147483648"},
            {"--log-level", "invalid"},
            {"--log-rotate-size", "-1"},
            {"--log-rotate-size", "1.5"},
            {"--log-rotate-size", "2147483648"},
            {"--log-rotate-size", "invalid"},
            {"--log-rotate-size"},
            {"--log-rotate-keep", "0"},
            {"--log-rotate-keep", "-1"},
            {"--log-rotate-keep", "1.5"},
            {"--log-rotate-keep", "2147483648"},
            {"--log-rotate-keep", "invalid"},
            {"--log-rotate-keep"}
#ifdef WIN32
            ,
            {"--no-event-log"},
            {"-E"},
            {"-U", "-E"}
#endif // WIN32
    };
    for (const auto& arguments : invalid_arguments) {
        // A rejected value must stay rejected with --help, and a regression must not spawn
        // thousands of workers or carry out a service operation while testing its status.
        auto options = arguments;
        options.emplace_back("--help");
        ldt::process::ipstream errors;
        ldt::process::child child(
                executable.string(),
                ldt::process::args(options),
                ldt::process::std_out > ldt::process::null,
                ldt::process::std_err > errors
        );
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (child.running() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const bool exited = !child.running();
        if (!exited) {
            child.terminate();
        }
        child.wait();
        BOOST_REQUIRE_MESSAGE(exited, "Invalid options exit without starting the daemon");
        const std::string output(
                (std::istreambuf_iterator<char>(errors)), std::istreambuf_iterator<char>()
        );
        BOOST_CHECK_MESSAGE(child.exit_code() != 0, "Invalid options return a failure status");
        BOOST_CHECK_MESSAGE(
                output.find("invalid program options:") != std::string::npos,
                "The program-options handler explains the invalid request"
        );
        BOOST_CHECK_MESSAGE(
                output.find("\033[31m") != std::string::npos,
                "Invalid input uses the ll_error foreground"
        );
        BOOST_CHECK_MESSAGE(
                output.find("\033[41m") == std::string::npos,
                "Invalid input does not use the ll_fatal background"
        );
    }
}


BOOST_AUTO_TEST_CASE(automatic_daemon_workers_preserve_small_counts_and_cap_large_counts)
{
    // Unknown hardware still needs a worker. Odd counts must stay intact, and a large
    // reported count must be capped before it reaches the signed command-line value.
    const std::array<std::pair<unsigned int, unsigned int>, 10> cases = {
            {{0, 1}, {1, 1}, {2, 2}, {3, 3}, {6, 6}, {7, 7}, {8, 8}, {9, 8}, {32, 8}, {65536, 8}}
    };
    for (const auto& [hardware_threads, expected] : cases) {
        BOOST_TEST_CONTEXT("Reported hardware threads: " << hardware_threads)
        {
            BOOST_CHECK_EQUAL(lkd::default_worker_count(hardware_threads), expected);
        }
    }

    // Inspect the daemon's actual option registration, not just the policy helper.
    const auto help = check_options_child(ldt::daemon_path().string(), {"--help"}, true, "Usage:");
    const auto option = help.find("--threads");
    BOOST_REQUIRE_MESSAGE(option != std::string::npos, "Worker option appears in help");
    const auto line = help.substr(option, help.find('\n', option) - option);
    const auto expected = lkd::default_worker_count(std::thread::hardware_concurrency());
    BOOST_CHECK_MESSAGE(
            line.find("(=" + std::to_string(expected) + ')') != std::string::npos,
            "Help advertises the resolved daemon worker default: " << line
    );
}


BOOST_AUTO_TEST_CASE(network_option_boundaries_are_accepted_without_starting_workers)
{
    // Exercise both ends of the valid ranges through the daemon CLI. Port zero remains
    // available for ephemeral listeners; help avoids binding ports or starting 65535 workers.
    for (const auto& arguments : std::vector<std::vector<std::string>>{
                 {"-p", "0", "-t", "1", "--help"},
                 {"--threads", "3", "--help"},
                 {"--threads", "6", "--help"},
                 {"--threads", "9", "--help"},
                 {"--port", "65535", "--threads", "65535", "--help"}
         })
        check_options_child(ldt::daemon_path().string(), arguments, true, "Usage:");
}


BOOST_AUTO_TEST_CASE(test_runner_help_does_not_start_tests)
{
    // Request runner help with and without a suite filter, plus Boost.Test's own help.
    // Both help paths must exit successfully without progress, test bodies, or a run banner.
    for (const auto& arguments : std::vector<std::vector<std::string>>{
                 {"--", "--help"},
                 {"--", "-h"},
                 {"--run_test=output_probes/passing", "--show_progress=yes", "--", "--help"},
                 {"--run_test=output_probes/passing", "--", "--he"},
                 {"--help"}
         }) {
        const auto output =
                check_options_child(ldt::master_test_suite_argv()[0], arguments, true, "Usage");
        BOOST_CHECK_MESSAGE(
                output.find("Running ") == std::string::npos &&
                        output.find("|----|") == std::string::npos &&
                        output.find("DAEMON_OUTPUT_PROBE") == std::string::npos,
                "Help exits before any test execution or progress output: " << output
        );
    }
}


BOOST_AUTO_TEST_CASE(test_options_fail_even_with_expected_assertion_failures)
{
    // Select the entire protocol suite, whose meta test has expected assertion failures.
    // Invalid options in its fixture must still produce visible diagnostics and a failed
    // process status; the expected-failure allowance belongs only to the meta test's body.
    for (const auto& invalid : std::vector<std::vector<std::string>>{
                 {"--unknown-lockd-test-option"},
                 {"-p", "-1"},
                 {"--port", "65536"},
                 {"-t", "0"},
                 {"--threads", "-1"},
                 {"--threads", "65536"},
                 {"--threads", "2147483648"},
                 {"--port", "1", "--port", "2"}
         }) {
        std::vector<std::string> arguments{
                "--run_test=protocol", "--color_output=no", "--show_progress=no", "--"
        };
        arguments.insert(arguments.end(), invalid.begin(), invalid.end());
        arguments.emplace_back("--help");
        check_options_child(
                ldt::master_test_suite_argv()[0], arguments, false, "invalid program options:"
        );
    }
}


BOOST_AUTO_TEST_CASE(test_port_and_executable_path_short_options_are_distinct)
{
    // Parse a path containing spaces through -P and numeric boundaries through -p/-t.
    // Verify the path alias appears in help, with no ambiguous -p or daemon startup.
    for (const auto& values : std::vector<std::vector<std::string>>{
                 {"-p", "0", "-t", "1"}, {"-p", "65535", "-t", "65535"}
         }) {
        std::vector<std::string> arguments{"--run_test=protocol", "--color_output=no", "--"};
        arguments.insert(arguments.end(), values.begin(), values.end());
        arguments.insert(arguments.end(), {"-P", "directory with spaces/lockd", "--help"});
        check_options_child(
                ldt::master_test_suite_argv()[0], arguments, true, "-P [ --lockd-path ]"
        );
    }
}


#ifdef WIN32
BOOST_AUTO_TEST_CASE(service_actions_cannot_be_combined)
{
    // Reject every pair and ordering of service actions, including the hidden SCM entry
    // point. Help makes these probes safe even if a conflicting request is accepted again.
    for (const auto& actions : std::vector<std::vector<std::string>>{
                 {"-S", "-U"},
                 {"-U", "-S"},
                 {"-S", "-A"},
                 {"-A", "-S"},
                 {"-U", "-A"},
                 {"-A", "-U"},
                 {"-S", "-U", "-A"},
                 {"--install-service", "--uninstall-service"},
                 {"--as-service", "--install-service"},
                 {"--uninstall-service", "--as-service"}
         }) {
        auto arguments = actions;
        arguments.emplace_back("--help");
        check_options_child(ldt::daemon_path().string(), arguments, false, "cannot be combined");
    }
    for (const auto& action : {"-S", "-U", "-A"})
        check_options_child(ldt::daemon_path().string(), {action, "--help"}, true, "Usage:");
}


BOOST_AUTO_TEST_CASE(event_log_opt_out_accepts_service_installation_and_execution)
{
    // Validate installation and service-execution options with Event Log disabled.
    // Use --help to verify acceptance without registering or starting a service.
    const auto executable = ldt::daemon_path();
    const std::vector<std::vector<std::string>> arguments = {
            {"-S", "-E", "--help"}, {"--as-service", "--no-event-log", "--help"}
    };
    for (const auto& options : arguments) {
        // Help exits after option validation, before elevation or service registration.
        ldt::process::child child(
                executable.string(),
                ldt::process::args(options),
                ldt::process::std_out > ldt::process::null,
                ldt::process::std_err > ldt::process::null
        );
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (child.running() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const bool exited = !child.running();
        if (!exited)
            child.terminate();
        child.wait();
        BOOST_REQUIRE_MESSAGE(exited, "Option validation exits without starting a service");
        BOOST_CHECK_MESSAGE(
                child.exit_code() == 0,
                "Both installation and the SCM entry point accept the Event Log opt-out"
        );
    }
}
#endif // WIN32


BOOST_AUTO_TEST_CASE(console_severity_colors_and_thresholds_apply_to_each_destination)
{
    // Capture every severity at every threshold on all console destinations. Verify
    // exact colors for visible messages and no text or escape codes for suppressed ones.
    constexpr std::array<lkd::log_level_t, 6> levels = {
            lkd::ll_trace,
            lkd::ll_debug,
            lkd::ll_info,
            lkd::ll_warning,
            lkd::ll_error,
            lkd::ll_fatal
    };
    constexpr std::array<const char*, 6> expected = {
            "message\n",
            "\033[36mmessage\033[39m\n",
            "\033[97mmessage\033[39m\n",
            "\033[33mmessage\033[39m\n",
            "\033[31mmessage\033[39m\n",
            "\033[37m\033[41mmessage\033[39m\033[49m\n"
    };
    for (const auto destination : {lkd::cp_stdout, lkd::cp_stderr, lkd::cp_stdlog}) {
        for (const auto threshold : levels) {
            for (std::size_t index = 0; index < levels.size(); ++index) {
                const auto output = capture_console_output(
                        "message\n",
                        levels[index],
                        destination,
                        std::nullopt,
                        std::nullopt,
                        threshold
                );
                BOOST_CHECK_MESSAGE(
                        output == (levels[index] >= threshold ? expected[index] : ""),
                        "Severity " << levels[index] << " at threshold " << threshold
                                    << " uses the expected colors on destination " << destination
                                    << "; suppressed messages emit neither text nor color codes"
                );
            }
        }
    }
}


BOOST_AUTO_TEST_CASE(explicit_console_colors_override_severity_colors)
{
    // Supply explicit foreground, background, and default colors for fatal messages.
    // Verify these choices replace severity colors and restore only the selected colors.
    BOOST_CHECK_MESSAGE(
            capture_console_output("message\n", lkd::ll_fatal, lkd::cp_stderr, lkd::ct_fg_green) ==
                    "\033[32mmessage\033[39m\n",
            "An explicit foreground suppresses the fatal background"
    );
    BOOST_CHECK_MESSAGE(
            capture_console_output(
                    "message\n", lkd::ll_fatal, lkd::cp_stderr, std::nullopt, lkd::ct_bg_blue
            ) == "\033[44mmessage\033[49m\n",
            "An explicit background suppresses the fatal foreground"
    );
    BOOST_CHECK_MESSAGE(
            capture_console_output(
                    "message\n", lkd::ll_fatal, lkd::cp_stderr, lkd::ct_fg_green, lkd::ct_bg_blue
            ) == "\033[32m\033[44mmessage\033[39m\033[49m\n",
            "Explicit foreground and background colors are both restored"
    );
    BOOST_CHECK_MESSAGE(
            capture_console_output(
                    "message\n",
                    lkd::ll_fatal,
                    lkd::cp_stderr,
                    lkd::ct_fg_default,
                    lkd::ct_bg_default
            ) == "message\n",
            "Explicit defaults leave even fatal messages uncolored"
    );
}


BOOST_AUTO_TEST_CASE(console_colors_do_not_extend_across_line_breaks)
{
    // Capture multiline, blank, empty, and unterminated messages. Verify color resets
    // surround text without coloring line breaks or adding escape codes to empty output.
    BOOST_CHECK_MESSAGE(
            capture_console_output("\nERROR: Service elevation was canceled\n", lkd::ll_fatal) ==
                    "\n\033[37m\033[41mERROR: Service elevation was canceled"
                    "\033[39m\033[49m\n",
            "Fatal colors surround only the error text, not either newline"
    );
    BOOST_CHECK_MESSAGE(
            capture_console_output("first\r\n\r\nsecond\n", lkd::ll_fatal) ==
                    "\033[37m\033[41mfirst\033[39m\033[49m\r\n\r\n"
                    "\033[37m\033[41msecond\033[39m\033[49m\n",
            "Multiline messages preserve CRLF and blank lines without coloring them"
    );
    BOOST_CHECK_MESSAGE(
            capture_console_output("\n\n", lkd::ll_fatal) == "\n\n",
            "Blank lines do not emit color changes"
    );
    BOOST_CHECK_MESSAGE(
            capture_console_output("", lkd::ll_fatal).empty(),
            "An empty message does not emit color changes"
    );
    BOOST_CHECK_MESSAGE(
            capture_console_output("ready", lkd::ll_info) == "\033[97mready\033[39m",
            "Messages without a newline still restore their foreground color"
    );
}


BOOST_AUTO_TEST_CASE(log_levels_format_with_their_names)
{
    // Stream each severity directly and through its program-options wrapper. Verify
    // both representations retain the same textual level names.
    constexpr std::array<lkd::log_level_t, 6> levels = {
            lkd::ll_trace,
            lkd::ll_debug,
            lkd::ll_info,
            lkd::ll_warning,
            lkd::ll_error,
            lkd::ll_fatal
    };
    constexpr std::array<const char*, 6> names = {
            "trace", "debug", "info", "warning", "error", "fatal"
    };
    for (std::size_t index = 0; index < levels.size(); ++index) {
        std::ostringstream severity;
        severity << levels[index];
        BOOST_CHECK_MESSAGE(
                severity.str() == names[index], "Severity attributes retain their textual names"
        );

        std::ostringstream option;
        option << lkd::log_level_input(levels[index]);
        BOOST_CHECK_MESSAGE(
                option.str() == names[index], "Program options use the same severity names"
        );
    }
}


BOOST_AUTO_TEST_CASE(numeric_log_levels_preserve_values_and_reject_partial_input)
{
    // Parse valid numeric levels, signed zero, and malformed or out-of-range inputs.
    // Verify exact level mappings and unchanged output values after rejected input.
    constexpr std::array<lkd::log_level_t, 6> levels = {
            lkd::ll_trace,
            lkd::ll_debug,
            lkd::ll_info,
            lkd::ll_warning,
            lkd::ll_error,
            lkd::ll_fatal
    };
    for (std::size_t index = 0; index < levels.size(); ++index) {
        lkd::log_level_input value(lkd::ll_info);
        BOOST_REQUIRE_MESSAGE(
                lkd::from_str(std::to_string(index), &value), "Each numeric level is accepted"
        );
        BOOST_CHECK_MESSAGE(value.log_level == levels[index], "Numeric severity is unchanged");
    }
    for (const char* const spelling : {"+0", "-0", "00"}) {
        lkd::log_level_input value(lkd::ll_info);
        BOOST_REQUIRE_MESSAGE(lkd::from_str(spelling, &value), "Signed zero remains valid");
        BOOST_CHECK_MESSAGE(value.log_level == lkd::ll_trace, "Zero selects trace severity");
    }
    for (const char* const spelling : {"", "6", "65536", "-1", "1x", " 1", "1 "}) {
        lkd::log_level_input value(lkd::ll_info);
        BOOST_CHECK_MESSAGE(
                !lkd::from_str(spelling, &value),
                "Invalid numeric spelling is rejected: '" << spelling << "'"
        );
        BOOST_CHECK_MESSAGE(value.log_level == lkd::ll_info, "Invalid input preserves severity");
    }
}


BOOST_AUTO_TEST_SUITE_END()

} // namespace

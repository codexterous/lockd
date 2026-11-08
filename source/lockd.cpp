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
 * File       : lockd.cpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Parse daemon options and coordinate startup and shutdown.
 * Start Date : 03/26/2012
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#ifdef WIN32
# include <vector>
#endif

#include <boost/asio/ip/address.hpp>

// The public Format header supplies the template definitions used by free_funcs.hpp.
#include <boost/format.hpp> // IWYU pragma: keep
#include <boost/format/free_funcs.hpp>
#include <boost/program_options/errors.hpp>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/parsers.hpp> // IWYU pragma: keep
#include <boost/program_options/value_semantic.hpp>
#include <boost/program_options/variables_map.hpp>

#include <liblockd/exceptions.hpp>
#include <liblockd/version.hpp>

#include "../include/lockd.hpp"
#include "../include/logging.hpp"
#include "../include/platform.hpp" // IWYU pragma: keep
#ifdef WIN32
# include "../include/platform/win32/detail/unicode.hpp"
#endif
#include "../include/server.hpp"
#include "../include/version.hpp"

int main(int argc, const char* const argv[])
{
    // Figure out program options.
    namespace po = boost::program_options;
    using namespace lkd;

    // Wrap this whole thing in a try block, so we can report any errors that occurred.
    try {
#ifdef WIN32
        // The CRT's narrow argv has already lost characters outside the ANSI code page.
        // Parse UTF-8 bytes with the narrow Program_options parser: its wide parser converts
        // values back through the local code page for value<std::string> semantics.
        const auto arguments = platform::win32::detail::command_line_arguments();
        std::vector<const char*> argument_pointers;
        argument_pointers.reserve(arguments.size() + 1);
        for (const auto& argument : arguments)
            argument_pointers.push_back(argument.c_str());
        argc = static_cast<int>(arguments.size());
        argument_pointers.push_back(nullptr);
        argv = argument_pointers.data();
#endif

        std::string listen_ip = "localhost";

        // http://www.iana.org/assignments/service-names-port-numbers/service-names-port-numbers.xml
        // As of 2036-11-06, port 7198 is unassigned. We should perhaps have it assigned officially.
        const unsigned int terminal_width = platform::native::terminal_width();
        const unsigned int min_desc_length = terminal_width - 2;
        int port = 7198;
        auto threads = static_cast<int>(default_worker_count(std::thread::hardware_concurrency()));
        log_level_input lli(lkd::ll_info);
        int log_rotate_size = static_cast<int>(logging::DEFAULT_LOG_ROTATE_SIZE_MIB);
        int log_rotate_keep = static_cast<int>(logging::DEFAULT_LOG_ROTATE_KEEP);
        const auto current_path =
                std::filesystem::weakly_canonical(std::filesystem::current_path());

        // POSIX startup changes state in prepare() and ready().
        // NOLINTNEXTLINE(misc-const-correctness)
        platform::native::startup platform_startup(current_path, terminal_width, min_desc_length);
        auto log_file = platform_startup.default_log_file();
        auto* const log_file_option =
                po::value<std::string>()->notifier([&log_file](const std::string& value) {
#ifdef WIN32
                    log_file = std::filesystem::u8path(value);
#else
                    log_file = value;
#endif
                });

        // And wrap it in another try block for reporting about exceptions we have a full or near
        // full understanding of, so that we can re-raise if necessary.
        try {
            po::options_description information_options(
                    "Informational options", terminal_width, min_desc_length
            );
            po::options_description allowed_options(
                    "Allowed options", terminal_width, min_desc_length
            );
            po::options_description logging_options(
                    "Logging options", terminal_width, min_desc_length
            );
            po::options_description hidden_options(
                    "Hidden options", terminal_width, min_desc_length
            );


            hidden_options.add_options()(
                    "emit-running,R", "emit \"lockd is running\" when the server starts"
            );

            information_options.add_options()("help,h", "produce help message and exit")(
                    "version,v", "print version information and exit"
            );

            allowed_options.add_options()(
                    "interface,i",
                    po::value<std::string>(&listen_ip)->default_value(listen_ip),
                    "interface to listen on: IPv4/IPv6 address or hostname (first resolved "
                    "address)")(
                    "port,p",
                    po::value<int>(&port)->default_value(port),
                    "port to listen on")(
                    "threads,t",
                    po::value<int>(&threads)->default_value(threads),
                    "number of daemon worker threads (defaults to hardware threads, capped at 8; "
                    "1 if unknown)");

            logging_options.add_options()(
                    "log-level,l",
                    po::value<lkd::log_level_input>(&lli)->default_value(
                            lkd::log_level_input(lkd::ll_info)),
                    "log level (fatal, error, warning, info, debug, trace)")(
                    "log-file,f", log_file_option, "log file name")(
                    "log-rotate-size",
                    po::value<int>(&log_rotate_size)->default_value(log_rotate_size),
#ifdef WIN32
                    "rotate the log at this size in MiB; 0 disables file rotation")(
#else
                    "rotate the log at this size in MiB; 0 uses external rotation "
                    "with SIGUSR1 reopening")(
#endif
                    "log-rotate-keep",
                    po::value<int>(&log_rotate_keep)->default_value(log_rotate_keep),
                    "number of rotated log files to retain; ignored when rotation "
                    "size is 0");

            // Generate a structure of all the user-visible options.
            po::options_description all_visible("All options");
            all_visible.add(information_options).add(allowed_options).add(logging_options);
            platform_startup.add_options(all_visible, hidden_options);

            // And an additional structure of all the options including the hidden ones.
            po::options_description all_options(all_visible);
            all_options.add(hidden_options);

            // Parse the options.
            const auto parsed_options = po::parse_command_line(argc, argv, all_options);
#ifdef WIN32
            // Select the mode-specific default after parsing recognizes aliases, but before
            // store() applies defaults. The same semantic supplies help and the runtime value.
            log_file_option->default_value(
                    platform_startup.default_log_file(parsed_options).u8string()
            );
#else
            log_file_option->default_value(log_file.string());
#endif
            po::variables_map vm;
            po::store(parsed_options, vm);
            po::notify(vm);

            // Parsing into unsigned types can turn negative inputs into valid large values.
            // Check signed values before narrowing, and require at least one I/O worker.
            if (port < 0 || port > std::numeric_limits<std::uint16_t>::max())
                throw po::validation_error(
                        po::validation_error::invalid_option_value, "--port", std::to_string(port)
                );
            if (threads <= 0 || threads > std::numeric_limits<unsigned short>::max())
                throw po::validation_error(
                        po::validation_error::invalid_option_value,
                        "--threads",
                        std::to_string(threads)
                );
            const auto listen_port = static_cast<std::uint16_t>(port);
            const auto worker_count = static_cast<unsigned short>(threads);

            // Signed option values reject negative inputs before converting to the logger's
            // unsigned sizes. Size zero explicitly selects external file management.
            if (log_rotate_size < 0)
                throw po::validation_error(
                        po::validation_error::invalid_option_value,
                        "--log-rotate-size",
                        std::to_string(log_rotate_size)
                );
            if (log_rotate_keep <= 0)
                throw po::validation_error(
                        po::validation_error::invalid_option_value,
                        "--log-rotate-keep",
                        std::to_string(log_rotate_keep)
                );

#ifdef WIN32
            lkd::platform::win32::startup::validate_options(vm);
#else
            platform_startup.validate_options(vm, log_file);
#endif

            console_print(
                    lkd::cp_stdout,
                    lkd::ll_info,
                    str(boost::format("lockd %1% built on liblockd %2%\n") % lkd::version_string() %
                        lld::version_string()),
                    lkd::ct_fg_default,
                    lkd::ct_bg_default
            );

            if (vm.count("help")) {
#ifdef WIN32
                // Print the service introduction separately from Boost's category captions.
                po::options_description common_options("All options");
                common_options.add(information_options).add(allowed_options).add(logging_options);
                const std::string help =
                        str(boost::format("%1%\n%2%") % common_options %
                            platform_startup.options_help(all_visible.get_option_column_width()));
#else
                const std::string help = str(boost::format("%1%") % all_visible);
#endif
                logging::print_help(
                        std::cout,
                        str(boost::format("\nUsage: %1% [options]\n\n%2%\n") % argv[0] % help),
                        all_visible,
                        argv[0]
                );
                return EXIT_SUCCESS;
            }

            // Exit immediately if -v is used.
            if (vm.count("version")) {
                return EXIT_SUCCESS;
            }

            // Resolve once before platform startup so foreground and forked reports agree
            // with the numeric address passed to the server.
            try {
                listen_ip = lkd::server::resolve_interface(listen_ip).to_string();
            }
            catch (const std::exception& error) {
                if (const auto result = lkd::logging::report_server_error(error))
                    return *result;
                throw;
            }

            // https://www.boost.org/doc/libs/1_65_1/libs/log/doc/html/log/rationale/fork_support.html
            // POSIX preparation may fork. Keep Boost.Log initialization and worker threads
            // after it so the child cannot inherit active logging sinks or locked mutexes.
            // Earlier console_print calls flush console output and only queue file messages;
            // the parent returns from prepare without initializing Boost.Log.
            if (const auto result = platform_startup.prepare(
                        vm, listen_ip, listen_port, worker_count, log_file, lli.log_level
                ))
                return *result;

            lkd::logging::setup_logging(
                    log_file,
                    lli.log_level,
                    static_cast<unsigned int>(log_rotate_size),
                    static_cast<unsigned int>(log_rotate_keep)
            );

            if (const auto result =
                        platform_startup.run_service(vm, listen_ip, listen_port, worker_count))
                return *result;

            std::shared_ptr<lkd::server> server_ptr;
            try {
                // Start everything. Print to the user, make a lock store, and create and run a
                // server.
                server_ptr = std::make_shared<lkd::server>(
                        listen_ip,
                        listen_port,
                        worker_count,
                        vm.count("emit-running") ? lkd::rr_report_running
                                                 : lkd::rr_no_report_running
                );
            }
            catch (const std::exception& error) {
                if (const auto result = lkd::logging::report_server_error(error))
                    return *result;
                throw;
            }

#ifdef WIN32
            lkd::platform::win32::startup::ready();
#else
            platform_startup.ready();
#endif
            console_print(
                    lkd::cp_stdout,
                    lkd::ll_info,
                    str(boost::format("starting lockd on interface %1% port %2%\n") %
                        (boost::asio::ip::make_address(listen_ip).is_v6() ? '[' + listen_ip + ']'
                                                                          : listen_ip) %
                        port)
            );

            // Run the server.
            const auto result = static_cast<int>(server_ptr->run());
            return result;
        }
        catch (boost::program_options::error& e) {
            console_print(
                    lkd::cp_stderr,
                    lkd::ll_error,
                    str(boost::format("invalid program options: %1%\n") % e.what())
            );
        }
        catch (lld::exception& e) {
            console_print(
                    lkd::cp_stderr,
                    lkd::ll_fatal,
                    str(boost::format("ERROR (%1%:%2% in %3%): 0x%4$#x - %5%\n") % e.file() %
                        e.line() % e.function() % e.error_type() % e.what())
            );
        }
    }

    // Report on other exceptions that we may not fully understand.
    catch (std::exception& e) {
        console_print(lkd::cp_stderr, lkd::ll_fatal, str(boost::format("ERROR: %1%\n") % e.what()));
    }
    catch (...) {
        console_print(lkd::cp_stderr, lkd::ll_fatal, "ERROR: An unknown exception was caught\n");
    }

    return EXIT_FAILURE;
}

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
 * File       : logging.cpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Configure daemon logging, severity filtering, and file rotation.
 * Start Date : 08/08/2023
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <iostream>
#include <mutex>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <system_error>
#include <tuple>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/date_time/posix_time/ptime.hpp>

// The public Format header supplies the template definitions used by free_funcs.hpp.
#include <boost/format.hpp> // IWYU pragma: keep
#include <boost/format/free_funcs.hpp>
#include <boost/log/core/core.hpp>

// Supplies the expression operators used by Boost.Log formatters and filters.
#include <boost/log/expressions.hpp> // IWYU pragma: keep
#include <boost/log/expressions/attr.hpp>
#include <boost/log/expressions/formatters/date_time.hpp>
#include <boost/log/expressions/formatters/stream.hpp>
#include <boost/log/expressions/message.hpp>
#include <boost/log/keywords/auto_flush.hpp>
#include <boost/log/keywords/enable_final_rotation.hpp>
#include <boost/log/keywords/file_name.hpp>
#include <boost/log/keywords/max_files.hpp>
#include <boost/log/keywords/open_mode.hpp>
#include <boost/log/keywords/target.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/sinks/text_file_backend.hpp>
#include <boost/log/sources/record_ostream.hpp>
#include <boost/log/sources/severity_feature.hpp>
#include <boost/log/sources/severity_logger.hpp>

// Registers the Boost.DateTime formatter support used by format_date_time.
#include <boost/log/support/date_time.hpp> // IWYU pragma: keep
#include <boost/log/utility/setup/common_attributes.hpp>

// The public factory header supplies make_shared's template definitions.
#include <boost/make_shared.hpp> // IWYU pragma: keep
#include <boost/program_options/options_description.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>
#include <boost/system/system_error.hpp>

#ifdef WIN32
# include <memory>
# include <string_view>

# include "../include/platform/win32/detail/unicode.hpp"

# if !defined(WIN32_LEAN_AND_MEAN)
#  define WIN32_LEAN_AND_MEAN
# endif
// Keep the Windows SDK's older Winsock declarations out of Boost.Asio.
# include <winsock2.h> // IWYU pragma: keep
# include <windows.h>  // IWYU pragma: keep
# include <consoleapi.h>
#else
# include <unistd.h>
#endif

#include "../include/lockd.hpp"
#include "../include/logging.hpp"
#include "../include/preprocessor.hpp"

namespace boost_logging = boost::log;
namespace src = boost::log::sources;
namespace sinks = boost::log::sinks;
namespace keywords = boost::log::keywords;
namespace expr = boost::log::expressions;

namespace {

bool LOGGING_SETUP = false;
bool LOGGING_FAILED = false;
typedef std::deque<std::tuple<lkd::log_level_t, std::string>> log_queue_t;
log_queue_t LOG_QUEUE;
typedef sinks::synchronous_sink<sinks::text_file_backend> file_sink_t;
typedef boost::shared_ptr<file_sink_t> file_sink_ptr;
file_sink_ptr FILE_SINK;
std::function<void()> REOPEN_FILE_SINK;
std::mutex PRINT_MUTEX;


file_sink_ptr create_file_sink(
        const std::filesystem::path& log_file,
        const lkd::log_level_t log_level,
        const unsigned int rotate_size_mib,
        const unsigned int rotate_keep
)
{
    // A stable active path permits append-on-restart and external rename/reopen rotation.
    // Final rotation would unexpectedly archive that path on shutdown or when handling USR1.
    const auto backend = boost::make_shared<sinks::text_file_backend>(
            keywords::auto_flush = true,
            keywords::open_mode = std::ios::app,
            keywords::file_name = log_file.native(),
            keywords::enable_final_rotation = false
    );
    if (rotate_size_mib != 0) {
        // Scan only this log's numbered archives. In external mode there is no collector:
        // the administrator's rotation tool owns naming, retention, and compression.
        const auto absolute_path = std::filesystem::absolute(log_file);
        backend->set_rotation_size(static_cast<std::uintmax_t>(rotate_size_mib) * 1024 * 1024);
        auto archive_pattern = absolute_path;
        archive_pattern += ".%N";
        backend->set_target_file_name_pattern(archive_pattern.native());
        backend->set_file_collector(
                sinks::file::make_collector(
                        keywords::target = absolute_path.parent_path().native(),
                        keywords::max_files = rotate_keep
                )
        );
        backend->scan_for_files();
    }

    // Configure before registration so setup failures cannot leave a partial sink in the core.
    // Boost.Log's TimeStamp remains a DateTime value at this formatter boundary.
    const auto sink = boost::make_shared<file_sink_t>(backend);
    sink->set_formatter(
            expr::stream
            << expr::format_date_time<boost::posix_time::ptime>("TimeStamp", "[%Y-%m-%d %H:%M:%S]")
            << ": <" << expr::attr<lkd::log_level_t>("Severity") << "> " << expr::smessage
    );
    sink->set_filter(expr::attr<lkd::log_level_t>("Severity") >= log_level);
    return sink;
}

} // namespace

lkd::log_level_t lkd::LOG_LEVEL = lkd::ll_info;


namespace lkd::logging {
namespace {
class modifier
{
public:
    explicit modifier(const console_color_code_t color_code)
            : color_code(color_code)
    {
    }
    friend std::ostream& operator<<(std::ostream& os, const modifier& mod)
    {
        return os << "\033[" << mod.color_code << "m";
    }
private:
    console_color_code_t color_code;
};
} // namespace
} // namespace lkd::logging


void lkd::logging::print_help(
        std::ostream& destination,
        const std::string& text,
        const boost::program_options::options_description& options,
        const std::string& executable,
        const bool color
)
{
    if (text.empty())
        return;
    bool use_color = color;
#ifdef WIN32
    std::ostringstream rendered;
    std::ostream& output = rendered;
    const auto handle = ::GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD original_mode = 0;
    bool changed_mode = false;
    if (use_color && &destination == &std::cout) {
        use_color = ::GetConsoleMode(handle, &original_mode) != FALSE;
        if (use_color && !(original_mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
            changed_mode =
                    ::SetConsoleMode(handle, original_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) !=
                    FALSE;
            use_color = changed_mode;
        }
    }
    // Older consoles fall back to plain text. Restore the caller's console mode after
    // flushing, including if a stream write throws; never leave a shell setting changed.
    const auto restore_mode = [handle](const DWORD* const mode) {
        ::SetConsoleMode(handle, *mode);
    };
    const std::unique_ptr<const DWORD, decltype(restore_mode)> guard(
            changed_mode ? &original_mode : nullptr, restore_mode
    );
#else
    std::ostream& output = destination;
    if (use_color && &destination == &std::cout)
        use_color = ::isatty(STDOUT_FILENO) != 0;
#endif
    if (!use_color) {
#ifdef WIN32
        platform::win32::detail::write_console(destination, text);
        destination.flush();
#else
        output << text << std::flush;
#endif
        return;
    }

    const std::string usage = "Usage: " + executable;
    output << modifier(ct_normal_intensity) << modifier(ct_fg_default);
    for (std::string::size_type begin = 0; begin < text.size();) {
        const auto line_end = text.find('\n', begin);
        const auto line_size = (line_end == std::string::npos ? text.size() : line_end) - begin;
        if (!executable.empty() && text.compare(begin, usage.size(), usage) == 0) {
            output << "Usage: " << modifier(ct_fg_yellow) << modifier(ct_bold) << executable
                   << modifier(ct_normal_intensity) << modifier(ct_fg_default);
            begin += usage.size();
        }
        else if (line_size != 0 && text[begin] != ' ' && text[begin + line_size - 1] == ':') {
            // Boost keeps captions private, but renders them as unindented lines ending
            // in a colon. Options, wrapped descriptions and the service introduction are
            // indented, so only those caption lines receive heading styling.
            // Match option defaults for the overall heading. Apply bold after color
            // selection so no subsequent color reset can override the emphasis.
            const auto foreground = text.compare(begin, line_size, "All options:") == 0
                                            ? ct_fg_bright_white
                                            : ct_fg_yellow;
            output << modifier(foreground) << modifier(ct_bold);
            output.write(text.data() + begin, static_cast<std::streamsize>(line_size));
            output << modifier(ct_normal_intensity) << modifier(ct_fg_default);
            begin += line_size;
        }

        // Boost prints this exact prefix without wrapping it; description wrapping has
        // already happened. Match the full API-generated label at a line boundary so
        // option names mentioned in prose never acquire option styling.
        for (const auto& option : options.options()) {
            const auto name = option->format_name();
            const auto parameter = option->format_parameter();
            std::string label = "  ";
            label += name;
            label += ' ';
            label += parameter;
            if (text.compare(begin, label.size(), label) != 0)
                continue;
            output << "  " << modifier(ct_fg_cyan) << modifier(ct_bold) << name
                   << modifier(ct_fg_bright_white) << ' ' << parameter
                   << modifier(ct_normal_intensity) << modifier(ct_fg_default);
            begin += label.size();
            break;
        }
        const auto end = text.find('\n', begin);
        output.write(
                text.data() + begin,
                static_cast<std::streamsize>((end == std::string::npos ? text.size() : end) - begin)
        );
        if (end == std::string::npos)
            break;
        output.put('\n');
        begin = end + 1;
    }
    output << std::flush;
#ifdef WIN32
    platform::win32::detail::write_console(destination, rendered.str());
    destination.flush();
#endif
}


std::optional<int> lkd::logging::report_server_error(const std::exception& error)
{
    // Convert Boost codes through their standard-library interoperability, preserving the
    // category as well as the value. Numeric values alone cannot identify a bind error.
    std::error_code code;
    if (const auto* const standard_error = dynamic_cast<const std::system_error*>(&error))
        code = standard_error->code();
    else if (
            const auto* const boost_error = dynamic_cast<const boost::system::system_error*>(&error)
    )
        code = boost_error->code();
    else
        return std::nullopt;

    std::stringstream message;
    if (code == std::errc::address_not_available)
        message << "bind: Could not bind to the specified address or port"
                << " (" << code.value() << "): " << code.message();
    else
        message << "lockd: Could not create server (" << code.value() << "): " << code.message();

    console_print(cp_stderr, ll_fatal, message.str() + "\n");

    // Retain the startup contract: negative return codes identify native system errors.
    return -code.value();
}


void lkd::logging::validate_log_file(const std::filesystem::path& path, const bool print_error)
{
    // Validate the log file path. The best way to determine if the file location exists and is
    // writable by us, is to open it for writing.
    using namespace boost::program_options;
    std::ofstream f(path, std::ios::binary | std::ios::app);
    if (!f.good()) {
        // u8string() returns std::u8string in C++20, while Boost still expects
        // std::string. Preserve POSIX path bytes and convert Windows' native UTF-16.
#ifdef WIN32
        const auto filename = platform::win32::detail::to_utf8(path.native());
#else
        const auto& filename = path.native();
#endif
        if (print_error)
            console_print(
                    lkd::cp_stdout,
                    lkd::ll_fatal,
                    str(boost::format("unable to open log file '%1%' for writing\n") % filename)
            );
        throw boost::program_options::validation_error(
                validation_error::invalid_option_value, "--log-file", filename
        );
    }
    f.close();
}


void lkd::logging::setup_logging(
        const std::filesystem::path& log_file,
        const lkd::log_level_t log_level,
        const unsigned int rotate_size_mib,
        const unsigned int rotate_keep
)
{
    // Don't set up logging more than once.
    if (LOGGING_SETUP)
        return;

    // Filter each destination independently so mandatory service lifecycle events can bypass
    // the Event Log threshold without changing the file's configured verbosity.
    FILE_SINK = create_file_sink(log_file, log_level, rotate_size_mib, rotate_keep);
    boost_logging::core::get()->add_sink(FILE_SINK);
    boost_logging::add_common_attributes();

    // Capture the original file settings independently of the console threshold. Reopening
    // must preserve them, including an explicitly disabled collector.
    REOPEN_FILE_SINK =
            [log_file = std::filesystem::absolute(log_file), rotate_size_mib, rotate_keep] {
                // Check the replacement path before releasing the old descriptor. Failure leaves the
                // previous sink usable, even when an external tool has already renamed its file.
                validate_log_file(log_file, false);
                const auto backend = FILE_SINK->locked_backend();
                if (rotate_size_mib == 0) {
                    backend->rotate_file();
                    return;
                }

                // Boost exposes rotation, but not a plain close/reopen operation. Detach archiving
                // while closing so USR1 neither renames nor collects the active path. Keep the same
                // backend and its archive counter; a fresh backend could reuse an existing number.
                const auto collector = sinks::file::make_collector(
                        keywords::target = log_file.parent_path().native(),
                        keywords::max_files = rotate_keep
                );
                const auto restore_archiving = [&backend, &collector, &log_file] {
                    backend->set_file_collector(collector);
                    auto archive_pattern = log_file;
                    archive_pattern += ".%N";
                    backend->set_target_file_name_pattern(archive_pattern.native());
                };
                backend->set_file_collector({});
                backend->set_target_file_name_pattern("");
                try {
                    backend->rotate_file();
                }
                catch (...) {
                    restore_archiving();
                    throw;
                }
                restore_archiving();
            };
    lkd::LOG_LEVEL = log_level;
    LOGGING_SETUP = true;
}


void lkd::logging::reopen_log_file()
{
    // Signal callbacks run on I/O workers. Serialize with console_print so closing cannot
    // interrupt a record. Each completed record is already flushed.
    const std::lock_guard<std::mutex> lock(PRINT_MUTEX);
    if (!FILE_SINK)
        return;
    REOPEN_FILE_SINK();
    LOGGING_FAILED = false;
    LOGGING_SETUP = true;
}


void lkd::console_print(
        const lkd::console_print_t type,
        const lkd::log_level_t log_level,
        const std::string& message,
        std::optional<lkd::console_color_code_t> fg_color,
        std::optional<lkd::console_color_code_t> bg_color
)
{
    // The configured threshold is fixed before worker threads start. Reject suppressed
    // records before taking the shared print lock or touching the queue and log sinks.
    if (log_level < LOG_LEVEL)
        return;

    // Print to console, keep quiet if we're forked.
    std::unique_lock<std::mutex> lock(PRINT_MUTEX);

    if (!lkd::IS_FORKED && log_level >= LOG_LEVEL) {
        std::ostream* s;
        switch (type) {
        case cp_stdlog: s = &std::clog; break;
        case cp_stdout: s = &std::cout; break;
        case cp_stderr: s = &std::cerr; break;
        default: NODEFAULT(); s = &std::cout; // silence clang-tidy
        }

        // If no explicit color codes are specified, color log messages based on severity.
        if (!fg_color && !bg_color) {
            switch (log_level) {
            case ll_fatal:
                fg_color = ct_fg_white;
                bg_color = ct_bg_red;
                break;
            case ll_error: fg_color = ct_fg_red; break;
            case ll_warning: fg_color = ct_fg_yellow; break;
            case ll_info: fg_color = ct_fg_bright_white; break;
            case ll_debug: fg_color = ct_fg_cyan; break;
            case ll_trace: break;
            }
        }

        // Reset colors before advancing a line. Otherwise a terminal can fill the rest of the
        // row and newly exposed blank rows with a fatal message's background color.
        for (std::string::size_type begin = 0; begin < message.size();) {
            const auto end = message.find_first_of("\r\n", begin);
            const auto count = (end == std::string::npos ? message.size() : end) - begin;
            if (count != 0) {
                if (fg_color && *fg_color != ct_fg_default)
                    *s << lkd::logging::modifier(*fg_color);
                if (bg_color && *bg_color != ct_bg_default)
                    *s << lkd::logging::modifier(*bg_color);

#ifdef WIN32
                platform::win32::detail::write_console(
                        *s, std::string_view(message).substr(begin, count)
                );
#else
                s->write(message.data() + begin, static_cast<std::streamsize>(count));
#endif

                if (fg_color && *fg_color != ct_fg_default)
                    *s << lkd::logging::modifier(ct_fg_default);
                if (bg_color && *bg_color != ct_bg_default)
                    *s << lkd::logging::modifier(ct_bg_default);
            }
            if (end == std::string::npos)
                break;

            *s << message[end];
            begin = end + 1;
        }

        // The flush is important here, otherwise lockd_test can't read our output.
        *s << std::flush;
    }

    // Once delivery has failed, keep console reporting without accumulating an unbounded queue.
    if (LOGGING_FAILED)
        return;

    // Always populate the queue.
    LOG_QUEUE.emplace_back(std::make_tuple(log_level, message));

    // Don't actually use boost.log if logging isn't set up yet. Log messages will be cached and be
    // emitted once it's set up.
    if (LOGGING_SETUP) {
        src::severity_logger<log_level_t> lg;

        log_level_t worst_severity = ll_trace;

        // Log messages are queued until we find an \n. Go through queued log messages until we find
        // one that ends in \n, and send the combined messages to the logger. When we finally
        // log combined messages, use the highest severity of the messages.
        log_queue_t::size_type pop_front = 0;
        log_queue_t::size_type found = 0;
        std::string log_message;
        for (auto&& pos : LOG_QUEUE) {
            if (static_cast<int>(std::get<0>(pos)) > static_cast<int>(worst_severity)) {
                worst_severity = std::get<0>(pos);
            }

            log_message += std::get<1>(pos);
            ++found;
            if (boost::algorithm::ends_with(log_message, "\n")) {
                boost::algorithm::trim_right_if(log_message, boost::algorithm::is_any_of("\r\n"));

                try {
                    BOOST_LOG_SEV(lg, worst_severity) << log_message;
                }
                catch (const std::exception& error) {
                    // Reporting a broken sink must not enter that sink again, especially while
                    // reporting another exception. Release the console lock before reporting it.
                    LOGGING_SETUP = false;
                    LOGGING_FAILED = true;
                    LOG_QUEUE.clear();
                    lock.unlock();
                    console_print(
                            cp_stderr,
                            ll_fatal,
                            std::string("ERROR: Disabling failed logging sink: ") + error.what() +
                                    "\n"
                    );
                    return;
                }

                // Reset everything, and instruct later code to remove these queued messages.
                log_message.erase();
                pop_front += found;
                found = 0;
                worst_severity = ll_trace;
            }
        }

        // Now that we're done iterating, pop all the messages we processed.
        for (log_queue_t::size_type i = 0; i < pop_front; ++i)
            LOG_QUEUE.pop_front();
    }
}


lkd::log_level_input::log_level_input(const lkd::log_level_t& ll)
        : log_level(ll)
{
}


lkd::log_level_input::log_level_input()
        : log_level(lkd::ll_info)
{
}


lkd::log_level_input::log_level_input(const std::string& s)
{
    using namespace boost::program_options;
    log_level_input lli(lkd::ll_info);
    if (!lkd::from_str(s, &lli))
        throw validation_error(validation_error::invalid_option_value);
    this->log_level = lli.log_level;
}


lkd::log_level_input& lkd::log_level_input::operator=(const lkd::log_level_t ll)
{
    log_level = ll;
    return *this;
}


bool lkd::from_str(const std::string& s, log_level_input* const lli_ptr)
{
    // Ensure we have a valid numerical value or text value.
    if (s == "trace")
        *lli_ptr = log_level_input(lkd::ll_trace);
    else if (s == "debug")
        *lli_ptr = log_level_input(lkd::ll_debug);
    else if (s == "info")
        *lli_ptr = log_level_input(lkd::ll_info);
    else if (s == "warning")
        *lli_ptr = log_level_input(lkd::ll_warning);
    else if (s == "error")
        *lli_ptr = log_level_input(lkd::ll_error);
    else if (s == "fatal")
        *lli_ptr = log_level_input(lkd::ll_fatal);
    else {
        // Require the entire string, without skipping whitespace. Stream extraction retains
        // the signed and unsigned numerical spellings accepted by the option parser.
        unsigned short value = 0;
        std::istringstream input(s);
        input >> std::noskipws >> value;
        if (!input || !input.eof())
            return false;

        switch (value) {
        case 0: *lli_ptr = lkd::ll_trace; break;
        case 1: *lli_ptr = lkd::ll_debug; break;
        case 2: *lli_ptr = lkd::ll_info; break;
        case 3: *lli_ptr = lkd::ll_warning; break;
        case 4: *lli_ptr = lkd::ll_error; break;
        case 5: *lli_ptr = lkd::ll_fatal; break;
        default: return false;
        }
        return true;
    }
    return true;
}

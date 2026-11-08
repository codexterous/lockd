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
 * File       : logging.hpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Configure daemon logging, severity filtering, and file rotation.
 * Start Date : 08/08/2023
 *
 * Description: The file sink uses Boost.Log's synchronous text backend, adapted from its
 *              logging examples. Rotation runs in the logging caller, so renaming and collecting
 *              archives can briefly hold up client handling. An active log keeps a stable append
 *              path; automatic rotation uses unpadded numbered archives and disables final
 *              rotation at shutdown. A zero rotation size leaves retention to an external tool.
 *
 *              Reopening preserves the backend and archive counter. It validates the new path
 *              before closing the old descriptor and detaches automatic archiving during the
 *              reopen, so SIGUSR1 doesn't rotate the active file or reuse an archive number.
 *
 *              Ordinary messages below LOG_LEVEL return before synchronization or queueing.
 *              The threshold is selected before daemon workers start and stays fixed while
 *              they run. Suppressed fragments, including newlines, are discarded; completing
 *              an accepted partial record requires an accepted newline. Callers still evaluate
 *              message arguments before console_print is entered.
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */


#if !defined(LOCKD_LOCKD_LOGGING_HPP_INCLUDED_)
# define LOCKD_LOCKD_LOGGING_HPP_INCLUDED_

# if defined(_MSC_VER) && (_MSC_VER >= 1020)
#  pragma once
# endif

# include <exception>
# include <filesystem>
# include <iosfwd>
# include <optional>
# include <string>

# include <boost/program_options.hpp>

# include "../include/preprocessor.hpp"

namespace lkd {

typedef enum
{
    cp_stdout,
    cp_stderr,
    cp_stdlog
} console_print_t;


typedef enum
{
    ll_fatal = 5,
    ll_error = 4,
    ll_warning = 3,
    ll_info = 2,
    ll_debug = 1,
    ll_trace = 0
} log_level_t;


typedef enum
{
    ct_bold = 1,
    ct_normal_intensity = 22,
    ct_fg_red = 31,
    ct_fg_green = 32,
    ct_fg_yellow = 33,
    ct_fg_blue = 34,
    ct_fg_magenta = 35,
    ct_fg_cyan = 36,
    ct_fg_white = 37,
    ct_fg_bright_white = 97,
    ct_fg_default = 39,
    ct_bg_black = 40,
    ct_bg_red = 41,
    ct_bg_green = 42,
    ct_bg_yellow = 43,
    ct_bg_blue = 44,
    ct_bg_magenta = 45,
    ct_bg_cyan = 46,
    ct_bg_white = 47,
    ct_bg_default = 49
} console_color_code_t;


extern log_level_t LOG_LEVEL;


struct log_level_input
{
    // This is mostly for boost::program_options, and serves no real purpose otherwise.
    explicit log_level_input(const lkd::log_level_t& ll);
    explicit log_level_input(const std::string& s);
    log_level_input(const log_level_input& ll) = default;
    log_level_input& operator=(const log_level_input& ll) = default;
    log_level_input();
    log_level_input& operator=(lkd::log_level_t ll);

    lkd::log_level_t log_level;
};


bool from_str(const std::string& s, log_level_input* lli_ptr);

void console_print(
        lkd::console_print_t type,
        lkd::log_level_t log_level,
        const std::string& message,
        std::optional<console_color_code_t> fg_color = std::optional<console_color_code_t>(),
        std::optional<console_color_code_t> bg_color = std::optional<console_color_code_t>()
);

template <typename C>
std::basic_ostream<C, std::char_traits<C>>&
operator<<(std::basic_ostream<C, std::char_traits<C>>& strm, const lkd::log_level_t level)
{
    switch (level) {
    case lkd::ll_fatal: strm << "fatal"; break;
    case lkd::ll_error: strm << "error"; break;
    case lkd::ll_warning: strm << "warning"; break;
    case lkd::ll_info: strm << "info"; break;
    case lkd::ll_debug: strm << "debug"; break;
    case lkd::ll_trace: strm << "trace"; break;
    default: NODEFAULT();
    }
    return strm;
}

template <typename C>
std::basic_ostream<C, std::char_traits<C>>&
operator<<(std::basic_ostream<C, std::char_traits<C>>& strm, const lkd::log_level_input& lli)
{
    return strm << lli.log_level;
}

template <typename C>
std::basic_istream<C, std::char_traits<C>>&
operator>>(std::basic_istream<C, std::char_traits<C>>& strm, lkd::log_level_input& lli)
{
    using namespace boost::program_options;
    std::string token;
    strm >> token;
    if (!lkd::from_str(token, &lli))
        throw validation_error(validation_error::invalid_option_value);
    return strm;
}

namespace logging {

inline constexpr unsigned int DEFAULT_LOG_ROTATE_SIZE_MIB = 10;
inline constexpr unsigned int DEFAULT_LOG_ROTATE_KEEP = 5;

// Decorate Boost.Program_options' already wrapped help; redirected stdout remains plain text.
void print_help(
        std::ostream& destination,
        const std::string& text,
        const boost::program_options::options_description& options,
        const std::string& executable,
        const bool color = true
);
void validate_log_file(const std::filesystem::path& path, const bool print_error);
// Report either standard or Boost system errors. Leave unrelated exceptions to the caller.
[[nodiscard]] std::optional<int> report_server_error(const std::exception& error);
void setup_logging(
        const std::filesystem::path& log_file,
        const log_level_t log_level,
        const unsigned int rotate_size_mib = DEFAULT_LOG_ROTATE_SIZE_MIB,
        const unsigned int rotate_keep = DEFAULT_LOG_ROTATE_KEEP
);
void reopen_log_file();

}
} // namespace lkd::logging

#endif // !defined(LOCKD_LOCKD_LOGGING_HPP_INCLUDED_)

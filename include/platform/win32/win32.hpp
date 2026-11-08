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
 * File       : win32.hpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Windows daemon startup and service options.
 * Start Date : 10/24/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#if !defined(LOCKD_PLATFORM_WIN32_HPP_INCLUDED_)
# define LOCKD_PLATFORM_WIN32_HPP_INCLUDED_

# if defined(_MSC_VER) && (_MSC_VER >= 1020)
#  pragma once
# endif

# include <cstdint>
# include <filesystem>
# include <optional>
# include <string>

# include <boost/program_options.hpp>

# include "../../logging.hpp"

namespace lkd::platform::win32 {

[[nodiscard]] unsigned int terminal_width();

// Program-options values refer to this instance. Keep it alive through parsing and execution.
class startup
{
    /* construction and destruction */
public:
    explicit startup(
            std::filesystem::path current_path,
            const unsigned int width,
            const unsigned int description_width
    );
    virtual ~startup() = default;

    /* discarded methods */
public:
    startup(const startup&) = delete;
    startup& operator=(const startup&) = delete;

    /* public methods */
public:
    [[nodiscard]] std::filesystem::path default_log_file() const;
    [[nodiscard]] std::filesystem::path
    default_log_file(const boost::program_options::parsed_options& options) const;
    [[nodiscard]] std::string options_help(const unsigned int option_column_width) const;
    void add_options(
            boost::program_options::options_description& visible,
            boost::program_options::options_description& hidden
    ) const;
    static void validate_options(const boost::program_options::variables_map& options);
    // An exit status ends this process; no value means normal daemon startup should continue.
    [[nodiscard]] std::optional<int>
    prepare(const boost::program_options::variables_map& options,
            const std::string& listen_ip,
            const std::uint16_t port,
            const unsigned short threads,
            const std::filesystem::path& log_file,
            const lkd::log_level_t log_level) const;
    [[nodiscard]] std::optional<int> run_service(
            const boost::program_options::variables_map& options,
            const std::string& listen_ip,
            const std::uint16_t port,
            const unsigned short threads
    ) const;
    static void ready();

    /* private data members */
private:
    const std::filesystem::path _currentPath;
    std::string _serviceName;
    boost::program_options::options_description _options;
};

} // namespace lkd::platform::win32

#endif // !defined(LOCKD_PLATFORM_WIN32_HPP_INCLUDED_)

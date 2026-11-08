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
 * File       : posix.hpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : POSIX daemon startup and identity selection.
 * Start Date : 10/24/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#if !defined(LOCKD_PLATFORM_POSIX_HPP_INCLUDED_)
# define LOCKD_PLATFORM_POSIX_HPP_INCLUDED_

# if defined(_MSC_VER) && (_MSC_VER >= 1020)
#  pragma once
# endif

# include <cstdint>
# include <filesystem>
# include <optional>
# include <string>

# include <boost/program_options.hpp>

// passwd uses the POSIX ID types.
# include <sys/types.h>
# include <pwd.h>

# include "../logging.hpp"

# if LOCKD_HAS_FORK
namespace lkd::platform::posix {

void add_identity_options(
        boost::program_options::options_description& options,
        std::string& user,
        std::string& group,
        const passwd* const default_user
);
void validate_identity_options(
        const boost::program_options::variables_map& options, const bool no_drop
);

// Own the startup handshake and the child's PID file. The parent never drops privileges or
// initializes logging; EOF before ready() tells it that child startup failed.
class posix_startup
{
    /* construction and destruction */
public:
    posix_startup();
    virtual ~posix_startup();

    /* discarded methods */
public:
    posix_startup(const posix_startup&) = delete;
    posix_startup& operator=(const posix_startup&) = delete;

    /* public methods */
public:
    [[nodiscard]] pid_t start();
    [[nodiscard]] int wait() const;
    static void report_stale_pid(const std::filesystem::path& path);
    void write_pid(const std::filesystem::path& path);
    void ready();

    /* private methods */
private:
    [[nodiscard]] static int
    claim_pid_file(const std::filesystem::path& path, std::optional<pid_t>& reclaimed_pid);

    /* private data members */
private:
    int _readFd;
    int _writeFd;
    pid_t _child;
    int _pidFd;
    std::filesystem::path _pidFile;
};

// Only newly created leaf directories are assigned to the service account. Existing shared
// directories and explicit file locations must never have their ownership changed implicitly.
void prepare_runtime_directory(const std::filesystem::path& path, const uid_t uid, const gid_t gid);

namespace detail {

// Assign a newly created leaf's ownership relative to its pinned parent. The name must be a
// single path component; this stage must never follow a replacement symlink.
void assign_runtime_directory_ownership(
        const int parent, const std::filesystem::path& name, const uid_t uid, const gid_t gid
);

} // namespace detail

} // namespace lkd::platform::posix
# endif // LOCKD_HAS_FORK

namespace lkd::platform::posix {

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
    void add_options(
            boost::program_options::options_description& visible,
            boost::program_options::options_description& hidden
    ) const;
    void validate_options(
            const boost::program_options::variables_map& options,
            const std::filesystem::path& log_file
    ) const;
    // An exit status ends this process; no value means normal daemon startup should continue.
    [[nodiscard]] std::optional<int>
    prepare(const boost::program_options::variables_map& options,
            const std::string& listen_ip,
            const std::uint16_t port,
            const unsigned short threads,
            std::filesystem::path& log_file,
            const lkd::log_level_t log_level);
    [[nodiscard]] std::optional<int> run_service(
            const boost::program_options::variables_map& options,
            const std::string& listen_ip,
            const std::uint16_t port,
            const unsigned short threads
    ) const;
    void ready();

    /* private data members */
private:
    const std::filesystem::path _currentPath;
# if LOCKD_HAS_FORK
    bool _noFork;
    bool _noDrop;
    std::string _user;
    std::string _group;
    std::string _pidFile;
    boost::program_options::options_description _options;
    lkd::platform::posix::posix_startup _process;
# endif
};

} // namespace lkd::platform::posix

#endif // !defined(LOCKD_PLATFORM_POSIX_HPP_INCLUDED_)

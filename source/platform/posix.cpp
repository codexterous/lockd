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
 * File       : posix.cpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : POSIX daemon startup and identity selection.
 * Start Date : 10/24/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include <boost/asio/ip/address.hpp>
// The public Format header supplies the template definitions used by free_funcs.hpp.
#include <boost/format.hpp> // IWYU pragma: keep
#include <boost/format/free_funcs.hpp>
#include <boost/program_options/errors.hpp>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/value_semantic.hpp>
#include <boost/program_options/variables_map.hpp>
#include <boost/system/system_error.hpp>

#include <sys/ioctl.h>

#if LOCKD_HAS_FORK
# include <fcntl.h>
# include <grp.h>
# include <pwd.h>
# include <signal.h> // NOLINT(modernize-deprecated-headers): POSIX kill, outside <csignal>.
# include <unistd.h>

# include <sys/file.h>
# include <sys/stat.h>
# include <sys/types.h>
# include <sys/wait.h>

#endif

#include "../../include/lockd.hpp"
#include "../../include/logging.hpp"
#include "../../include/platform/posix.hpp"

bool lkd::IS_FORKED = false;


#if LOCKD_HAS_FORK
namespace {

bool is_plain_digits(const std::string& value)
{
    // PID files and numeric account IDs use ASCII digits, independent of the locale.
    // Passing a negative signed char to isdigit would be undefined for malformed input.
    return !value.empty() &&
           std::all_of(value.begin(), value.end(), [](const unsigned char character) {
               return character >= '0' && character <= '9';
           });
}


void throw_system_error(const char* const operation)
{
    throw boost::system::system_error(errno, boost::system::generic_category(), operation);
}

pid_t stale_pid(const int descriptor, const std::string& context)
{
    char buffer[64];
    const auto count = pread(descriptor, buffer, sizeof(buffer), 0);
    if (count < 0)
        throw boost::system::system_error(errno, boost::system::generic_category(), context);
    const std::string contents(buffer, static_cast<std::size_t>(count));
    const auto end = contents.find_last_not_of("\r\n");
    const auto digits = contents.substr(0, end == std::string::npos ? 0 : end + 1);
    pid_t pid = 0;
    const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), pid);
    if (count == static_cast<ssize_t>(sizeof(buffer)) || !is_plain_digits(digits) || pid <= 0 ||
        result.ec != std::errc() || result.ptr != digits.data() + digits.size())
        throw std::runtime_error(context + ": invalid PID; refusing to replace the file");
    if (kill(pid, 0) == 0)
        throw std::runtime_error(
                context + ": process " + digits + " still exists (is lockd still running?)"
        );
    if (errno != ESRCH)
        throw boost::system::system_error(
                errno, boost::system::generic_category(), context + ": checking process " + digits
        );
    return pid;
}


} // namespace


void lkd::platform::posix::add_identity_options(
        boost::program_options::options_description& options,
        std::string& user,
        std::string& group,
        const passwd* const default_user
)
{
    namespace po = boost::program_options;
    auto* const user_option = po::value<std::string>(&user);
    std::string default_group = "user's primary group";
    if (default_user) {
        // Copy the account name before another account database lookup can reuse its storage.
        user_option->default_value(default_user->pw_name);
        const auto gid = default_user->pw_gid;
        const auto* const entry = getgrgid(gid);
        default_group = entry ? entry->gr_name : std::to_string(gid);
    }
    options.add_options()("user,u", user_option, "user name or uid to drop to (required without nobody unless --no-drop is used)")(
            "group,g",
            po::value<std::string>(&group)->default_value("", default_group),
            "group name or gid to drop to (defaults to the user's primary group)"
    );
}


void lkd::platform::posix::validate_identity_options(
        const boost::program_options::variables_map& options, const bool no_drop
)
{
    // Help and version exit before this check, and --no-drop needs no target identity.
    if (!no_drop && !options.count("user"))
        throw boost::program_options::required_option("user");
}


lkd::platform::posix::posix_startup::posix_startup()
        : _readFd(-1)
        , _writeFd(-1)
        , _child(0)
        , _pidFd(-1)
{
}


lkd::platform::posix::posix_startup::~posix_startup()
{
    if (_readFd != -1)
        close(_readFd);
    if (_writeFd != -1)
        close(_writeFd);
    if (_pidFd != -1) {
        // Do not remove a replacement file if an administrator changed the path while we ran.
        struct stat owned{}, current{};
        if (fstat(_pidFd, &owned) == 0 && lstat(_pidFile.c_str(), &current) == 0 &&
            owned.st_dev == current.st_dev && owned.st_ino == current.st_ino)
            unlink(_pidFile.c_str());
        close(_pidFd);
    }
}


pid_t lkd::platform::posix::posix_startup::start()
{
    int descriptors[2];
    if (pipe(descriptors) != 0)
        throw_system_error("Creating startup pipe");
    _readFd = descriptors[0];
    _writeFd = descriptors[1];
    _child = fork();
    if (_child < 0)
        throw_system_error("Forking daemon");
    if (_child == 0) {
        close(_readFd);
        _readFd = -1;
    }
    else {
        close(_writeFd);
        _writeFd = -1;
    }
    return _child;
}


int lkd::platform::posix::posix_startup::wait() const
{
    char status = 0;
    ssize_t count;
    do {
        count = read(_readFd, &status, sizeof(status));
    } while (count < 0 && errno == EINTR);
    if (count == 1 && status == 1)
        return EXIT_SUCCESS;

    // Reap a failed child. A successful child remains running independently of the parent.
    int result = 0;
    while (waitpid(_child, &result, 0) < 0 && errno == EINTR) {
    }
    return EXIT_FAILURE;
}


void lkd::platform::posix::posix_startup::report_stale_pid(const std::filesystem::path& path)
{
    // This is an advisory check before fork, not ownership. The child must still claim the
    // file after dropping privileges; it may have changed, or be unwritable by that account.
    const int descriptor = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (descriptor < 0)
        return;
    std::optional<pid_t> pid;
    try {
        struct stat file{};
        if (fstat(descriptor, &file) == 0 && S_ISREG(file.st_mode) && file.st_nlink == 1 &&
            flock(descriptor, LOCK_SH | LOCK_NB) == 0)
            pid = stale_pid(descriptor, "Checking PID file '" + path.string() + "'");
    }
    catch (const std::exception&) {
        // Leave failures to the authoritative claim, which reports them with the file path.
    }
    close(descriptor);
    if (pid)
        console_print(
                cp_stdout,
                ll_info,
                str(boost::format(
                            "Found stale PID file '%1%' (PID %2%); "
                            "will attempt to reclaim it during startup.\n"
                    ) %
                    path.string() % *pid)
        );
}


int lkd::platform::posix::posix_startup::claim_pid_file(
        const std::filesystem::path& path, std::optional<pid_t>& reclaimed_pid
)
{
    const auto context = "Claiming PID file '" + path.string() + "'";
    constexpr int flags = O_RDWR | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC;
    int descriptor = open(path.c_str(), flags | O_CREAT | O_EXCL, 0644);
    const bool created = descriptor >= 0;
    if (!created && errno == EEXIST)
        descriptor = open(path.c_str(), flags);
    if (descriptor < 0)
        throw boost::system::system_error(errno, boost::system::generic_category(), context);

    try {
        // Keep this lock for our entire lifetime. Reuse the locked inode instead of unlinking
        // a stale file: unlink/recreate could let concurrent starters lock different files.
        if (flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
            const int error = errno;
            if (error == EWOULDBLOCK || error == EAGAIN)
                throw std::runtime_error(
                        "PID file '" + path.string() +
                        "' is already in use (is lockd still running?)"
                );
            throw boost::system::system_error(error, boost::system::generic_category(), context);
        }
        struct stat owned{}, current{};
        if (fstat(descriptor, &owned) != 0 || lstat(path.c_str(), &current) != 0)
            throw boost::system::system_error(errno, boost::system::generic_category(), context);
        if (!S_ISREG(owned.st_mode) || owned.st_nlink != 1 || owned.st_dev != current.st_dev ||
            owned.st_ino != current.st_ino)
            throw std::runtime_error(context + ": not an unchanged, singly linked regular file");

        if (!created) {
            reclaimed_pid = stale_pid(descriptor, context);
        }
        return descriptor;
    }
    catch (...) {
        close(descriptor);
        throw;
    }
}


void lkd::platform::posix::posix_startup::write_pid(const std::filesystem::path& path)
{
    if (_pidFd != -1)
        throw std::logic_error("Already owning PID file '" + _pidFile.string() + "'");
    _pidFile = path;
    std::optional<pid_t> reclaimed_pid;
    _pidFd = claim_pid_file(path, reclaimed_pid);
    const auto context = "Writing PID file '" + path.string() + "'";
    if (ftruncate(_pidFd, 0) != 0)
        throw boost::system::system_error(errno, boost::system::generic_category(), context);
    const auto value = std::to_string(getpid()) + "\n";
    const auto count = write(_pidFd, value.data(), value.size());
    if (count != static_cast<ssize_t>(value.size()))
        throw boost::system::system_error(
                count < 0 ? errno : EIO, boost::system::generic_category(), context
        );
    if (reclaimed_pid)
        console_print(
                cp_stdout,
                ll_info,
                str(boost::format(
                            "Reclaimed stale PID file '%1%' (previous PID %2%, current "
                            "PID %3%).\n"
                    ) %
                    path.string() % *reclaimed_pid % getpid())
        );
}


void lkd::platform::posix::posix_startup::ready()
{
    if (_writeFd == -1)
        return;
    constexpr char status = 1;
    ssize_t count;
    do {
        count = write(_writeFd, &status, sizeof(status));
    } while (count < 0 && errno == EINTR);
    if (count != sizeof(status))
        throw_system_error("Reporting daemon startup");
    close(_writeFd);
    _writeFd = -1;
    lkd::IS_FORKED = true;

    // Keep the standard descriptors occupied so future file opens cannot reuse stdout/stderr.
    const int null_descriptor = open("/dev/null", O_RDWR);
    if (null_descriptor >= 0) {
        dup2(null_descriptor, STDIN_FILENO);
        dup2(null_descriptor, STDOUT_FILENO);
        dup2(null_descriptor, STDERR_FILENO);
        if (null_descriptor > STDERR_FILENO)
            close(null_descriptor);
    }
}


void lkd::platform::posix::prepare_runtime_directory(
        const std::filesystem::path& path, const uid_t uid, const gid_t gid
)
{
    // Keep trailing separators and root paths equivalent to their usual directory spelling.
    // Do not normalize '..' across a symlink: that could select a different parent directory.
    const auto absolute = std::filesystem::absolute(path);
    const auto directory = absolute.filename().empty() ? absolute.parent_path() : absolute;
    const auto name =
            directory.filename().empty() ? std::filesystem::path(".") : directory.filename();
    const auto context = "Preparing runtime directory '" + path.string() + "'";

    // Pin the parent before creation so replacing any part of its pathname cannot redirect
    // the subsequent ownership change. Intentional parent symlinks, such as /var/run, still work.
    const int parent = open(directory.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (parent < 0)
        throw boost::system::system_error(errno, boost::system::generic_category(), context);
    try {
        if (mkdirat(parent, name.c_str(), 0777) == 0)
            detail::assign_runtime_directory_ownership(parent, name, uid, gid);
        else {
            if (errno != EEXIST)
                throw boost::system::system_error(
                        errno, boost::system::generic_category(), context
                );

            // Preserve the existing-directory contract, including administrator-owned symlinks
            // to directories. Neither an existing directory nor its target is ever chowned.
            struct stat existing{};
            if (fstatat(parent, name.c_str(), &existing, 0) != 0)
                throw boost::system::system_error(
                        errno, boost::system::generic_category(), context
                );
            if (!S_ISDIR(existing.st_mode))
                throw boost::system::system_error(
                        EEXIST, boost::system::generic_category(), context
                );
        }
    }
    catch (...) {
        close(parent);
        throw;
    }
    close(parent);
}


void lkd::platform::posix::detail::assign_runtime_directory_ownership(
        const int parent, const std::filesystem::path& name, const uid_t uid, const gid_t gid
)
{
    const auto context = "Assigning runtime directory ownership for '" + name.string() + "'";
    const int descriptor =
            openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0)
        throw boost::system::system_error(errno, boost::system::generic_category(), context);
    try {
        // Once opened, renaming or replacing the leaf cannot redirect this operation either.
        if (fchown(descriptor, uid, gid) != 0)
            throw boost::system::system_error(errno, boost::system::generic_category(), context);
    }
    catch (...) {
        close(descriptor);
        throw;
    }
    close(descriptor);
}

#endif // LOCKD_HAS_FORK

unsigned int lkd::platform::posix::terminal_width()
{
    winsize window{};
    // sys/ioctl.h is the public interface; Linux defines this macro in a private header.
    // NOLINTNEXTLINE(misc-include-cleaner)
    if (-1 != ioctl(0, TIOCGWINSZ, &window))
        return window.ws_col;
    return 80;
}


lkd::platform::posix::startup::startup(
        std::filesystem::path current_path,
        const unsigned int width,
        const unsigned int description_width
)
        : _currentPath(std::move(current_path))
#if LOCKD_HAS_FORK
        , _noFork(true)
        , _noDrop(false)
        , _options("Forking options", width, description_width)
#endif
{
#if LOCKD_HAS_FORK
    namespace po = boost::program_options;
    std::filesystem::path run_dir = "/var/run/";
    if (std::filesystem::exists("/run/") && std::filesystem::is_directory("/run/"))
        run_dir = "/run/";
    const auto pid_file_location = run_dir / "lockd/lockd.pid";
    _options.add_options()(
            "pid-file,P",
            po::value<std::string>(&_pidFile)->default_value(pid_file_location),
            "pid file to write after forking")(
            "no-fork,n",
            po::bool_switch(&_noFork)->default_value(false),
            "don't fork into the background. only write a PID file if --pid-file is specified.")(
            "no-drop,d",
            po::bool_switch(&_noDrop)->default_value(false),
            "don't drop privileges (and don't require root to do so).");
    lkd::platform::posix::add_identity_options(_options, _user, _group, getpwnam("nobody"));
#else
    static_cast<void>(width);
    static_cast<void>(description_width);
#endif
}


// The non-forking branch uses this instance's working directory.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
std::filesystem::path lkd::platform::posix::startup::default_log_file() const
{
#if LOCKD_HAS_FORK
    return "/var/log/lockd/lockd.log";
#else
    return _currentPath / "lockd.log";
#endif
}


void lkd::platform::posix::startup::add_options(
        boost::program_options::options_description& visible,
        boost::program_options::options_description& /*hidden*/
) const
{
#if LOCKD_HAS_FORK
    visible.add(_options);
#else
    static_cast<void>(visible);
#endif
}


void lkd::platform::posix::startup::validate_options(
        const boost::program_options::variables_map& /*options*/,
        const std::filesystem::path& /*log_file*/
) const
{
}


std::optional<int> lkd::platform::posix::startup::
        prepare(const boost::program_options::variables_map& options,
                const std::string& listen_ip,
                const std::uint16_t port,
                const unsigned short /*threads*/,
                std::filesystem::path& log_file,
                const lkd::log_level_t /*log_level*/)
{
#if LOCKD_HAS_FORK
    using namespace lkd;
    const passwd* pwd = nullptr;
    const group* grp = nullptr;
    lkd::platform::posix::validate_identity_options(options, _noDrop);

    // If we're instructed not to drop privileges, don't do any of that.
    if (!_noDrop) {
        // Make sure we're root so we can write the PID file (and we'll drop privileges
        // later).
        if (0 != getuid()) {
            console_print(
                    lkd::cp_stderr,
                    lkd::ll_fatal,
                    "ERROR: lockd must be initially run as root. If you can't run as root, "
                    "use --no-drop/-d.\n"
            );
            return 2;
        }

        // Get the pwd entry from either a uid or user name, making sure it's valid.
        if (!is_plain_digits(_user))
            pwd = getpwnam(_user.c_str());
        else {
            uid_t uid = 0;
            const auto result = std::from_chars(_user.data(), _user.data() + _user.size(), uid);
            if (result.ec != std::errc() || result.ptr != _user.data() + _user.size())
                throw std::out_of_range("User ID is outside the platform's valid range.");
            pwd = getpwuid(uid);
        }
        if (!pwd) {
            console_print(
                    lkd::cp_stderr,
                    lkd::ll_fatal,
                    str(boost::format(
                                "ERROR: %1% is not a valid uid/user name on this system or "
                                "I could not figure out who that is (errno=%2%).\n"
                        ) %
                        _user % errno)
            );
            return 4;
        }

        // Resolve an omitted group from the selected user, including an explicit --user.
        if (options["group"].defaulted())
            _group = std::to_string(pwd->pw_gid);

        // Get the group entry from either a gid or group name, making sure it's valid.
        if (!is_plain_digits(_group))
            grp = getgrnam(_group.c_str());
        else {
            gid_t gid = 0;
            const auto result = std::from_chars(_group.data(), _group.data() + _group.size(), gid);
            if (result.ec != std::errc() || result.ptr != _group.data() + _group.size())
                throw std::out_of_range("Group ID is outside the platform's valid range.");
            grp = getgrgid(gid);
        }
        if (!grp) {
            console_print(
                    lkd::cp_stderr,
                    lkd::ll_fatal,
                    str(boost::format(
                                "ERROR: %1% is not a valid gid/group name on this system "
                                "or I could not figure out who that is (errno=%2%).\n"
                        ) %
                        _group % errno)
            );
            return 5;
        }
    }

    if (options["log-file"].defaulted() && _noDrop)
        log_file = _currentPath / "lockd.log";
    if (options["pid-file"].defaulted() && _noDrop)
        _pidFile = (_currentPath / "lockd.pid").string();
    log_file = std::filesystem::absolute(log_file);
    _pidFile = std::filesystem::absolute(_pidFile).string();

    // Create only the daemon's own directories while privileged. Explicit log paths
    // remain the administrator's responsibility, including directory traversal rights.
    if (!_noDrop) {
        if (options["log-file"].defaulted())
            lkd::platform::posix::prepare_runtime_directory(
                    log_file.parent_path(), pwd->pw_uid, grp->gr_gid
            );
        if (!_noFork || !options["pid-file"].defaulted())
            lkd::platform::posix::prepare_runtime_directory(
                    std::filesystem::absolute(_pidFile).parent_path(), pwd->pw_uid, grp->gr_gid
            );
    }

    if (!_noFork || !options["pid-file"].defaulted())
        posix_startup::report_stale_pid(_pidFile);

    if (!_noFork) {
        const auto pid = _process.start();
        if (pid != 0) {
            const int result = _process.wait();
            if (result == EXIT_SUCCESS)
                console_print(
                        lkd::cp_stdout,
                        lkd::ll_info,
                        str(boost::format(
                                    "started lockd on interface %1% port %2% "
                                    "(forked, background pid=%3%)\n"
                            ) %
                            (boost::asio::ip::make_address(listen_ip).is_v6()
                                     ? '[' + listen_ip + ']'
                                     : listen_ip) %
                            port % pid)
                );
            return result;
        }
    }

    // Supplementary groups must change before dropping root, followed by the primary GID
    // and UID. A failure at any step must stop startup; otherwise we could serve clients
    // with root's UID or supplementary group access despite --user/--group.
    if (!_noDrop) {
        if (0 != initgroups(pwd->pw_name, grp->gr_gid)) {
            console_print(
                    lkd::cp_stderr,
                    lkd::ll_fatal,
                    str(boost::format(
                                "ERROR: Unable to initialize groups for '%1%' "
                                "(errno=%2%)\n"
                        ) %
                        _user % errno)
            );
            return EXIT_FAILURE;
        }

        if (0 != setgid(grp->gr_gid)) {
            console_print(
                    lkd::cp_stderr,
                    lkd::ll_fatal,
                    str(boost::format(
                                "ERROR: Unable to drop gid to '%1%' "
                                "(gid=%2%, errno=%3%)\n"
                        ) %
                        _group % grp->gr_gid % errno)
            );
            return EXIT_FAILURE;
        }

        if (0 != setuid(pwd->pw_uid)) {
            console_print(
                    lkd::cp_stderr,
                    lkd::ll_fatal,
                    str(boost::format(
                                "ERROR: Unable to drop uid to '%1%' (uid=%2%, "
                                "errno=%3%)\n"
                        ) %
                        _user % pwd->pw_uid % errno)
            );
            return EXIT_FAILURE;
        }
    }

    // Validate as the account that will actually write the log. Do not attach a file
    // sink until this succeeds, so reporting a permission error cannot hit that sink.
    lkd::logging::validate_log_file(log_file, false);
    if (!_noFork || !options["pid-file"].defaulted()) {
        try {
            _process.write_pid(std::filesystem::absolute(_pidFile));
        }
        catch (const std::exception& error) {
            // Return through normal teardown. The startup pipe closes without ready(), so
            // a waiting parent reports failure and only our own PID file can be cleaned up.
            console_print(cp_stderr, ll_error, std::string("ERROR: ") + error.what() + "\n");
            return EXIT_FAILURE;
        }
    }
#else
    static_cast<void>(options);
    static_cast<void>(listen_ip);
    static_cast<void>(port);
    static_cast<void>(log_file);
#endif
    return std::nullopt;
}


// Keep the common platform interface; other platform branches use instance state.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
std::optional<int> lkd::platform::posix::startup::run_service(
        const boost::program_options::variables_map& /*options*/,
        const std::string& /*listen_ip*/,
        const std::uint16_t /*port*/,
        const unsigned short /*threads*/
) const
{
    return std::nullopt;
}


void lkd::platform::posix::startup::ready()
{
#if LOCKD_HAS_FORK
    _process.ready();
#endif
}

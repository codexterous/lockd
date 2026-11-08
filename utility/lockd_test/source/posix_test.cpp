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
 * File       : posix_test.cpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Test POSIX startup, signals, PID files, runtime directories, and identity changes.
 * Start Date : 10/24/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

// Claim the POSIX typedefs and wait macros before libc's other public headers define
// them conditionally. This keeps include-cleaner tied to their documented providers.
#include <sys/types.h>
#include <sys/wait.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/io/ios_state.hpp>
#include <boost/program_options/errors.hpp>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/program_options/variables_map.hpp>
#include <boost/system/system_error.hpp>
#include <boost/test/tools/context.hpp>
#include <boost/test/tools/interface.hpp> // IWYU pragma: keep
#include <boost/test/unit_test.hpp>       // IWYU pragma: keep
#include <boost/test/unit_test_log.hpp>
#include <boost/test/unit_test_suite.hpp>

#include <dirent.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h> // NOLINT(modernize-deprecated-headers): POSIX kill, outside <csignal>.
#include <stdlib.h> // NOLINT(modernize-deprecated-headers): POSIX mkdtemp, outside <cstdlib>.
#include <sys/stat.h>
#include <unistd.h>

#include "../../../include/logging.hpp"
#include "../../../include/platform/posix.hpp"
#include "../include/daemon_fixture.hpp"
#include "../include/launcher.hpp"
#include "../include/process.hpp"

namespace {

void with_runtime_parent(const std::function<void(const std::filesystem::path&, const int)>& test)
{
    // Use a private native POSIX directory, rather than a Windows-mounted checkout. Keep the
    // descriptor open across pathname replacements and remove only this test's temporary tree.
    auto pattern = (std::filesystem::temp_directory_path() / "lockd-runtime-XXXXXX").string();
    const char* const created = mkdtemp(pattern.data());
    BOOST_REQUIRE_MESSAGE(created != nullptr, "Create an isolated runtime-directory test tree");
    const std::filesystem::path root(created);
    try {
        std::filesystem::create_directory(root / "parent");
        const std::unique_ptr<DIR, int (*)(DIR*)> parent(
                opendir((root / "parent").c_str()), closedir
        );
        BOOST_REQUIRE_MESSAGE(parent, "Open the runtime parent directory");
        test(root, dirfd(parent.get()));
    }
    catch (...) {
        std::filesystem::remove_all(root);
        throw;
    }
    std::filesystem::remove_all(root);
}


BOOST_AUTO_TEST_SUITE(lifetimes)


BOOST_AUTO_TEST_CASE(usr1_reopens_logs_and_preserves_locks_in_both_rotation_modes)
{
    // Rename a live daemon's log, then signal it repeatedly, including a failed reopen.
    // Verify both rotation modes reopen without archiving, keep locks, and still honor SIGTERM.
    for (const unsigned int rotate_size : {0U, 1U}) {
        boost::asio::io_context service;
        boost::asio::ip::tcp::acceptor reservation(
                service, {boost::asio::ip::address_v4::loopback(), 0}
        );
        const auto endpoint = reservation.local_endpoint();
        reservation.close();

        // Exercise POSIX rename semantics on the system's temporary filesystem. A checkout
        // mounted from Windows can invalidate access to an open, renamed file under WSL.
        const auto directory =
                std::filesystem::temp_directory_path() /
                ("signal log " +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        BOOST_REQUIRE_MESSAGE(
                std::filesystem::create_directory(directory), "Use a fresh directory"
        );
        const auto log_file = directory / "lockd.log";
        const auto console_file = directory / "console.txt";
        if (rotate_size != 0) {
            std::ofstream(log_file.string() + ".0") << "old archive zero\n";
            std::ofstream(log_file.string() + ".1") << "old archive one\n";
        }
        ldt::process::child child(
                ldt::daemon_path().string(),
                ldt::process::args(
                        std::vector<std::string>{
                                "--no-fork",
                                "--no-drop",
                                "--interface",
                                "127.0.0.1",
                                "--port",
                                std::to_string(endpoint.port()),
                                "--threads",
                                "2",
                                "--log-file",
                                log_file.string(),
                                "--log-rotate-size",
                                std::to_string(rotate_size)
                        }
                ),
                (ldt::process::std_out & ldt::process::std_err) > console_file.string()
        );
        std::shared_ptr<ldt::protocol_connection> owner;
        ldt::poll_until(
                service,
                [&] {
                    try {
                        owner = std::make_shared<ldt::protocol_connection>(service, endpoint);
                        return true;
                    }
                    catch (const boost::system::system_error& error) {
                        if (error.code() != boost::asio::error::connection_refused)
                            throw;
                        return false;
                    }
                },
                "daemon startup for log reopening"
        );
        BOOST_REQUIRE_MESSAGE(owner->line() == "LOCKD", "The daemon has started its signal wait");
        owner->send("TRY_LOCK signal_probe\n");
        BOOST_REQUIRE_MESSAGE(owner->line() == "LOCK_ACQUIRED", "Hold a lock across signals");

        const auto read_file = [](const std::filesystem::path& path) {
            std::ifstream input(path);
            return std::string(
                    std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()
            );
        };
        for (unsigned int index = 0; index < 2; ++index) {
            const auto archive = directory / ("external." + std::to_string(index));
            const auto previous = read_file(log_file);
            std::filesystem::rename(log_file, archive);
            BOOST_REQUIRE_MESSAGE(kill(child.id(), SIGUSR1) == 0, "Signal only the test daemon");
            ldt::poll_until(
                    service,
                    [&] {
                        return read_file(log_file).find("Reopened log file") != std::string::npos;
                    },
                    "successful USR1 reopen"
            );
            BOOST_CHECK_MESSAGE(
                    read_file(archive) == previous, "The external archive stops growing"
            );
            BOOST_CHECK_MESSAGE(
                    !std::filesystem::exists(log_file.string() + ".2"),
                    "USR1 reopens without creating a built-in archive"
            );
            ldt::protocol_connection contender(service, endpoint);
            BOOST_REQUIRE_MESSAGE(contender.line() == "LOCKD", "The daemon still accepts clients");
            contender.send("TRY_LOCK signal_probe\n");
            BOOST_CHECK_MESSAGE(
                    contender.line() == "LOCK_UNAVAILABLE", "Reopening preserves the owner's lock"
            );
        }

        // Make the replacement path unusable. The old descriptor must remain available for
        // diagnostics, and a subsequent signal must recover after the path is repaired.
        const auto failed_archive = directory / "external.failed";
        std::filesystem::rename(log_file, failed_archive);
        std::filesystem::create_directory(log_file);
        BOOST_REQUIRE_MESSAGE(kill(child.id(), SIGUSR1) == 0, "Request the failed reopen");
        ldt::poll_until(
                service,
                [&] {
                    return read_file(failed_archive).find("Unable to reopen log file") !=
                           std::string::npos;
                },
                "failed reopen diagnostic"
        );

        std::filesystem::remove(log_file);
        BOOST_REQUIRE_MESSAGE(kill(child.id(), SIGUSR1) == 0, "Retry after repairing the path");
        ldt::poll_until(
                service,
                [&] { return read_file(log_file).find("Reopened log file") != std::string::npos; },
                "recovered USR1 reopen"
        );
        if (rotate_size != 0) {
            // Replace the active path with a full file while the backend still owns the old
            // inode. Older Boost versions rotate the replacement on the first record after
            // reopening; newer versions may wait for the next record. Both must preserve the
            // full payload and previous archives, then keep writing to the active path.
            constexpr auto rotation_bytes = 1024U * 1024U;
            const std::string full_marker = "size-full USR1 rotation payload\n";
            const auto before_full = directory / "external.before-full";
            const auto previous = read_file(log_file);
            std::filesystem::rename(log_file, before_full);
            {
                std::ofstream replacement(log_file);
                replacement << full_marker;
                BOOST_REQUIRE_MESSAGE(replacement.good(), "Create the size-full replacement");
            }
            std::filesystem::resize_file(log_file, rotation_bytes);
            BOOST_REQUIRE_MESSAGE(kill(child.id(), SIGUSR1) == 0, "Reopen before size rotation");
            ldt::poll_until(
                    service,
                    [&] {
                        return read_file(log_file).find("Reopened log file\n") != std::string::npos;
                    },
                    "reopen accepts the size-full replacement"
            );
            BOOST_CHECK_MESSAGE(
                    read_file(before_full) == previous, "Reopening stops writes to the old inode"
            );
            ldt::protocol_connection malformed(service, endpoint);
            BOOST_REQUIRE_MESSAGE(malformed.line() == "LOCKD", "Connect the logging probe");
            malformed.send(std::string(1024, 'x'));
            ldt::poll_until(
                    service,
                    [&] {
                        std::error_code error;
                        const auto size = std::filesystem::file_size(log_file, error);
                        return !error && size < rotation_bytes &&
                               read_file(log_file).find("<error> Command from client ") !=
                                       std::string::npos;
                    },
                    "automatic rotation after USR1"
            );
            BOOST_CHECK_MESSAGE(
                    read_file(log_file.string() + ".0") == "old archive zero\n" &&
                            read_file(log_file.string() + ".1") == "old archive one\n",
                    "Reopening preserves the automatic archive counter"
            );

            // Wait for completed log records above, rather than requiring the active path to
            // exist during the rename/recreate interval. Rotation must archive the full payload
            // exactly once, regardless of which of those records crossed Boost's threshold.
            const auto prefix = log_file.filename().string() + '.';
            unsigned int full_archives = 0;
            for (const auto& entry : std::filesystem::directory_iterator(directory)) {
                const auto name = entry.path().filename().string();
                if (name.compare(0, prefix.size(), prefix) == 0 && name.size() > prefix.size() &&
                    name.find_first_not_of("0123456789", prefix.size()) == std::string::npos) {
                    const auto contents = read_file(entry.path());
                    if (contents.find(full_marker) == 0) {
                        BOOST_CHECK_MESSAGE(
                                contents.size() >= rotation_bytes, "The full payload is retained"
                        );
                        ++full_archives;
                    }
                }
            }
            BOOST_CHECK_EQUAL(full_archives, 1);
        }
        owner->send("UNLOCK\n");
        BOOST_CHECK_MESSAGE(owner->line() == "LOCK_RELEASED", "The held lock remains usable");
        BOOST_REQUIRE_MESSAGE(kill(child.id(), SIGTERM) == 0, "Shutdown still works after USR1");
        ldt::poll_until(service, [&child] { return !child.running(); }, "shutdown after USR1");
        child.wait();
        BOOST_CHECK_MESSAGE(child.exit_code() == 0, "The daemon exits normally");
        std::filesystem::remove_all(directory);
    }
}


BOOST_AUTO_TEST_CASE(missing_default_account_requires_explicit_user_only_when_dropping)
{
    // Parse options without a default account, then with an explicit user. Verify that
    // privilege dropping requires an identity while --no-drop remains usable.
    namespace po = boost::program_options;
    std::string user, group;
    po::options_description options;
    lkd::platform::posix::add_identity_options(options, user, group, nullptr);
    po::variables_map values;
    po::store(po::command_line_parser(std::vector<std::string>()).options(options).run(), values);
    po::notify(values);
    BOOST_CHECK_MESSAGE(!values.count("user"), "No fabricated default account is supplied");
    BOOST_CHECK_THROW(
            lkd::platform::posix::validate_identity_options(values, false), po::required_option
    );
    BOOST_CHECK_NO_THROW(lkd::platform::posix::validate_identity_options(values, true));
    std::ostringstream help;
    help << options;
    BOOST_CHECK_MESSAGE(
            help.str().find("(=nobody)") == std::string::npos,
            "Help does not advertise a missing account"
    );

    values.clear();
    po::store(
            po::command_line_parser(std::vector<std::string>{"--user", "1234"})
                    .options(options)
                    .run(),
            values
    );
    po::notify(values);
    BOOST_CHECK_NO_THROW(lkd::platform::posix::validate_identity_options(values, false));
    BOOST_CHECK_MESSAGE(user == "1234", "An explicit identity is retained for account lookup");
}


BOOST_AUTO_TEST_CASE(pid_file_is_exclusive_and_removed_by_its_owner)
{
    // Claim a PID file and try a competing claim. Verify the recorded PID, exclusive
    // ownership, and removal only when the owning startup object is destroyed.
    const auto path = std::filesystem::current_path() / "startup_test.pid";
    BOOST_REQUIRE_MESSAGE(!std::filesystem::exists(path), "Test PID file must not already exist");
    {
        lkd::platform::posix::posix_startup owner;
        owner.write_pid(path);
        std::ifstream file(path);
        long pid = 0;
        file >> pid;
        BOOST_CHECK_MESSAGE(pid == getpid(), "The PID file contains the owning process ID");
        lkd::platform::posix::posix_startup contender;
        BOOST_CHECK_THROW(contender.write_pid(path), std::runtime_error);
        BOOST_CHECK_MESSAGE(std::filesystem::exists(path), "A contender cannot remove the file");
    }
    BOOST_CHECK_MESSAGE(!std::filesystem::exists(path), "Unwinding removes the owned PID file");
}


BOOST_AUTO_TEST_CASE(stale_pid_file_is_reclaimed_after_its_owner_exits)
{
    // Leave a PID file behind in an exited child, then reclaim it. Check that the
    // advisory notice leaves it untouched and successful reclamation reports both PIDs.
    const auto path = std::filesystem::current_path() / "stale_startup_test.pid";
    BOOST_REQUIRE_MESSAGE(!std::filesystem::exists(path), "Test PID path must be absent");
    const auto child = fork();
    BOOST_REQUIRE_MESSAGE(child >= 0, "Create an isolated PID-file owner");
    if (child == 0) {
        lkd::platform::posix::posix_startup owner;
        owner.write_pid(path);
        _exit(EXIT_SUCCESS); // Simulate exit without destructor cleanup.
    }
    int status = 0;
    BOOST_REQUIRE_MESSAGE(
            waitpid(child, &status, 0) == child && WIFEXITED(status) &&
                    WEXITSTATUS(status) == EXIT_SUCCESS,
            "Reap the owner before checking its PID"
    );
    BOOST_REQUIRE_MESSAGE(
            kill(child, 0) == -1 && errno == ESRCH, "The recorded process no longer exists"
    );
    {
        // std::cout writes into this stream through its rdbuf().
        // NOLINTNEXTLINE(misc-const-correctness)
        std::ostringstream notice;
        {
            const boost::io::basic_ios_rdbuf_saver<char> restore(std::cout, notice.rdbuf());
            lkd::platform::posix::posix_startup::report_stale_pid(path);
        }
        BOOST_CHECK_MESSAGE(
                notice.str().find(
                        "Found stale PID file '" + path.string() + "' (PID " +
                        std::to_string(child) + "); will attempt to reclaim it during startup."
                ) != std::string::npos,
                "The notice identifies the stale path, PID and intended attempt"
        );
        std::ifstream file(path);
        pid_t recorded = 0;
        file >> recorded;
        BOOST_CHECK_MESSAGE(recorded == child, "The advisory check does not change the PID file");
    }
    {
        lkd::platform::posix::posix_startup replacement;

        // std::cout writes into this stream through its rdbuf().
        // NOLINTNEXTLINE(misc-const-correctness)
        std::ostringstream confirmation;
        {
            const boost::io::basic_ios_rdbuf_saver<char> restore(std::cout, confirmation.rdbuf());
            replacement.write_pid(path);
        }
        BOOST_CHECK_MESSAGE(
                confirmation.str().find(
                        "\033[97mReclaimed stale PID file '" + path.string() + "' (previous PID " +
                        std::to_string(child) + ", current PID " + std::to_string(getpid()) +
                        ").\033[39m\n"
                ) == 0,
                "Successful rewriting produces an informational confirmation with both PIDs"
        );

        // std::cout writes into this stream through its rdbuf().
        // NOLINTNEXTLINE(misc-const-correctness)
        std::ostringstream notice;
        {
            const boost::io::basic_ios_rdbuf_saver<char> restore(std::cout, notice.rdbuf());
            lkd::platform::posix::posix_startup::report_stale_pid(path);
        }
        BOOST_CHECK_MESSAGE(notice.str().empty(), "An owned file is not announced as stale");
        std::ifstream file(path);
        pid_t pid = 0;
        file >> pid;
        BOOST_CHECK_MESSAGE(pid == getpid(), "Reclaimed file contains the new owner PID");
        lkd::platform::posix::posix_startup contender;
        BOOST_CHECK_THROW(contender.write_pid(path), std::runtime_error);
    }
    BOOST_CHECK_MESSAGE(!std::filesystem::exists(path), "New owner removes the reclaimed file");
}


BOOST_AUTO_TEST_CASE(ambiguous_pid_files_are_preserved_and_errors_identify_the_path)
{
    // Try malformed PID records, including non-ASCII bytes, and a live process ID. Each claim
    // must fail with the path in its diagnostic and preserve the original file contents.
    const auto path = std::filesystem::current_path() / "invalid_startup_test.pid";
    BOOST_REQUIRE_MESSAGE(!std::filesystem::exists(path), "Test PID path must be absent");
    for (const auto& contents : std::vector<std::string>{
                 "",
                 "0\n",
                 "-1\n",
                 "123junk\n",
                 "123\xff\n",
                 "999999999999999999999999\n",
                 std::to_string(getpid()) + "\n"
         }) {
        {
            std::ofstream file(path);
            file << contents;
        }
        {
            lkd::platform::posix::posix_startup contender;
            BOOST_CHECK_EXCEPTION(
                    contender.write_pid(path),
                    std::exception,
                    [&path](const std::exception& error) {
                        return std::string(error.what()).find(path.string()) != std::string::npos;
                    }
            );
        }
        std::ifstream file(path);
        const std::string actual(
                (std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>()
        );
        BOOST_CHECK_MESSAGE(actual == contents, "Rejected files remain unchanged");
    }
    std::filesystem::remove(path);
}


BOOST_AUTO_TEST_CASE(pid_file_symlinks_and_replacements_are_not_removed)
{
    // Try a symlink claim, then replace an owned file before cleanup. Verify that
    // neither rejection nor destruction removes a file belonging to someone else.
    const auto path = std::filesystem::current_path() / "replaced_startup_test.pid";
    const auto target = std::filesystem::current_path() / "replacement_target.pid";
    BOOST_REQUIRE_MESSAGE(
            !std::filesystem::exists(path) && !std::filesystem::exists(target),
            "Test paths must be absent"
    );
    {
        std::ofstream file(target);
        file << "preserve this file\n";
    }
    std::filesystem::create_symlink(target, path);
    {
        lkd::platform::posix::posix_startup contender;
        BOOST_CHECK_THROW(contender.write_pid(path), boost::system::system_error);
    }
    BOOST_CHECK_MESSAGE(std::filesystem::is_symlink(path), "Rejected symlink is preserved");
    std::filesystem::remove(path);
    {
        lkd::platform::posix::posix_startup owner;
        owner.write_pid(path);
        std::filesystem::rename(target, path);
    }
    std::ifstream file(path);
    std::string contents;
    std::getline(file, contents);
    BOOST_CHECK_MESSAGE(contents == "preserve this file", "Cleanup preserves a replacement inode");
    file.close();
    std::filesystem::remove(path);
}


BOOST_AUTO_TEST_CASE(pid_claim_failure_exits_cleanly_with_error_severity)
{
    // Launch the daemon against an occupied PID file using paths containing spaces.
    // Check a prompt normal failure, error severity, option routing, and owner preservation.
    const auto executable = ldt::daemon_path();
    const auto path = std::filesystem::current_path() / "contended startup test.pid";
    const auto log_path = std::filesystem::current_path() / "contended startup test.log";
    BOOST_REQUIRE_MESSAGE(!std::filesystem::exists(path), "Test PID file must be absent");
    lkd::platform::posix::posix_startup owner;
    owner.write_pid(path);
    ldt::process::ipstream errors;

    // Both short options must select their own file, including paths containing spaces.
    ldt::process::child child(
            executable.string(),
            "--no-drop",
            "-P",
            path.string(),
            "-f",
            log_path.string(),
            ldt::process::std_out > ldt::process::null,
            ldt::process::std_err > errors
    );
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (child.running() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool exited = !child.running();
    if (!exited)
        child.terminate();
    child.wait();
    BOOST_REQUIRE_MESSAGE(
            exited, "The parent returns promptly when its child cannot claim the file"
    );
    const std::string diagnostic(
            (std::istreambuf_iterator<char>(errors)), std::istreambuf_iterator<char>()
    );
    BOOST_CHECK_MESSAGE(
            child.exit_code() == EXIT_FAILURE, "Startup returns a normal failure status"
    );
    BOOST_CHECK_MESSAGE(
            diagnostic == "\033[31mERROR: PID file '" + path.string() +
                                  "' is already in use (is lockd still running?)\033[39m\n",
            "Contention explains the likely running daemon and path using ll_error colors"
    );
    BOOST_CHECK_MESSAGE(
            diagnostic.find("\033[41m") == std::string::npos &&
                    diagnostic.find("terminate called") == std::string::npos,
            "The failure neither uses fatal colors nor aborts"
    );
    std::ifstream file(path);
    pid_t recorded = 0;
    file >> recorded;
    BOOST_CHECK_MESSAGE(
            recorded == getpid(), "Failed startup preserves the competing owner's file"
    );
    BOOST_CHECK_MESSAGE(std::filesystem::exists(log_path), "The -f option selects the log file");
    std::filesystem::remove(log_path);
}


BOOST_AUTO_TEST_CASE(startup_parent_distinguishes_ready_from_early_exit)
{
    // Fork children that either signal readiness or exit first. The parent must report
    // success only after the explicit readiness notification.
    for (const bool ready : {false, true}) {
        lkd::platform::posix::posix_startup startup;
        const auto pid = startup.start();
        if (pid == 0) {
            if (ready)
                startup.ready();
            _exit(ready ? EXIT_SUCCESS : EXIT_FAILURE);
        }
        BOOST_CHECK_MESSAGE(
                startup.wait() == (ready ? EXIT_SUCCESS : EXIT_FAILURE),
                "Only an explicit ready message permits successful startup"
        );
        if (ready) {
            int status = 0;
            BOOST_CHECK_MESSAGE(waitpid(pid, &status, 0) == pid, "Reap the successful test child");
        }
    }
}


BOOST_AUTO_TEST_CASE(runtime_directory_preparation_preserves_existing_ownership)
{
    // Prepare a new runtime directory, then request a different identity for it.
    // Verify that preparing an existing directory does not change its ownership.
    const auto directory = std::filesystem::current_path() / "startup_directory_test";
    BOOST_REQUIRE_MESSAGE(!std::filesystem::exists(directory), "Test directory must be absent");
    lkd::platform::posix::prepare_runtime_directory(directory, getuid(), getgid());
    struct stat before{};
    BOOST_REQUIRE_MESSAGE(stat(directory.c_str(), &before) == 0, "Created directory is accessible");
    // Existing paths must not be chowned, even when another identity is requested.
    lkd::platform::posix::prepare_runtime_directory(directory, getuid() + 1, getgid() + 1);
    struct stat after{};
    BOOST_REQUIRE_MESSAGE(
            stat(directory.c_str(), &after) == 0, "Existing directory remains accessible"
    );
    BOOST_CHECK_MESSAGE(
            before.st_uid == after.st_uid && before.st_gid == after.st_gid,
            "Existing directory ownership is preserved"
    );
    std::filesystem::remove(directory);
}


BOOST_AUTO_TEST_CASE(runtime_directory_creation_preserves_path_and_mode_semantics)
{
    // Create directories through relative paths, a trailing separator, and a parent symlink.
    // Verify ownership and umask behavior match ordinary creation, and existing root paths work.
    with_runtime_parent([](const auto& root, const int /*parent*/) {
        std::filesystem::create_directory_symlink(root / "parent", root / "alias");
        std::filesystem::create_directory(root / "parent" / "nested");
        std::filesystem::create_directory_symlink(root / "parent" / "nested", root / "deeper");
        std::filesystem::create_directory(root / "parent" / "reference");
        struct stat reference{};
        BOOST_REQUIRE_EQUAL(stat((root / "parent" / "reference").c_str(), &reference), 0);
        for (const auto& path :
             {std::filesystem::relative(root / "parent" / "relative"),
              root / "parent" / "trailing" / "",
              root / "alias" / "linked",
              root / "deeper" / ".." / "dotdot"}) {
            lkd::platform::posix::prepare_runtime_directory(path, getuid(), getgid());
            struct stat created{};
            BOOST_REQUIRE_EQUAL(stat(path.c_str(), &created), 0);
            BOOST_CHECK_MESSAGE(S_ISDIR(created.st_mode), "Created object is a directory");
            BOOST_CHECK_EQUAL(created.st_uid, getuid());
            BOOST_CHECK_EQUAL(created.st_gid, getgid());
            BOOST_CHECK_EQUAL(created.st_mode & 0777, reference.st_mode & 0777);
        }
        BOOST_CHECK_NO_THROW(
                lkd::platform::posix::prepare_runtime_directory(
                        root.root_path(), getuid(), getgid()
                )
        );
    });
}


BOOST_AUTO_TEST_CASE(runtime_directory_existing_links_preserve_target_ownership)
{
    // An administrator's existing directory symlink must keep working without chowning its
    // target, even if another identity is requested. Files and dangling links must still fail.
    with_runtime_parent([](const auto& root, const int /*parent*/) {
        const auto target = root / "target";
        std::filesystem::create_directory(target);
        struct stat before{};
        BOOST_REQUIRE_EQUAL(stat(target.c_str(), &before), 0);
        const auto alias = root / "parent" / "alias";
        std::filesystem::create_directory_symlink(target, alias);
        BOOST_CHECK_NO_THROW(
                lkd::platform::posix::prepare_runtime_directory(alias, getuid() + 1, getgid() + 1)
        );
        struct stat after{};
        BOOST_REQUIRE_EQUAL(stat(target.c_str(), &after), 0);
        BOOST_CHECK_EQUAL(before.st_uid, after.st_uid);
        BOOST_CHECK_EQUAL(before.st_gid, after.st_gid);
        BOOST_CHECK_MESSAGE(std::filesystem::is_symlink(alias), "Existing link is preserved");

        const auto file = root / "parent" / "file";
        std::ofstream(file) << "preserve this file\n";
        const auto dangling = root / "parent" / "dangling";
        std::filesystem::create_directory_symlink(root / "missing", dangling);
        for (const auto& path : {file, dangling, root / "missing" / "child", file / "child"})
            BOOST_CHECK_THROW(
                    lkd::platform::posix::prepare_runtime_directory(path, getuid(), getgid()),
                    boost::system::system_error
            );
        BOOST_CHECK_MESSAGE(std::filesystem::is_regular_file(file), "Existing file is preserved");
        BOOST_CHECK_MESSAGE(std::filesystem::is_symlink(dangling), "Dangling link is preserved");
    });
}


BOOST_AUTO_TEST_CASE(runtime_directory_rejects_leaf_replacements_before_chown)
{
    // Replace a just-created leaf before the production ownership stage. Symlinks to files or
    // directories and regular files must be rejected before fchown, without changing the target.
    with_runtime_parent([](const auto& root, const int parent) {
        for (const int replacement : {0, 1, 2}) {
            BOOST_TEST_CONTEXT("replacement=" << replacement)
            {
                const auto target = root / "target";
                if (replacement == 0)
                    std::filesystem::create_directory(target);
                else
                    std::ofstream(target) << "preserve this target\n";
                struct stat before{};
                BOOST_REQUIRE_EQUAL(stat(target.c_str(), &before), 0);
                BOOST_REQUIRE_EQUAL(mkdirat(parent, "runtime", 0700), 0);
                const auto leaf = root / "parent" / "runtime";
                std::filesystem::remove(leaf);
                if (replacement < 2)
                    std::filesystem::create_symlink(target, leaf);
                else
                    std::ofstream(leaf) << "replacement file\n";
                try {
                    lkd::platform::posix::detail::assign_runtime_directory_ownership(
                            parent, "runtime", getuid() + 1, getgid() + 1
                    );
                    BOOST_ERROR("A replaced runtime directory must not receive ownership");
                }
                catch (const boost::system::system_error& error) {
                    BOOST_CHECK_MESSAGE(
                            error.code().value() == ELOOP || error.code().value() == EMLINK ||
                                    error.code().value() == ENOTDIR,
                            "Reject replacement during open, before ownership: " << error.what()
                    );
                }
                struct stat after{};
                BOOST_REQUIRE_EQUAL(stat(target.c_str(), &after), 0);
                BOOST_CHECK_EQUAL(before.st_uid, after.st_uid);
                BOOST_CHECK_EQUAL(before.st_gid, after.st_gid);
                std::filesystem::remove(leaf);
                std::filesystem::remove(target);
            }
        }
    });
}


BOOST_AUTO_TEST_CASE(runtime_directory_parent_replacements_cannot_redirect_ownership)
{
    // Rename the pinned parent before or after mkdirat, then replace its path with a symlink.
    // Creation and ownership must stay in the original parent, without opening the decoy file.
    for (const bool replace_before_creation : {false, true}) {
        with_runtime_parent([replace_before_creation](const auto& root, const int parent) {
            std::filesystem::create_directory(root / "decoy");
            std::ofstream(root / "decoy" / "runtime") << "preserve this decoy\n";
            if (!replace_before_creation)
                BOOST_REQUIRE_EQUAL(mkdirat(parent, "runtime", 0700), 0);
            std::filesystem::rename(root / "parent", root / "moved");
            std::filesystem::create_directory_symlink(root / "decoy", root / "parent");
            if (replace_before_creation)
                BOOST_REQUIRE_EQUAL(mkdirat(parent, "runtime", 0700), 0);
            BOOST_CHECK_NO_THROW(
                    lkd::platform::posix::detail::assign_runtime_directory_ownership(
                            parent, "runtime", getuid(), getgid()
                    )
            );
            BOOST_CHECK_MESSAGE(
                    std::filesystem::is_directory(root / "moved" / "runtime"),
                    "The pinned parent contains the runtime directory"
            );
            BOOST_CHECK_MESSAGE(
                    std::filesystem::is_regular_file(root / "decoy" / "runtime"),
                    "The replacement parent remains untouched"
            );
        });
    }
}


BOOST_AUTO_TEST_CASE(runtime_directory_preparation_closes_descriptors_on_success_and_failure)
{
    // Check the lowest available descriptor after successful preparation and failed open/chown.
    // Repeating these startup paths must not leak either the parent or the leaf descriptor.
    with_runtime_parent([](const auto& root, const int parent) {
        const int available = dup(parent);
        BOOST_REQUIRE_MESSAGE(available >= 0, "Find the next unused descriptor");
        close(available);
        for (const bool fail_chown : {false, true}) {
            if (fail_chown && geteuid() == 0)
                continue;
            const auto leaf = root / "parent" / "runtime";
            if (fail_chown)
                BOOST_CHECK_EXCEPTION(
                        lkd::platform::posix::prepare_runtime_directory(
                                leaf, getuid() + 1, getgid()
                        ),
                        boost::system::system_error,
                        [](const boost::system::system_error& error) {
                            return error.code().value() == EPERM;
                        }
                );
            else
                BOOST_CHECK_NO_THROW(
                        lkd::platform::posix::prepare_runtime_directory(leaf, getuid(), getgid())
                );
            const int next = dup(parent);
            BOOST_REQUIRE_MESSAGE(next >= 0, "Recheck the next unused descriptor");
            BOOST_CHECK_EQUAL(next, available);
            close(next);
            std::filesystem::remove(leaf);
        }

        std::ofstream(root / "parent" / "file") << "not a directory\n";
        for (const auto& path : {root / "missing" / "child", root / "parent" / "file"}) {
            BOOST_CHECK_THROW(
                    lkd::platform::posix::prepare_runtime_directory(path, getuid(), getgid()),
                    boost::system::system_error
            );
            const int next = dup(parent);
            BOOST_REQUIRE_MESSAGE(next >= 0, "Recheck descriptors after a failed open or mkdir");
            BOOST_CHECK_EQUAL(next, available);
            close(next);
        }
    });
}


BOOST_AUTO_TEST_CASE(runtime_directory_privileged_creation_assigns_the_service_account)
{
    // In a separately requested root run, assign a new directory to nobody and verify its
    // actual ownership. A second preparation must not change it back to the privileged caller.
    if (geteuid() != 0) {
        BOOST_TEST_MESSAGE("Ownership transfer requires a separately requested root run");
        return;
    }
    const auto* const account = getpwnam("nobody");
    BOOST_REQUIRE_MESSAGE(account != nullptr, "Ownership transfer needs an unprivileged account");
    const auto uid = account->pw_uid;
    const auto gid = account->pw_gid;
    with_runtime_parent([uid, gid](const auto& root, const int /*parent*/) {
        const auto path = root / "parent" / "runtime";
        lkd::platform::posix::prepare_runtime_directory(path, uid, gid);
        lkd::platform::posix::prepare_runtime_directory(path, getuid(), getgid());
        struct stat created{};
        BOOST_REQUIRE_EQUAL(stat(path.c_str(), &created), 0);
        BOOST_CHECK_EQUAL(created.st_uid, uid);
        BOOST_CHECK_EQUAL(created.st_gid, gid);
    });
}


BOOST_AUTO_TEST_CASE(background_startup_reports_bad_log_without_abort_or_pid_file)
{
    // Start a background daemon with a directory as its log filename. Verify prompt
    // failure, a separate error line, and no abort or leftover PID file.
    const auto executable = ldt::daemon_path();
    const auto directory = std::filesystem::current_path();
    const auto pid_path = directory / "bad_log_startup.pid";
    const auto output_path = directory / "bad_log_startup.txt";
    BOOST_REQUIRE_MESSAGE(!std::filesystem::exists(pid_path), "Test PID must not already exist");
    ldt::process::child child(
            executable.string(),
            "--no-drop",
            "--log-file",
            directory.string(),
            "--pid-file",
            pid_path.string(),
            (ldt::process::std_out & ldt::process::std_err) > output_path.string()
    );
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (child.running() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool exited = !child.running();
    if (!exited)
        child.terminate();
    child.wait();
    BOOST_REQUIRE_MESSAGE(exited, "The parent returns promptly when its child cannot open the log");
    BOOST_CHECK_MESSAGE(child.exit_code() == EXIT_FAILURE, "Startup reports failure, not success");
    std::ifstream output_file(output_path);
    const std::string output(
            (std::istreambuf_iterator<char>(output_file)), std::istreambuf_iterator<char>()
    );
    BOOST_CHECK_MESSAGE(
            output.find("\n\033[31minvalid program options: ") != std::string::npos,
            "Post-banner option errors start on a new line with error colors"
    );
    BOOST_CHECK_MESSAGE(
            output.find("terminate called") == std::string::npos,
            "Reporting the failure does not throw again"
    );
    BOOST_CHECK_MESSAGE(!std::filesystem::exists(pid_path), "Failed startup leaves no PID file");
    output_file.close();
    std::filesystem::remove(output_path);
}

#if defined(__linux__)
BOOST_AUTO_TEST_CASE(privileged_startup_drops_identity_and_cleans_pid_on_shutdown)
{
    // When explicitly run as root, launch a daemon as nobody and inspect its kernel
    // identity. Verify readiness, log creation, and PID-file cleanup after SIGTERM.
    if (geteuid() != 0) {
        BOOST_TEST_MESSAGE("Privilege-drop integration requires a separately requested root run");
        return;
    }
    const auto* const account = getpwnam("nobody");
    BOOST_REQUIRE_MESSAGE(
            account != nullptr, "The privilege-drop test needs an unprivileged account"
    );
    const auto uid = account->pw_uid;
    const auto gid = account->pw_gid;
    const auto directory = std::filesystem::current_path() / "privilege_startup_test";
    lkd::platform::posix::prepare_runtime_directory(directory, uid, gid);
    const auto pid_path = directory / "lockd.pid";
    const auto log_path = directory / "lockd.log";
    const auto executable = ldt::daemon_path();
    ldt::process::child parent(
            executable.string(),
            "--user",
            std::to_string(uid),
            "--log-file",
            log_path.string(),
            "--pid-file",
            pid_path.string(),
            "--port",
            "0",
            "--threads",
            "1",
            ldt::process::std_out > ldt::process::null
    );
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (parent.running() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool exited = !parent.running();
    if (!exited)
        parent.terminate();
    parent.wait();
    BOOST_REQUIRE_MESSAGE(
            exited && parent.exit_code() == 0,
            "The parent reports success after the unprivileged daemon is ready"
    );
    std::ifstream pid_file(pid_path);
    pid_t pid = 0;
    pid_file >> pid;
    BOOST_REQUIRE_MESSAGE(pid > 1, "Read the isolated daemon's PID");

    // Read the kernel identity, rather than file ownership (DrvFS may synthesize ownership).
    std::ifstream status_file("/proc/" + std::to_string(pid) + "/status");
    std::string line;
    bool correct_uid = false, correct_gid = false;
    while (std::getline(status_file, line)) {
        std::istringstream fields(line);
        std::string name;
        unsigned long real = 0, effective = 0;
        fields >> name >> real >> effective;
        if (name == "Uid:")
            correct_uid = real == uid && effective == uid;
        if (name == "Gid:")
            correct_gid = real == gid && effective == gid;
    }
    const int stopped = kill(pid, SIGTERM);
    BOOST_CHECK_MESSAGE(
            correct_uid && correct_gid, "The daemon uses the selected UID and its primary GID"
    );
    BOOST_CHECK_MESSAGE(stopped == 0, "Stop only the daemon launched by this test");
    const auto shutdown_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::filesystem::exists(pid_path) &&
           std::chrono::steady_clock::now() < shutdown_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    BOOST_CHECK_MESSAGE(!std::filesystem::exists(pid_path), "Shutdown removes the child PID file");
    BOOST_CHECK_MESSAGE(std::filesystem::exists(log_path), "The unprivileged daemon opens its log");
    std::filesystem::remove(log_path);
    std::filesystem::remove(directory);
}
#endif

#if defined(__linux__)
BOOST_AUTO_TEST_CASE(failed_log_sink_does_not_throw_from_error_reporting)
{
    // Break a real file sink in an isolated child and report a startup error through it.
    // Verify the sink is disabled without recursive exceptions, termination, or a hang.
    const auto output_path = std::filesystem::current_path() / "failed_sink_test.txt";
    // Exercise the real logger in a separate process so its disabled-sink state cannot leak
    // into other tests. Break the destination after setup, when the collector has created it.
    const auto pid = fork();
    BOOST_REQUIRE_MESSAGE(pid >= 0, "Fork the isolated logging failure probe");
    if (pid == 0) {
        const int output = open(output_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (output < 0 || dup2(output, STDERR_FILENO) < 0)
            _exit(2);
        close(output);
        try {
            const auto directory = std::filesystem::current_path() /
                                   ("failed log sink " + std::to_string(getpid()));
            BOOST_REQUIRE_MESSAGE(
                    std::filesystem::create_directory(directory), "Use a fresh sink directory"
            );
            const auto blocked = directory / "blocked";
            lkd::logging::setup_logging(blocked / "lockd.log", lkd::ll_info);

            // A regular file cannot serve as the log's parent directory. The first write
            // must fail even though collector initialization succeeded before this change.
            std::filesystem::remove(blocked);
            std::ofstream(blocked) << "not a directory\n";
            lkd::console_print(lkd::cp_stderr, lkd::ll_fatal, "ERROR: startup failed\n");
            std::filesystem::remove_all(directory);
        }
        catch (...) {
            _exit(3);
        }
        _exit(0);
    }
    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    pid_t result = 0;
    while ((result = waitpid(pid, &status, WNOHANG)) == 0 &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (result == 0) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    }
    BOOST_REQUIRE_MESSAGE(result == pid, "Logging failure does not hang exception reporting");
    BOOST_CHECK_MESSAGE(
            WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "Reporting through a failed sink does not throw or terminate"
    );
    std::ifstream file(output_path);
    const std::string output(
            (std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>()
    );
    BOOST_CHECK_MESSAGE(
            output.find("Disabling failed logging sink") != std::string::npos,
            "The failed sink is reported through the console: " << output
    );
    BOOST_CHECK_MESSAGE(
            output.find("terminate called") == std::string::npos,
            "Error reporting does not recursively enter the broken sink"
    );
    file.close();
    std::filesystem::remove(output_path);
}
#endif

BOOST_AUTO_TEST_SUITE_END()

} // namespace

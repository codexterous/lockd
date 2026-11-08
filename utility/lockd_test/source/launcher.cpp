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
 * File       : launcher.cpp
 * Author     : Brett Gmoser
 * Part Of    : lockd_test, a unit test suite for lockd and liblockd.
 * Purpose    : Launch a test daemon and report startup, termination, and exit status.
 * Start Date : 08/03/2023
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#if LOCKD_PLATFORM_POSIX
# include <cerrno>

#endif

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib> // IWYU pragma: keep
#include <exception>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include <boost/algorithm/string/predicate.hpp>

// The public Format header supplies the template definitions used by free_funcs.hpp.
#include <boost/format.hpp> // IWYU pragma: keep
#include <boost/format/free_funcs.hpp>
#include <boost/system/error_code.hpp> // IWYU pragma: keep
#include <boost/system/system_error.hpp>

#ifdef WIN32
# include <boost/winapi/get_last_error.hpp>
# include <boost/winapi/process.hpp>

#endif

#if LOCKD_PLATFORM_POSIX
# include <signal.h> // NOLINT(modernize-deprecated-headers): POSIX kill, outside <csignal>.

#endif

#include "../include/launcher.hpp"
#include "../include/lockd_test.hpp"

// Selects the supported Process v1 API across Boost versions.
#include "../include/process.hpp" // IWYU pragma: keep

#ifdef WIN32
# include "../../../include/platform/win32/service_elevation.hpp"

#endif


std::filesystem::path ldt::daemon_path(const std::string& test_executable)
{
#ifdef WIN32
    static_cast<void>(test_executable);

    // A bare argv[0] need not be on PATH. Use the loaded module's location so the
    // extracted tests find their sibling daemon even when launched by name.
    const auto executable =
            std::filesystem::path(lkd::platform::win32::service_elevation::executable_path());
#else
    // Help resolves the daemon before Boost.Test has initialized its argument storage.
    auto executable = std::filesystem::path(
            test_executable.empty() ? ldt::master_test_suite_argv()[0] : test_executable
    );
    if (!executable.has_parent_path())
        executable = ldt::process::search_path(executable.string()).string();
#endif

    // The ports work directory can remain after installation. Its daemon belongs only to
    // tests run from that build tree, not to the installed copy of the test executable.
    std::error_code error;
    if (!executable.empty() &&
        std::filesystem::equivalent(
                std::filesystem::absolute(executable).parent_path(), LOCKD_TEST_OUTPUT_DIR, error
        ))
        return std::filesystem::path(LOCKD_LOCKD_OUTPUT_DIR) / LOCKD_LOCKD_OUTPUT_NAME;

#ifdef WIN32
    // The portable Windows archive keeps the daemon beside the tests. Resolve it from the
    // executable, not the working directory, so an extracted archive can run from any shell.
    if (!executable.empty()) {
        auto sibling =
                std::filesystem::absolute(executable).parent_path() / LOCKD_LOCKD_OUTPUT_NAME;
        if (std::filesystem::is_regular_file(sibling, error))
            return sibling;
    }
#endif

    return LOCKD_INSTALLED_DAEMON_PATH;
}


ldt::launcher::launcher(
        std::string lockd_path,
        std::string listen_interface,
        const std::uint16_t port,
        const unsigned short threads
)
        : _interface(std::move(listen_interface))
        , _lockdPath(std::move(lockd_path))
        , _port(port)
        , _threads(threads)
        , _runStatus(ldt::rs_not_successful)
{
}


bool ldt::launcher::launch()
{
    namespace bp = ldt::process;

    // Each launch has its own one-shot startup notification, including restarts after join().
    _runningPromise = std::promise<void>();
    auto running = _runningPromise.get_future();
    _terminateRequested = false;
    _launchError = nullptr;
    run_status(rs_not_successful);
    _launchThread = std::make_shared<std::thread>([this] {
        bool startup_notified = false;
        try {
            bp::ipstream out;

            // The launch options include --emit-running, which causes lockd to emit
            // "lockd is running" when the server is fully operational. That message uses
            // std::clog (stderr). Keep stdout out of this pipe because its trailing color
            // resets would precede the startup message and break the prefix check. Redirect
            // each descriptor once; sharing one pipe in separate POSIX redirects fails dup2.
            this->_childProcess = std::make_shared<bp::child>(
                    str(boost::format(
#if LOCKD_HAS_FORK
                                "%1% -l info -d -n --emit-running -i %2% -p %3% -t %4%"
#else
                                "\"%1%\" -l info --emit-running -i %2% -p %3% -t %4%"
#endif
                        ) %
                        this->_lockdPath % this->_interface % this->_port % this->_threads),
                    bp::std_out > bp::null,
                    bp::std_err > out
            );

            std::string line;
            while (_childProcess->running() && std::getline(out, line) && !line.empty()) {
                // If the output matches what --emit-running emits, that means we've
                // successfully launched the lockd server.
                if (boost::algorithm::starts_with(line, "lockd is running")) {
                    this->run_status(rs_successful);

                    // Alert the main thread that it can continue.
                    this->_runningPromise.set_value();
                    startup_notified = true;

                    // And finally, await the termination of lockd. The thread exits once it
                    // has.
                    this->_waitForExit();
                    return;
                }
            }

            // Reap failed startups before publishing their result. Otherwise exit_code()
            // can still report a running child and the fixture can retain a joinable thread.
            _childProcess->wait();

            // Startup failure must also release the thread waiting in launch().
            if (run_status() != rs_successful)
                this->_runningPromise.set_value();
        }
        catch (...) {
            _launchError = std::current_exception();
            if (!startup_notified)
                _runningPromise.set_exception(_launchError);
        }
    });

    try {
        running.get();
    }
    catch (...) {
        join();
        throw;
    }

    if (run_status() != rs_successful) {
        join();
        std::cout << "ERROR launching lockd (" << _childProcess->exit_code() << ")!" << std::endl;

        // Keep the requested endpoint on failure. Silently choosing another port would leave
        // the clients targeting a different listener and invalidate the test's configuration.
        ldt::throw_framework_setup_error("lockd failed to launch!");
    }
    return true;
}


void ldt::launcher::terminate()
{
    // Only the launch thread accesses the child. POSIX terminate() also reaps it, so calling
    // it concurrently with wait() would leave one of the two waiters without a child.
    _terminateRequested = true;
}


void ldt::launcher::_waitForExit()
{
    while (_childProcess->running()) {
        if (_terminateRequested) {
#if LOCKD_PLATFORM_POSIX
            // Send the signal without reaping: wait() must collect the actual exit status.
            if (::kill(_childProcess->id(), SIGKILL) == -1 && errno != ESRCH)
                throw boost::system::system_error(errno, boost::system::generic_category());
#else
            // Process v1 marks terminate() as already exited, so its following wait() can
            // return before Windows has released the listening socket. Leave waiting to Boost.
            if (!boost::winapi::TerminateProcess(_childProcess->native_handle(), EXIT_FAILURE))
                throw boost::system::system_error(
                        static_cast<int>(boost::winapi::GetLastError()),
                        boost::system::system_category()
                );
#endif
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    _childProcess->wait();
}


void ldt::launcher::join()
{
    if (_launchThread && _launchThread->joinable())
        _launchThread->join();
    if (_launchError) {
        const auto error = std::exchange(_launchError, nullptr);
        std::rethrow_exception(error);
    }
}


int ldt::launcher::exit_code() const
{
    assert(_childProcess.get());
    return _childProcess->exit_code();
}


void ldt::launcher::run_status(const ldt::run_status_t run_status)
{
    const std::lock_guard<std::mutex> lock(_mutex);
    _runStatus = run_status;
}


ldt::run_status_t ldt::launcher::run_status() const
{
    const std::lock_guard<std::mutex> lock(_mutex);
    return _runStatus;
}

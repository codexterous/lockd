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
 * File       : launcher.hpp
 * Author     : Brett Gmoser
 * Part Of    : lockd_test, a unit test suite for lockd and liblockd.
 * Purpose    : Launch a test daemon and report startup, termination, and exit status.
 * Start Date : 03/26/2012
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#if !defined(LOCKD_LOCKD_TEST_LAUNCHER_HPP_INCLUDED_)
# define LOCKD_LOCKD_TEST_LAUNCHER_HPP_INCLUDED_

# if defined(_MSC_VER) && (_MSC_VER >= 1020)
#  pragma once
# endif

# include <atomic>
# include <cstdint>
# include <exception>
# include <filesystem>
# include <future>
# include <memory>
# include <mutex>
# include <string>
# include <thread>

# include "../include/process.hpp"


namespace ldt {

[[nodiscard]] std::filesystem::path daemon_path(const std::string& test_executable = "");

typedef enum
{
    rs_successful,
    rs_not_successful
} run_status_t;


class launcher
{
    /* construction and destruction */
public:
    launcher(
            std::string lockd_path,
            std::string listen_interface,
            std::uint16_t port,
            unsigned short threads
    );
    virtual ~launcher() = default;

    /* public methods */
public:
    bool launch();
    void terminate();
    void join();

    /* public methods, observers */
public:
    [[nodiscard]] const std::string& interface() const { return _interface; }
    [[nodiscard]] const std::string& lockd_path() const { return _lockdPath; }
    [[nodiscard]] std::uint16_t port() const { return _port; }
    [[nodiscard]] unsigned short threads() const { return _threads; }
    [[nodiscard]] run_status_t run_status() const;
    // Read the child's termination status only after join() has completed.
    [[nodiscard]] int exit_code() const;

    /* private methods */
private:
    void run_status(run_status_t run_status);
    void _waitForExit();

    /* private data members */
private:
    std::string _interface;
    std::string _lockdPath;
    std::uint16_t _port;
    unsigned short _threads;
    std::shared_ptr<ldt::launcher> _launcher;
    std::promise<void> _runningPromise;
    std::atomic<bool> _terminateRequested{false};
    std::exception_ptr _launchError;
    std::shared_ptr<ldt::process::child> _childProcess;
    std::shared_ptr<std::thread> _launchThread;
    run_status_t _runStatus;
    mutable std::mutex _mutex;
};

} // namespace ldt

#endif // !defined(LOCKD_LOCKD_TEST_LAUNCHER_HPP_INCLUDED_)

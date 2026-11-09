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
 * File       : output_test.cpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Verify quiet test output preserves unexpected failure diagnostics.
 * Start Date : 10/24/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <chrono>
#include <future>
#include <initializer_list>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <boost/test/tools/context.hpp>
#include <boost/test/tools/interface.hpp> // IWYU pragma: keep
#include <boost/test/unit_test.hpp>       // IWYU pragma: keep
#include <boost/test/unit_test_suite.hpp>

#include "../include/lockd_test.hpp"

// Selects the supported Process v1 API across Boost versions.
#include "../include/process.hpp" // IWYU pragma: keep

// These probes run only in child processes. Their intentional failures must never become
// part of the normal suite's results; the parent verifies both status and visible output.
namespace {

BOOST_AUTO_TEST_SUITE(output_probes, *boost::unit_test::disabled())

BOOST_AUTO_TEST_CASE(passing)
{
    // Emit a recognizable daemon log and pass. The parent uses this probe to check
    // that successful tests hide captured logs unless verbose output is requested.
    std::clog << "DAEMON_OUTPUT_PROBE\n";
    BOOST_CHECK_MESSAGE(true, "Passing output probe");
}

BOOST_AUTO_TEST_CASE(failing)
{
    // Emit a recognizable daemon log and deliberately fail. The parent checks that
    // output capture preserves both the failure status and its diagnostics.
    std::clog << "DAEMON_OUTPUT_PROBE\n";
    BOOST_ERROR("UNEXPECTED_ASSERTION_PROBE");
}

BOOST_AUTO_TEST_SUITE_END()


BOOST_AUTO_TEST_SUITE(lifetimes)

BOOST_AUTO_TEST_CASE(test_output_is_quiet_on_success_and_preserves_failures)
{
    // Run isolated passing, failing, expected-failure, and verbosity probes. Verify
    // exit status and captured diagnostics, including command-line and environment overrides.
    // The POSIX signal test also launches real daemon children whose stderr must stay captured.
    const std::string executable = ldt::master_test_suite_argv()[0];
    for (const std::string scenario :
         {"passing", "failing", "expected", "verbose", "overrides", "environment", "signals"}) {
#ifdef WIN32
        if (scenario == "signals")
            continue;
#endif
        BOOST_TEST_CONTEXT("Output capture scenario: " << scenario)
        {
            const bool verbose = scenario == "verbose";
            const bool overrides = scenario == "overrides";
            const bool quiet = overrides || scenario == "environment";
            const bool failed = scenario == "failing";
            std::vector<std::string> arguments = {
                    scenario == "expected"
                            ? "--run_test=protocol/lockd_test_meta_test_cases"
                            : "--run_test=output_probes/" +
                                      (verbose || quiet ? std::string("passing") : scenario),
                    "--color_output=no",
                    verbose ? "--log_level=message" : "--log_level=error"
            };
            if (scenario == "signals")
                arguments[0] =
                        "--run_test=lifetimes/signals_drain_lock_holders_and_exit_successfully";
            if (overrides) {
                arguments.emplace_back("--show_progress=no");
                arguments.emplace_back("--report_level=no");
            }

            // Test defaults independently of the parent runner's chosen presentation.
            ldt::process::environment environment = ldt::process::native_environment();
            environment.erase("BOOST_TEST_SHOW_PROGRESS");
            environment.erase("BOOST_TEST_REPORT_LEVEL");
            if (scenario == "environment") {
                environment["BOOST_TEST_SHOW_PROGRESS"] = "no";
                environment["BOOST_TEST_REPORT_LEVEL"] = "no";
            }
            int exit_code;
            std::string output;
            {
                ldt::process::ipstream captured;
                ldt::process::child child(
                        executable,
                        ldt::process::args(arguments),
                        ldt::process::env = environment,
                        (ldt::process::std_out & ldt::process::std_err) > captured
                );
                auto reader = std::async(std::launch::async, [&captured] {
                    std::ostringstream text;
                    text << captured.rdbuf();
                    return text.str();
                });
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                while (child.running() && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                const bool exited = !child.running();
                if (!exited)
                    child.terminate();
                child.wait();
                output = reader.get();
                BOOST_REQUIRE_MESSAGE(exited, "Output probe exits within its deadline");
                exit_code = child.exit_code();
            }

            BOOST_CHECK_MESSAGE(
                    (exit_code != 0) == failed,
                    "Capture preserves the test's exit status: " << output
            );
            BOOST_CHECK_MESSAGE(
                    (output.find("DAEMON_OUTPUT_PROBE") != std::string::npos) ==
                            (failed || verbose),
                    "Daemon logs appear only on failure or request: " << output
            );
            BOOST_CHECK_MESSAGE(
                    (output.find("ASSERTION_PROBE") != std::string::npos) == failed,
                    "Unexpected assertion failures remain visible: " << output
            );
            BOOST_CHECK_MESSAGE(
                    output.find("expectedly_fails") == std::string::npos,
                    "Expected assertion diagnostics stay quiet: " << output
            );
            if (scenario == "signals")
                BOOST_CHECK_MESSAGE(
                        output.find("Stopping lockd;") == std::string::npos,
                        "Successful signal tests keep child-daemon diagnostics captured: " << output
                );
            BOOST_CHECK_MESSAGE(
                    (output.find("100%") != std::string::npos) == !quiet,
                    "Progress defaults on and can be disabled: " << output
            );
            BOOST_CHECK_MESSAGE(
                    (output.find("Test module") != std::string::npos) == !quiet,
                    "The short report defaults on and can be disabled: " << output
            );
            if (scenario == "expected")
                BOOST_CHECK_MESSAGE(
                        output.find("7 expected failures") != std::string::npos,
                        "The summary retains expected failure accounting: " << output
                );
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace

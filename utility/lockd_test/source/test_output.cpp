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
 * File       : test_output.cpp
 * Author     : Brett Gmoser
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Capture test diagnostics without hiding unexpected failures.
 * Start Date : 10/24/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */


#if !defined(WIN32)
# include <cerrno>

# include <stdlib.h> // NOLINT(modernize-deprecated-headers): POSIX setenv, outside <cstdlib>.

#endif

#include <cstdlib> // IWYU pragma: keep
#include <iostream>

#include <boost/system/error_code.hpp> // IWYU pragma: keep
#include <boost/system/system_error.hpp>
#include <boost/test/framework.hpp>
#include <boost/test/results_collector.hpp>
#include <boost/test/tree/test_unit.hpp>
#include <boost/test/unit_test.hpp> // IWYU pragma: keep
#include <boost/test/unit_test_log.hpp>

// Selects the supported Process v1 API across Boost versions.
#include "../include/process.hpp" // IWYU pragma: keep
#include "../include/test_output.hpp"

namespace {

void set_default_environment(const char* const name, const char* const value)
{
    const ldt::process::native_environment environment;
    if (environment.count(name))
        return;

    // Boost.Test reads the C runtime's environment. On Windows, SetEnvironmentVariable
    // alone does not update that copy; _putenv_s updates both the CRT and OS environment.
#ifdef WIN32
    const int error = _putenv_s(name, value); // NOLINT(misc-include-cleaner): provided by cstdlib.
#else
    const int error = ::setenv(name, value, 0) == 0 ? 0 : errno;
#endif
    if (error)
        throw boost::system::system_error(error, boost::system::generic_category(), name);
}

} // namespace


ldt::test_output::test_output()
{
    // Supply process-local defaults through Boost.Test's own configuration mechanism.
    // Existing environment settings and every supported command-line spelling take precedence.
    set_default_environment("BOOST_TEST_SHOW_PROGRESS", "yes");
    set_default_environment("BOOST_TEST_REPORT_LEVEL", "short");

    boost::unit_test::framework::register_observer(*this);
}


ldt::test_output::~test_output()
{
    _restoreStreams();
    boost::unit_test::framework::deregister_observer(*this);
}


void ldt::test_output::test_unit_start(const boost::unit_test::test_unit& test)
{
    if (test.p_type != boost::unit_test::TUT_CASE)
        return;

    _daemonOutput.str("");
    _expectedOutput.str("");
    _daemonStream = std::clog.rdbuf(_daemonOutput.rdbuf());

    // Only human-readable expected-failure diagnostics are buffered. Machine-readable
    // reports retain their individual assertion records and configured destinations.
    if (test.p_expected_failures != 0) {
        _testStream = boost::unit_test::unit_test_log.get_stream(boost::unit_test::OF_CLF);
        if (_testStream)
            boost::unit_test::unit_test_log.set_stream(boost::unit_test::OF_CLF, _expectedOutput);
    }
}


void ldt::test_output::
        test_unit_finish(const boost::unit_test::test_unit& test, const unsigned long /*elapsed*/)
{
    if (test.p_type != boost::unit_test::TUT_CASE)
        return;

    auto* const test_stream = _testStream;
    _restoreStreams();

    // Finish notifications run in reverse priority order. Our priority is below the
    // results collector and logger, so the result and captured log are complete here.
    const auto& result = boost::unit_test::results_collector.results(test.p_id);
    if (!result.passed() || result.p_assertions_failed != test.p_expected_failures) {
        if (test_stream)
            *test_stream << _expectedOutput.str() << std::flush;
        if (!_daemonOutput.str().empty())
            std::cerr << "Daemon output for " << test.full_name() << ":\n"
                      << _daemonOutput.str() << std::flush;
    }
    else {
        if (!_daemonOutput.str().empty())
            BOOST_TEST_MESSAGE(
                    "Daemon output for " << test.full_name() << ":\n"
                                         << _daemonOutput.str()
            );
        if (!_expectedOutput.str().empty())
            BOOST_TEST_MESSAGE("Expected assertion diagnostics:\n" << _expectedOutput.str());
    }
}


void ldt::test_output::_restoreStreams()
{
    if (_daemonStream) {
        std::clog.rdbuf(_daemonStream);
        _daemonStream = nullptr;
    }
    if (_testStream) {
        boost::unit_test::unit_test_log.set_stream(boost::unit_test::OF_CLF, *_testStream);
        _testStream = nullptr;
    }
}

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
 * File       : program_options.cpp
 * Author     : Brett Gmoser
 * Part Of    : lockd_test, a unit test suite for lockd and liblockd.
 * Purpose    : Parse test-runner options and manage the protocol suite fixture.
 * Start Date : 08/03/2023
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <thread>
#include <vector>

// The public Format header supplies the template definitions used by free_funcs.hpp.
#include <boost/format.hpp> // IWYU pragma: keep
#include <boost/format/free_funcs.hpp>
#include <boost/program_options/errors.hpp>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/parsers.hpp> // IWYU pragma: keep
#include <boost/program_options/value_semantic.hpp>
#include <boost/program_options/variables_map.hpp>

#include "../include/launcher.hpp"
#include "../include/lockd_test.hpp"
#include "../include/program_options.hpp"

program_options_fixture* PROGRAM_OPTIONS_FIXTURE_PTR = nullptr;

namespace {

// The fixture is destroyed before main checks its result. An option error must survive
// that teardown, without changing Boost.Test's expected-failure handling for test bodies.
bool INVALID_OPTIONS = false;

} // namespace


bool program_options_fixture::invalid_options()
{
    return INVALID_OPTIONS;
}


program_options_fixture::program_options_fixture(const std::string& executable)
        : _interface("localhost")
        , _lockdPath(ldt::daemon_path(executable).string())
        , _port(LOCKD_TEST_PORT)
        , _threads(
                  static_cast<unsigned short>(
                          !std::thread::hardware_concurrency() ? 1
                                                               : std::thread::hardware_concurrency()
                  )
          )
{
    // So that the tests know where to find us. boost.test does not seem to have a way to retrieve
    // the global fixtures, and documentation shows usage of static members instead.
    PROGRAM_OPTIONS_FIXTURE_PTR = this;
}


program_options_fixture::~program_options_fixture()
{
    PROGRAM_OPTIONS_FIXTURE_PTR = nullptr;
}


std::optional<int>
program_options_fixture::process_options(const int argc, const char* const* const argv)
{
    const auto separator = std::find(argv + 1, argv + argc, std::string("--"));
    if (separator == argv + argc) {
        if (std::find(argv + 1, argv + argc, std::string("--help")) != argv + argc)
            std::cout << "lockd_test options: " << argv[0] << " -- --help\n"
                      << "Benchmark mode: " << argv[0] << " -- -B --help\n"
                      << std::endl;
        return std::nullopt;
    }

    // Boost.Test handles its own options before --. Parse runner options before entering
    // the framework so help cannot announce a test run or depend on the selected suite.
    std::vector<const char*> arguments{argv[0]};
    arguments.insert(arguments.end(), separator + 1, argv + argc);
    program_options_fixture options(argv[0]);
    try {
        if (options._parseOptions(static_cast<int>(arguments.size()), arguments.data()))
            return EXIT_SUCCESS;
    }
    catch (const boost::program_options::error& error) {
        std::cerr << "invalid program options: " << error.what() << std::endl;
        return EXIT_FAILURE;
    }
    return std::nullopt;
}


bool program_options_fixture::_parseOptions(const int argc, const char* const* const argv)
{
    namespace po = boost::program_options;

    // Parse into signed values so negative inputs cannot wrap into valid ports or counts.
    int port = _port;
    int threads = _threads;
    po::options_description allowed_options("Allowed options");
    allowed_options.add_options()(
            "help,h", "produce lockd_test specific help message and exit")(
            "benchmark-test,B", "load an existing daemon instead of running tests; use -- -B --help")(
            "interface,i",
            po::value<std::string>(&_interface)->default_value(_interface),
            str(boost::format("IPv4/IPv6 address or hostname for %1% to listen on") %
                std::string(LOCKD_LOCKD_OUTPUT_NAME))
                    .c_str())(
            "port,p",
            po::value<int>(&port)->default_value(port),
            str(boost::format("port for %1% to listen on") %
                std::string(LOCKD_LOCKD_OUTPUT_NAME))
                    .c_str())(
            "threads,t",
            po::value<int>(&threads)->default_value(threads),
            "number of daemon worker threads (defaults to hardware threads, capped at 8; "
                    "1 if unknown)")(
            "lockd-path,P",
            po::value<std::string>(&_lockdPath)->default_value(_lockdPath),
            str(boost::format("the fully qualified path for %1%") %
                std::string(LOCKD_LOCKD_OUTPUT_NAME))
                    .c_str());

    // Parse the options.
    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, allowed_options), vm);
    po::notify(vm);

    // The normal parser accepts abbreviated long options. Only the separate benchmark dispatcher
    // may run that mode; an abbreviation must not fall through into the functional tests.
    if (vm.count("benchmark-test"))
        throw po::error("Use -- -B or -- --benchmark-test without Boost.Test options");

    if (port < 0 || port > std::numeric_limits<std::uint16_t>::max())
        throw po::validation_error(
                po::validation_error::invalid_option_value, "--port", std::to_string(port)
        );
    if (threads <= 0 || threads > std::numeric_limits<unsigned short>::max())
        throw po::validation_error(
                po::validation_error::invalid_option_value, "--threads", std::to_string(threads)
        );
    _port = static_cast<std::uint16_t>(port);
    _threads = static_cast<unsigned short>(threads);

    if (vm.count("help")) {
        std::cout << std::endl
                  << "Usage: " << argv[0] << " [boost-test-options] -- [lockd_test_options]"
                  << std::endl
                  << "       " << argv[0] << " --help" << std::endl
                  << "       " << argv[0] << " -- --help" << std::endl
                  << "       " << argv[0] << " -- -B [benchmark-options]" << std::endl
                  << allowed_options << std::endl;

        return true;
    }
    return false;
}


void program_options_fixture::setup()
{
    try {
        if (_parseOptions(ldt::master_test_suite_argc(), ldt::master_test_suite_argv()))
            return;

        // Tests launch only after checking that the protocol port is available.
        _launcher = std::make_shared<ldt::launcher>(_lockdPath, _interface, _port, _threads);
    }
    catch (boost::program_options::error& e) {
        // Boost.Test can count a suite setup exception against its children's expected
        // assertion failures. Keep a separate process result so invalid options cannot pass.
        INVALID_OPTIONS = true;
        ldt::throw_framework_setup_error(
                str(boost::format("invalid program options: %1%\n") % e.what())
        );
    }
}


void program_options_fixture::teardown() const
{
    ldt::release_protocol_clients();

    // Stop the protocol daemon before releasing the fixture's launcher.
    if (_launcher) {
        _launcher->terminate();
        _launcher->join();
    }
}

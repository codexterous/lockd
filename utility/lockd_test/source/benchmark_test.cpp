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
 * File       : benchmark_test.cpp
 * Author     : Brett Gmoser
 * Part Of    : lockd_test, a unit test suite for lockd and liblockd.
 * Purpose    : Run explicit load tests against an external daemon.
 * Start Date : 11/01/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <boost/program_options/cmdline.hpp>
#include <boost/program_options/errors.hpp>
#include <boost/program_options/option.hpp>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/program_options/value_semantic.hpp>
#include <boost/program_options/variables_map.hpp>

#include <liblockd/detail/client_contract.hpp>

#include "../include/benchmark_test.hpp"
#include "../include/detail/benchmark_detail.hpp"

std::optional<int> ldt::benchmark_test(const int argc, const char* const* const argv)
{
    namespace po = boost::program_options;
    const auto style =
            po::command_line_style::default_style & ~po::command_line_style::allow_guessing;
    try {
        const auto separator = std::find(argv + 1, argv + argc, std::string("--"));
        std::vector<const char*> arguments{argv[0]};
        arguments.insert(
                arguments.end(), separator == argv + argc ? argv + 1 : separator + 1, argv + argc
        );
        po::options_description selector;
        selector.add_options()("benchmark-test,B", "run the external-daemon load generator");

        // Let Program_options recognize short options and reject ambiguous mode spellings. Do not
        // give Boost.Test any opportunity to consume or run a benchmark invocation.
        const auto selected =
                po::command_line_parser(static_cast<int>(arguments.size()), arguments.data())
                        .options(selector)
                        .style(style)
                        .allow_unregistered()
                        .run();
        const bool benchmark = std::any_of(
                selected.options.begin(),
                selected.options.end(),
                [](const po::basic_option<char>& option) {
                    return option.string_key == "benchmark-test";
                }
        );
        if (!benchmark)
            return std::nullopt;
        if (separator == argv + argc)
            throw po::error("--benchmark-test is a lockd_test option; use -- -B [options]");
        if (separator != argv + 1)
            throw po::error(
                    "--benchmark-test cannot be combined with Boost.Test options before --"
            );

        detail::benchmark_options options;
        options.port = LOCKD_TEST_PORT;
        const unsigned int hardware = std::thread::hardware_concurrency();
        options.threads = hardware == 0 ? 1 : std::min(hardware, 65535U);
        int port = options.port;
        int threads = static_cast<int>(options.threads);
        int connections = 1;
        int duration = 60;
        int warmup = 5;
        int connect_timeout = 5;
        int acquire_timeout = 1000;
        int hold_ms = 0;
        int lock_count = 1;
        po::options_description allowed("Benchmark options");
        allowed.add_options()
                ("benchmark-test,B", "load an existing daemon; never run normal tests")
                ("help,h", "show benchmark options and exit")
                ("interface,i", po::value<std::string>(&options.interface)->default_value(options.interface),
                 "daemon IPv4/IPv6 address or hostname")
                ("port,p", po::value<int>(&port)->default_value(port), "daemon TCP port")
                ("threads,t", po::value<int>(&threads)->default_value(threads), "client worker threads")
                ("connections", po::value<int>(&connections)->default_value(connections),
                 "connections per worker")
                ("engine", po::value<std::string>(&options.engine)->default_value(options.engine),
                 "sync: sequential C++ clients; async: concurrent protocol connections per worker")
                ("duration", po::value<int>(&duration)->default_value(duration), "measurement seconds (1-86400)")
                ("warmup", po::value<int>(&warmup)->default_value(warmup), "warm-up seconds (0-3600)")
                ("connect-timeout", po::value<int>(&connect_timeout)->default_value(connect_timeout),
                 "connection timeout seconds (1-60)")
                ("acquire-timeout", po::value<int>(&acquire_timeout)->default_value(acquire_timeout),
                 "lock acquisition budget in milliseconds (1-60000)")
                ("hold-ms", po::value<int>(&hold_ms)->default_value(hold_ms), "critical-section milliseconds (0-60000)")
                ("workload", po::value<std::string>(&options.workload)->default_value(options.workload),
                 "independent, contended, idle, or churn")
                ("lock-count", po::value<int>(&lock_count)->default_value(lock_count),
                 "number of shared lock names for the contended workload")
                ("lock-prefix", po::value<std::string>(&options.prefix)->default_value(options.prefix),
                 "lock namespace; use the same prefix on generators that should contend")
                ("require-all-clients", po::bool_switch(&options.require_all_clients),
                 "fail unless every connection completes a successful measured cycle")
                ("results", po::value<std::string>(&options.results), "write per-worker counters and mergeable latency histograms as CSV");
        po::variables_map values;
        const auto parsed =
                po::command_line_parser(static_cast<int>(arguments.size()), arguments.data())
                        .options(allowed)
                        .style(style)
                        .run();
        if (std::any_of(
                    parsed.options.begin(),
                    parsed.options.end(),
                    [](const po::basic_option<char>& option) { return option.position_key >= 0; }
            ))
            throw po::error("Positional arguments cannot be combined with --benchmark-test");
        po::store(parsed, values);
        po::notify(values);

        // The first separator selects runner options; a second one would hide load arguments.
        if (std::find(arguments.begin() + 1, arguments.end(), std::string("--")) != arguments.end())
            throw po::error("Benchmark options must follow a single -- separator");
        const auto bounded = [](const int value, const int low, const int high, const char* name) {
            if (value < low || value > high)
                throw po::validation_error(
                        po::validation_error::invalid_option_value, name, std::to_string(value)
                );
            return static_cast<unsigned int>(value);
        };
        options.port = static_cast<std::uint16_t>(bounded(port, 1, 65535, "port"));
        options.threads = bounded(threads, 1, 65535, "threads");
        options.connections = bounded(connections, 1, 65535, "connections");
        options.duration = bounded(duration, 1, 86400, "duration");
        options.warmup = bounded(warmup, 0, 3600, "warmup");
        options.connect_timeout = bounded(connect_timeout, 1, 60, "connect-timeout");
        options.acquire_timeout = bounded(acquire_timeout, 1, 60000, "acquire-timeout");
        options.hold_ms = bounded(hold_ms, 0, 60000, "hold-ms");
        options.lock_count = bounded(lock_count, 1, 65535, "lock-count");
        if (options.engine != "sync" && options.engine != "async")
            throw po::error("engine must be sync or async");
        lld::detail::validate_lock_name(options.prefix);
        if (options.prefix.size() > 180)
            throw po::error(
                    "lock-prefix must be at most 180 ASCII characters to leave room for client "
                    "identifiers"
            );
        if (options.workload != "independent" && options.workload != "contended" &&
            options.workload != "idle" && options.workload != "churn")
            throw po::error("workload must be independent, contended, idle, or churn");
        if (values.count("lock-count") && !values["lock-count"].defaulted() &&
            options.workload != "contended")
            throw po::error("--lock-count requires --workload=contended");
        if (options.workload == "idle" && options.hold_ms != 0)
            throw po::error("--hold-ms does not apply to idle connections");
        if (options.workload == "idle" && options.require_all_clients)
            throw po::error("--require-all-clients does not apply to idle connections");
        if (values.count("help")) {
            std::cout << "Usage: " << argv[0] << " -- --benchmark-test [options]\n"
                      << allowed
                      << "\nNormal Boost.Test options and --lockd-path cannot be combined with "
                         "this mode.\n";
            return EXIT_SUCCESS;
        }
        detail::benchmark_runner runner(options);
        return runner.run();
    }
    catch (const std::exception& error) {
        std::cerr << "benchmark test: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

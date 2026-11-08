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
 * File       : benchmark_detail.cpp
 * Author     : Brett Gmoser
 * Part Of    : lockd_test, a unit test suite for lockd and liblockd.
 * Purpose    : Measure concurrent client load against an external daemon.
 * Start Date : 11/01/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <ostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <liblockd/mutex.hpp>
#include <liblockd/timed_mutex.hpp>

#include "../../include/detail/async_benchmark_detail.hpp"
#include "../../include/detail/benchmark_detail.hpp"

void ldt::detail::benchmark_result::record(const std::chrono::steady_clock::duration& latency)
{
    const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(latency).count();
    auto value = static_cast<std::uint64_t>(nanoseconds / 1000 + (nanoseconds % 1000 != 0));
    std::size_t scale = 0;
    while (value > 31) {
        value = value / 2 + value % 2;
        ++scale;
    }
    ++histogram[scale * 32 + static_cast<std::size_t>(value)];
    ++cycles;
}


void ldt::detail::benchmark_result::merge(const benchmark_result& other)
{
    cycles += other.cycles;
    timeouts += other.timeouts;
    errors += other.errors;
    clients_completed += other.clients_completed;
    tcp_connected += other.tcp_connected;
    greetings_received += other.greetings_received;
    for (std::size_t index = 0; index < histogram.size(); ++index)
        histogram[index] += other.histogram[index];
}


std::uint64_t ldt::detail::benchmark_result::bucket_upper(const std::size_t bucket)
{
    return static_cast<std::uint64_t>(bucket % 32) << (bucket / 32);
}


std::uint64_t ldt::detail::benchmark_result::percentile(const unsigned int percent) const
{
    const std::uint64_t rank = cycles / 100 * percent + (cycles % 100 * percent + 99) / 100;
    std::uint64_t cumulative = 0;
    if (rank == 0)
        return 0;
    for (std::size_t index = 0; index < histogram.size(); ++index) {
        cumulative += histogram[index];
        if (cumulative >= rank)
            return bucket_upper(index);
    }
    return 0;
}


void ldt::detail::benchmark_result::csv(
        std::ostream& stream, const std::size_t worker, const bool setup
) const
{
    stream << "cycles," << worker << ',' << cycles << ",0\n"
           << "timeouts," << worker << ',' << timeouts << ",0\n"
           << "errors," << worker << ',' << errors << ",0\n"
           << "clients_completed," << worker << ',' << clients_completed << ",0\n";
    if (setup)
        stream << "tcp_connected," << worker << ',' << tcp_connected << ",0\n"
               << "greetings_received," << worker << ',' << greetings_received << ",0\n";
    for (std::size_t index = 0; index < histogram.size(); ++index)
        if (histogram[index] != 0)
            stream << "acquisition_us," << worker << ',' << bucket_upper(index) << ','
                   << histogram[index] << '\n';
}


ldt::detail::benchmark_runner::benchmark_runner(const benchmark_options& options)
        : _options(options)
        , _runId(boost::uuids::to_string(boost::uuids::random_generator()()))
        , _results(options.threads)
{
}


std::string
ldt::detail::benchmark_runner::_name(const std::size_t worker, const std::size_t connection) const
{
    if (_options.workload == "contended")
        return _options.prefix + '_' + std::to_string((worker + connection) % _options.lock_count);
    return _options.prefix + '_' + _runId + '_' + std::to_string(worker) + '_' +
           std::to_string(connection);
}


std::unique_ptr<lld::timed_mutex>
ldt::detail::benchmark_runner::_connect(const std::string& name) const
{
    return std::make_unique<lld::timed_mutex>(
            _options.interface, _options.port, std::chrono::seconds(_options.connect_timeout), name
    );
}


bool ldt::detail::benchmark_runner::_cycle(lld::timed_mutex& client, benchmark_result& result) const
{
    const auto started = std::chrono::steady_clock::now();
    if (started >= _measurementEnd)
        return false;
    const bool measured = started >= _measurementStart;
    const auto deadline = std::min(
            _measurementEnd, started + std::chrono::milliseconds(_options.acquire_timeout)
    );
    std::unique_lock<lld::timed_mutex> lock(client, std::defer_lock);
    if (!lock.try_lock_until(deadline)) {
        if (measured && std::chrono::steady_clock::now() <= _measurementEnd)
            ++result.timeouts;
        return false;
    }
    const auto acquired = std::chrono::steady_clock::now();
    if (_options.hold_ms != 0)
        std::this_thread::sleep_until(
                std::min(_measurementEnd, acquired + std::chrono::milliseconds(_options.hold_ms))
        );

    // Explicit throwing release must succeed before counting a completed lock cycle. The
    // destructor remains the fallback on exceptions, without hiding release failures here.
    lock.release()->unlock(lld::mutex::tt_throw_on_failure);
    if (measured && std::chrono::steady_clock::now() <= _measurementEnd) {
        result.record(acquired - started);
        return true;
    }
    return false;
}


void ldt::detail::benchmark_runner::_worker(const std::size_t worker)
{
    auto& result = _results[worker];
    std::vector<std::unique_ptr<lld::timed_mutex>> clients;
    std::vector<bool> completed;
    std::unique_ptr<async_benchmark_worker> asynchronous;
    try {
        completed.resize(_options.connections, false);
        if (_options.engine == "async") {
            asynchronous =
                    std::make_unique<async_benchmark_worker>(_options, result, _failed, worker);
            std::vector<std::string> names;
            names.reserve(_options.connections);
            for (std::size_t index = 0; index < _options.connections; ++index)
                names.push_back(_name(worker, index));
            asynchronous->prepare(names);
        }
        else
            for (std::size_t index = 0; index < _options.connections && !_failed; ++index) {
                clients.push_back(_connect(_name(worker, index)));
            }
    }
    catch (const std::exception& error) {
        ++result.errors;
        result.error = error.what();
        _failed = true;
    }

    {
        std::unique_lock<std::mutex> lock(_gateMutex);
        ++_ready;
        _gate.notify_all();
        _gate.wait(lock, [this] { return _started; });
    }
    if (_failed)
        return;
    try {
        if (asynchronous) {
            asynchronous->run(_measurementStart, _measurementEnd);
            return;
        }
        if (_options.workload == "idle") {
            std::unique_lock<std::mutex> lock(_gateMutex);
            _gate.wait_until(lock, _measurementEnd, [this] { return _failed.load(); });
            return;
        }
        std::size_t index = 0;
        std::size_t iteration = 0;
        while (!_failed && std::chrono::steady_clock::now() < _measurementEnd) {
            auto& client = clients[index];
            if (_options.workload == "contended")
                client->lock_name(_name(worker, iteration++));
            if (_options.workload == "churn") {
                client.reset();
                client = _connect(_name(worker, index));
            }
            if (_cycle(*client, result) && !completed[index]) {
                completed[index] = true;
                ++result.clients_completed;
            }
            index = (index + 1) % clients.size();
        }
    }
    catch (const std::exception& error) {
        ++result.errors;
        result.error = error.what();
        _failed = true;
        _gate.notify_all();
    }
}


int ldt::detail::benchmark_runner::run()
{
    // Open the result file before creating load, so an invalid destination cannot waste a run.
    if (!_options.results.empty()) {
        const std::ofstream probe(_options.results);
        if (!probe)
            throw std::runtime_error("Cannot open benchmark results file: " + _options.results);
    }
    std::cout << "Benchmarking host=" << _options.interface << ", port=" << _options.port
              << " with " << _options.threads << " workers and "
              << static_cast<std::uint64_t>(_options.threads) * _options.connections
              << " connections; workload=" << _options.workload << "; engine=" << _options.engine
              << '\n'
              << "Connecting clients..." << std::endl;
    std::vector<std::thread> workers;
    try {
        workers.reserve(_options.threads);
        for (std::size_t index = 0; index < _options.threads; ++index)
            workers.emplace_back([this, index] { _worker(index); });
        std::unique_lock<std::mutex> lock(_gateMutex);
        _gate.wait(lock, [this] { return _ready == _options.threads; });
        _measurementStart =
                std::chrono::steady_clock::now() + std::chrono::seconds(_options.warmup);
        _measurementEnd = _measurementStart + std::chrono::seconds(_options.duration);
        _wallStart = std::chrono::system_clock::now() + std::chrono::seconds(_options.warmup);
        _started = true;
    }
    catch (...) {
        // A thread-creation failure must release already-created workers before their thread
        // objects are destroyed. Each worker still owns and closes its own client sockets.
        _failed = true;
        {
            const std::lock_guard<std::mutex> lock(_gateMutex);
            _started = true;
        }
        _gate.notify_all();
        for (auto& worker : workers)
            worker.join();
        throw;
    }
    _gate.notify_all();
    std::cout << "Warm-up: " << _options.warmup << "s; measurement: " << _options.duration << "s"
              << std::endl;
    for (auto& worker : workers)
        worker.join();
    return _report();
}


int ldt::detail::benchmark_runner::_report() const
{
    benchmark_result total;
    std::ofstream csv;
    if (!_options.results.empty()) {
        csv.open(_options.results);
        if (!csv)
            throw std::runtime_error("Cannot write benchmark results file: " + _options.results);
        csv << "metric,worker,value,count\n"
            << "workload,all," << _options.workload << ",0\n"
            << "engine,all," << _options.engine << ",0\n"
            << "run_id,all," << _runId << ",0\n"
            << "lock_prefix,all," << _options.prefix << ",0\n"
            << "threads,all," << _options.threads << ",0\n"
            << "connections_per_worker,all," << _options.connections << ",0\n"
            << "setup_handshakes_per_worker,all,"
            << (_options.engine == "async"
                        ? std::min<std::size_t>(async_setup_concurrency, _options.connections)
                        : 1)
            << ",0\n"
            << "warmup_seconds,all," << _options.warmup << ",0\n"
            << "hold_ms,all," << _options.hold_ms << ",0\n"
            << "lock_count,all," << _options.lock_count << ",0\n"
            << "require_all_clients,all," << _options.require_all_clients << ",0\n"
            << "aborted,all," << _failed.load() << ",0\n";
        csv << "measurement_seconds,all," << _options.duration << ",0\n"
            << "start_unix_ms,all,"
            << std::chrono::duration_cast<std::chrono::milliseconds>(_wallStart.time_since_epoch())
                        .count()
            << ",0\n";
    }
    for (std::size_t index = 0; index < _results.size(); ++index) {
        const auto& result = _results[index];
        total.merge(result);
        if (csv.is_open())
            result.csv(csv, index, _options.engine == "async");
        if (!result.error.empty())
            std::cerr << "Worker " << index << ": " << result.error << '\n';
    }
    if (csv.is_open()) {
        csv.flush();
        if (!csv)
            throw std::runtime_error("Failed writing benchmark results file: " + _options.results);
    }
    std::cout << "Completed cycles: " << total.cycles;
    if (!_failed)
        std::cout << "; cycles/s: " << static_cast<double>(total.cycles) / _options.duration;
    std::cout << "; timeouts: " << total.timeouts << "; errors: " << total.errors << '\n'
              << "Acquisition latency upper bounds (us): p50=" << total.percentile(50)
              << ", p95=" << total.percentile(95) << ", p99=" << total.percentile(99) << '\n';
    if (_options.workload == "idle" && !_failed)
        std::cout << "Idle window finished; no lock cycles were requested.\n";
    const std::uint64_t expected =
            static_cast<std::uint64_t>(_options.threads) * _options.connections;
    const bool incomplete = _options.require_all_clients && total.clients_completed != expected;
    std::cout << "Measured clients completed: " << total.clients_completed << '/' << expected
              << '\n';
    if (incomplete)
        std::cerr << "Client coverage incomplete; not every connection completed a successful "
                     "measured cycle.\n";
    if (_failed)
        std::cerr
                << "Run aborted; counters are partial and are not valid throughput measurements.\n";
    return _failed || incomplete ? EXIT_FAILURE : EXIT_SUCCESS;
}

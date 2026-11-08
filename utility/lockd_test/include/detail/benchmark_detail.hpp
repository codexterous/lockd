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
 * File       : benchmark_detail.hpp
 * Author     : Brett Gmoser
 * Part Of    : lockd_test, a unit test suite for lockd and liblockd.
 * Purpose    : Measure concurrent client load against an external daemon.
 * Start Date : 11/01/2026
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#if !defined(LOCKD_TEST_DETAIL_BENCHMARK_DETAIL_HPP_INCLUDED_)
# define LOCKD_TEST_DETAIL_BENCHMARK_DETAIL_HPP_INCLUDED_

# include <array>
# include <atomic>
# include <chrono>
# include <condition_variable>
# include <cstddef>
# include <cstdint>
# include <memory>
# include <mutex>
# include <ostream>
# include <string>
# include <vector>

namespace lld {
class timed_mutex;
} // namespace lld

namespace ldt::detail {

// Every client belongs to exactly one worker. The common gate excludes connection setup from
// the measurement window. Warm-up operations and operations crossing either window boundary
// do not contribute samples. In-flight requests are drained before connections are destroyed.
// Histograms use bounded storage: 32 buckets per power of two in microseconds, rounded upward.
// The nonempty buckets above the first range have at most roughly 6.25% relative rounding error.
// Each worker writes only its own result, so recording does not serialize the measured loop.
// Client coverage counts distinct logical connections with a successful acquire/hold/release
// cycle wholly inside measurement. Warm-up grants, timeouts and failed releases don't count.
// Requiring full coverage keeps the duration bounded but fails an incomplete run, rather than
// inferring participation from a large total cycle count. Churn tracks logical connection slots.
struct benchmark_options
{
    virtual ~benchmark_options() = default;
    std::string interface = "localhost";
    std::uint16_t port = 27198;
    unsigned int threads = 1;
    unsigned int connections = 1;
    unsigned int duration = 60;
    unsigned int warmup = 5;
    unsigned int connect_timeout = 5;
    unsigned int acquire_timeout = 1000;
    unsigned int hold_ms = 0;
    unsigned int lock_count = 1;
    bool require_all_clients = false;
    std::string workload = "independent";
    std::string engine = "sync";
    std::string prefix = "lockd_benchmark";
    std::string results;
};

struct benchmark_result
{
    virtual ~benchmark_result() = default;
    void record(const std::chrono::steady_clock::duration& latency);
    void merge(const benchmark_result& other);
    [[nodiscard]] std::uint64_t percentile(const unsigned int percent) const;
    void csv(std::ostream& stream, const std::size_t worker, const bool setup = false) const;
    [[nodiscard]] static std::uint64_t bucket_upper(const std::size_t bucket);

    std::uint64_t cycles = 0;
    std::uint64_t timeouts = 0;
    std::uint64_t errors = 0;
    std::uint64_t clients_completed = 0;
    // Initial async setup only, excluding churn reconnects. The synchronous library doesn't
    // expose TCP and greeting completion separately, so its CSV omits these diagnostic rows.
    std::uint64_t tcp_connected = 0;
    std::uint64_t greetings_received = 0;
    std::array<std::uint64_t, 2048> histogram{};
    std::string error;
};

class benchmark_runner
{
    /* construction and destruction */
public:
    explicit benchmark_runner(const benchmark_options& options);
    benchmark_runner(const benchmark_runner&) = delete;
    benchmark_runner& operator=(const benchmark_runner&) = delete;
    virtual ~benchmark_runner() = default;

    /* public methods */
public:
    int run();

    /* private methods */
private:
    void _worker(const std::size_t worker);
    [[nodiscard]] std::unique_ptr<lld::timed_mutex> _connect(const std::string& name) const;
    [[nodiscard]] bool _cycle(lld::timed_mutex& client, benchmark_result& result) const;
    [[nodiscard]] std::string _name(const std::size_t worker, const std::size_t connection) const;
    [[nodiscard]] int _report() const;

    /* private data members */
private:
    const benchmark_options _options;
    const std::string _runId;
    std::vector<benchmark_result> _results;
    std::mutex _gateMutex;
    std::condition_variable _gate;
    std::size_t _ready = 0;
    bool _started = false;
    std::atomic<bool> _failed{false};
    std::chrono::steady_clock::time_point _measurementStart;
    std::chrono::steady_clock::time_point _measurementEnd;
    std::chrono::system_clock::time_point _wallStart;
};

} // namespace ldt::detail

#endif // !defined(LOCKD_TEST_DETAIL_BENCHMARK_DETAIL_HPP_INCLUDED_)

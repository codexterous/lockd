# Benchmarking lockd

lockd is designed to make distributed locking and synchronization a tiny fraction of an
application's work. Performance tests were designed to check how well that promise holds up as more
machines, connections, and requests are added.

## Methodology

### Load generation

A total of eight different scenarios were designed to simulate the synchronization of typical client
application workloads. Groups of client VMs, referred to as **generators**, were used to simulate
load and usage of lockd.

The load generators have two modes to test large connection pools and heavy traffic: **Sync** and
**Async**. **Sync** models real-world application traffic. Each client thread uses the C++ client
library to acquire a lock, do protected work, and then release the lock, in a synchronous fashion.
**Async** uses a specially designed native event loop to drive every connection at once. Those tests
don't simulate any protected work: they acquire and immediately release their lock, and are designed
to be an extreme stress test; they don't simulate a typical real-world scenario.

#### Initial experimentation

Prior to selecting the final test scenarios, initial experimentation was performed. Those
experiments compared daemon worker thread counts under the same client load. Increasing the daemon
from eight to 32 I/O workers on a 32-vCPU VM produced only about **6% more synchronous throughput
and 24% more asynchronous throughput**, while CPU consumption roughly quadrupled. More threads kept
the processor busier without delivering a proportional increase in completed operations.

The results established eight daemon worker threads as an efficient baseline for these benchmarks.
The daemon's automatic default uses the hardware-thread count up to a maximum of eight, or one if
detection is unavailable. Smaller counts, including four or six, are preserved. Keeping that setting
fixed allowed the subsequent tests to focus on application workload, connection count, and
contention. The resulting profiles cover ordinary protected work, large connected client pools, and
continuous request pressure, with six runs per profile to check consistency.

### Application scenarios

The application scenarios connect each benchmark profile to a recognizable use case, showing how
lockd performs under everyday workloads, heavy contention, and extreme request traffic.

| Profile               | Intended application or stress scenario                                                                              |
|-----------------------|----------------------------------------------------------------------------------------------------------------------|
| Sync light            | A booking service checking seat availability and saving a reservation before checkout.                               |
| Sync medium           | A busy retailer reserving stock and recording orders across different products.                                      |
| Sync high             | A large background-processing platform claiming and updating independent jobs.                                       |
| Sync very high        | The same platform during a traffic surge, with twice the active work.                                                |
| Sync capacity, 250 ms | An auction or trading API: a large audience competing for 64 resources, with 250 ms to validate and record each bid. |
| Sync capacity, 50 ms  | The same auction platform with a faster, 50 ms protected update.                                                     |
| Async high            | A zero-work stress test: every connection continuously acquires and immediately releases its own lock.               |
| Async very high       | The same request storm with twice as many active connections.                                                        |

### Protected work and contention

Sync light through very high and both async profiles use independent lock names for each connection,
meaning the usage was not measured across contentious locks. The capacity profiles simulate more
highly contentious locks, sharing **64 lock names across every generator**, allowing at most 64
holders at any given point in time.

The application scenarios simulate database or API work by waiting while holding the lock. The 250
ms budget is deliberately conservative; shortening that work is an application optimization. The 50
ms capacity profile represents an application with a shorter, more efficient critical section.

Every connected client must complete an acquisition, hold, and release during measurement.

### Test setup and validation

Eight scenarios were each run six times on fresh VMs using the same native source, with a 30-second
warm-up and one-second resource sampling. The tests measured acquisition speed, throughput, CPU use,
and memory usage, requiring every connected client to acquire a lock, perform the simulated work,
and release it. **lockd itself synchronized the benchmark VMs**, using a shared launch lock to
coordinate their start. Profiling was disabled; the daemon logged at info and generators logged
errors only.

In terms of virtual hardware for these tests, one Linux server ran lockd with eight worker threads,
serving between 4 and 20 client VMs over a private network. These client VMs each had 8 vCPUs and 16
GiB RAM. The daemon had 12 vCPUs and 24 GiB. All ran Ubuntu 26.10 in Vultr's `ewr` region, using a
Release/shared build with GCC 15.3 and Boost 1.90.

All 48 published runs completed with zero client errors or acquisition timeouts.

## Results

### Workload scenarios

| Profile               | Generator VMs | Connected clients | Maximum active cycles | Protected work | Measurement |
|-----------------------|--------------:|------------------:|----------------------:|---------------:|------------:|
| Sync light            |             4 |               320 |                    32 |         250 ms |       120 s |
| Sync medium           |            10 |               800 |                    80 |         250 ms |       120 s |
| Sync high             |            10 |            16,000 |                   160 |         250 ms |       120 s |
| Sync very high        |            20 |            32,000 |                   320 |         250 ms |       120 s |
| Sync capacity, 250 ms |            20 |           256,000 |                   320 |         250 ms |     1,500 s |
| Sync capacity, 50 ms  |            20 |           256,000 |                   320 |          50 ms |     1,500 s |
| Async high            |            10 |            16,000 |                16,000 |           0 ms |       120 s |
| Async very high       |            20 |            32,000 |                32,000 |           0 ms |       120 s |

#### Reading the results

The protected work sets its own speed limit. A lock held for 250 ms can be used at most four times
per second. With 64 shared locks, that's **256 operations per second**, even if locking were
instantaneous. This is the **work ceiling**. Shortening the work to 50 ms raises it to 1,280
operations per second. With independent locks, the active worker count sets the limit on
simultaneous holders.

The table shows how close lockd came to that ceiling - near 100%. Almost all the available capacity
went to protected work, with little lost to locking, networking, and scheduling. The zero-work
stress tests have no hold-time-based ceiling, so their percentage is N/A.

- **Cycles/s:** completed acquire/work/release operations, summed across generators.
- **Acquisition:** time waiting for a grant, including contention. p50, p95, and p99 are the waits
  within which 50%, 95%, and 99% of successful acquisitions completed. They are histogram upper
  bounds calculated from merged generator samples.
- **CPU:** average daemon use as a percentage of the whole VM, during the generators' shared
  measurement window. Their windows overlap without starting at exactly the same instant; setup and
  teardown are excluded.
- **RSS:** peak sampled daemon memory, including setup but excluding kernel socket memory.

The table summarizes the typical result for each scenario, using the median of six runs. Latency
columns give the median of each run's p50, p95, and p99.

| Profile               |  Cycles/s | % of Ideal work ceiling | Acquisition p50 / p95 / p99, ms | Daemon CPU | Daemon RSS, MiB |
|-----------------------|----------:|------------------------:|--------------------------------:|-----------:|----------------:|
| Sync light            |     126.5 |                  98.82% |          0.624 / 1.856 / 10.496 |      0.17% |             8.5 |
| Sync medium           |     315.8 |                  98.69% |          0.608 / 2.784 / 14.336 |      0.36% |            10.5 |
| Sync high             |     632.1 |                  98.77% |          0.624 / 2.112 / 12.288 |      0.66% |            73.2 |
| Sync very high        |   1,264.0 |                  98.75% |          0.608 / 2.368 / 12.800 |      1.23% |           139.2 |
| Sync capacity, 250 ms |     252.8 |                  98.75% | 102.400 / 5,242.880 / 6,029.312 |      0.55% |           689.8 |
| Sync capacity, 50 ms  |   1,219.2 |                  95.25% |   106.496 / 835.584 / 1,310.720 |      1.67% |           690.5 |
| Async high            | 109,923.9 |                     N/A |       73.728 / 81.920 / 102.400 |     54.76% |            78.8 |
| Async very high       | 104,218.0 |                     N/A |     159.744 / 172.032 / 200.704 |     54.01% |           147.6 |

### Measured performance

The [full reports and raw data](https://www.codexterous.com/lockd/benchmark-20261106.zip) include
all 48 runs, grouped by profile.

**A quarter of a million connections needed very little daemon CPU.** The 250 ms capacity workload
retained **256,000 clients using about 690 MiB of memory and 0.55% CPU**. Its 320 synchronous
workers rotated through those connections, with at most 64 lock holders. The measured 252.8 cycles/s
reached **98.75% of the 256 cycles/s work ceiling**. Shortening protected work to 50 ms allowed
nearly five times the throughput. Acquisition waits include time queued behind other clients' work
on the 64 shared resources.

**Application load scaled well.** Going from 16,000 to 32,000 sync connections doubled throughput,
with CPU rising from 0.66% to 1.23% and memory from 73 to 139 MiB. Typical acquisition waits stayed
around 0.6 ms, which is tiny beside 250 ms of application work.

The Ubuntu VM's default connection-tracking limit was 262,144 entries, about 97.7% occupied during
the capacity tests. This is a [tunable kernel
limit](https://docs.kernel.org/networking/nf_conntrack-sysctl.html), rather than a fixed Ubuntu
connection maximum. The results show that memory use grew approximately with connection count,
meaning a much larger capacity is very feasible, but is not tested here.

**The stress tests showed where throughput leveled off.** Async runs sustained 93,000-130,000
cycles/s at 16,000 connections and 88,000-132,000 at 32,000. These profiles allowed a cycle on every
connection at once. More simultaneous requests increased latency without raising median throughput.

### A note on the Async tests

The **Async** tests deliberately apply unusually heavy traffic: every connection continuously
acquires and releases locks, with no application work between requests. Although this isn't typical
application behavior, the measured throughput gives a useful sense of scale.

Assuming **64 continuously active lock connections per application server**:

| Async profile      | Equivalent application servers | Completed lock cycles/s | Cycles/s per server |
|--------------------|-------------------------------:|------------------------:|--------------------:|
| 16,000 connections |                            250 |                 109,924 |               439.7 |
| 32,000 connections |                            500 |                 104,218 |               208.4 |

The larger test represents the theoretical locking traffic of **500 application servers, each
coordinating about 208 operations per second**, with 64 operations underway at once. If each request
uses one acquire/release cycle, that's roughly **104,000 application requests per second worth of
locking traffic**.

One daemon, configured with eight worker threads, handled that load using about **54% of a 12-vCPU
VM and 148 MiB of memory**. That is a modest dedicated synchronization service alongside a fleet of
hundreds of application servers, especially under continuous request pressure.

These server counts illustrate how the measured traffic could be distributed; the actual benchmarks
used 10 or 20 generator VMs.

## Conclusion

The application-style tests showed fast independent-lock acquisition, low CPU use, and consistent
memory growth. Under contention, throughput stayed close to the theoretical limit set by the
protected work. For similar workloads, lockd is unlikely to be the bottleneck; time spent doing
protected database or API work is likely to matter much more.

The low resource usage at 256,000 connections suggests much larger pools are likely to scale well
with higher configured host limits and similar activity. Continuous request traffic is a heavier
workload, and the async tests show where adding more active clients stops improving throughput.

## Repeating the benchmarks

### Automated Linux runs

The [controller](scripts/benchmarking/) needs Python 3.11+, local `ssh`/`scp`, a Vultr API key, and
an existing VPC, firewall group, and uploaded SSH key. It can run from Windows, Linux, or macOS;
remote VMs are Linux. Generate a configuration and inspect available plans and images:

```sh
python -m scripts.benchmarking example --output scripts/benchmarking/benchmark.json
python -m scripts.benchmarking catalog plans scripts/benchmarking/benchmark.json
python -m scripts.benchmarking catalog os scripts/benchmarking/benchmark.json
```

Set `VULTR_API_KEY` in the environment. Edit the JSON with your region, VM plans, OS, VPC CIDR,
firewall, and SSH settings. Allow controller SSH access and daemon traffic within the VPC. Relative
paths resolve against the JSON directory. Account-specific configurations are ignored by Git.
Package your source and copy the printed archive settings into the JSON:

```sh
python -m scripts.benchmarking.package
```

Set `daemon_threads` to 8 and `profile_daemon` to false. Each profile has its own configuration:

| Profile              | VMs | Threads per VM | Connections per thread | Engine | Hold ms | Duration s |
|----------------------|----:|---------------:|-----------------------:|--------|--------:|-----------:|
| `sync_light`         |   4 |              8 |                     10 | sync   |     250 |        120 |
| `sync_medium`        |  10 |              8 |                     10 | sync   |     250 |        120 |
| `sync_high`          |  10 |             16 |                    100 | sync   |     250 |        120 |
| `sync_very_high`     |  20 |             16 |                    100 | sync   |     250 |        120 |
| `sync_capacity`      |  20 |             16 |                    800 | sync   |     250 |       1500 |
| `sync_capacity_50ms` |  20 |             16 |                    800 | sync   |      50 |       1500 |
| `async_high`         |  10 |             16 |                    100 | async  |       0 |        120 |
| `async_very_high`    |  20 |             16 |                    100 | async  |       0 |        120 |

The VMs column sets `worker_count`. Put `--threads`, `--connections`, `--engine`, `--hold-ms`, and
`--duration` in `worker_arguments`, using the values above. Add
`--warmup=30 --connect-timeout=5 --require-all-clients`. Independent profiles use
`--workload=independent --acquire-timeout=1000`. Capacity uses
`--workload=contended --lock-count=64 --acquire-timeout=10000`, with the same `--lock-prefix` across
generators. Set `nofile` to 524288 and `run_timeout_seconds` to 2400 for capacity; use 131072 and
1800 otherwise. Keep `sample_seconds` at 1.

```sh
python -m scripts.benchmarking plan scripts/benchmarking/benchmark.json
python -m scripts.benchmarking run scripts/benchmarking/benchmark.json
```

`plan` validates locally. **`run` creates billable VMs without a prompt.** It builds once on the
daemon VM, distributes binaries, and launches generators. Cleanup removes recorded benchmark VMs
automatically, including after failure or Ctrl+C; existing shared resources are retained. If cleanup
is incomplete, use the printed recovery manifest:

```sh
python -m scripts.benchmarking cleanup path/to/manifest.json
```

Raw data and journals go under `output/obj/<arch>/opt/shared/benchmarking/<run-id>/`; HTML reports
go under `output/<arch>/opt/shared/benchmarking/<run-id>/`. Matching UUIDs describe the same run.
Preserve CSVs, process samples, and summaries with published results.

For randomized rounds, save configurations as `scripts/benchmarking/benchmark-<profile>.json`. The
runner discovers all matching files, with no fixed number of profiles; `benchmark.json` is for
individual runs and is not included. This follows the [randomized block design described by
NIST](https://www.itl.nist.gov/div898/handbook/pri/section3/pri332.htm): each round includes every
profile once, in a shuffled order, to reduce order and timing effects. From the checkout root, use
the same Python interpreter or activated virtual environment for all commands. Preview locally, run
one complete round, or resume a stopped round:

```sh
python -m scripts.benchmarking.run_round --plan-only
python -m scripts.benchmarking.run_round
python -m scripts.benchmarking.run_round --resume path/to/round.json
```

The runner uses Python's standard library. `--plan-only` makes no API calls or journal changes; a
fresh preview does not reserve its displayed order. A normal invocation runs each discovered profile
sequentially and creates billable VMs for each, with no automatic repetition of failed profiles. Its
`round.json` records the chosen order and each attempt's UUID and manifest path.

On failure or Ctrl+C, let the current run finish its cleanup, then use the printed resume command.
Resume preserves the saved order, skips completed profiles, and retries the failed or interrupted
profile with a fresh UUID, retaining previous attempts and their files. It requires unchanged
configuration hashes and a prior manifest recording all resources as deleted or rejected; if
resources remain, run `cleanup` first. Missing manifests block resumption. An exclusive OS lock
prevents two controllers from resuming the same round concurrently.

Saved `round.json` files retain their original profile list; new configurations are picked up by new
rounds, rather than added to a round being resumed.

Add `--plan-only` to the resume command to validate and preview without running benchmarks. Use
`python -m scripts.benchmarking guide` for additional options and recovery details.

### Running clients yourself

Build `lockd` and `lockd_test` using [BUILDING.md](BUILDING.md). Start the daemon on a private
interface, then run a generator on each client machine:

```sh
output/x86_64/opt/shared/lockd --no-fork --no-drop -i 10.20.0.10 -p 7198 -t 8 -l info

utility/lockd_test/output/x86_64/opt/shared/lockd_test -- -B \
  -i 10.20.0.10 -p 7198 -t 16 --connections=100 --engine=sync \
  --duration=120 --warmup=30 --hold-ms=250 --require-all-clients \
  --results=output/obj/x86_64/opt/shared/client-a.csv
```

Benchmark options follow `--`; see `lockd_test -- -B --help`. Use a different CSV per generator.
Direct mode doesn't manage the daemon or collect process metrics. Keep logging fixed: debug and
especially trace output can substantially reduce performance. Enable profiling only for separate
diagnostic runs.

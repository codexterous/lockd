"""
Orchestrate Linux Vultr benchmarks using only Python's standard library and SSH.

Run from the checkout root with Python 3.11 or newer::

    python -m scripts.benchmarking example --output benchmark.json
    python -m scripts.benchmarking guide
    python -m scripts.benchmarking catalog os benchmark.json
    python -m scripts.benchmarking catalog plans benchmark.json
    python -m scripts.benchmarking plan benchmark.json
    python -m scripts.benchmarking run benchmark.json
    python -m scripts.benchmarking cleanup path/to/manifest.json
    python -m scripts.benchmarking report path/to/manifest.json
    python -m unittest discover -s scripts/benchmarking/tests -v

python -m scripts.benchmarking.run_round discovers benchmark-*.json configurations
in scripts/benchmarking and runs each once in a random order, with no fixed count.
--plan-only validates locally without API calls or journal writes.
--resume path/to/round.json resumes a saved journal. It keeps
the saved order, skips completed profiles and explicitly retries the failed one.
All configuration hashes are checked before any run. An exclusive round lock
prevents concurrent runners. A new attempt's UUID and canonical manifest path
are journaled before calling the single-run controller; retries preserve earlier attempts.
Resumption checks configuration and recorded cleanup locally. Missing or unresolved
manifests block resumption. --plan-only with --resume previews the remaining
order without changing the journal or running a benchmark.

Edit the example before running. Supply VULTR_API_KEY in the controller's
environment, never in configuration. Use an existing VPC, firewall group allowing
SSH from your controller, and uploaded SSH key. These shared resources are never
created, modified, or deleted. On each new VM, the tool identifies the VPC network
interface by its API-reported MAC address and brings up its assigned address if
needed. It refuses to overwrite a different existing IPv4 configuration.
Choose an image whose ordinary packages meet BUILDING.md's requirements, such as
Ubuntu 24.04 or newer. worker_overrides maps zero-based generator indices to
complete argument lists, replacing worker_arguments for those machines. Omitting
--threads uses every generator's native hardware-thread default. daemon_threads=0
uses the daemon's automatic default: hardware threads capped at eight, or one if
unknown. VMs share worker_plan; vary thread/connection counts
using overrides. The source SHA-256 can be obtained with Python's hashlib, or
Get-FileHash on Windows and sha256sum on Linux.

Every catalog takes the configuration path and displays a table. Only region must
be configured at this stage; the source archive, SSH files and resource IDs can
remain unfinished. Plans show current availability in that region, sorted by
monthly cost, and include only plans supporting local boot storage. VPCs and region
details are limited to the configured region. OS images, firewall groups and SSH
keys are global Vultr catalogs and are labeled
accordingly; Vultr does not expose regional OS availability through its OS catalog.
run checks both chosen plans before creating VMs; capacity can still change afterward.
Only plans with local storage are supported. Vultr's storage_type determines this;
a positive disk value alone does not imply a local disk. VX1 deployment explicitly
selects the local boot device. Block-only plans are rejected before creation
because separate block volumes require their own billing and cleanup handling.

The source must be a local source tarball with its SHA-256. Run
python -m scripts.benchmarking.package to bundle current Git-included working-tree
files into .private/lockd-dev.tar.gz without CI. Copy the printed source_archive
and source_sha256 fields into the configuration. Ignored files and .private are
excluded. Existing CI tarballs work too. The daemon VM
installs Ubuntu build packages, builds once, and supplies both executables and
their non-glibc runtime libraries to every generator. All VMs use the same image
and architecture. No benchmark code is compiled on generators.

Add --engine=async to worker_arguments (and any worker_overrides) to allow every
connection to drive its own acquisition/release cycle concurrently. The default
sync engine visits connections sequentially through the C++ client. Both engines
use the same workloads, measurement windows and histograms. Async mode uses a
private native protocol driver with one event loop per worker thread. It measures
daemon capacity, rather than the public C++ client's throughput. Reports identify
the engine and reject combined results with mixed engines. Increase concurrent
connections gradually and expect queueing latency to change with load.

The run command creates billable VMs without a confirmation prompt. Use plan to
check configuration locally first. Each run saves a durable creation journal before
making billable API requests. Build output stays in the journal directory's build.log;
on failure its last 40 lines are shown before cleanup. Runtime logs are
collected only once monitoring has started.

Cleanup automatically deletes only recorded instances whose UUID, request tags,
label and recorded identity still match. No confirmation prompt is required.
Identity mismatches retain machines. A creation response lost in transit is
reconciled using that individual request's random tag and label; ambiguous matches
are retained. Shared account resources are never deleted. Stopped or retained Vultr
machines continue to cost money. Re-run cleanup after interruptions.
Explicit creation rejections are journaled as rejected and need no deletion.
Timeouts and server errors remain unresolved until reconciliation; a missing ID
does not establish that a billable instance was created.

A newly returned UUID may briefly return 404. Provisioning and cleanup retry that
UUID for up to 120 seconds without repeating creation. A visibility timeout retains
the resource as unresolved. Cleanup records deletion only after an acknowledged
DELETE and a subsequent not-found response; a pre-deletion 404 is insufficient.

SSH uses a run-specific known_hosts file and accept-new for first-contact trust.
Later host-key changes fail. The controller needs working ssh/scp commands and
the configured private key. Remote commands run on newly created benchmark VMs
as root; the API key and private key are never uploaded.

The daemon and first generator must become ready before more generators are created.
Remaining generators are prepared concurrently, with up to ten tasks in flight.
Creation requests and shared journal updates stay serialized. On failure, pending
preparations are cancelled; active tasks finish or stop before VM cleanup begins.
In-flight SSH commands retain their existing timeouts during cancellation.
This catches unreachable VPC endpoints before provisioning the full fleet. Setup
logs retain interface, route and guest UFW diagnostics. If UFW is active on the daemon, setup
inserts an inbound TCP rule limited to the VPC interface, subnet and private daemon
endpoint. It does not enable or disable UFW or alter the public interface's rules.
The existing Vultr firewall group is unchanged.
Network connection attempts retry for up to 30 seconds;
the controller reports the failing private endpoint if readiness cannot be established.

Agents use a lock on the benchmark daemon as a launch barrier. Every generator
queues before the daemon-side agent releases it. Native benchmark mode still
establishes connections before its own measurement window. Async initial setup
allows four TCP-plus-greeting handshakes per native worker at once, keeping each
slot until LOCKD arrives.
All requested connections are retained before measurement. This avoids opening
the whole fleet at once; the cap still scales with generator and native worker
counts. It doesn't limit measured cycle concurrency or churn reconnections.
The native CSV records setup_handshakes_per_worker. Reports retain measurement
windows and their overlap; summed per-generator rates are not advertised as a
single synchronized-window throughput result. Histograms are merged by count,
never by averaging percentiles. Displayed CPU averages use the overlap of all
generators' measurement windows, excluding setup, warm-up and drain. They require
valid results from every generator. Partial boundary intervals are weighted by
their overlap; clock offsets between VMs remain unverified. When sample coverage
is partial, the report averages the observed portion and labels it partial, with
covered/window seconds. Missing time isn't treated as idle or extrapolated. No
overlap or unknown CPU capacity shows unavailable. Lifetime averages are in the raw
summary; the separate lifetime resource table retains peaks. Main CPU graphs use the same
shared window, clipping overlapping rate intervals at both boundaries without
discarding in-window spikes. Full-lifetime graphs remain in an expandable section.
CPU graphs and tables use whole-VM capacity:
100% means all guest vCPUs are occupied by the measured process. Eight busy cores
on a 32-vCPU VM are 25%, even if the daemon is configured with only eight workers.
CPU graphs use a 0-100% scale unless a sample exceeds capacity; such values are
shown without clipping. Guest-reported CPU count takes precedence over API metadata;
when neither is available, normalized CPU output is unavailable. Live console CPU
also uses full VM capacity, based on the guest-reported vCPU count; it shows
unavailable until both an interval and that count are known. Raw one-core CPU
fields retain their original units. RSS maxima and
VmHWM are observed through the last sample; exit-time peaks can be missed. VmHWM
includes pre-measurement allocations, rather than being a measurement-only maximum.

For compatible complete results, reports relate raw acquisition percentiles to
the configured protected-work period and compare rates with an ideal work ceiling.
The ceiling is concurrent holders divided by work duration: sync limits holders
by native workers, async by connections, and contention also by the global lock
pool. Zero-duration work has no finite work-derived ceiling. Neither the wait
ratio nor the gap from the ceiling isolates daemon overhead; queue depth and
actual hold intervals aren't recorded. Raw histograms and rates remain unchanged.

Monitoring starts before the daemon listens or a generator connects. JSONL samples
retain process CPU, RSS, high-water RSS, threads, established process sockets,
host memory, load, and interface counters. Monitoring has overhead, especially
socket enumeration; vary sample_seconds and repeat runs to measure its effect.
Reports distinguish process and host resources. Keep the raw results with claims.

Async setup failures identify the active protocol phase, connection index, timeout
budget and elapsed time, plus completed TCP connections and greetings on that native
worker. Initial handshake counts also survive aborted runs in result.csv and the
report; they exclude churn reconnects. The sync library doesn't expose these stages
separately, so its setup counts are unavailable.

Read-only Linux diagnostics capture listener accept queues, SYN_RECV counts on each
listener's namespace family/port, process socket states and descriptor counts. Host
events record kernel limits and a pre-launch TCP counter baseline. Samples retain
listen overflows/drops, SYN cookies, retransmissions, TCP memory and conntrack usage.
The report shows counter increases and sampled queue peaks even when setup aborts.
TCP counters cover the network namespace, including SSH and other processes; they
aren't proof that lockd caused every event. Missing modules, denied files and reset
counters stay unavailable. Sampling can miss short queue spikes. These diagnostics
don't change connection concurrency, timeouts, kernel settings or resource limits.
Raw sockstat TCP.mem is in kernel pages; orphan/alloc/mem gauges are system-wide.

Set profile_daemon to true for an optional native CPU profile. The controller and
agents remain Python, but the daemon VM installs linux-tools for its running
Ubuntu kernel and tests PID attachment, enable acknowledgement, CPU sampling and
report generation before any worker VMs are created. If the matching package,
permissions or DWARF support are unavailable, the run fails with
build.log diagnostics rather than silently running without a profile. Profiling
does not change kernel security settings. Builds keep Release optimization and
add debug information. The normal unprofiled build is unchanged.

perf records the daemon's threads using the software cpu-clock event at 49 Hz and
8 KiB DWARF stacks. Sampling is enabled and acknowledged before releasing the
launch gate and stopped before daemon shutdown. It includes client connection
setup, warm-up, measurement and drain, excluding the long VM preparation interval.
CPU samples show where running threads spend CPU time, not time blocked on disk,
network or mutexes. Sampling and stack capture add overhead; compare a profiled
run with a separate unprofiled baseline before publishing throughput claims.

The recovery journal retains daemon-perf-report.txt (hot functions and call
stacks), daemon-perf.data, daemon-perf.log, daemon-profile.json and the matching
bundle.tar.gz. Reports are symbolized on the VM while its binaries and libraries
are still present. Distribution libraries may lack debug information, kernel
symbols may be restricted, stacks may truncate, and perf.log can report dropped
samples. These limitations remain visible in the local profiler output. Raw
recordings can be large because each CPU sample includes up to 8 KiB of stack data.
Keep the first profiled run short and allow time for collection. Raw
profile artifacts can contain addresses, stack memory and machine paths; they
are not copied into public report exports. The public report labels profiled
measurements and records profiling settings and completion status.

The daemon explicitly uses info logging. Generators retain only stderr errors in
process.log; their measurements remain in result.csv. Benchmark mode has no
per-operation logging or daemon-style log-level option. Debug and especially
trace daemon logging can substantially reduce throughput. Waiting for log I/O
can lower CPU utilization while increasing latency; lower CPU usage alone does
not establish better performance. Keep logging settings consistent when comparing
runs. The report records each machine's logging policy.

Local journals and raw results live under output/obj/<host>/opt/shared/benchmarking;
rendered reports live under output/<host>/opt/shared/benchmarking. Public report
exports omit the cleanup manifest, account resource IDs and SSH/setup logs; all
those diagnostics remain in the local journal directory. Agents retain
remote logs until automatic VM cleanup. A controller crash cannot guarantee
cleanup: re-run cleanup with its journal to verify and remove remaining owned VMs.

API fields follow https://github.com/vultr/govultr/blob/master/instance.go.
Linux metrics follow https://www.kernel.org/doc/html/latest/filesystems/proc.html.
"""

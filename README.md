# lockd

lockd provides familiar mutex and locking primitives across processes, machines, and even
datacenters. Written in C++17. Use them directly from C++, through native Python bindings, or from
other languages via the included C API.

## Rationale

lockd was originally built to replace MySQL's `GET_LOCK`, which provides similar functionality, but
is extremely inefficient: keeping a database connection open while a background process does its
work is an expensive way to hold a lock, especially when dozens of processes across multiple
machines need to coordinate. lockd gives that job to a small, dedicated daemon.

The goal is straightforward: named locks shared between processes and machines, with client
interfaces that fit ordinary C++ and Python code. If you're working in a single process on a single
machine, your operating system's native locking mechanisms are probably more appropriate.

Locks live in memory and belong to connections, and thus lockd is not designed to be used as
persistent storage. Use lockd to coordinate access to data your application keeps in persistent
storage, such as a database. For example, if you were designing a travel or concert booking service,
you would acquire a seat's lock, check availability, save a reservation in the database, then
release the lock. The reservation remains in the database while the customer completes checkout. An
auction service can use the same pattern to validate and record a bid, ensuring there is not a race
condition with two valid bids of the same amount.

A lost connection or daemon failure can lose a held lock, and restarting the daemon doesn't restore
ownership. Applications must handle those failures and use their storage's transactions and
constraints to keep persistent updates valid. See [failure handling](USAGE.md#connection-failures),
the [persistent update example](USAGE.md#coordinating-a-persistent-update), and
[security](SECURITY.md) before deploying it. Keep the daemon on a trusted, firewalled network.

## Quick start

Run a daemon, then connect clients using the same lock name. For a local foreground daemon on Linux
or FreeBSD:

```sh
./output/x86_64/opt/shared/lockd --no-fork --no-drop
```

On Windows, run `lockd.exe` from the portable download, or use the installer. The default address is
`localhost`, on TCP port 7198. [Running lockd](RUNNING.md) covers addresses, logging, and background
operation; [Windows](WINDOWS.md) covers installation and services.

### C++

These examples use ordinary scoped locks: acquisition waits until the lock is available, and leaving
scope releases it.

```cpp
#include <chrono>
#include <mutex>
#include <liblockd/mutex.hpp>

int main()
{
    // 5-second timeout for connecting to the daemon.
    lld::mutex mutex("localhost", 7198, std::chrono::seconds(5), "my_lock");
    const std::lock_guard<lld::mutex> lock(mutex);
    // Do the work protected by my_lock. Leaving scope releases it.
}
```

### Python

Install from PyPI with `python -m pip install pylockd`, or download a wheel matching your Python
version and platform from the release downloads.

```python
import pylockd

mutex = pylockd.Mutex(
    hostname='localhost',
    port=7198,
    connect_timeout=5,
    lock_name='my_lock',
)
with pylockd.ScopedLock(mutex):
    pass  # Do the work protected by my_lock.
```

Timed locks, recursive locks, and asynchronous Python locks are also available; see
[USAGE.md](USAGE.md) for examples and details. [Python builds](PYTHON.md) covers installing from
source.

### Other languages

`liblockd_dll` provides a C API so other languages can use lockd through native libraries. See [the
C interface guide](DLL.md) for C, C#, VB.NET, Node.js, and Rust examples. The examples provided are
purely examples - any language that can call functions from a native C API can use lockd.

## Building

Install CMake 3.24+, Ninja, a C++17 compiler, and Boost 1.82+ development packages. On Linux and
FreeBSD, CMake uses the system's installed Boost packages. On Windows, use a matching Visual Studio
developer environment and your existing vcpkg installation. See [Windows build
setup](BUILDING.md#windows) for dependency configuration.

```sh
cmake --preset x86_64-opt-shared
cmake --build output/obj/x86_64/opt/shared --parallel 2
```

Use `arm64-opt-shared` on ARM64. See [BUILDING.md](BUILDING.md) for dependencies, presets,
installation, linking your application, and Windows setup.

## Benchmarks

One Linux daemon with eight worker threads served up to **256,000 connected clients** across up to
20 client VMs. Tests covered application work, contention, and continuous request traffic. Each
scenario ran six times, with **zero client errors or acquisition timeouts**; every connected client
completed an operation. Selected results are medians across those six runs:

| Workload                                          | Connections | Completed lock cycles/s | Daemon CPU | Daemon memory |
|---------------------------------------------------|------------:|------------------------:|-----------:|--------------:|
| 64 shared resources, 250 ms of work               |     256,000 |                   252.8 |      0.55% |     689.8 MiB |
| Independent resources, 250 ms of work             |      32,000 |                 1,264.0 |      1.23% |     139.2 MiB |
| Continuous request stress test, no simulated work |      16,000 |               109,923.9 |     54.76% |      78.8 MiB |

The application workloads reached nearly **99% of the throughput permitted by their protected
work**. With 32,000 connections, median acquisition took **0.608 ms**. CPU percentages cover the
whole 12-vCPU daemon VM; memory is peak sampled process RSS, excluding kernel socket memory.

For typical applications using lockd for synchronization and coordination of protected resources,
database or API work is likely to cost far more than coordination.

See [BENCHMARKING.md](BENCHMARKING.md) for all eight scenarios, methodology, full results, and
instructions for repeating the tests.

## Testing

Tests build with the project by default and launch their own daemon instances. From the repository
root:

```sh
ctest --preset x86_64-opt-shared --output-on-failure --parallel 2
```

For a direct Windows run, use `utility\lockd_test\output\x86_64\opt\shared\lockd_test.exe`, or run
`lockd_test.exe` beside `lockd.exe` in an extracted portable download. [TESTING.md](TESTING.md)
describes coverage, filters, Python tests, and CI. [CONTRIBUTING.md](CONTRIBUTING.md) covers editor
setup, clang-tidy, and clang-format.

## Compatibility

lockd supports major Linux distributions, Raspberry Pi OS on ARM64, WSL, FreeBSD, and native
Windows. CI checks native builds and Python wheels across the [compatibility
matrix](COMPATIBILITY.md). [FREEBSD.md](FREEBSD.md) covers installation from ports.

For daemon options, see [RUNNING.md](RUNNING.md). For the text protocol, see
[PROTOCOL.md](PROTOCOL.md).

## License

lockd is available under the [MIT license](LICENSE.md).

## Generative AI Usage in This Project

> The first 90 percent of the code accounts for the first 90 percent of the development time. The
> remaining 10 percent of the code accounts for the other 90 percent of the development time.
>
> - Tom Cargill, Bell Labs

lockd was originally developed more than 15 years ago as a system to manage concurrency between
dozens of processes, each running across dozens of web servers, handling a request load of upwards
of 5,000 requests per second on a real-time trading API. Over the ensuing years, the project saw
development dormancy but remained stable and in production that entire time.

The bones of this project, along with many of the unit tests validating basic functionality, were
developed long before the birth of generative AI, and remain largely the same.

I've always wanted to do a "v1.0" release of this project as an open source project under an MIT
license. While the project had been stable, it was never quite release-ready - there's a large gap
between "local developers know how to use this service" and "this service is ready to use by the
developer population at large".

Between 2022 and 2024, the project was modernized significantly by hand, without agentic AI
assistance. A basic CMake configuration was created, the project was converted to C++17, and altered
to fit within the modern C++ ecosystem. Hundreds of initial automated unit tests were created during
this time.

By 2026, AI had gotten good enough to polish off the last 10% of the work required to get this
project into your hands. Agentic AI made quick work of applying those changes.

Generative AI was not used for major architecture decisions, but rather for work well suited to it -
things like additional tests, documentation, release engineering, and some final polish work. Each
change was reviewed and refined to ensure quality and maintainability.

AI was used for the following aspects of this project:

- **Documentation**: AI was used to generate additional documentation, improve existing
  documentation, and ensure consistency across the project. The documentation was useful, and not
  just for users: a careful read exposed inconsistencies in the client library and protocol that
  were subsequently fixed.
- **Build configuration**: Reorganized the CMake build profiles into more cohesive shared modules
  and presets, separated static and shared build outputs, and improved dependency discovery. The
  goal was a portable, command-line-first build that also works in common IDEs. clang-tidy and
  clang-format were also integrated into shared build tooling.
- **Tests**: AI was used to generate additional automated tests, beyond the several hundred that
  were originally handwritten. The resulting test suite now contains over a hundred test cases with
  over 12,000 individual assertions. Some test portability and lifecycle issues were also fixed.
- **Release engineering**: Handled release engineering including an NSIS installer for Windows,
  GitHub Actions, and the FreeBSD port - all things that could have taken years off of my life. The
  amount of code generated for this is quite large, but the benefits are significant and the code is
  scoped to be unrelated to production usage.
- **Benchmarking**: Performance has always been an extremely important goal for this project, and
  that performance needed to be proven rather than assumed. A benchmarking suite was developed and
  run to prove the performance claims. See [BENCHMARKING.md](BENCHMARKING.md). Benchmarking brought
  some minor performance issues to light, and those issues were addressed. The main logging function
  now returns early for suppressed log messages to avoid unnecessary logging synchronization, and
  the default worker thread count is now capped at eight threads, while preserving explicit
  overrides.
- **Boost library modernization**: Migrated protocol grammars to Spirit X3 from their former Boost
  Spirit and Phoenix v1 versions, replaced Asio and Boost.Process usage with Boost APIs that are
  modern, supported, and not deprecated versions.
- **Mutex APIs**: Added recursive mutexes and standard chrono duration/deadline support. These
  additions made the client libraries more useful with contemporary C++ locking patterns. Tests were
  added to validate all changes.
- **Python integration**: Replaced `setup.py` with a `pyproject.toml` and CMake-based build,
  provided more standardized exceptions, and added C++-implemented asyncio mutexes and lock guards.
  The native Python test coverage was also expanded to cover these changes.
- **Platform startup**: The `main()` function had ballooned far out of control, and a major goal for
  a "v1.0" release was to refactor it and move platform-specific things out of it. Windows and POSIX
  startup code was moved into platform-specific sources to ensure portability of privilege dropping,
  account defaults, runtime paths, PID-file recovery, and startup diagnostics. This simplified the
  daemon's entry point and addressed differences exposed by Windows, Linux/WSL, and FreeBSD builds.
- **Vulnerability audit**: While the daemon is meant to be exposed only to trusted internal
  networks, vulnerabilities still matter. Vulnerability testing revealed minor issues, such as the
  ability of a client to consume arbitrary amounts of memory by sending very long lock names. These
  vulnerabilities were addressed and fixed.
- **Daemon quality of life**: Minor changes were applied to the daemon, including coloring `--help`
  output, reclaiming stale PID files on POSIX-like systems, and allowing immediate restart after a
  crash or forced termination while preventing competing daemon listeners.
- **Windows service**: Windows has also always been a first-party supported platform for this
  project, including the native Python library. While the project added the ability for the daemon
  to install itself as a service in 2024, AI-directed changes added UAC elevation for service
  installation/removal and Windows Event Log integration, with configurable logging preserved in the
  installed service.

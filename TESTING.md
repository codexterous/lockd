# Testing lockd

Native tests cover the daemon, C++ and C clients, contention, timeouts, network failures, and
cleanup. Python tests cover the installed extension, including threaded and asyncio use. CI also
tests packages and Windows service installation.

## Native tests

Build first, then run CTest:

```sh
cmake --preset x86_64-opt-shared
cmake --build output/obj/x86_64/opt/shared --parallel 2
ctest --preset x86_64-opt-shared --output-on-failure --parallel 2
```

CTest does not build the executables for you. See [BUILDING.md](BUILDING.md) for prerequisites and
other build configurations.

Or run `utility/lockd_test/output/x86_64/opt/shared/lockd_test` directly from the repository root
(append `.exe` on Windows). In a Windows portable package, run `lockd_test.exe` beside `lockd.exe`.
Tests launch their own daemons; you don't need a running service.

Use `lockd_test --help` for Boost `test` options, or `lockd_test -- --help` for lockd-specific
options. For verbose output:

```sh
utility/lockd_test/output/x86_64/opt/shared/lockd_test --log_level=all --report_level=short
```

Progress and a short report are enabled by default. Failed tests show their daemon logs;
`--log_level=message` also shows logs for passing tests.

## Selecting tests

CTest runs the protocol, lifetime/network, and C interface suites. Windows builds also check service
logging, without installing a service. Shared builds test loading the shared library and calling its
exports. CTest also checks component output paths and, when configured, clang-tidy integration. Run
`ctest --preset x86_64-opt-shared -N` to list the configured tests, and use `-R <pattern>` to select
one. Keep the protocol suite together because its cases share state.

The protocol suite uses loopback port 27198. Change this default at configure time with
`-DLOCKD_TEST_PORT=<port>`, or append `-- --port <port>` to a direct test run. Don't run direct
protocol tests alongside CTest on the same port.

CLion's Run/Debug Configurations window groups the checked-in native test runs under **Testing**.
These launch executables directly; they are not CTest configurations. Use the CTest commands above
to run the tests registered with CMake.

## Build and packaging checks

CTest also checks native compiler configuration, SDK selection, version generation, source paths,
and platform build scripts. Select these with `ctest --test-dir <build> -L native_build`. Windows
checks use Windows PowerShell and isolated fixtures for installer and signing behavior. They don't
install services. Python wheel configuration checks additionally need a supported x64 Python
interpreter on Windows.

To run the build checks independently of the daemon and Boost dependencies, configure their entry
point with the same compiler environment used for a native build:

```sh
cmake -S cmake/tests -B output/obj/x86_64/opt/shared/verifications/build_checks -G Ninja
ctest --test-dir output/obj/x86_64/opt/shared/verifications/build_checks --output-on-failure
```

Workflow graph and cache metadata checks require `.github` files. Compiler and version fixtures use
local files and local Git repositories. These checks don't download packages or contact GitHub.
Python release-tool tests cover the Python implementations and Python wheel packaging separately:

```sh
python -m unittest discover -s scripts/release -p 'test_*.py' -v
```

## Load testing an external daemon

Use `lockd_test -- --benchmark-test` (`-B`) to generate load against an already-running daemon,
including from several client machines at once. This mode never launches or stops a daemon and
cannot be combined with Boost.Test options or `--lockd-path`. Pass `-B`, `-i`, `-p`, and `-t` after
the runner's `--` separator:

```sh
lockd_test -- -B -i server.example -p 7198 -t 32 --duration 60 --results client-a.csv
```

Defaults match the test runner: `localhost`, port 27198, and the available hardware thread count
(one if unknown). `-t` controls client workers, not the remote daemon. Use `lockd_test -- -B --help`
for load options and [benchmarking.md](BENCHMARKING.md) for workloads and result interpretation.

## Python tests

Install the extension into the interpreter you're testing and build the daemon. From the repository
root:

```sh
export LOCKD_TEST_DAEMON="$PWD/output/x86_64/opt/shared/lockd"
python -m unittest discover -s lib/pylockd/tests -p 'test_*.py' -v
```

In PowerShell, set `$env:LOCKD_TEST_DAEMON` to the absolute `lockd.exe` path and use the same
unittest command. See [PYTHON.md](PYTHON.md) for installation.

These tests start isolated daemons. They cover lock ownership, exceptions, thread handoff, async
cancellation, timeouts, and recursive acquisition.

## Privilege dropping and services

Windows service and installer tests run on disposable CI machines because they install, start, and
remove real services. Ordinary native tests don't do that.

Linux privilege-drop tests belong to the native `lifetimes` suite. The two syscall/identity cases
skip unless the test runner is already root, just as their privilege checks require. Ordinary test
runs don't request elevation. On a disposable Linux machine, an existing root session can select
them directly:

```sh
lockd_test --run_test=lifetimes/privilege_drop_failures_prevent_daemon_startup
lockd_test --run_test=lifetimes/real_privilege_drop_removes_root_groups_and_preserves_pid_parent
```

Use the built or installed executable's path. In dynamically linked Linux builds, keep
`lockd_startup_failures.so` beside it; CMake builds and installs this fault-injection fixture with
the native tests. Failed `fork` handling runs as a regular user. Fault injection is confined to each
test's child process. Fully static daemon builds omit the two fault-injection cases because
`LD_PRELOAD` cannot intercept their libc calls. The real privilege-drop case remains available.

## What CI runs

Compatibility checks run on pushes and pull requests across the [platform matrix](COMPATIBILITY.md).

| Category                        | Checks                                                                        |
|---------------------------------|-------------------------------------------------------------------------------|
| Package test source             | Ruff, Python release-tool tests, native build checks, and source packaging    |
| clang-tidy / Linux and Windows  | Strict analysis of configured native and Python sources                       |
| Compatibility Testing - Windows | x86/x86_64/ARM64 static/shared builds, CTest, ZIPs, and installers            |
| Service Testing - Windows       | Daemon service options using each architecture's portable package             |
| Python - Linux                  | Build, repair, and test manylinux wheels for each Python ABI and architecture |
| Python - Windows                | Build and test each Python wheel against packaged Windows binaries            |
| Compatibility Testing - Linux   | Optimized shared builds, unchanged rebuilds, CTest, wheel tests               |
| Compatibility Testing - FreeBSD | Optimized shared builds, unchanged rebuilds, CTest in two VMs                 |

Release reuses successful checks and tested artifacts for the same commit and version. Missing or
expired results are rebuilt and tested before publication. After release assembly passes, Release
also uploads the validated wheels as `pylockd`. See [publishing
wheels](CONTRIBUTING.md#publishing-wheels-to-pypi).

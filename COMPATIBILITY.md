# Compatibility

lockd supports and is tested against native Windows, major Linux distributions, Raspberry Pi OS on
ARM64, and FreeBSD. WSL uses the build for its Linux distribution. See [BUILDING.md](BUILDING.md)
for setup and [TESTING.md](TESTING.md#what-ci-runs) for test coverage.

## Tested platforms

| System           | Versions tested         | Environment                                                           |
|------------------|-------------------------|-----------------------------------------------------------------------|
| Debian           | 13                      | Distribution container                                                |
| Ubuntu           | 24.04, 26.10            | Distribution containers                                               |
| Fedora           | 43, 44                  | Distribution containers                                               |
| CentOS Stream    | 10                      | Distribution container                                                |
| Rocky Linux      | 10                      | Distribution container                                                |
| openSUSE Leap    | 16.0                    | Distribution container                                                |
| Arch Linux       | Current rolling release | Distribution container                                                |
| Raspberry Pi OS  | Trixie (64-bit Lite)    | Official image user space on an ARM64 runner                          |
| FreeBSD          | 14.4, 15.1              | amd64 FreeBSD VMs                                                     |
| Windows[^cygwin] | Server 2022, Windows 11 | x86/x86_64 on Server 2022; ARM64 on Windows 11; Release static/shared |

[^cygwin]: Cygwin is experimental and unsupported; use native Windows builds instead.

Linux and FreeBSD use shared Release builds. Windows tests static and shared Release builds for x86,
x86_64, and ARM64, including the daemon, client libraries, DLL, and native test suite.

Windows reserves each daemon's TCP port across all local addresses and both IP families. Separate
daemons must use different ports, even when listening on different local IPs. This prevents
competing lockd listeners while retaining immediate restart after forced termination, including when
old client sockets remain open. POSIX uses its native TCP binding rules.

Python release wheels cover CPython 3.11 through 3.15 on Windows and Linux, each on x86, x86_64, and
ARM64. Windows ARM64 wheels run their Python tests against the native ARM64 portable daemon.
Free-threaded Python and FreeBSD wheels aren't included.

## Coverage limits

Linux containers test distribution user space on the runner's kernel. Raspberry Pi OS checks run its
64-bit user space on an ARM64 runner, not Pi hardware or its kernel. FreeBSD runs in VMs. Windows
native CI uses Server 2022 for x86/x86_64 and Windows 11 for ARM64. Not every Windows release is
tested.

The matrix targets the oldest and newest normally supported distribution releases that meet the
build requirements: CMake 3.24+, Boost 1.82+, and a C++17 compiler. Rolling distributions have one
current entry. Older systems may need newer dependencies and aren't covered by this matrix. While
not officially supported, other systems, including older or newer systems, may still build and pass
tests.

## Python support

lockd officially supports CPython 3.11 through 3.15 on x86, x86_64, and ARM64 running across the
supported system compatibility matrix. Python 3.10 reached EOL on October 1st, 2026. Older or newer
versions may still build and pass tests; lockd was originally developed for CPython 2.7.

### Portable Linux wheels

Release wheels use the `manylinux_2_28` baseline and bundle Boost `python` and the C++ client. They
don't require a system Boost installation. Python and standard system runtime libraries remain
external.

A wheel built locally isn't automatically portable to other machines. It may require shared
dependencies from the build machine. Inspect and repair them with `auditwheel` on Linux or
`delvewheel` on Windows. Build separately for each Python version and architecture.

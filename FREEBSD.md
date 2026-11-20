# FreeBSD ports and service

`net/lockd` provides the daemon, C++ and C client SDKs, and optional native tests. Both client
libraries are included in the daemon package with the default `LIBRARIES` option enabled.
`devel/py-pylockd` provides the Python bindings and can be installed independently of the daemon.

Run package installation and service commands as root. The paths below assume the default
`/usr/local` prefix; adjust them if the ports were installed with a different `PREFIX`.

## Install packages

Install the daemon and its default client SDK:

```sh
pkg install lockd
```

The `lockd` package includes the C++ client library and its C wrapper, along with their public
headers and CMake package files. Applications can use these clients with a remote server without
enabling or running the installed daemon.

To install Python bindings separately, choose the package matching the application's interpreter.
For Python 3.12:

```sh
pkg install py312-pylockd
```

Binary packages use the options selected by their package repository. Build from the ports tree to
choose different options, including installation of the native test executable. The daemon's install
message includes service instructions and, when tests are installed, their location and invocation.
Display that message again with `pkg info -D lockd`.

## Install from ports

With an up-to-date ports tree, configure and install the daemon:

```sh
cd /usr/ports/net/lockd
make config
make install clean
```

`make showconfig` displays selected options. See [Port options and tests](#port-options-and-tests)
for option details and Python version selection. Installation registers the package with `pkg` and
creates the `distlockd` user and group for the service.

The default SDK installs headers under `/usr/local/include/liblockd/` and
`/usr/local/include/liblockd_dll/`, libraries under `/usr/local/lib/`, and CMake package files under
`/usr/local/lib/cmake/lockd/`. C++ applications can include `<liblockd/liblockd.hpp>` and use
`find_package(lockd CONFIG REQUIRED)`. See [USAGE.md](USAGE.md) for the C++ interface and
[DLL.md](DLL.md) for the C wrapper.

## Service configuration

Installation does not enable or start the service. Enable it at boot and start it now:

```sh
sysrc distlockd_enable=YES
service distlockd start
```

Check its status with `service distlockd status`. After changing settings in `/etc/rc.conf`,
apply them with `service distlockd restart`; use `service distlockd stop` to stop it.

These are the service defaults, which can be overridden in `/etc/rc.conf`:

```sh
distlockd_interface="localhost"
distlockd_port="7198"
distlockd_log_level="info"
distlockd_user="distlockd"
distlockd_group="distlockd"
distlockd_pidfile="/var/run/distlockd/lockd.pid"
distlockd_logfile="/var/log/distlockd/lockd.log"
distlockd_flags=""
```

Use `distlockd_flags` for additional daemon options, without repeating those managed by the
settings above. For example, `distlockd_flags="--threads 4"` selects four worker threads. Without
an explicit count, the daemon uses the available hardware threads, capped at eight, or one if the
count is unknown. See [RUNNING.md](RUNNING.md) for logging and other daemon options.

The daemon starts as root, prepares its runtime paths, creates missing log directories, and drops
privileges to the configured user and group. Existing directories retain their ownership and
permissions. When changing accounts, update existing directory ownership as needed.

The default listener accepts only local connections. To allow remote clients, set
`distlockd_interface` to the appropriate local address and permit the chosen TCP port through the
firewall for trusted clients. Deployment requirements are described in [SECURITY.md](SECURITY.md).

## Port options and tests

Select options with `make config` in `/usr/ports/net/lockd`:

- `LIBRARIES` (on by default) includes the shared C++ and C client libraries, public headers, and
  CMake package files in the daemon package. Disabling it embeds the client in the daemon and omits
  the SDK files.
- `PYTHON` (off by default) installs the matching `devel/py-pylockd` package as a dependency.
- `STATIC` (off by default) links the daemon and installed native test executable fully statically,
  including Boost and system runtimes. With `LIBRARIES` enabled, the SDK still provides shared
  libraries and registers them with ldconfig. Optional components keep their own dependencies.
- `TESTS` (off by default) builds and installs `/usr/local/tests/lockd/lockd_test`. It controls
  whether the tests are installed, not whether they can be run from the ports tree.

`STATIC` does not change the linkage of the separately packaged Python bindings.

### Python version selection

The daemon port is unflavored. To enable its Python dependency and select Python 3.12:

```sh
cd /usr/ports/net/lockd
make WITH=PYTHON DEFAULT_VERSIONS=python=3.12 install clean
```

The installed packages are `lockd` and `py312-pylockd`. Replace `3.12` with another supported Python
version as needed. `DEFAULT_VERSIONS` selects versions for that make invocation; to retain the
preference for future ports builds, add `DEFAULT_VERSIONS+=python=3.12` to `/etc/make.conf`.

The Python port itself uses Python flavors. To install its Python 3.12 variant independently:

```sh
cd /usr/ports/devel/py-pylockd
make FLAVOR=py312 install clean
```

This package contains the Python extension and its embedded C++ client. It does not install or
require a local daemon during normal use. See [PYTHON.md](PYTHON.md) for the Python API.

### Run native tests from the ports tree

Run the native test suite before or after installation:

```sh
cd /usr/ports/net/lockd
make test
```

`make test` builds and runs the suite even when `TESTS` is disabled. It does not require the test
executable to be installed. Keep the same option and `DEFAULT_VERSIONS` settings when testing a
custom build.

### Test an installed package

Enable `TESTS` when building `net/lockd` to install the native test executable. As a regular user,
run it from a writable working directory:

```sh
cd ~
/usr/local/tests/lockd/lockd_test --report_level=short
```

The tests start their own daemon instances and can run while the system service is running. The
protocol tests use loopback port 27198 by default, which must be available. To use another port:

```sh
/usr/local/tests/lockd/lockd_test --report_level=short -- --port 27199
```

The executable is outside PATH, following FreeBSD's installed test-suite layout. See
[TESTING.md](TESTING.md) for test groups and other runner options.

### Run Python tests

Run the Python unit tests from their port directory, using the same interpreter selection as the
package being tested:

```sh
cd /usr/ports/devel/py-pylockd
make FLAVOR=py312 test
```

The tests import the staged extension, so pylockd does not need to be installed first. Testing adds
`net/lockd` as a test-only dependency and launches isolated daemon instances on loopback ports. It
does not start or use the system service.

After installing the Python package, check that the selected interpreter can import it:

```sh
python3.12 -c 'import pylockd'
```

## Upgrades and removal

For repository packages, use `pkg upgrade lockd` and restart the service afterward. For a customized
ports build, update through the same ports workflow with its selected options and Python version.

To remove the daemon package, stop and disable its service first:

```sh
service distlockd stop
sysrc distlockd_enable=NO
pkg delete lockd
```

Logs, administrator settings in `/etc/rc.conf`, and the service user and group remain after package
removal. Remove them separately when they are no longer needed. The Python bindings are a separate
package and can remain installed for applications using a remote daemon.

## Host failover with CARP

CARP can keep lockd available at one IP address when its primary server becomes unavailable. Run a
daemon on each of two FreeBSD hosts and give them a shared virtual IP. CARP directs traffic to the
current master; the backup takes over that address if the master stops advertising. Clients continue
using the same endpoint, without a DNS or application configuration change. See the [FreeBSD CARP
guide](https://docs.freebsd.org/en/books/handbook/advanced-networking/#carp).

For example, use two hosts on the same private network:

| Role                  | Address              |
|-----------------------|----------------------|
| Primary host          | `192.168.50.11`      |
| Backup host           | `192.168.50.12`      |
| Shared lockd endpoint | `192.168.50.10:7198` |

### Configure the shared address

Install lockd on both hosts. On each, enable CARP in `/boot/loader.conf`:

```sh
carp_load="YES"
```

Load the module with `kldload carp` if it isn't already loaded. Configure the network in
`/etc/rc.conf`, adapting the existing interface settings. These examples use `em0`, an unused alias
slot, and virtual host ID (VHID) 42; choose an unused VHID on your network. Replace `CHANGE_ME` with
the same private CARP password on both hosts.

On the primary:

```sh
ifconfig_em0="inet 192.168.50.11/24"
ifconfig_em0_alias0="inet vhid 42 advskew 0 pass CHANGE_ME alias 192.168.50.10/32"
```

On the backup:

```sh
ifconfig_em0="inet 192.168.50.12/24"
ifconfig_em0_alias0="inet vhid 42 advskew 100 pass CHANGE_ME alias 192.168.50.10/32"
```

The lower `advskew` makes the primary preferred. Keep preemption disabled on both hosts so a
recovered primary doesn't cause another handover. The default is `net.inet.carp.preempt=0`; set this
in `/etc/sysctl.conf` if needed. Firewall rules must allow CARP advertisements (IP protocol 112)
between the hosts, and TCP port 7198 to the shared IP from trusted clients. The [carp (4)
manual](https://man.freebsd.org/cgi/man.cgi?query=carp&sektion=4) describes election settings and
state-change hooks.

### Run lockd on both hosts

Add the same service settings to `/etc/rc.conf` on both:

```sh
distlockd_enable="YES"
distlockd_interface="192.168.50.10"
distlockd_port="7198"
```

Configure the CARP address before starting either daemon, including on the backup. Binding
specifically to the shared IP keeps clients from reaching independent lock tables through the hosts'
management addresses. Apply the network changes through a console or a planned reboot, then start or
restart `distlockd` on both hosts.

On each host, inspect `ifconfig em0`: one should report `MASTER` for VHID 42 and the other `BACKUP`.
From a separate client machine, check the shared listener:

```sh
nc -vz -w 2 192.168.50.10 7198
```

Test host failover during a maintenance window or in a test environment by taking the master
offline. Confirm the backup becomes `MASTER` and new connections to the same endpoint succeed. Test
application recovery with an actual lock acquisition too; a TCP connection check only confirms that
the listener is reachable.

Client recovery follows the existing [connection-failure contract](USAGE.md#connection-failures).
Connections and in-memory locks aren't transferred; applications handle interrupted operations as
they would after a daemon restart. CARP handles host availability; detecting a failed daemon on an
otherwise healthy host requires separate service monitoring.

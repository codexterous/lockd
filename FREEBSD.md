# FreeBSD ports and service

Use `net/lockd` for the daemon, C++ client SDK, and native tests. Use `devel/py-pylockd` for the
Python binding; it does not require a local daemon. For a build outside the ports tree, see
[BUILDING.md](BUILDING.md).

While FreeBSD is the only officially supported BSD, other BSDs such as OpenBSD and NetBSD should
also work. If you encounter an issue compiling, running, or testing lockd or the client libraries on
a different BSD, please submit an issue.

## Install from ports

With the ports tree available, build and install the daemon as root:

```sh
cd /usr/ports/net/lockd
make config
make install clean
```

Install the Python binding separately if needed:

```sh
cd /usr/ports/devel/py-pylockd
make install clean
```

## Service configuration

Enable and start the service as root:

```sh
sysrc lockd_daemon_enable=YES
service lockd_daemon start
```

Override defaults in `/etc/rc.conf` as needed:

```sh
lockd_daemon_interface="127.0.0.1"
lockd_daemon_port="7198"
lockd_daemon_log_level="info"
lockd_daemon_user="lockd_daemon"
lockd_daemon_group="lockd_daemon"
lockd_daemon_pidfile="/var/run/lockd_daemon/lockd.pid"
lockd_daemon_logfile="/var/log/lockd_daemon/lockd.log"
```

These are the defaults. Use `lockd_daemon_flags` for additional options only, without repeating
options managed by the settings above. The daemon starts as root and drops privileges to the
configured user and group after preparing its files.

The port uses `/var/run/lockd_daemon/lockd.pid` and `/var/log/lockd_daemon/lockd.log`. Override
these with `lockd_daemon_pidfile` and `lockd_daemon_logfile`, respectively. The startup script
creates the default log directory with mode 0750 and the configured user and group as owners when
absent; lockd prepares the PID directory itself. Existing directories are left unchanged.
Administrators choosing another log path must prepare its directory and permissions before starting
the service. When changing accounts, update existing directory ownership as needed.

## Port options and tests

Run `make config` in `net/lockd` to select installation options:

- `LIBRARIES` (on by default) installs the C++ client and C wrapper libraries, public headers, and
  CMake package files. Disabling it embeds the client in the daemon and omits the development files.
- `STATIC` (off by default) links the daemon and native tests fully statically, including Boost and
  system runtimes. With `LIBRARIES` enabled, the SDK contains static archives instead of shared
  libraries. Boost is required only while building a static package.
- `TESTS` (off by default) builds and installs `tests/lockd/lockd_test` under `${PREFIX}`, following
  FreeBSD's installed test suite layout.

`make test` builds and runs the native test suite regardless of the `TESTS` option. Run installed
tests from a writable working directory, for example:

```sh
/usr/local/tests/lockd/lockd_test --report_level=short
```

The tests launch their own daemon instances. The protocol tests default to port 27198, which must be
unused; append `-- --port <port>` to choose another port.

`devel/py-pylockd` builds a wheel containing the Python extension and embedded C++ client library.
It neither installs nor requires the daemon. Select the Python flavor when building and installing,
for example `make install FLAVOR=py312`.

Run `make test FLAVOR=py312` in the Python port to run the Python unittest suite against the staged
extension, without installing pylockd first. Testing adds `net/lockd` as a test-only dependency. The
tests launch isolated daemon instances on loopback ports; they do not start or use the system
service.

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
lockd_daemon_enable="YES"
lockd_daemon_interface="192.168.50.10"
lockd_daemon_port="7198"
```

Configure the CARP address before starting either daemon, including on the backup. Binding
specifically to the shared IP keeps clients from reaching independent lock tables through the hosts'
management addresses. Apply the network changes through a console or a planned reboot, then start or
restart `lockd_daemon` on both hosts.

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

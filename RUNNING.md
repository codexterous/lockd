# Running lockd

Run one daemon for the clients that need to share named locks. It listens on `localhost:7198` by
default. Use a trusted, firewalled interface when clients are on other machines; there is no
authentication or TLS. See [SECURITY.md](SECURITY.md).

## Start locally

On Linux or FreeBSD, a foreground development run needs neither root nor a dedicated service
account:

```sh
lockd --no-fork --no-drop
```

On Windows, run `lockd.exe` for console operation. For a managed service, use the [Windows installer
or service options](WINDOWS.md). FreeBSD service setup is in [FREEBSD.md](FREEBSD.md).

## Program options

`lockd --help` shows the options available on the current platform and their defaults. Common
options are:

| Option              | Meaning                                                               |
|---------------------|-----------------------------------------------------------------------|
| `--interface`, `-i` | Local IPv4/IPv6 address or hostname; default `localhost`              |
| `--port`, `-p`      | TCP listening port; default 7198                                      |
| `--threads`, `-t`   | Worker count; defaults to hardware threads, capped at 8; 1 if unknown |
| `--log-level`, `-l` | Minimum severity; default `info`                                      |
| `--log-file`, `-f`  | Log file path; platform defaults are described below                  |
| `--log-rotate-size` | Rotation size in MiB; default 10, or 0 to disable automatic rotation  |
| `--log-rotate-keep` | Number of rotated log files to retain; default 5                      |
| `--version`, `-v`   | Print version and exit                                                |
| `--help`, `-h`      | Print help and exit                                                   |

Worker counts must be between 1 and 65535. Ports must be between 0 and 65535; zero lets the
operating system choose an available port.

Automatic selection preserves smaller hardware-thread counts, such as four or six. An explicit
`--threads` value overrides the automatic cap and may exceed eight.

The daemon and `lockd_test` accept an IPv4 address, IPv6 address, or hostname for `--interface`
(`-i`), defaulting to `localhost`. For example, use `lockd -i localhost` or
`lockd_test -- -i localhost`. Hostnames resolve at startup, and the daemon listens on the first
returned address, which may be IPv6 (`::1`) for `localhost`.

## Logging

Levels are `trace`, `debug`, `info`, `warning`, `error`, and `fatal`, also accepted as numbers 0
through 5. The threshold includes that level and everything above it. Quote paths containing spaces.
`-l` selects the level; `--log-file` selects the file.

Verbose logging can substantially reduce throughput, especially `trace`, which logs individual lock
and connection activity. Waiting for log I/O can increase latency while lowering CPU utilization.
Use verbose levels for diagnosis and keep the logging level consistent when comparing performance
measurements.

Automatic rotation defaults to 10 MiB with five archives, named `<log-file>.0`, `<log-file>.1`, and
so on, with increasing numbers. Set `--log-rotate-size=0` to disable automatic rotation and
retention; file logging continues, and `--log-rotate-keep` is ignored. The archive count must be
positive.

Rotation is synchronous and can briefly delay client handling. On POSIX, external rotation can move
archiving and compression outside of lockd if desired. Windows service [Event Log
settings][windows-event-log] are independent of file rotation.

Ordinary invalid commands aren't logged. Oversized commands and read/write deadline violations are
logged as errors, including at the default `info` level.

### External rotation on POSIX

Run lockd with `--log-rotate-size=0` and configure the system's rotation tool to rename the log and
send `SIGUSR1`. lockd always handles this signal, including with automatic rotation enabled; it
reopens `--log-file` without interrupting held locks. Choose one rotation policy for each log.

#### Linux

For Linux with systemd, add the option to your service's `ExecStart`. This example
`/etc/logrotate.d/lockd` assumes a `lockd.service` unit and a `lockd` user and group; adjust them to
your installation:

```text
/var/log/lockd/lockd.log {
    daily
    maxsize 10M
    rotate 5
    missingok
    notifempty
    compress
    delaycompress
    su lockd lockd
    create 0640 lockd lockd
    postrotate
        if systemctl --quiet is-active lockd.service; then
            systemctl kill --kill-whom=main --signal=USR1 lockd.service
        fi
    endscript
}
```

Ensure `logrotate.timer` (or the distribution's cron job) is enabled. Size checks happen when
logrotate runs; use a more frequent schedule if needed. See [logrotate (8)][logrotate] and
[systemctl (1)][systemctl].

#### FreeBSD

For the FreeBSD port, add `--log-rotate-size=0` to `lockd_daemon_flags` in `/etc/rc.conf`. With the
port's default paths and account, add this entry to `/usr/local/etc/newsyslog.conf.d/lockd.conf`:

```text
/var/log/lockd_daemon/lockd.log lockd_daemon: 640 5 10M * B /var/run/lockd_daemon/lockd.pid SIGUSR1
```

The system's scheduled newsyslog run handles rotation. See [newsyslog.conf (5)][newsyslog] for
time-based rotation and compression options.

[logrotate]: https://github.com/logrotate/logrotate/blob/main/logrotate.8.in
[systemctl]: https://www.freedesktop.org/software/systemd/man/252/systemctl.html
[newsyslog]: https://man.freebsd.org/cgi/man.cgi?query=newsyslog.conf&sektion=5
[windows-event-log]: WINDOWS.md#event-logging

## POSIX background operation and files

Use `--user` and `--group` to choose the account after startup. `--no-drop` keeps the invoking
identity; `--no-fork` stays in the foreground. `--pid-file` selects the PID file; `--log-file`
selects the log file.

```sh
lockd --no-fork --no-drop -P ./lockd.pid -f ./lockd.log
```

On POSIX systems, privilege-dropping daemon startup defaults to `nobody` when that account exists.
Otherwise `--user` is required. An omitted `--group` uses the selected user's primary group;
`--help` displays the default account's group when available. `--help`, `--version`, and `--no-drop`
do not require a target account.

The default PID file is `/run/lockd/lockd.pid` when `/run` exists, otherwise
`/var/run/lockd/lockd.pid`. The default log is `/var/log/lockd/lockd.log`. Startup creates the
daemon's directories before dropping privileges and assigns only newly created directories to the
selected account. Existing directories and explicit log paths must already permit that account to
traverse directories and write files.

With `--no-drop`, default log and PID paths use the working directory. Foreground mode (`--no-fork`)
writes a PID file only when `--pid-file` is explicitly supplied. Background startup returns success
only after the daemon is ready to serve clients; startup failure removes any PID file owned by that
child.

Startup can reclaim a stale PID file when its recorded process no longer exists. It leaves live,
malformed, and inaccessible files untouched and reports the problem. Keep PID files on a local
filesystem in the daemon's PID namespace.

## Graceful shutdown

On POSIX systems, `SIGINT` and `SIGTERM` request a graceful shutdown. The daemon stops accepting
connections and lock acquisitions, closes idle and queued clients, and gives current lock holders up
to 30 seconds in total to release their locks or disconnect. Successful releases receive
`LOCK_RELEASED` before their connections close. The daemon exits as soon as those clients finish;
after 30 seconds it closes remaining connections and releases their locks. Repeated stop requests do
not extend the deadline. A Windows service stop uses the same drain period.

A graceful POSIX exit removes the daemon's PID file. `SIGKILL` cannot be handled: it skips the grace
period and cleanup, and may leave a stale PID file.

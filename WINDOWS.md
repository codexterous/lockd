# lockd on Windows

Release downloads include an installer and a portable ZIP for x86, x86_64, and ARM64. Use the ZIP
for console operation or manual deployment, or the NSIS installer when you want installation and
optional service setup in one place. The ARM64 package contains native ARM64 binaries; its NSIS
setup program runs through Windows' x86 emulation.

For source builds and installer packaging, see [Building on Windows](BUILDING.md#windows).

## Portable download

Extract all files into one directory. Run `lockd.exe` to start the console daemon, or
`lockd_test.exe` to test the package. Keep the test executable beside the daemon. The portable
executables use static Boost and compiler runtimes, while still depending on Windows system
libraries.

Only one lockd daemon can use a TCP port on a Windows machine, across all local addresses and
IPv4/IPv6. Each instance holds a non-inheritable named kernel-object handle until shutdown finishes;
Windows releases it after a crash or forced termination too. This keeps immediate port reuse without
allowing another lockd instance to share the listener. It coordinates lockd instances, rather than
preventing unrelated programs from using Windows socket reuse themselves.

The downloads are currently unsigned. Windows may display an unknown-publisher warning. Check that
the download came from the project's release page and compare it with the published `SHA256SUMS`.

Executable paths, log paths, and command-line service names support Unicode on Windows. Paths in
redirected console output and log files are encoded as UTF-8.

## Installer

The installer offers files-only installation by default. Enabling the service exposes its name,
listening address, TCP port, worker count, log path and level, Windows Event Log output, automatic
startup, and immediate startup. Defaults match the daemon: loopback, port 7198, automatic worker
count, and information logging. Leaving worker count empty selects the hardware-thread count, capped
at eight, or one if unknown; an explicit count may exceed eight. The default log is
`%ProgramData%\lockd\lockd.log`. Services run as LocalSystem, matching the existing
`lockd --install-service` command. The installer does not change PATH.

**Allow connections on private networks** is unchecked by default. Selecting it adds an inbound TCP
rule restricted to the installed `lockd.exe`, the chosen port, and Windows' **Private** network
profile. The option is disabled and unchecked for `localhost` and loopback IP addresses, which don't
need a rule. Private refers to Windows' network classification, not the client's IP range; Domain
and Public profiles aren't included. Remote access also requires a non-loopback listening address.

The installer requires administrator privileges and Windows 10 or later. It includes the same
executables and licenses as the portable ZIP. Upgrades preserve service settings. Existing services
retain their command, account, and startup settings, and are restarted only if they were running
before the upgrade. Installer-created firewall rules are preserved too. A service pointing to
another executable is not modified. Downgrades require uninstalling the newer version first.

x86, x86_64, and ARM64 packages have separate installation registrations. Use separate installation
directories, service names, and listening ports when installing more than one architecture.

### Uninstall and cleanup

**A successful uninstall removes lockd's installed files and installer-owned system configuration:**
its service, installer-created firewall rule, Event Log source registration, and uninstall registry
entry. The installer leaves PATH alone and does not install shared runtimes. Windows-maintained
history and caches are outside this contract; Windows Event Log history is preserved.

Logs are kept by default. Interactive uninstall offers to delete the installer-recorded log files
and their numbered archives, with **No** selected by default. Silent uninstall also preserves logs
unless explicitly requested:

```powershell
& 'C:\Program Files\lockd\uninstall.exe' /S /RemoveLogs=1
```

Only the recorded active log paths and archives named `<log-file>.<number>` are removed. Archives
using other naming schemes (including compressed archives) and logs at paths configured separately
after installation are preserved. Uninstall never empties a whole log directory.

The installer records newly created directories and removes them only when empty. Pre-existing
directories, pre-existing Event Log sources, unrelated files, and other firewall rules are
preserved. Upgrades retain these ownership records.

If ownership has changed, a recorded path has become a junction or symbolic link, or cleanup fails,
interactive uninstall offers **Retry / Cancel**. Resolve the problem and choose Retry; completed
removals are skipped and ownership is checked again. Cancel leaves the uninstall registration and
uninstaller available for a later attempt. Silent uninstall reports failure with a nonzero exit code
and never waits for a response. Failed installations attempt to roll back their changes and report
any incomplete cleanup; diagnostic logs may remain.

## Installing a service from the command line

Install a service with its listening address, port, and logging options:

```powershell
lockd.exe -S -n lockd -i localhost -p 7198 -l warning -f "C:\Logs\lockd.log"
```

`-S` is `--install-service`; `-n` is `--service-name`. Installation and removal require
administrator privileges. lockd requests UAC approval if needed; for unattended use, start from an
elevated terminal. If an elevated operation fails, rerun it from an administrator terminal to see
the detailed diagnostics.

Installation (`-S`) and removal (`-U`) cannot be combined.

Repeat `-S` with the same service name to replace its settings. The existing service must point to
the same executable. lockd stops it, removes its registration, waits for deletion to finish, then
installs it with the supplied options. The replacement uses fresh-install defaults: **Manual**
startup, **LocalSystem**, and stopped. Custom accounts, recovery actions, and other SCM settings
must be reapplied. Supply every desired daemon option; omitted options use their normal defaults.
For example, change the port and worker count:

```powershell
lockd.exe -S -n lockd -i localhost -p 7200 -t 4 -l warning -f "C:\Logs\lockd.log"
Start-Service lockd
```

This command does not change firewall rules; adjust those separately when changing the port. The
service account needs write access to the log directory. Without `--log-file`, service installation
and `--as-service` use `%ProgramData%\lockd\lockd.log`; lockd creates the `lockd` directory if
needed. `--install-service --help` and `--as-service --help` show the resolved default. Console runs
use `lockd.log` in the current directory. An explicit `--log-file` overrides either default; its
parent directory must already exist.

## Removing a service

```powershell
lockd.exe --uninstall-service
```

This removes the service whose executable path matches the running `lockd.exe`.

## Event logging

Services write to both the log file and the Windows Application event log. The event source is
`lockd.<service-name>`. Install with `--no-event-log` (`-E`) to use file logging alone:

```powershell
lockd.exe -S -n lockd -f "C:\Logs\lockd.log" -E
```

Ordinary events respect the selected log level. Service startup and shutdown always produce
Information events when Event Log output is enabled. Console runs don't write to the Windows Event
Log.

Keep the installed executable at its registered path so Event Viewer can display messages. If
changing the service account from LocalSystem, provide permission to register the event source on
first startup as well as access to the log file.

## Unattended installation

Silent installation uses `/S`; files-only installation remains the default. Service options are
`/Service=1`, `/ServiceName=lockd`, `/Address=127.0.0.1`, `/Port=7198`, `/Threads=4`,
`/LogLevel=info`, `/LogFile=C:\logs\lockd.log`, `/EventLog=1`, `/Automatic=1`, and `/Start=1`. Add
`/Firewall=1` to allow Private-network connections for a non-loopback service listener; the default
is `/Firewall=0`. Selecting it with a loopback address or without a service is rejected. Boolean
options accept 0 or 1. Omit `/Threads` for automatic selection. Quote values containing spaces, for
example `/LogFile="C:\service logs\lockd.log"`.

NSIS' optional `/D=C:\path\to\lockd` must come last and must not be quoted. Existing service
settings are preserved during silent upgrades too.

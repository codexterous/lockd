# Security

lockd is intended for trusted clients on a private network. It has no authentication, authorization,
or TLS. Bind it to loopback when possible and use firewall rules to restrict remote access. It's not
designed to be exposed to the public Internet, and you shouldn't do so.

## Deployment boundaries

Any client that can connect can request locks by name. Lock names are not secrets or access
controls, and a client can hold a lock indefinitely. An acquisition timeout limits how long a waiter
waits; it does not expire the owner's lock.

Run with only the privileges needed for its files and listening socket. POSIX startup can drop
privileges to a selected account. Windows services default to LocalSystem; administrators choosing
another account must provide the required file and Event Log permissions. See
[RUNNING.md](RUNNING.md) and [WINDOWS.md](WINDOWS.md).

## Reporting a vulnerability

Please report suspected vulnerabilities privately to `lockd@codexterous.com`. Include the affected
version, platform, reproduction steps, and likely impact. Don't include credentials or sensitive
production data, and don't post exploit details in a public issue before we've had a chance to
investigate.

Test the latest release when possible and note whether it is affected.

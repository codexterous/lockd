"""Build once or supervise one Linux benchmark process over a persistent SSH session."""

import argparse
import contextlib
import hashlib
import ipaddress
import json
import os
import platform
import queue
import select
import shlex
import shutil
import signal
import socket
import subprocess
import sys
import tarfile
import threading
import time
from collections import Counter
from collections.abc import Callable
from pathlib import Path, PurePosixPath

BARRIER_CONNECT_TIMEOUT_SECONDS = 30
DAEMON_STOP_TIMEOUT_SECONDS = 35
PROFILE_FREQUENCY_HZ = 49
PROFILE_CONTROL_TIMEOUT_SECONDS = 30

# These are cumulative namespace counters, not per-process failures. Capture a
# baseline before launch so the report can show increases during this run.
TCP_COUNTERS = {
    'TcpExt': (
        'ListenOverflows',
        'ListenDrops',
        'TCPReqQFullDoCookies',
        'TCPReqQFullDrop',
        'SyncookiesSent',
        'SyncookiesRecv',
        'SyncookiesFailed',
        'EmbryonicRsts',
        'TCPBacklogDrop',
        'TCPMemoryPressures',
        'TCPAbortOnMemory',
        'TCPSynRetrans',
        'TCPTimeouts',
    ),
    'Tcp': (
        'ActiveOpens',
        'PassiveOpens',
        'AttemptFails',
        'EstabResets',
        'RetransSegs',
        'InErrs',
        'OutRsts',
    ),
}
KERNEL_LIMITS = (
    'net/core/somaxconn',
    'net/ipv4/tcp_max_syn_backlog',
    'net/ipv4/tcp_syncookies',
    'net/ipv4/tcp_synack_retries',
    'net/ipv4/tcp_abort_on_overflow',
    'net/netfilter/nf_conntrack_max',
    'fs/file-max',
)
TCP_STATES = {
    '01': 'ESTABLISHED',
    '02': 'SYN_SENT',
    '03': 'SYN_RECV',
    '04': 'FIN_WAIT1',
    '05': 'FIN_WAIT2',
    '06': 'TIME_WAIT',
    '07': 'CLOSE',
    '08': 'CLOSE_WAIT',
    '09': 'LAST_ACK',
    '0A': 'LISTEN',
    '0B': 'CLOSING',
    '0C': 'NEW_SYN_RECV',
}


def extract(archive: Path, destination: Path) -> None:
    """
    Extract regular files only, rejecting traversal, links, and unbounded archives.
    """
    destination.mkdir(parents=True, exist_ok=False)
    total = 0
    seen: set[str] = set()
    with tarfile.open(archive) as source:
        for index, member in enumerate(source):
            name = PurePosixPath(member.name)
            total += member.size
            if (
                name.is_absolute()
                or member.size < 0
                or '..' in name.parts
                or '\\' in member.name
                or member.name in seen
                or index >= 100000
                or total > 2 * 1024**3
                or not (member.isfile() or member.isdir())
            ):
                raise ValueError("Unsafe or oversized benchmark archive")
            seen.add(member.name)
            target = destination.joinpath(*name.parts)
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                with source.extractfile(member) as stream, target.open('xb') as output:
                    shutil.copyfileobj(stream, output)
                target.chmod(0o755 if member.mode & 0o111 else 0o644)


def execute(
    arguments: list[str],
    *,
    cwd: Path | None = None,
    env: dict | None = None,
    stream: bool = False,
    timeout: float = 1800,
) -> str:
    """Capture tool output for build provenance; never use a shell for arguments."""
    return (
        subprocess.run(
            arguments,
            cwd=cwd,
            env=env,
            text=True,
            stdout=None if stream else subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=True,
            timeout=timeout,
        ).stdout
        or ''
    )


def build(
    work: Path,
    expected_hash: str,
    *,
    profile: bool = False,
    require_async: bool = False,
) -> None:
    """Build the pinned source once and bundle its actual linked runtime libraries."""
    archive = work / 'source.tar.gz'
    with archive.open('rb') as stream:
        if hashlib.file_digest(stream, 'sha256').hexdigest() != expected_hash:
            raise ValueError("Uploaded source SHA-256 mismatch")
    os_release = Path('/etc/os-release').read_text()
    release = platform.freedesktop_os_release()
    if release['ID'] != 'ubuntu' or tuple(
        int(part) for part in release['VERSION_ID'].split('.')
    ) < (24, 4):
        raise ValueError("Benchmark provisioning requires Ubuntu 24.04 or newer")
    env = os.environ | {'DEBIAN_FRONTEND': 'noninteractive'}
    for command in (
        ['apt-get', 'update'],
        [
            'apt-get',
            'install',
            '-y',
            'build-essential',
            'cmake',
            'ninja-build',
            # Boost.System is header-only. Recent Ubuntu releases no longer ship
            # libboost-system-dev; the common headers cover both old and new Boost.
            'libboost-dev',
            'libboost-log-dev',
            'libboost-program-options-dev',
            'libboost-filesystem-dev',
            'libboost-random-dev',
            'libboost-test-dev',
        ],
    ):
        print('Running ' + shlex.join(command), flush=True)
        execute(command, env=env, stream=True)
    if profile:
        prepare_perf(work)
    extracted = work / 'source'
    extract(archive, extracted)
    roots = [path.parent for path in extracted.glob('*/CMakeLists.txt')]
    if len(roots) != 1:
        raise ValueError(
            "Expected one top-level source directory in the source archive"
        )
    root = roots[0]
    arch = {'aarch64': 'arm64', 'x86_64': 'x86_64'}[platform.machine()]
    build_dir = root / f'output/obj/{arch}/opt/shared'

    # Profiling keeps Release optimization. Debug information lets perf resolve
    # our C++ frames without changing code generation in the normal build.
    commands = [
        [
            'cmake',
            '-S',
            str(root),
            '-B',
            str(build_dir),
            '-G',
            'Ninja',
            '-DCMAKE_BUILD_TYPE=Release',
            '-DBUILD_SHARED_LIBS=ON',
            '-DLOCKD_STATIC_RUNTIME=OFF',
            '-DLOCKD_DEPENDENCY_PROVIDER=SYSTEM',
            '-DLOCKD_BUILD_PYTHON=OFF',
            '-DLOCKD_CLANG_TIDY=OFF',
            '-DBUILD_TESTING=ON',
            *(['-DCMAKE_CXX_FLAGS=-g'] if profile else []),
        ],
        [
            'cmake',
            '--build',
            str(build_dir),
            '--target',
            'lockd_test',
            '--parallel',
            str(os.cpu_count() or 1),
        ],
    ]
    for command in commands:
        print('Running ' + shlex.join(command), flush=True)
        execute(command, stream=True)
    bundle = work / 'bundle'
    bundle.mkdir()
    libraries = bundle / 'lib'
    libraries.mkdir()
    binaries = [
        root / f'output/{arch}/opt/shared/lockd',
        root / f'utility/lockd_test/output/{arch}/opt/shared/lockd_test',
    ]
    if require_async:
        # Reject an old source archive before creating any worker VMs. Execute the
        # built parser, since checking a version or source filename cannot establish
        # whether this binary accepts the selected transport.
        execute([str(binaries[1]), '--', '-B', '--engine=async', '--help'])
    for binary in binaries:
        shutil.copy2(binary, bundle / binary.name)
        listing = execute(['ldd', str(binary)])
        for line in listing.splitlines():
            words = line.split()
            if 'not found' in line:
                raise ValueError("Unresolved runtime dependency: " + line)
            if '=>' not in words:
                continue
            library = Path(words[words.index('=>') + 1])
            if library.name.split('.so')[0] in (
                'libc',
                'libm',
                'libdl',
                'libpthread',
                'librt',
                'libresolv',
            ):
                continue
            if library.is_absolute():
                shutil.copy2(library, libraries / library.name)
    provenance = {
        'source_sha256': expected_hash,
        'os_release': os_release,
        'architecture': arch,
        'compiler': execute(['c++', '--version']),
        'cmake': execute(['cmake', '--version']),
        'boost': execute(
            [
                'dpkg-query',
                '-W',
                'libboost-log-dev',
                'libboost-program-options-dev',
                'libboost-filesystem-dev',
            ]
        ),
        'version': (root / 'cmake/version.txt').read_text().strip(),
        'configure_commands': commands,
        'profile_daemon': profile,
        'perf_version': execute(['perf', 'version']) if profile else None,
    }
    (bundle / 'build.json').write_text(json.dumps(provenance, indent=2))
    with tarfile.open(work / 'bundle.tar.gz', 'w:gz', dereference=True) as target:
        target.add(bundle, arcname='bundle')


def prepare_perf(work: Path) -> None:
    """Install tools for the running kernel and test sampling before worker creation."""
    execute(
        ['apt-get', 'install', '-y', 'linux-tools-' + platform.release()],
        env=os.environ | {'DEBIAN_FRONTEND': 'noninteractive'},
        stream=True,
    )

    probe_perf(work)


def probe_perf(work: Path) -> None:
    """Exercise the real PID attachment, enable acknowledgement and report pipeline."""
    directory = work / 'perf-probe'
    directory.mkdir()
    profile = PerfProfile(directory)
    print('Checking perf attachment, enable acknowledgement and report...', flush=True)
    try:
        profile.start(os.getpid(), threading.Event())

        # Sampling an idle process can produce an empty recording that only fails
        # at report time. Burn half a CPU second after acknowledgement, with a
        # wall-clock bound for a VM that cannot make progress. cpu-clock is a
        # software event, so this does not require virtualized hardware counters.
        cpu_deadline = time.process_time() + 0.5
        deadline = time.monotonic() + 30
        while time.process_time() < cpu_deadline:
            if time.monotonic() >= deadline:
                raise TimeoutError('perf sampling probe exceeded 30s')
            sum(range(10000))
        profile.finish()
    except BaseException:
        with contextlib.suppress(OSError, RuntimeError, subprocess.SubprocessError):
            profile.finish()
        if (directory / 'perf.log').exists():
            print(
                (directory / 'perf.log').read_text(errors='replace')[-4096:], flush=True
            )
        raise
    print('perf attachment, enable acknowledgement and report passed.', flush=True)


class PerfProfile:
    """Own one daemon-only recorder, its control pipes and private profile artifacts."""

    def __init__(self, work: Path) -> None:
        self.work = work
        self.process: subprocess.Popen | None = None
        self.descriptors: list[int] = []
        self.log = (work / 'perf.log').open('w')
        self.finished = False
        self.metadata = {
            'status': 'not_started',
            'event': 'cpu-clock',
            'frequency_hz': PROFILE_FREQUENCY_HZ,
            'call_graph': 'dwarf,8192',
            'scope': 'daemon from launch-barrier release through workload completion',
        }

    def start(self, pid: int, cancelled: threading.Event) -> None:
        """Acknowledge sampling enablement before letting generators leave the gate."""
        if self.process is not None:
            raise RuntimeError("Daemon profiling has already started")
        self.metadata['status'] = 'incomplete'
        control_read, control_write = os.pipe()
        self.descriptors.extend((control_read, control_write))
        ack_read, ack_write = os.pipe()
        self.descriptors.extend((ack_read, ack_write))
        self.process = subprocess.Popen(
            [
                'perf',
                'record',
                '-e',
                'cpu-clock',
                '-F',
                str(PROFILE_FREQUENCY_HZ),
                '--call-graph',
                'dwarf,8192',
                '--pid',
                str(pid),
                '--delay=-1',
                f'--control=fd:{control_read},{ack_write}',
                '--output',
                str(self.work / 'perf.data'),
            ],
            pass_fds=(control_read, ack_write),
            stdin=subprocess.DEVNULL,
            stdout=self.log,
            stderr=subprocess.STDOUT,
        )
        for descriptor in (control_read, ack_write):
            os.close(descriptor)
            self.descriptors.remove(descriptor)
        os.write(control_write, b'enable\n')
        deadline = time.monotonic() + PROFILE_CONTROL_TIMEOUT_SECONDS

        # evlist__ctlfd_ack writes sizeof("ack\n"), including the C string's NUL.
        # Read the whole frame even when a pipe read splits off that final byte.
        # https://github.com/torvalds/linux/blob/master/tools/perf/util/evlist.c
        acknowledgement = b'ack\n\0'
        received = b''
        while received != acknowledgement:
            self.check()
            if cancelled.is_set():
                raise InterruptedError("Daemon profiling cancelled before release")
            if time.monotonic() >= deadline:
                raise TimeoutError("perf did not acknowledge sampling; see perf.log")
            if select.select([ack_read], [], [], 0.1)[0]:
                chunk = os.read(ack_read, len(acknowledgement) - len(received))
                received += chunk
                if not chunk or not acknowledgement.startswith(received):
                    raise RuntimeError(
                        f'Invalid perf acknowledgement {received!r}; see perf.log'
                    )
        self.metadata.update(start_unix=time.time(), start_monotonic=time.monotonic())

    def check(self) -> None:
        """Reject early recorder exit, including a clean exit before workload end."""
        if self.process is not None and self.process.poll() is not None:
            raise RuntimeError("perf exited before workload completion; see perf.log")

    def finish(self) -> None:
        """Flush and symbolize while the daemon and its mapped libraries still exist."""
        if self.finished:
            return
        self.finished = True
        try:
            if self.process is None:
                return
            self.check()
            self.process.send_signal(signal.SIGINT)
            result = self.process.wait(timeout=30)
            if result not in (0, -signal.SIGINT):
                raise RuntimeError(f"perf recording failed ({result}); see perf.log")
            self.metadata.update(stop_unix=time.time(), stop_monotonic=time.monotonic())
            if 'start_monotonic' not in self.metadata:
                return
            with (self.work / 'perf-report.txt').open('w') as output:
                subprocess.run(
                    [
                        'perf',
                        'report',
                        '--stdio',
                        '--stdio-color=never',
                        '--input',
                        str(self.work / 'perf.data'),
                        '--show-nr-samples',
                        '--show-cpu-utilization',
                        '--sort=dso,symbol',
                        '--call-graph=graph,0.5,caller',
                    ],
                    stdin=subprocess.DEVNULL,
                    stdout=output,
                    stderr=self.log,
                    check=True,
                    timeout=120,
                    env=os.environ | {'DEBUGINFOD_URLS': '', 'LC_ALL': 'C'},
                )
            self.metadata['status'] = 'completed'
        finally:
            if self.process is not None and self.process.poll() is None:
                self.process.kill()
                self.process.wait()
            for descriptor in self.descriptors:
                os.close(descriptor)
            self.descriptors.clear()
            self.log.close()
            self.metadata['artifacts'] = [
                name
                for name in ('perf.data', 'perf.log', 'perf-report.txt')
                if (self.work / name).exists()
            ]
            (self.work / 'profile.json').write_text(
                json.dumps(self.metadata, indent=2) + '\n', encoding='utf-8'
            )


class Barrier:
    """Use one connection-owned lock solely for orchestration, outside timed load."""

    def __init__(
        self,
        host: str,
        port: int,
        name: str,
        timeout: float,
        cancelled: threading.Event | None = None,
    ) -> None:
        self.cancelled = cancelled
        self.timeout = timeout
        deadline = time.monotonic() + min(timeout, BARRIER_CONNECT_TIMEOUT_SECONDS)
        while True:
            if cancelled is not None and cancelled.is_set():
                raise InterruptedError("Launch barrier cancelled before connection")
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(
                    f"Cannot connect to benchmark daemon at {host}:{port}; "
                    "check VPC interface state, routing and firewall rules"
                )
            try:
                self.socket = socket.create_connection(
                    (host, port), timeout=min(1, remaining)
                )
                break
            except OSError:
                # Newly configured guest networking can take time to converge.
                # Keep retries bounded and let controller loss cancel promptly.
                if cancelled is None:
                    time.sleep(min(0.25, remaining))
                else:
                    cancelled.wait(min(0.25, remaining))
        try:
            self.expect('LOCKD')
            self.socket.sendall(f'LOCK {name}\n'.encode('ascii'))
            self.expect('LOCK_ACQUIRING')
        except BaseException:
            self.close()
            raise

    def expect(self, expected: str) -> None:
        """Reject EOF, oversized lines, or unexpected protocol responses."""
        line = bytearray()
        deadline = time.monotonic() + self.timeout
        while not line.endswith(b'\n'):
            if self.cancelled is not None and self.cancelled.is_set():
                raise InterruptedError("Launch barrier cancelled")
            if time.monotonic() >= deadline:
                raise TimeoutError("Launch barrier deadline exceeded")
            try:
                byte = self.socket.recv(1)
            except TimeoutError:
                continue
            if not byte or len(line) >= 1024:
                raise RuntimeError("Truncated or oversized launch-barrier reply")
            line.extend(byte)
        if line.rstrip(b'\r\n') != expected.encode('ascii'):
            raise RuntimeError("Unexpected launch-barrier reply")

    def release(self) -> None:
        """Release explicitly so a failed handoff aborts orchestration."""
        self.socket.sendall(b'UNLOCK\n')
        self.expect('LOCK_RELEASED')

    def close(self) -> None:
        """Disconnect on every exit, releasing ownership or removing a queued waiter."""
        self.socket.close()


def process_stat(text: str) -> tuple[int, int]:
    """Read CPU ticks and process start time without splitting a spaced comm field."""
    fields = text[text.rindex(')') + 2 :].split()
    return int(fields[11]) + int(fields[12]), int(fields[19])


def kernel_network(proc: Path, net: Path, *, limits: bool = False) -> dict:
    """
    Read optional kernel diagnostics; unsupported or denied files stay unavailable.

    This agent is uploaded alone, so keep its proc readers here. Never tune the
    kernel or load modules to make diagnostics available. Missing counters must
    not look like zero drops, and monitoring must not abort a valid workload.
    """
    counters, gauges, unavailable = {}, {}, {}

    def read(name: str, path: Path, parser: Callable) -> object | None:
        try:
            return parser(path.read_text())
        except (OSError, ValueError, IndexError) as error:
            unavailable[name] = f'{type(error).__name__}: {error}'
            return None

    def paired(text: str, group: str) -> dict:
        lines = [line.split() for line in text.splitlines()]
        for index, fields in enumerate(lines):
            if fields and fields[0] == group + ':':
                values = lines[index + 1]
                if len(values) != len(fields) or values[0] != fields[0]:
                    raise ValueError('Mismatched kernel counter header and values')
                return dict(zip(fields[1:], map(int, values[1:]), strict=True))
        raise ValueError(f'Missing {group} counters')

    for group, filename in (('TcpExt', 'netstat'), ('Tcp', 'snmp')):
        values = read(
            group, net / filename, lambda text, group=group: paired(text, group)
        )
        if values is not None:
            for name in TCP_COUNTERS[group]:
                key = f'{group}.{name}'
                if name in values and values[name] >= 0:
                    counters[key] = values[name]
                else:
                    unavailable[key] = 'Counter unavailable'

    def sockstat(text: str) -> dict:
        fields = next(
            line.split()[1:] for line in text.splitlines() if line.startswith('TCP:')
        )
        if len(fields) % 2:
            raise ValueError('Unpaired TCP sockstat fields')
        return dict(zip(fields[::2], map(int, fields[1::2]), strict=True))

    # StopIteration is normalized here rather than escaping an optional reader.
    def tcp_gauges(text: str) -> dict:
        try:
            return sockstat(text)
        except StopIteration as error:
            raise ValueError('Missing TCP sockstat fields') from error

    values = read('sockstat', net / 'sockstat', tcp_gauges)
    if values is not None:
        # Preserve sockstat's units and scope. TCP.mem is kernel pages, and
        # orphan/alloc/mem are system-wide rather than namespace-only counts.
        gauges.update({f'TCP.{key}': value for key, value in values.items()})
    count = read(
        'nf_conntrack_count',
        proc / 'sys/net/netfilter/nf_conntrack_count',
        lambda text: int(text.strip()),
    )
    if count is not None:
        gauges['nf_conntrack_count'] = count
    result = {'counters': counters, 'gauges': gauges, 'unavailable': unavailable}
    if limits:
        result['limits'] = {}
        for name in KERNEL_LIMITS:
            value = read(name, proc / 'sys' / name, lambda text: int(text.strip()))
            if value is not None:
                result['limits'][name] = value
    return result


def tcp_snapshot(net: Path, sockets: set[str]) -> dict:
    """
    Count owned TCP states and sample each owned listener's accept queue.

    Linux exports sk_ack_backlog as rx_queue for LISTEN rows. Pending SYN rows
    have no inode, so count those by local family and port across the namespace;
    they aren't process-owned sockets. Reuse this scan for established counts.
    """
    states, pending, listeners, unavailable = Counter(), Counter(), [], {}
    for name, family in (('tcp', 'IPv4'), ('tcp6', 'IPv6')):
        try:
            family_states, family_pending, family_listeners = Counter(), Counter(), []
            for line in (net / name).read_text().splitlines()[1:]:
                fields = line.split()
                state, inode = fields[3], fields[9]
                port = int(fields[1].rsplit(':', 1)[1], 16)
                if state in ('03', '0C'):
                    family_pending[port] += 1
                if inode not in sockets:
                    continue
                family_states[TCP_STATES.get(state, state)] += 1
                if state == '0A':
                    family_listeners.append(
                        {
                            'family': family,
                            'port': port,
                            'accept_queue': int(fields[4].split(':')[1], 16),
                        }
                    )
            states.update(family_states)
            pending.update(
                {(family, port): count for port, count in family_pending.items()}
            )
            listeners.extend(family_listeners)
        except (OSError, ValueError, IndexError) as error:
            unavailable[name] = f'{type(error).__name__}: {error}'
    for listener in listeners:
        listener['syn_recv_on_port'] = pending[listener['family'], listener['port']]
    return {
        'socket_states': dict(states),
        'listeners': listeners,
        'unavailable': unavailable,
    }


def sample(pid: int, proc: Path = Path('/proc')) -> dict:
    """Read process and host counters without changing kernel tuning or limits."""
    process = proc / str(pid)
    ticks, started = process_stat((process / 'stat').read_text())
    status = dict(
        line.split(':', 1) for line in (process / 'status').read_text().splitlines()
    )
    memory = dict(
        line.split(':', 1) for line in (proc / 'meminfo').read_text().splitlines()
    )
    sockets: set[str] = set()
    descriptors = list((process / 'fd').iterdir())
    for descriptor in descriptors:
        try:
            link = os.readlink(descriptor)
        except FileNotFoundError:
            continue
        if link.startswith('socket:['):
            sockets.add(link[8:-1])
    tcp = tcp_snapshot(process / 'net', sockets)
    network = {}
    for line in (proc / 'net/dev').read_text().splitlines()[2:]:
        name, values = line.split(':', 1)
        fields = values.split()
        network[name.strip()] = {'rx_bytes': int(fields[0]), 'tx_bytes': int(fields[8])}
    return {
        'event': 'sample',
        'unix': time.time(),
        'monotonic': time.monotonic(),
        'pid': pid,
        'start_ticks': started,
        'cpu_ticks': ticks,
        'clock_ticks': os.sysconf('SC_CLK_TCK'),
        'rss_bytes': int(status.get('VmRSS', '0').split()[0]) * 1024,
        'hwm_bytes': int(status.get('VmHWM', '0').split()[0]) * 1024,
        'threads': int(status['Threads']),
        'established': tcp['socket_states'].get('ESTABLISHED', 0),
        'fd_count': len(descriptors),
        'tcp': tcp,
        'kernel_network': kernel_network(proc, process / 'net'),
        'host_available_bytes': int(memory['MemAvailable'].split()[0]) * 1024,
        'host_total_bytes': int(memory['MemTotal'].split()[0]) * 1024,
        'host_cpu_ticks': [
            int(value)
            for value in (proc / 'stat').read_text().splitlines()[0].split()[1:9]
        ],
        'load_average': (proc / 'loadavg').read_text().split()[:3],
        'network': network,
    }


def host_info() -> dict:
    """Record the VM and process limits needed to reproduce a capacity result."""
    import resource

    return {
        'event': 'host',
        'kernel': platform.release(),
        'machine': platform.machine(),
        'os_release': Path('/etc/os-release').read_text(),
        'cpuinfo': Path('/proc/cpuinfo').read_text(),
        'cpu_count': os.cpu_count(),
        'nofile': resource.getrlimit(resource.RLIMIT_NOFILE),
        'kernel_network': kernel_network(Path('/proc'), Path('/proc/net'), limits=True),
        'ephemeral_ports': Path('/proc/sys/net/ipv4/ip_local_port_range')
        .read_text()
        .strip(),
    }


def configure_guest_firewall(
    interface: str, address: ipaddress.IPv4Interface, port: int | None
) -> None:
    """Allow the private daemon endpoint through an already active guest UFW."""
    ufw = shutil.which('ufw')
    if ufw is None:
        print('Guest UFW: not installed', flush=True)
        return
    environment = os.environ | {'LC_ALL': 'C'}
    status = execute([ufw, 'status', 'verbose'], env=environment)
    print('Guest UFW before configuration:\n' + status, flush=True)
    lines = status.splitlines()
    if not lines or lines[0] not in ('Status: active', 'Status: inactive'):
        raise ValueError('Cannot determine guest UFW status; see the setup log')
    if lines[0] == 'Status: inactive' or port is None:
        return
    if type(port) is not int or not 1 <= port <= 65535:
        raise ValueError('Guest firewall port must be between 1 and 65535')

    # The provider firewall and the image's own firewall are independent. Keep
    # SSH and the public interface untouched, and allow only the VPC subnet to
    # reach this daemon's private address and port. Insert before stock deny rules.
    execute(
        [
            ufw,
            'insert',
            '1',
            'allow',
            'in',
            'on',
            interface,
            'proto',
            'tcp',
            'from',
            str(address.network),
            'to',
            str(address.ip),
            'port',
            str(port),
            'comment',
            'lockd-benchmark',
        ],
        env=environment,
    )
    print(
        'Guest UFW after configuration:\n'
        + execute([ufw, 'status', 'verbose'], env=environment),
        flush=True,
    )


def configure_address(network: dict) -> None:
    """
    Bring up only the new VM's API-identified VPC interface when its image has not.
    """
    address = ipaddress.IPv4Address(network['address'])
    interface_address = ipaddress.IPv4Interface(f'{address}/{network["prefix"]}')
    interfaces = json.loads(execute(['ip', '-j', 'address', 'show']))
    print('VPC interfaces before configuration: ' + json.dumps(interfaces), flush=True)
    matches = [
        interface
        for interface in interfaces
        if interface.get('address', '').lower() == network['mac_address'].lower()
    ]
    if len(matches) != 1:
        raise ValueError("VPC MAC address does not identify exactly one interface")
    interface = matches[0]
    assigned = [
        item for item in interface.get('addr_info', []) if item['family'] == 'inet'
    ]
    matches_address = any(
        item['local'] == str(address) and item['prefixlen'] == network['prefix']
        for item in assigned
    )
    if assigned and not matches_address:
        raise ValueError("VPC interface already has a different IPv4 configuration")
    if not matches_address:
        execute(
            ['ip', 'address', 'add', str(interface_address), 'dev', interface['ifname']]
        )

    # The image can assign an address while leaving its NIC down. Local binds
    # and self-connections still work then, but worker connections cannot arrive.
    execute(['ip', 'link', 'set', 'dev', interface['ifname'], 'up'])
    print(
        'VPC interfaces after configuration: '
        + execute(['ip', '-j', 'address', 'show']),
        flush=True,
    )
    print('VPC routes: ' + execute(['ip', '-j', 'route', 'show']), flush=True)
    if network.get('peer'):
        peer = str(ipaddress.IPv4Address(network['peer']))
        route = json.loads(execute(['ip', '-j', 'route', 'get', peer]))
        print('Route to daemon: ' + json.dumps(route), flush=True)
        if peer != str(address) and (
            len(route) != 1
            or route[0].get('dev') != interface['ifname']
            or route[0].get('prefsrc') != str(address)
        ):
            raise ValueError(
                f"Route to daemon {peer} must use VPC interface {interface['ifname']} "
                f"and source address {address}; see the setup log"
            )
    configure_guest_firewall(
        interface['ifname'], interface_address, network.get('listen_port')
    )


def supervise(job: dict, emit: Callable[[dict], None]) -> int:
    """Gate launches through lockd, sample the child, and stop it on controller EOF."""
    import resource

    work = Path(job['work'])
    limit = job['nofile']
    _, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    soft = limit if hard == resource.RLIM_INFINITY else min(limit, hard)
    resource.setrlimit(resource.RLIMIT_NOFILE, (soft, hard))
    emit(host_info())
    controls: queue.Queue[str] = queue.Queue()
    cancelled = threading.Event()

    def request_stop(signum: int, frame: object) -> None:
        """
        Run child cleanup on SSH hangup instead of inheriting Python's abrupt exit.
        """
        cancelled.set()
        controls.put('stop')

    previous_signals = {
        number: signal.signal(number, request_stop)
        for number in (signal.SIGHUP, signal.SIGTERM)
    }

    def read_controls() -> None:
        """Make SSH EOF a stop request; never leave a daemon after transport loss."""
        for line in sys.stdin:
            command = line.strip()
            controls.put(command)
            if command == 'stop':
                cancelled.set()
                return
        cancelled.set()
        controls.put('stop')

    threading.Thread(target=read_controls, daemon=True).start()
    barrier: Barrier | None = None
    child: subprocess.Popen | None = None
    profile: PerfProfile | None = None
    deadline = time.monotonic() + job['timeout']
    try:
        if job.get('profile_daemon'):
            if job['role'] != 'daemon':
                raise ValueError("Only the daemon can be profiled")
            profile = PerfProfile(work)
        if job['role'] != 'daemon':
            barrier = Barrier(
                job['host'], job['port'], job['barrier'], job['timeout'], cancelled
            )
            emit({'event': 'ready', 'role': job['role'], 'unix': time.time()})
            barrier.expect('LOCK_ACQUIRED')
            barrier.release()
            barrier.close()
            barrier = None
        if cancelled.is_set():
            raise InterruptedError("Controller disconnected before launch")
        environment = os.environ | {'LD_LIBRARY_PATH': str(work / 'bundle/lib')}
        with (work / 'process.log').open('w') as log:
            child = subprocess.Popen(
                job['command'],
                cwd=work,
                env=environment,
                stdout=subprocess.DEVNULL if job.get('errors_only') else log,
                stderr=log if job.get('errors_only') else subprocess.STDOUT,
            )
            emit(
                {
                    'event': 'launched',
                    'pid': child.pid,
                    'unix': time.time(),
                    'command': job['command'],
                    'profile_daemon': profile is not None,
                }
            )
            ready_deadline = time.monotonic() + 30
            while child.poll() is None:
                if cancelled.is_set():
                    break
                if profile is not None and profile.process is not None:
                    profile.check()
                try:
                    emit(sample(child.pid))
                except (FileNotFoundError, ProcessLookupError):
                    if child.poll() is None:
                        raise
                if job['role'] == 'daemon' and barrier is None:
                    try:
                        barrier = Barrier(job['host'], job['port'], job['barrier'], 1)
                        barrier.expect('LOCK_ACQUIRED')
                        emit({'event': 'ready', 'role': 'daemon', 'unix': time.time()})
                    except OSError:
                        if time.monotonic() > ready_deadline:
                            raise TimeoutError("Daemon did not become ready")
                try:
                    control = controls.get(timeout=job['interval'])
                except queue.Empty:
                    control = ''
                if control == 'release' and barrier is not None:
                    if profile is not None:
                        profile.start(child.pid, cancelled)
                        emit(
                            {
                                **profile.metadata,
                                'sampling_event': profile.metadata['event'],
                                'event': 'profiling',
                            }
                        )
                    barrier.release()

                    # Keep the connection open without reacquiring. It is harmless
                    # and avoids mistaking release for a need to create a new gate.
                    emit({'event': 'released', 'unix': time.time()})
                if control == 'stop':
                    break
                if time.monotonic() >= deadline:
                    raise TimeoutError("Remote supervision deadline exceeded")
            if profile is not None:
                profile.finish()
                emit(
                    {
                        **profile.metadata,
                        'sampling_event': profile.metadata['event'],
                        'event': 'profile_finished',
                    }
                )

            # Release our coordination lock before requesting daemon shutdown;
            # otherwise graceful shutdown waits for the agent's own held lock.
            if barrier is not None:
                barrier.close()
                barrier = None
            if child.poll() is None:
                child.terminate()
            result = child.wait(
                timeout=DAEMON_STOP_TIMEOUT_SECONDS if job['role'] == 'daemon' else 10
            )
            emit({'event': 'finished', 'returncode': result, 'unix': time.time()})
            return result if job['role'] != 'daemon' else 0
    finally:
        try:
            if profile is not None and not profile.finished:
                try:
                    profile.finish()
                except (OSError, RuntimeError, subprocess.SubprocessError) as error:
                    # Keep the initiating failure, but retain profiling diagnostics.
                    emit({'event': 'error', 'message': 'Profiling: ' + str(error)})
        finally:
            if barrier is not None:
                barrier.close()
            if child is not None and child.poll() is None:
                child.kill()
                child.wait()
            for number, previous in previous_signals.items():
                signal.signal(number, previous)


def main() -> None:
    """Run a single remote build, bundle extraction, address check, or monitored job."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('build', 'unpack', 'address', 'run'))
    parser.add_argument('work', type=Path)
    parser.add_argument('value', nargs='?')
    parser.add_argument('--profile', action='store_true')
    parser.add_argument('--require-async', action='store_true')
    args = parser.parse_args()
    match args.action:
        case 'build':
            build(
                args.work,
                args.value,
                profile=args.profile,
                require_async=args.require_async,
            )
        case 'unpack':
            extract(args.work / 'bundle.tar.gz', args.work / 'unpacked')
            (args.work / 'unpacked/bundle').rename(args.work / 'bundle')
        case 'address':
            configure_address(json.loads(args.value))
        case 'run':
            job = json.loads((args.work / 'job.json').read_text())
            with (args.work / 'metrics.jsonl').open('w', buffering=1) as output:

                def emit(event: dict) -> None:
                    """Persist each sample remotely and stream it to the controller."""
                    line = json.dumps(event)
                    output.write(line + '\n')
                    print(line, flush=True)

                try:
                    result = supervise(job, emit)
                except (
                    OSError,
                    RuntimeError,
                    ValueError,
                    subprocess.SubprocessError,
                ) as error:
                    emit({'event': 'error', 'message': str(error)})
                    raise
                raise SystemExit(result)


if __name__ == '__main__':
    with contextlib.suppress(BrokenPipeError):
        main()

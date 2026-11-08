"""Transfer files and stream Linux agents through the controller's OpenSSH tools."""

import contextlib
import ipaddress
import json
import shlex
import subprocess
import threading
import time
from pathlib import Path


class Remote:
    """Keep SSH trust and private-key use local to one benchmark run."""

    def __init__(self, address: str, key: Path, directory: Path, work: str) -> None:
        self.address = str(ipaddress.IPv4Address(address))
        self.work = work
        self.options = [
            '-i',
            str(key),
            '-o',
            'BatchMode=yes',
            '-o',
            'ConnectTimeout=10',
            '-o',
            'ServerAliveInterval=15',
            '-o',
            'ServerAliveCountMax=3',
            '-o',
            'StrictHostKeyChecking=accept-new',
            '-o',
            'UserKnownHostsFile=' + str(directory / 'known_hosts'),
        ]

    def command(self, arguments: list[str]) -> list[str]:
        """Quote each remote argument for SSH's required remote POSIX shell."""
        return [
            'ssh',
            *self.options,
            '-T',
            'root@' + self.address,
            shlex.join(arguments),
        ]

    def call(self, arguments: list[str], timeout: float, log: Path) -> None:
        """Retain diagnostics from finite setup commands without terminal buffering."""
        with log.open('a', encoding='utf-8') as output:
            subprocess.run(
                self.command(arguments),
                stdin=subprocess.DEVNULL,
                stdout=output,
                stderr=subprocess.STDOUT,
                timeout=timeout,
                check=True,
            )

    def wait(
        self, timeout: float, log: Path, *, cancelled: threading.Event | None = None
    ) -> None:
        """Wait for first boot without treating an initial SSH refusal as fatal."""
        deadline = time.monotonic() + timeout
        while True:
            if cancelled is not None and cancelled.is_set():
                raise InterruptedError('Worker preparation cancelled before SSH setup')
            try:
                self.call(
                    [
                        'python3',
                        '-c',
                        (
                            'import sys; print(sys.version); '
                            'sys.exit(0 if sys.version_info >= (3, 11) else 2)'
                        ),
                    ],
                    20,
                    log,
                )
                return
            except subprocess.CalledProcessError as error:
                if error.returncode != 255:
                    raise RuntimeError(
                        "Remote image needs Python 3.11 or newer"
                    ) from error
                if time.monotonic() >= deadline:
                    raise TimeoutError(
                        "SSH did not become ready: " + self.address
                    ) from None
                if cancelled is None:
                    time.sleep(3)
                else:
                    cancelled.wait(3)
            except subprocess.TimeoutExpired:
                if time.monotonic() >= deadline:
                    raise TimeoutError(
                        "SSH did not become ready: " + self.address
                    ) from None
                if cancelled is None:
                    time.sleep(3)
                else:
                    cancelled.wait(3)

    def copy(self, local: Path, name: str, *, download: bool = False) -> None:
        """Transfer named files only; no remote glob or recursive directory deletion."""
        remote = f'root@{self.address}:{self.work}/{name}'
        endpoints = [remote, str(local)] if download else [str(local), remote]
        subprocess.run(['scp', *self.options, *endpoints], check=True, timeout=300)


class Session:
    """
    Drain agent output continuously while exposing explicit readiness and stop controls.
    """

    def __init__(self, remote: Remote, directory: Path, role: str) -> None:
        self.ready = threading.Event()
        self.released = threading.Event()
        self.has_ready_event = False
        self.has_released_event = False
        self.has_launched_event = False
        self.profile_enabled = False
        self.last_sample: dict | None = None
        self.cpu_count: int | None = None
        self.error: str | None = None
        self.read_failed = False
        self.role = role
        self.log = (directory / (role + '-ssh.log')).open('w', encoding='utf-8')
        self.process = subprocess.Popen(
            remote.command(['python3', remote.work + '/agent.py', 'run', remote.work]),
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=self.log,
            text=True,
            encoding='utf-8',
        )
        self.output = directory / (role + '-stream.jsonl')
        self.thread = threading.Thread(target=self._read, daemon=True)
        self.thread.start()

    def _read(self) -> None:
        """
        Write each event before announcing readiness, and surface malformed output.
        """
        try:
            with self.output.open('w', encoding='utf-8') as stream:
                for line in self.process.stdout:
                    stream.write(line)
                    stream.flush()
                    event = json.loads(line)
                    if event['event'] == 'ready':
                        self.has_ready_event = True
                        self.ready.set()
                    elif event['event'] == 'released':
                        self.has_released_event = True
                        self.released.set()
                    elif event['event'] == 'launched':
                        self.has_launched_event = True
                        self.profile_enabled = event.get('profile_daemon', False)
                    elif event['event'] == 'error':
                        self.error = str(event['message'])
                        self.ready.set()
                        self.released.set()
                    elif event['event'] == 'host':
                        count = event.get('cpu_count')
                        self.cpu_count = (
                            count if type(count) is int and count > 0 else None
                        )
                    elif event['event'] == 'sample':
                        event['cpu_capacity_vcpus'] = self.cpu_count
                        previous = self.last_sample
                        if (
                            previous is not None
                            and event['pid'] == previous['pid']
                            and event['start_ticks'] == previous['start_ticks']
                            and event['monotonic'] > previous['monotonic']
                        ):
                            event['cpu_percent'] = (
                                (event['cpu_ticks'] - previous['cpu_ticks'])
                                / event['clock_ticks']
                                / (event['monotonic'] - previous['monotonic'])
                                * 100
                            )
                            if self.cpu_count is not None:
                                event['cpu_capacity_percent'] = (
                                    event['cpu_percent'] / self.cpu_count
                                )
                        self.last_sample = event
        except (OSError, ValueError, KeyError) as error:
            self.error = str(error)
            self.read_failed = True
        finally:
            self.ready.set()
            self.released.set()

    def progress(self) -> str | None:
        """Describe the latest sample with CPU normalized to the guest's vCPU count."""
        sampled = self.last_sample
        if sampled is None:
            return None
        percentage = sampled.get('cpu_capacity_percent')
        cpu = f'{percentage:.1f}% of VM' if percentage is not None else 'unavailable'
        return (
            f'{self.role}: CPU {cpu} RSS {sampled["rss_bytes"] / 1024**2:.1f} MiB; '
            f'threads {sampled["threads"]}; established {sampled["established"]}'
        )

    def control(self, command: str) -> None:
        """Send only the agent's release/stop vocabulary over its persistent stdin."""
        if command not in ('release', 'stop'):
            raise ValueError("Unknown agent control")
        if self.process.poll() is None:
            try:
                self.process.stdin.write(command + '\n')
                self.process.stdin.flush()
            except (BrokenPipeError, OSError):
                pass

    def wait_ready(self, timeout: float) -> None:
        """
        Require a live ready event, not merely an SSH process that happened to start.
        """
        if (
            not self.ready.wait(timeout)
            or not self.has_ready_event
            or self.error
            or self.process.poll() is not None
        ):
            raise RuntimeError(
                f"{self.role} agent did not become ready: {self.error or 'no live ready event'}. "
                f"See {self.output} and {self.log.name}"
            )

    def release(self, timeout: float) -> None:
        """Wait for daemon-side enablement and gate release before watching workers."""
        self.control('release')
        if (
            not self.released.wait(timeout)
            or not self.has_released_event
            or self.error
            or self.process.poll() is not None
        ):
            raise RuntimeError(
                f"{self.role} launch gate release failed: "
                f"{self.error or 'no live release acknowledgement'}. "
                f"See {self.output} and {self.log.name}"
            )

    def finish(self, timeout: float) -> int:
        """Join the output reader before reporting agent completion."""
        result = self.process.wait(timeout=timeout)
        self.thread.join(timeout=10)
        if self.thread.is_alive() or self.read_failed:
            raise RuntimeError(f"{self.role} agent output was incomplete")
        if self.error and result == 0:
            raise RuntimeError(f"{self.role} agent failed: {self.error}")
        return result

    def close(self) -> None:
        """Stop the owned remote child, then close the SSH process and local streams."""
        self.control('stop')
        try:
            # The daemon itself allows lock holders 30 seconds to release.
            # A profiled daemon first flushes perf (30s) and renders its local
            # report (120s), then has the usual 35s graceful-shutdown allowance.
            self.process.wait(timeout=190 if self.profile_enabled else 40)
        except subprocess.TimeoutExpired:
            # Closing SSH makes the remote agent see EOF. Its own deadline remains
            # a second stop condition if the network is broken rather than closed.
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.thread.join(timeout=10)
        # A failed pipe write can remain buffered on Windows and fail again at
        # close. Still close stdout and the log so they don't leak handles.
        with contextlib.suppress(OSError):
            self.process.stdin.close()
        self.process.stdout.close()
        self.log.close()
        if self.profile_enabled and (
            self.thread.is_alive()
            or self.read_failed
            or self.error
            or self.process.returncode != 0
        ):
            raise RuntimeError(
                'Daemon profile supervision failed: ' + (self.error or str(self.output))
            )

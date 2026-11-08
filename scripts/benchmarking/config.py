"""Validate inputs before provisioning and keep generated paths predictable."""

import argparse
import hashlib
import ipaddress
import json
import platform
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def host_architecture() -> str:
    """Map the controller architecture to the project's output directory names."""
    machine = platform.machine().lower()
    return {'amd64': 'x86_64', 'aarch64': 'arm64', 'i386': 'x86'}.get(machine, machine)


def output_directory(run_id: str, *, final: bool = False) -> Path:
    """Keep journals separate from exported reports in the canonical host tree."""
    base = ROOT / ('output' if final else 'output/obj')
    return base / host_architecture() / 'opt/shared/benchmarking' / run_id


def object_value(value: object) -> dict:
    """Require a JSON object at an external boundary."""
    if not isinstance(value, dict):
        raise TypeError("Expected a JSON object")
    return value


def file_digest(path: Path) -> str:
    """Hash an archive incrementally without holding it in controller memory."""
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def example() -> dict:
    """Return editable configuration without secrets or machine-specific paths."""
    return {
        'region': 'ewr',
        'os_id': 0,
        'vpc_id': 'CHOOSE_EXISTING_VPC_ID',
        'vpc_cidr': '10.20.0.0/24',
        'firewall_group_id': 'CHOOSE_EXISTING_FIREWALL_ID',
        'ssh_key_id': 'CHOOSE_UPLOADED_SSH_KEY_ID',
        'ssh_private_key': '~/.ssh/id_ed25519',
        'source_archive': 'lockd-source.tar.gz',
        'source_sha256': 'REPLACE_WITH_SHA256',
        'daemon_plan': 'CHOOSE_DAEMON_PLAN',
        'worker_plan': 'CHOOSE_WORKER_PLAN',
        'worker_count': 2,
        'daemon_threads': 0,
        'worker_arguments': ['--connections=100', '--duration=120', '--warmup=5'],
        'worker_overrides': {},
        'sample_seconds': 1.0,
        'profile_daemon': False,
        'setup_timeout_seconds': 1800,
        'run_timeout_seconds': 1800,
        'nofile': 131072,
    }


def worker_options(arguments: list[str]) -> dict:
    """Accept benchmark workload options while reserving endpoints and result paths."""
    parser = argparse.ArgumentParser(
        add_help=False, allow_abbrev=False, exit_on_error=False
    )
    for name, default in (
        ('threads', 0),
        ('connections', 1),
        ('duration', 60),
        ('warmup', 5),
        ('connect-timeout', 5),
        ('acquire-timeout', 1000),
        ('hold-ms', 0),
        ('lock-count', 1),
    ):
        flags = ['--' + name, '-t'] if name == 'threads' else ['--' + name]
        parser.add_argument(
            *flags,
            type=int,
            default=None if name in ('threads', 'lock-count') else default,
        )
    parser.add_argument(
        '--workload',
        choices=('independent', 'contended', 'idle', 'churn'),
        default='independent',
    )
    parser.add_argument('--lock-prefix', default='lockd_benchmark')
    parser.add_argument('--engine', choices=('sync', 'async'), default='sync')
    parser.add_argument('--require-all-clients', action='store_true')
    parsed, unknown = parser.parse_known_args(arguments)
    if unknown:
        raise ValueError(f"Unsupported or reserved worker options: {unknown}")
    values = vars(parsed)
    explicit_pool = values['lock_count'] is not None
    if values['threads'] is not None and values['threads'] < 1:
        raise ValueError("threads must be between 1 and 65535 when specified")
    values['threads'] = 0 if values['threads'] is None else values['threads']
    values['lock_count'] = 1 if values['lock_count'] is None else values['lock_count']
    for name, low, high in (
        ('threads', 0, 65535),
        ('connections', 1, 65535),
        ('duration', 1, 86400),
        ('warmup', 0, 3600),
        ('connect_timeout', 1, 60),
        ('acquire_timeout', 1, 60000),
        ('hold_ms', 0, 60000),
        ('lock_count', 1, 65535),
    ):
        if not low <= values[name] <= high:
            raise ValueError(f"{name} must be between {low} and {high}")
    prefix = values['lock_prefix']
    if not 1 <= len(prefix) <= 180 or any(
        char not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-'
        for char in prefix
    ):
        raise ValueError("lock-prefix must contain 1-180 ASCII letters, digits, _ or -")
    if values['workload'] != 'contended' and explicit_pool:
        raise ValueError("lock-count requires contended mode")
    if values['workload'] == 'idle' and values['hold_ms']:
        raise ValueError("hold-ms does not apply to idle mode")
    if values['workload'] == 'idle' and values['require_all_clients']:
        raise ValueError("require-all-clients does not apply to idle mode")
    return values


def read_settings(path: Path) -> dict:
    """Read known settings without requiring provisioning inputs to be complete."""
    settings = object_value(json.loads(path.read_text(encoding='utf-8')))
    if set(settings) - set(example()):
        raise ValueError(f"Unknown settings: {set(settings) - set(example())}")
    return settings


def catalog_region(path: Path) -> str:
    """Require only the region while the remaining IDs are still being selected."""
    region = read_settings(path).get('region')
    if (
        not isinstance(region, str)
        or not region.strip()
        or region != region.strip()
        or region.startswith('CHOOSE')
    ):
        raise ValueError("Configure region before listing catalogs")
    return region


def load(path: Path) -> dict:
    """Validate the entire configuration and pinned source before any API mutation."""
    values = example() | read_settings(path)
    for name in (
        'region',
        'vpc_id',
        'firewall_group_id',
        'ssh_key_id',
        'daemon_plan',
        'worker_plan',
    ):
        value = values[name]
        if not isinstance(value, str) or not value or value.startswith('CHOOSE'):
            raise ValueError(f"Configure {name}")
    for name, low, high in (
        ('os_id', 1, 1000000),
        ('worker_count', 1, 100),
        ('daemon_threads', 0, 65535),
        ('setup_timeout_seconds', 60, 86400),
        ('run_timeout_seconds', 30, 100000),
        ('nofile', 1024, 1048576),
    ):
        value = values[name]
        if type(value) is not int or not low <= value <= high:
            raise ValueError(f"{name} must be an integer between {low} and {high}")
    interval = values['sample_seconds']
    if type(interval) not in (int, float) or not 0.1 <= interval <= 60:
        raise ValueError("sample_seconds must be between 0.1 and 60")
    if type(values['profile_daemon']) is not bool:
        raise ValueError("profile_daemon must be true or false")
    network = ipaddress.ip_network(values['vpc_cidr'])
    if network.version != 4 or not any(
        network.subnet_of(ipaddress.ip_network(cidr))
        for cidr in ('10.0.0.0/8', '172.16.0.0/12', '192.168.0.0/16')
    ):
        raise ValueError("vpc_cidr must be a private IPv4 network")
    for name in ('source_archive', 'ssh_private_key'):
        candidate = Path(values[name]).expanduser()
        candidate = (
            candidate if candidate.is_absolute() else path.resolve().parent / candidate
        )
        if not candidate.is_file():
            raise ValueError(f"{name} is not a file: {candidate}")
        values[name] = str(candidate.resolve())
    digest = values['source_sha256']
    digest = digest.lower() if isinstance(digest, str) else digest
    if (
        not isinstance(digest, str)
        or len(digest) != 64
        or any(char not in '0123456789abcdef' for char in digest)
        or file_digest(Path(values['source_archive'])) != digest
    ):
        raise ValueError("source_sha256 must match the source archive")
    values['source_sha256'] = digest
    overrides = object_value(values['worker_overrides'])
    if any(
        key not in {str(i) for i in range(values['worker_count'])} for key in overrides
    ):
        raise ValueError("worker_overrides keys must be zero-based worker indices")
    for index in range(values['worker_count']):
        arguments = overrides.get(str(index), values['worker_arguments'])
        if not isinstance(arguments, list) or any(
            not isinstance(x, str) for x in arguments
        ):
            raise ValueError("Worker arguments must be lists of strings")
        options = worker_options(arguments)
        if (
            options['duration'] + options['warmup'] + 60
            >= values['run_timeout_seconds']
        ):
            raise ValueError("run_timeout_seconds must allow setup and drain time")
    return values


def benchmark_arguments(options: dict) -> list[str]:
    """
    Forward validated options with native defaults, including automatic thread count.
    """
    return [
        f'--{key.replace("_", "-")}={value}'
        for key, value in options.items()
        if not (key == 'threads' and value == 0)
        and not (key == 'lock_count' and options['workload'] != 'contended')
        and not (key == 'engine' and value == 'sync')
        and key != 'require_all_clients'
    ] + (['--require-all-clients'] if options['require_all_clients'] else [])

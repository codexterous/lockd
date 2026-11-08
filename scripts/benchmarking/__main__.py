"""Provision, monitor, report, and clean up journal-owned benchmark VMs."""

import argparse
import datetime
import ipaddress
import json
import os
import shlex
import shutil
import subprocess
import threading
import time
import uuid
from collections import deque
from concurrent.futures import ThreadPoolExecutor, as_completed
from contextlib import AbstractContextManager, nullcontext
from decimal import Decimal, InvalidOperation
from pathlib import Path
from urllib.parse import quote

from . import config, report, vultr
from .remote import Remote, Session

AGENT = Path(__file__).with_name('agent.py')
BUILD_LOG_TAIL_LINES = 40
WORKER_PREPARATION_JOBS = 10


def format_table(
    headers: list[str], rows: list[list[str]], *, numeric: tuple[int, ...] = ()
) -> str:
    """Align catalog columns without letting embedded newlines split table rows."""
    rows = [
        [''.join(char if char.isprintable() else ' ' for char in cell) for cell in row]
        for row in rows
    ]
    widths = [
        max(len(row[index]) for row in [headers, *rows])
        for index in range(len(headers))
    ]
    lines = []
    for row in [headers, ['-' * width for width in widths], *rows]:
        lines.append(
            '  '.join(
                value.rjust(width) if index in numeric else value.ljust(width)
                for index, (value, width) in enumerate(zip(row, widths))
            ).rstrip()
        )
    if not rows:
        lines.append('(no matching entries)')
    return '\n'.join(lines)


def format_plans(plans: list[dict]) -> str:
    """Sort the full plan catalog by monthly cost and align its display columns."""
    priced = []
    for plan in plans:
        try:
            cost = Decimal(str(plan['monthly_cost']))
        except InvalidOperation:
            raise ValueError(f"Invalid monthly cost for plan {plan['id']}") from None
        if not cost.is_finite() or cost < 0:
            raise ValueError(f"Invalid monthly cost for plan {plan['id']}")
        priced.append(
            (
                cost,
                str(plan['id']),
                str(plan['vcpu_count']),
                str(plan['ram']),
                str(plan['disk']),
            )
        )

    # Numeric prices determine order; the ID makes equal-price rows consistent
    # across requests even when Vultr returns them in a different order.
    rows = [
        [identifier, cpus, memory, disk, f'{cost:.2f}']
        for cost, identifier, cpus, memory, disk in sorted(priced)
    ]
    return format_table(
        ["Plan", "vCPUs", "RAM (MiB)", "Local disk (GB)", "Monthly cost"],
        rows,
        numeric=(1, 2, 3, 4),
    )


def available_plans(api: vultr.Api, region: str) -> set[str]:
    """Read current regional capacity without submitting a creation request."""
    response = api.request(
        'GET', '/regions/' + quote(region, safe='') + '/availability'
    )
    return set(response['available_plans'])


def show_catalog(api: vultr.Api, kind: str, region: str) -> None:
    """Show regional resources and explicitly label Vultr's global catalogs."""

    # Vultr names these response collections differently from their URLs.
    collection = {'firewalls': 'firewall_groups', 'ssh-keys': 'ssh_keys'}.get(
        kind, kind
    )
    items = api.items('/' + kind, collection)
    if kind == 'plans':
        # locations lists supported regions, even when current capacity is zero.
        # Query availability instead so choosing a listed plan is actually useful.
        available = available_plans(api, region)
        items = [
            item
            for item in items
            if item['id'] in available and vultr.supports_local_boot(item)
        ]
    elif kind == 'vpcs':
        items = [item for item in items if item['region'] == region]
    elif kind == 'regions':
        items = [item for item in items if item['id'] == region]

    if kind in ('os', 'firewalls', 'ssh-keys'):
        # These API resources have no region field or regional availability query.
        # In particular, an OS listing is not a guarantee of deployability with
        # every plan in the configured region.
        print(f'Configured region: {region}; {kind} catalog is global.')
        if kind == 'os':
            print('Vultr does not report regional OS availability in this catalog.')
    else:
        print(f'Region: {region}')
    if kind == 'plans':
        print(
            'Only plans with local boot storage are shown; no separate volumes are created.'
        )
        print(format_plans(items))
        return

    # Keep explicit columns so account responses cannot dump keys or unrelated
    # fields into the terminal. IDs remain complete for copying into configuration.
    columns = {
        'regions': [('id', 'ID'), ('city', 'City'), ('country', 'Country')],
        'os': [
            ('id', 'ID'),
            ('name', 'Name'),
            ('arch', 'Architecture'),
            ('family', 'Family'),
        ],
        'vpcs': [
            ('id', 'ID'),
            ('region', 'Region'),
            ('description', 'Description'),
            ('v4_subnet', 'IPv4 subnet'),
            ('v4_subnet_mask', 'Prefix length'),
        ],
        'firewalls': [('id', 'ID'), ('description', 'Description')],
        'ssh-keys': [('id', 'ID'), ('name', 'Name')],
    }[kind]
    rows = [
        [str(item.get(field, '')) for field, _ in columns]
        for item in sorted(items, key=lambda item: str(item['id']))
    ]
    print(format_table([title for _, title in columns], rows))


def preflight(api: vultr.Api, settings: dict) -> dict:
    """Check plan availability and shared resource IDs before billable requests."""
    plans = {plan['id']: plan for plan in api.items('/plans', 'plans')}
    available = available_plans(api, settings['region'])
    for role in ('daemon', 'worker'):
        identifier = settings[role + '_plan']
        if identifier not in plans:
            raise ValueError(f"Unknown {role} plan: {identifier}")
        if not vultr.supports_local_boot(plans[identifier]):
            raise ValueError(
                f"The {role} plan {identifier} needs local storage; "
                f"Vultr reports storage_type={plans[identifier].get('storage_type')!r}. "
                "Choose a local-boot plan from catalog plans <config.json>. "
                "This tool does not create separate block volumes."
            )
        if identifier not in available:
            raise ValueError(
                f"The {role} plan {identifier} is currently unavailable in "
                f"{settings['region']}. Use catalog plans <config.json> "
                "to see available plans for the configured region."
            )
    vpc = api.request('GET', '/vpcs/' + str(uuid.UUID(settings['vpc_id'])))['vpc']
    if vpc['region'] != settings['region']:
        raise ValueError("VPC and benchmark region differ")
    actual = ipaddress.ip_network(f'{vpc["v4_subnet"]}/{vpc["v4_subnet_mask"]}')
    if actual != ipaddress.ip_network(settings['vpc_cidr']):
        raise ValueError("Configured VPC CIDR does not match Vultr")
    api.request('GET', '/firewalls/' + str(uuid.UUID(settings['firewall_group_id'])))
    api.request('GET', '/ssh-keys/' + str(uuid.UUID(settings['ssh_key_id'])))
    images = api.items('/os', 'os')
    image = next(item for item in images if item['id'] == settings['os_id'])
    if image.get('family') != 'ubuntu' or image.get('arch') not in (
        'x64',
        'arm64',
    ):
        raise ValueError("Choose a supported Ubuntu x64 or ARM64 image")
    return {
        'image': {key: image[key] for key in ('id', 'name', 'arch')},
        'plans': {
            settings[role + '_plan']: plans[settings[role + '_plan']]
            for role in ('daemon', 'worker')
        },
    }


def connect(
    api: vultr.Api,
    path: Path,
    state: dict,
    record: dict,
    *,
    journal_guard: AbstractContextManager | None = None,
    cancelled: threading.Event | None = None,
) -> Remote:
    """Validate ownership and wait for an address before opening an SSH session."""
    settings = state['config']
    guard = journal_guard if journal_guard is not None else nullcontext()
    deadline = time.monotonic() + settings['setup_timeout_seconds']
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError(
                'Instance provisioning did not finish: ' + record['label']
            )
        instance = vultr.wait_for_instance(
            api,
            record['id'],
            min(remaining, vultr.INSTANCE_VISIBILITY_TIMEOUT_SECONDS),
            cancelled=cancelled,
        )
        if not vultr.owned(record, instance, state['run_id']):
            raise ValueError("Instance does not match its creation journal")
        if instance.get('status') == 'active' and instance.get('main_ip'):
            address = ipaddress.IPv4Address(instance['main_ip'])
            if not address.is_global:
                raise ValueError(
                    "Instance does not have a public management IPv4 address"
                )
            save_details = {
                key: instance.get(key) for key in ('vcpu_count', 'ram', 'os')
            }
            with guard:
                record['identity'] = {key: instance[key] for key in vultr.IDENTITY}
                record['public_ip'] = str(address)
                record.update(save_details)
                vultr.save(path, state)
            break
        if time.monotonic() >= deadline:
            raise TimeoutError(
                "Instance provisioning did not finish: " + record['label']
            )
        if cancelled is None:
            time.sleep(5)
        elif cancelled.wait(5):
            raise InterruptedError('Worker preparation cancelled during provisioning')
    remote = remote_for(state, record, path.parent)
    log = path.parent / (record['role'] + '-setup.log')
    remote.wait(max(1, deadline - time.monotonic()), log, cancelled=cancelled)
    if cancelled is not None and cancelled.is_set():
        raise InterruptedError('Worker preparation cancelled before cloud-init')
    remote.call(['cloud-init', 'status', '--wait'], 300, log)
    if cancelled is not None and cancelled.is_set():
        raise InterruptedError('Worker preparation cancelled after cloud-init')
    remote.call(
        [
            'python3',
            '-c',
            'import pathlib,sys; pathlib.Path(sys.argv[1]).mkdir(mode=0o700)',
            remote.work,
        ],
        30,
        log,
    )
    if cancelled is not None and cancelled.is_set():
        raise InterruptedError('Worker preparation cancelled before agent transfer')
    remote.copy(AGENT, 'agent.py')
    if cancelled is not None and cancelled.is_set():
        raise InterruptedError('Worker preparation cancelled after agent transfer')
    networks = api.items('/instances/' + record['id'] + '/vpcs', 'vpcs')
    attached = next(item for item in networks if item['id'] == settings['vpc_id'])
    address = ipaddress.ip_address(attached['ip_address'])
    network = ipaddress.ip_network(settings['vpc_cidr'])
    if address not in network:
        raise ValueError("Instance address is outside the configured VPC")
    with guard:
        record['private_ip'] = str(address)
        vultr.save(path, state)
    if cancelled is not None and cancelled.is_set():
        raise InterruptedError('Worker preparation cancelled before VPC setup')
    remote.call(
        [
            'python3',
            remote.work + '/agent.py',
            'address',
            remote.work,
            json.dumps(
                {
                    'address': str(address),
                    'prefix': network.prefixlen,
                    'mac_address': attached['mac_address'],
                    'peer': state['resources'][0]['private_ip'],
                    'listen_port': 7198 if record['role'] == 'daemon' else None,
                }
            ),
        ],
        30,
        log,
    )
    return remote


def remote_for(state: dict, record: dict, directory: Path) -> Remote:
    """Reconstruct a transport for one recorded instance without discovering by name."""
    return Remote(
        record['public_ip'],
        Path(state['config']['ssh_private_key']),
        directory,
        '/root/lockd-benchmark-' + state['run_id'],
    )


def write_job(
    remote: Remote,
    state: dict,
    record: dict,
    directory: Path,
    *,
    journal_guard: AbstractContextManager | None = None,
) -> None:
    """Reserve endpoint, lifetime, and output arguments for the supervisor."""
    settings = state['config']
    daemon = state['resources'][0]
    job = {
        'work': remote.work,
        'role': record['role'],
        'host': daemon['private_ip'],
        'port': 7198,
        'barrier': 'benchmark_launch_' + state['run_id'].replace('-', ''),
        'interval': settings['sample_seconds'],
        # Early agents remain queued while later VMs are prepared.
        'timeout': settings['setup_timeout_seconds'] + settings['run_timeout_seconds'],
        'nofile': settings['nofile'],
        'profile_daemon': record['role'] == 'daemon'
        and settings.get('profile_daemon', False),
        'errors_only': record['role'] != 'daemon',
    }
    if record['role'] == 'daemon':
        command = [
            remote.work + '/bundle/lockd',
            '--no-fork',
            '--no-drop',
            '--interface=' + job['host'],
            '--port=7198',
            '--log-file=' + remote.work + '/daemon.log',
            '--log-level=info',
        ]
        if settings['daemon_threads']:
            command.append('--threads=' + str(settings['daemon_threads']))
    else:
        arguments = settings['worker_overrides'].get(
            str(record['worker_index']), settings['worker_arguments']
        )
        options = config.worker_options(arguments)
        with journal_guard if journal_guard is not None else nullcontext():
            record['options'] = options
        command = [
            remote.work + '/bundle/lockd_test',
            '--',
            '-B',
            '--interface=' + job['host'],
            '--port=7198',
            '--results=' + remote.work + '/result.csv',
            *config.benchmark_arguments(options),
        ]
    job['command'] = command
    with journal_guard if journal_guard is not None else nullcontext():
        record['logging'] = (
            'info; daemon.log and process.log'
            if record['role'] == 'daemon'
            else 'errors only on stderr; measurements in result.csv'
        )
    destination = directory / (record['role'] + '-job.json')
    vultr.save(destination, job)
    remote.copy(destination, 'job.json')


def collect(state: dict, directory: Path) -> list[str]:
    """Recover every available log even when a different VM or download has failed."""
    failures = []
    for record in state['resources']:
        if 'public_ip' not in record or record['status'] == 'deleted':
            continue

        # Build failures happen before runtime logs exist. The stream file also
        # identifies monitoring started by older journals without this flag.
        if (
            not record.get('monitoring_started')
            and not (directory / (record['role'] + '-stream.jsonl')).is_file()
        ):
            continue
        remote = remote_for(state, record, directory)
        names = ['metrics.jsonl']
        if record.get('process_started', True):
            names += ['process.log']
            names += ['daemon.log'] if record['role'] == 'daemon' else ['result.csv']
        if record['role'] == 'daemon' and state['config'].get('profile_daemon'):
            profile_path = directory / 'daemon-profile.json'
            try:
                remote.copy(profile_path, 'profile.json', download=True)
                profile = config.object_value(
                    json.loads(profile_path.read_text(encoding='utf-8'))
                )

                # The remote inventory can only nominate these profiling files.
                # Never turn downloaded JSON into an arbitrary SCP source path.
                names += [
                    name
                    for name in ('perf.data', 'perf.log', 'perf-report.txt')
                    if name in profile['artifacts']
                ]
            except (
                OSError,
                ValueError,
                TypeError,
                KeyError,
                subprocess.SubprocessError,
            ) as error:
                failures.append('daemon/profile.json: ' + str(error))
        for name in names:
            target = directory / (record['role'] + '-' + name)
            try:
                remote.copy(target, name, download=True)
            except (OSError, subprocess.SubprocessError) as error:
                failures.append(record['role'] + '/' + name + ': ' + str(error))
    return failures


def build_bundle(server: Remote, settings: dict, directory: Path) -> None:
    """Build once and show local diagnostics before failure triggers VM cleanup."""
    server.copy(Path(settings['source_archive']), 'source.tar.gz')
    require_async = any(
        config.worker_options(
            settings['worker_overrides'].get(str(index), settings['worker_arguments'])
        )['engine']
        == 'async'
        for index in range(settings['worker_count'])
    )
    log = directory / 'build.log'
    print(
        'Building the pinned source once on the daemon VM; log: ' + str(log), flush=True
    )
    try:
        server.call(
            [
                'python3',
                server.work + '/agent.py',
                'build',
                server.work,
                settings['source_sha256'],
                *(['--profile'] if settings.get('profile_daemon') else []),
                *(['--require-async'] if require_async else []),
            ],
            settings['setup_timeout_seconds'],
            log,
        )
    except (OSError, subprocess.SubprocessError) as error:
        print('BUILD FAILED: ' + str(error), flush=True)
        try:
            with log.open(encoding='utf-8', errors='replace') as source:
                tail = ''.join(deque(source, maxlen=BUILD_LOG_TAIL_LINES))
            print(tail, end='' if tail.endswith('\n') else '\n', flush=True)
        except OSError as log_error:
            print('Could not read build log: ' + str(log_error), flush=True)
        raise RuntimeError('Daemon build failed; see ' + str(log)) from error
    server.copy(directory / 'bundle.tar.gz', 'bundle.tar.gz', download=True)
    server.copy(directory / 'build.json', 'bundle/build.json', download=True)


def prepare_worker(
    api: vultr.Api,
    path: Path,
    state: dict,
    record: dict,
    sessions: list[Session],
    journal_guard: AbstractContextManager,
    cancelled: threading.Event,
) -> None:
    """Prepare one recorded VM while serializing shared journal changes only."""
    if cancelled.is_set():
        raise InterruptedError('Worker preparation cancelled before setup')
    directory = path.parent
    print('Preparing ' + record['role'] + '...', flush=True)
    remote = connect(
        api, path, state, record, journal_guard=journal_guard, cancelled=cancelled
    )
    if cancelled.is_set():
        raise InterruptedError('Worker preparation cancelled before bundle transfer')
    remote.copy(directory / 'bundle.tar.gz', 'bundle.tar.gz')
    if cancelled.is_set():
        raise InterruptedError('Worker preparation cancelled before bundle extraction')
    remote.call(
        ['python3', remote.work + '/agent.py', 'unpack', remote.work],
        120,
        directory / (record['role'] + '-setup.log'),
    )
    if cancelled.is_set():
        raise InterruptedError('Worker preparation cancelled before agent launch')
    write_job(remote, state, record, directory, journal_guard=journal_guard)
    if cancelled.is_set():
        raise InterruptedError('Worker preparation cancelled after job transfer')

    # Keep even a failed-start session so the owner closes it after all preparation
    # tasks have stopped. Cloud cleanup must never race an in-flight SSH operation.
    session = Session(remote, directory, record['role'])
    with journal_guard:
        sessions.append(session)
        record['monitoring_started'] = True
        vultr.save(path, state)
    session.wait_ready(60)


def prepare_workers(
    api: vultr.Api, path: Path, state: dict, sessions: list[Session]
) -> None:
    """Verify worker-0, then overlap the remaining VMs' boot and SSH preparation."""
    settings = state['config']
    guard = threading.RLock()
    cancelled = threading.Event()

    def create_worker(index: int) -> dict:
        """Journal each billable allocation before a preparation thread can use it."""
        with guard:
            record = vultr.create(
                api,
                path,
                state,
                settings,
                f'worker-{index}',
                state['plans'][settings['worker_plan']],
            )
            record['worker_index'] = index
            vultr.save(path, state)
            return record

    first = create_worker(0)
    prepare_worker(api, path, state, first, sessions, guard, cancelled)
    remaining = settings['worker_count'] - 1
    if not remaining:
        return
    jobs = min(WORKER_PREPARATION_JOBS, remaining)
    print(
        f'First worker ready; preparing the rest with up to {jobs} concurrent tasks.',
        flush=True,
    )
    # The executor joins active tasks before execute_run closes their sessions,
    # collects files and permits automatic deletion of the VMs.
    with ThreadPoolExecutor(max_workers=jobs) as executor:
        futures = []
        try:
            for index in range(1, settings['worker_count']):
                # Stop allocating promptly when an earlier preparation has failed.
                # A POST already in progress is still journaled before unwinding.
                for completed in futures:
                    if completed.done():
                        completed.result()
                record = create_worker(index)
                futures.append(
                    executor.submit(
                        prepare_worker,
                        api,
                        path,
                        state,
                        record,
                        sessions,
                        guard,
                        cancelled,
                    )
                )
            for completed in as_completed(futures):
                completed.result()
        except BaseException:
            cancelled.set()
            for pending in futures:
                pending.cancel()
            raise


def execute_run(api: vultr.Api, path: Path, state: dict) -> None:
    """Build and distribute one bundle, then release a lockd-owned launch barrier."""
    settings = state['config']
    sessions: list[Session] = []
    directory = path.parent
    try:
        daemon = vultr.create(
            api,
            path,
            state,
            settings,
            'daemon',
            state['plans'][settings['daemon_plan']],
        )
        print('Waiting for daemon VM and SSH...', flush=True)
        server = connect(api, path, state, daemon)
        build_bundle(server, settings, directory)
        write_job(server, state, daemon, directory)
        sessions.append(Session(server, directory, 'daemon'))
        daemon['monitoring_started'] = True
        vultr.save(path, state)
        sessions[0].wait_ready(60)

        prepare_workers(api, path, state, sessions)
        print('All generators are queued on the launch lock; releasing it.', flush=True)
        sessions[0].release(40)
        deadline = time.monotonic() + settings['run_timeout_seconds']
        while any(session.process.poll() is None for session in sessions[1:]):
            for session in sessions:
                if session.error:
                    raise RuntimeError(
                        session.role + ' monitoring failed: ' + session.error
                    )
            if sessions[0].process.poll() is not None:
                raise RuntimeError(
                    "Daemon monitoring session exited during the benchmark"
                )
            for session in sessions[1:]:
                if session.process.poll() not in (None, 0):
                    log = directory / (session.role + '-process.log')
                    raise RuntimeError(f"{session.role} failed; see {log}")
            if time.monotonic() >= deadline:
                raise TimeoutError("Benchmark deadline exceeded")
            for session in sessions:
                progress = session.progress()
                if progress is not None:
                    print(progress, flush=True)
            time.sleep(10)
        for session in sessions[1:]:
            if session.finish(10):
                log = directory / (session.role + '-process.log')
                raise RuntimeError(f"{session.role} failed; see {log}")
        if sessions[0].process.poll() is not None:
            raise RuntimeError("Daemon exited before completion")
        state['outcome'] = 'completed'
    except BaseException as error:
        # Surface the initiating failure before collection and cloud cleanup;
        # missing artifacts must not bury the network or launch error.
        print('BENCHMARK FAILED: ' + str(error), flush=True)
        raise
    finally:
        for session in reversed(sessions):
            try:
                session.close()
            except (OSError, RuntimeError, subprocess.SubprocessError) as error:
                state['outcome'] = 'incomplete'
                print('SUPERVISION FAILED: ' + session.role + ': ' + str(error))
            record = next(
                item for item in state['resources'] if item['role'] == session.role
            )
            record['process_started'] = session.has_launched_event
        for failure in collect(state, directory):
            print('COLLECTION FAILED: ' + failure)
        vultr.save(path, state)


def cleanup_command(manifest: Path) -> str:
    """Show recovery for the VM manifest, with literal paths for the user's shell."""
    arguments = ['python', '-m', 'scripts.benchmarking', 'cleanup', str(manifest)]
    if os.name == 'nt':
        return ' '.join(arguments[:-1]) + " '" + str(manifest).replace("'", "''") + "'"
    return shlex.join(arguments)


def check_resume(
    manifest: Path, configuration: Path, started: str, finished: str
) -> str:
    """Check a prior attempt's identity and recorded cleanup without API calls.

    Older round journals don't link their attempts to VM manifests. Require an
    explicit manifest and match its configuration and creation times to that
    attempt. This doesn't delete anything or infer ownership from timestamps;
    the manifest must already record every resource as deleted or rejected.
    """
    state = vultr.load(manifest)
    if state['config'] != config.load(configuration):
        raise ValueError("Prior manifest configuration differs from this profile")
    start = datetime.datetime.fromisoformat(started).timestamp()
    end = (
        datetime.datetime.fromisoformat(finished).timestamp()
        if finished and finished != '-'
        else time.time()
    )
    if any(not start <= record['intent_unix'] <= end for record in state['resources']):
        raise ValueError("Prior manifest creation times differ from this attempt")
    if any(
        record['status'] not in ('deleted', 'rejected') for record in state['resources']
    ):
        raise ValueError(
            "Prior attempt has unresolved resources. Clean up before resuming:\n"
            + cleanup_command(manifest)
        )
    return state['run_id']


def run(api: vultr.Api, settings: dict, run_id: str | None = None) -> None:
    """Start billable provisioning and automatically clean up journal-owned VMs."""
    for command in ('ssh', 'scp'):
        if not shutil.which(command):
            raise ValueError('Required command is unavailable: ' + command)
    run_id = str(uuid.UUID(run_id)) if run_id is not None else str(uuid.uuid4())
    directory = config.output_directory(run_id)
    if directory.exists():
        raise ValueError("Run directory already exists: " + str(directory))
    selection = preflight(api, settings)
    image = selection['image']
    print(
        f"Create {settings['worker_count'] + 1} billable VMs "
        f"in datacenter {settings['region']!r}."
    )
    print(
        json.dumps(
            {
                'image': image,
                'daemon_plan': settings['daemon_plan'],
                'worker_plan': settings['worker_plan'],
                'run_id': run_id,
            },
            indent=2,
        )
    )
    directory.mkdir(parents=True, exist_ok=False)
    path = directory / 'manifest.json'
    state = {
        'schema': 1,
        'run_id': run_id,
        'config': settings,
        'image': image,
        'plans': selection['plans'],
        'resources': [],
        'outcome': 'incomplete',
    }
    vultr.save(path, state)
    print('Recovery journal: ' + str(path), flush=True)
    with vultr.journal_lock(path):
        try:
            execute_run(api, path, state)
        finally:
            # Reporting failure must never prevent cleanup. Unverified ownership
            # or deletion failures retain the journal for a later recovery attempt.
            try:
                report.generate(path, state)
            except (OSError, ValueError, KeyError) as error:
                print('REPORT FAILED: ' + str(error))
            finally:
                remaining = vultr.cleanup(api, path, state)
                if remaining:
                    print(
                        'Retained or unresolved instances may incur charges. '
                        'Re-run cleanup:\n' + cleanup_command(path)
                    )
                elif state['resources'] and all(
                    record['status'] == 'rejected' for record in state['resources']
                ):
                    print(
                        'All recorded creation requests were rejected; '
                        'nothing to delete.'
                    )
    if remaining:
        raise RuntimeError(
            "Benchmark finished, but cleanup is incomplete: " + str(path)
        )


def main() -> None:
    """Expose configuration, read-only planning, execution, and recovery separately."""
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    commands.add_parser(
        'guide', help='show setup, synchronization and cleanup instructions'
    )
    example_parser = commands.add_parser(
        'example', help='print editable JSON configuration'
    )
    example_parser.add_argument(
        '--output', type=Path, help='write UTF-8 JSON to this file'
    )
    catalog = commands.add_parser(
        'catalog', help='list Vultr regions, plans, images or shared IDs'
    )
    catalog.add_argument(
        'kind', choices=('regions', 'plans', 'os', 'vpcs', 'firewalls', 'ssh-keys')
    )
    catalog.add_argument('path', type=Path, help='JSON configuration containing region')
    for name in ('plan', 'run', 'cleanup', 'collect', 'report'):
        command = commands.add_parser(name)
        command.add_argument('path', type=Path)
        if name == 'run':
            command.add_argument(
                '--run-id', help='use the UUID recorded by the round runner'
            )
    args = parser.parse_args()
    match args.command:
        case 'guide':
            from . import __doc__ as guide

            print(guide)
        case 'example':
            text = json.dumps(config.example(), indent=2) + '\n'
            if args.output:
                args.output.write_text(text, encoding='utf-8')
            else:
                print(text, end='')
        case 'plan':
            settings = config.load(args.path)
            print(
                json.dumps(
                    {
                        key: value
                        for key, value in settings.items()
                        if key != 'ssh_private_key'
                    },
                    indent=2,
                )
            )
            print(f"Creates {settings['worker_count'] + 1} VMs; no API calls made.")
        case 'report':
            report.generate(args.path, vultr.load(args.path))
        case 'collect':
            with vultr.journal_lock(args.path):
                failures = collect(vultr.load(args.path), args.path.parent)
                if failures:
                    raise RuntimeError('\n'.join(failures))
        case 'catalog':
            # Catalogs are needed to finish the configuration. Do not require
            # source archives, SSH files or selected resource IDs at this stage.
            region = config.catalog_region(args.path)
            api = vultr.Api(os.environ.get('VULTR_API_KEY', ''))
            show_catalog(api, args.kind, region)
        case _:
            api = vultr.Api(os.environ.get('VULTR_API_KEY', ''))
            match args.command:
                case 'run':
                    run(api, config.load(args.path), args.run_id)
                case 'cleanup':
                    with vultr.journal_lock(args.path):
                        remaining = vultr.cleanup(api, args.path, vultr.load(args.path))
                        if remaining:
                            raise RuntimeError(
                                "Cleanup incomplete; retained VMs may be billable. "
                                "Re-run cleanup:\n" + cleanup_command(args.path)
                            )


if __name__ == '__main__':
    try:
        main()
    except (
        OSError,
        TypeError,
        ValueError,
        RuntimeError,
        KeyError,
        StopIteration,
        argparse.ArgumentError,
        subprocess.SubprocessError,
    ) as error:
        raise SystemExit(str(error)) from None

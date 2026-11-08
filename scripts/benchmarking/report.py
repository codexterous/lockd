"""Merge native histograms and render honest, self-contained benchmark reports."""

import csv
import html
import itertools
import json
import shutil
from collections import Counter
from pathlib import Path

from .config import object_value, output_directory


def read_result(path: Path) -> dict:
    """Reject partial counters and histogram mismatches before publishing rates."""
    metadata = {}
    counters = {'cycles': 0, 'timeouts': 0, 'errors': 0}
    histogram: Counter[int] = Counter()
    client_counts: dict[str, int] = {}
    worker_cycles: dict[str, int] = {}
    seen: set[tuple[str, str, str]] = set()
    with path.open(newline='', encoding='utf-8') as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames != ['metric', 'worker', 'value', 'count']:
            raise ValueError("Unexpected benchmark CSV columns")
        for row in reader:
            metric, worker, value, count = (row[key] for key in reader.fieldnames)
            identity = (metric, worker, value if metric == 'acquisition_us' else '')
            if identity in seen:
                raise ValueError("Duplicate benchmark metric")
            seen.add(identity)
            if worker == 'all':
                metadata[metric] = value
            elif metric in counters:
                number = int(value)
                if number < 0:
                    raise ValueError("Negative benchmark counter")
                counters[metric] += number
                if metric == 'cycles':
                    worker_cycles[worker] = number
            elif metric == 'clients_completed':
                client_counts[worker] = int(value)
            elif metric == 'acquisition_us':
                upper, samples = int(value), int(count)
                if upper < 0 or samples <= 0:
                    raise ValueError("Invalid histogram bucket")
                histogram[upper] += samples
    metadata.setdefault('engine', 'sync')
    if metadata['engine'] not in ('sync', 'async'):
        raise ValueError("Unknown benchmark engine")
    duration = int(metadata['measurement_seconds'])
    start = int(metadata['start_unix_ms']) / 1000
    if duration <= 0 or start <= 0 or metadata['aborted'] != '0':
        raise ValueError("Aborted or invalid measurement window")
    if sum(histogram.values()) != counters['cycles']:
        raise ValueError("Histogram sample count differs from completed cycles")
    threads = int(metadata['threads'])
    connections = int(metadata['connections_per_worker'])
    if not 1 <= threads <= 65535 or not 1 <= connections <= 65535:
        raise ValueError("Invalid worker or connection count")
    for worker in range(threads):
        if not all((metric, str(worker), '') in seen for metric in counters):
            raise ValueError("Missing worker counters")
    required = metadata.get('require_all_clients', '0')
    if required not in ('0', '1'):
        raise ValueError("Invalid client coverage requirement")
    if client_counts:
        if set(client_counts) != {str(worker) for worker in range(threads)}:
            raise ValueError("Missing or unexpected client coverage workers")
        if any(
            not 0 <= count <= min(connections, worker_cycles[worker])
            for worker, count in client_counts.items()
        ):
            raise ValueError("Invalid measured client coverage")
    elif required == '1':
        raise ValueError("Required measured client coverage is missing")
    completed = sum(client_counts.values()) if client_counts else None
    return {
        'metadata': metadata,
        **counters,
        'histogram': dict(histogram),
        'duration_seconds': duration,
        'start_unix': start,
        'end_unix': start + duration,
        'cycles_per_second': counters['cycles'] / duration,
        'connections': threads * connections,
        'threads': threads,
        'clients_completed': completed,
        'client_coverage_complete': completed == threads * connections
        if completed is not None
        else None,
    }


def percentile(histogram: dict[int, int], percent: int) -> int:
    """Calculate an upper-bound percentile from merged bucket counts."""
    total = sum(histogram.values())
    rank = (total * percent + 99) // 100
    cumulative = 0
    for upper, count in sorted(histogram.items()):
        cumulative += count
        if cumulative >= rank:
            return upper
    return 0


def workload_context(results: list[dict]) -> dict | None:
    """Relate measured waits and throughput to the configured protected work.

    Call only for a complete, compatible fleet. These bounds describe the load
    generator's concurrency and simulated work, not time spent inside lockd.
    We don't record queue depth or actual hold intervals, so subtracting an
    estimated queueing delay from latency would invent a daemon overhead figure.
    """
    metadata = results[0]['metadata']
    if metadata.get('workload') not in ('independent', 'contended') or not all(
        key in metadata for key in ('hold_ms', 'lock_count', 'lock_prefix')
    ):
        return None
    try:
        hold_ms = int(metadata['hold_ms'])
        lock_count = int(metadata['lock_count'])
    except ValueError:
        return None
    if hold_ms < 0 or lock_count <= 0:
        return None

    concurrency = sum(
        result['threads'] if metadata['engine'] == 'sync' else result['connections']
        for result in results
    )
    holders = (
        min(concurrency, lock_count)
        if metadata['workload'] == 'contended'
        else concurrency
    )
    ceiling = holders * 1000 / hold_ms if hold_ms else None
    histogram: Counter[int] = Counter()
    for result in results:
        histogram.update(result['histogram'])
    return {
        'hold_ms': hold_ms,
        'shared_lock_count': lock_count
        if metadata['workload'] == 'contended'
        else None,
        'maximum_concurrent_holders': holders,
        'ideal_work_ceiling_cycles_per_second': ceiling,
        'work_ceiling_reached_percent': (
            100 * sum(result['cycles_per_second'] for result in results) / ceiling
            if ceiling is not None
            else None
        ),
        'latency_upper_bound_work_periods': {
            f'p{percent}': percentile(histogram, percent) / (hold_ms * 1000)
            if hold_ms and histogram
            else None
            for percent in (50, 95, 99)
        },
    }


def read_setup(path: Path) -> dict | None:
    """Read initial handshakes even when setup aborted, without publishing a rate."""
    with path.open(newline='', encoding='utf-8') as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames != ['metric', 'worker', 'value', 'count']:
            raise ValueError('Unexpected benchmark CSV columns')
        rows = list(reader)
    metrics = ('tcp_connected', 'greetings_received')
    if not any(row['metric'] in metrics for row in rows):
        return None
    metadata, counts = {}, {name: {} for name in metrics}
    for row in rows:
        metric, worker, value = row['metric'], row['worker'], row['value']
        if worker == 'all':
            if metric in metadata:
                raise ValueError('Duplicate setup metadata')
            metadata[metric] = value
        elif metric in counts:
            if worker in counts[metric]:
                raise ValueError('Duplicate setup counter')
            counts[metric][worker] = int(value)
    threads = int(metadata['threads'])
    connections = int(metadata['connections_per_worker'])
    if not 1 <= threads <= 65535 or not 1 <= connections <= 65535:
        raise ValueError('Invalid setup worker or connection count')
    expected = {str(worker) for worker in range(threads)}
    if any(set(values) != expected for values in counts.values()):
        raise ValueError('Missing or unexpected setup workers')
    for worker in expected:
        if not (
            0
            <= counts['greetings_received'][worker]
            <= counts['tcp_connected'][worker]
            <= connections
        ):
            raise ValueError('Invalid initial handshake counts')
    return {
        'requested': threads * connections,
        **{name: sum(values.values()) for name, values in counts.items()},
    }


def network_summary(path: Path, samples: list[dict]) -> dict:
    """Keep namespace counter deltas distinct from sampled process queue peaks."""
    events = [
        json.loads(line) for line in path.read_text(encoding='utf-8').splitlines()
    ]
    host = next((event for event in events if event['event'] == 'host'), {})
    baseline = host.get('kernel_network', {})
    snapshots = [sample.get('kernel_network', {}) for sample in samples]
    counters = {}
    for name, start in baseline.get('counters', {}).items():
        values = [snapshot.get('counters', {}).get(name) for snapshot in snapshots]
        observed = [start, *values]
        complete = all(value is not None for value in observed)
        delta = values[-1] - start if values and complete else None
        if complete and any(b < a for a, b in itertools.pairwise(observed)):
            delta = None
        counters[name] = {
            'baseline': start,
            'last': values[-1] if values else None,
            'increase': delta,
        }
    listeners = {}
    for sample in samples:
        queues = Counter()
        for listener in sample.get('tcp', {}).get('listeners', []):
            key = f'{listener["family"]}:{listener["port"]}'
            queues[key] += listener['accept_queue']
            entry = listeners.setdefault(
                key, {'accept_queue_peak': 0, 'syn_recv_on_port_peak': 0}
            )
            entry['syn_recv_on_port_peak'] = max(
                entry['syn_recv_on_port_peak'], listener['syn_recv_on_port']
            )
        for key, count in queues.items():
            listeners[key]['accept_queue_peak'] = max(
                listeners[key]['accept_queue_peak'], count
            )
    gauges = {}
    states = {}
    for sample, snapshot in zip(samples, snapshots, strict=True):
        for name, value in snapshot.get('gauges', {}).items():
            gauges[name] = max(gauges.get(name, value), value)
        for name, value in sample.get('tcp', {}).get('socket_states', {}).items():
            states[name] = max(states.get(name, value), value)
    return {
        'limits': baseline.get('limits', {}),
        'nofile': host.get('nofile'),
        'counter_deltas': counters,
        'peak_gauges': gauges,
        'peak_owned_socket_states': states,
        'listeners': listeners,
        'peak_fd_count': max(
            (sample['fd_count'] for sample in samples if 'fd_count' in sample),
            default=None,
        ),
        'unavailable': sorted(
            {
                name
                for snapshot in [baseline, *snapshots]
                for name in snapshot.get('unavailable', {})
            }
            | {
                name
                for sample in samples
                for name in sample.get('tcp', {}).get('unavailable', {})
            }
        ),
    }


def read_samples(path: Path, cpu_count: int | None = None) -> list[dict]:
    """Derive CPU usage with guest-reported vCPUs, falling back to VM metadata."""
    events = [
        json.loads(line) for line in path.read_text(encoding='utf-8').splitlines()
    ]
    samples = [event for event in events if event['event'] == 'sample']
    observed_count = next(
        (event.get('cpu_count') for event in events if event['event'] == 'host'),
        None,
    )
    if type(observed_count) is int and observed_count > 0:
        cpu_count = observed_count
    if type(cpu_count) is not int or cpu_count < 1:
        cpu_count = None
    for sample in samples:
        sample['cpu_capacity_vcpus'] = cpu_count
    for previous, current in itertools.pairwise(samples):
        elapsed = current['monotonic'] - previous['monotonic']
        if (
            elapsed <= 0
            or current['start_ticks'] != previous['start_ticks']
            or current['pid'] != previous['pid']
        ):
            raise ValueError("Process identity or monotonic clock changed")
        current['cpu_percent'] = (
            (current['cpu_ticks'] - previous['cpu_ticks'])
            / current['clock_ticks']
            / elapsed
            * 100
        )
        if cpu_count is not None:
            current['cpu_capacity_percent'] = current['cpu_percent'] / cpu_count
    return samples


def resources(samples: list[dict]) -> dict:
    """Separate sampled peaks from Linux's lifetime RSS high-water mark."""
    if not samples:
        raise ValueError("No process samples collected")
    elapsed = samples[-1]['monotonic'] - samples[0]['monotonic']
    ticks = samples[-1]['cpu_ticks'] - samples[0]['cpu_ticks']
    average = ticks / samples[-1]['clock_ticks'] / elapsed * 100 if elapsed else None
    peak = max(x.get('cpu_percent', 0) for x in samples)
    cpu_count = samples[0].get('cpu_capacity_vcpus')
    return {
        'samples': len(samples),
        'observed_seconds': elapsed,
        # Preserve one-core fields for existing raw-data consumers. Report-facing
        # fields explicitly name whole-VM capacity and retain their denominator.
        'cpu_average_percent': average,
        'cpu_peak_sample_percent': peak,
        'cpu_capacity_vcpus': cpu_count,
        'cpu_capacity_average_percent': average / cpu_count
        if average is not None and cpu_count is not None
        else None,
        'cpu_capacity_peak_sample_percent': peak / cpu_count
        if cpu_count is not None and len(samples) > 1
        else None,
        'baseline_rss_mib': samples[0]['rss_bytes'] / 1024**2,
        'peak_sampled_rss_mib': max(x['rss_bytes'] for x in samples) / 1024**2,
        'largest_observed_hwm_rss_mib': max(x['hwm_bytes'] for x in samples) / 1024**2,
        'peak_threads': max(x['threads'] for x in samples),
        'peak_established_process_sockets': max(x['established'] for x in samples),
        'minimum_host_available_mib': min(x['host_available_bytes'] for x in samples)
        / 1024**2,
    }


def measurement_cpu(samples: list[dict], start: float, end: float) -> dict:
    """
    Average interval CPU over the shared window, weighting clipped interval lengths.

    Window boundaries use the generators' reported wall clocks. CPU rates come
    from monotonic intervals. At a boundary, assume CPU was evenly distributed
    within that sampling interval. Never extrapolate beyond recorded samples or
    fill missing intervals with zero. Partial coverage averages only the observed
    intersection and is labeled as partial in the report, not as a full-window mean.
    """
    if not samples or end <= start:
        raise ValueError(
            "CPU averaging requires samples and a positive measurement window"
        )
    covered = weighted = 0.0
    for previous, current in itertools.pairwise(samples):
        if current['unix'] <= previous['unix']:
            raise ValueError("Process wall clock did not advance during CPU sampling")
        overlap = min(end, current['unix']) - max(start, previous['unix'])
        if overlap > 0:
            covered += overlap
            weighted += current['cpu_percent'] * overlap
    complete = samples[0]['unix'] <= start and samples[-1]['unix'] >= end
    average = weighted / covered if covered > 0 else None
    cpu_count = samples[0].get('cpu_capacity_vcpus')
    return {
        'window_seconds': end - start,
        'covered_seconds': covered,
        'coverage_complete': complete and covered > 0,
        'coverage_percent': 100 * covered / (end - start),
        'cpu_average_percent': average,
        'cpu_capacity_average_percent': average / cpu_count
        if average is not None and cpu_count is not None
        else None,
    }


def plot(
    samples: list[dict],
    key: str,
    label: str,
    divisor: float = 1,
    *,
    percentage: bool = False,
    window: tuple[float, float] | None = None,
) -> str:
    """
    Draw a standalone SVG trace without requiring plotting packages or a browser
    service. For interval rates, an optional wall-clock window clips each
    preceding sample interval, matching the weighting used by measurement_cpu.
    Missing boundary coverage stays missing rather than stretching the trace.
    """
    if window is not None:
        start, end = window
        duration = end - start
        values = []
        for previous, current in itertools.pairwise(samples):
            left = max(start, previous['unix'])
            right = min(end, current['unix'])
            if right > left and key in current:
                values.extend(
                    (elapsed - start, current[key] / divisor)
                    for elapsed in (left, right)
                )
    else:
        first = samples[0]['monotonic'] if samples else 0
        duration = max(1, samples[-1]['monotonic'] - first) if samples else 1
        values = [
            (sample['monotonic'] - first, sample[key] / divisor)
            for sample in samples
            if key in sample
        ]
    if not values:
        return ''
    # Keep capacity graphs on a common 0-100% scale, including quiet runs. If a
    # measurement overshoots capacity, show it rather than clipping the evidence.
    maximum = max(100 if percentage else 1, max(value for _, value in values))
    points = ' '.join(
        f'{40 + elapsed / duration * 610:.2f},{170 - value / maximum * 140:.2f}'
        for elapsed, value in values
    )
    ticks = (
        ''.join(
            f'<text x="35" y="{174 - fraction * 140:.0f}" text-anchor="end" '
            f'font-size="11">{maximum * fraction:.0f}%</text>'
            for fraction in (0, 0.25, 0.5, 0.75, 1)
        )
        if percentage
        else ''
    )
    return (
        '<svg viewBox="0 0 680 210" role="img" xmlns="http://www.w3.org/2000/svg">'
        f'<title>{html.escape(label)}</title><path d="M40 25V170H650" '
        'fill="none" stroke="#888"/>'
        + ticks
        + f'<polyline points="{points}" fill="none" stroke="#1466aa" stroke-width="2"/>'
        f'<text x="40" y="20">{html.escape(label)} (scale 0-{maximum:.2f})</text>'
        f'<text x="40" y="195">0</text><text x="550" y="195">{duration:.1f} s</text>'
        '</svg>'
    )


def table(headers: list[str], rows: list[list[object]]) -> str:
    """Render readable report tables while treating every recorded value as text."""
    return (
        '<div class="table"><table><thead><tr>'
        + ''.join('<th>' + html.escape(header) + '</th>' for header in headers)
        + '</tr></thead><tbody>'
        + ''.join(
            '<tr>'
            + ''.join('<td>' + html.escape(str(value)) + '</td>' for value in row)
            + '</tr>'
            for row in rows
        )
        + '</tbody></table></div>'
    )


def workload_tables(summary: dict) -> str:
    """Keep raw waits beside work-relative figures, without subtracting queueing."""
    context = summary.get('workload_context')
    if context is None:
        return ''
    aggregate = summary['aggregate']
    ceiling = context['ideal_work_ceiling_cycles_per_second']
    ceiling_reached = (
        f'{context["work_ceiling_reached_percent"]:.2f}%'
        if ceiling is not None
        else 'not applicable'
    )
    return (
        '<h2>Protected-work context</h2>'
        + table(
            ['Metric', 'Value'],
            [
                ['Simulated protected work per cycle, ms', context['hold_ms']],
                [
                    'Shared lock names across all generators',
                    context['shared_lock_count']
                    if context['shared_lock_count'] is not None
                    else 'independent names',
                ],
                [
                    'Ideal upper bound on concurrent holders',
                    context['maximum_concurrent_holders'],
                ],
                [
                    'Sum of generator rates, cycles/s',
                    f'{aggregate["sum_individual_cycles_per_second"]:.2f}',
                ],
                [
                    'Ideal protected-work ceiling, cycles/s',
                    f'{ceiling:.2f}' if ceiling is not None else 'not applicable',
                ],
                [
                    'Protected-work ceiling reached',
                    ceiling_reached,
                ],
            ],
        )
        + '<p>The ideal ceiling is maximum concurrent holders divided by simulated '
        'work duration, assuming immediate handoff and no other costs. This is an '
        'optimistic bound from configured concurrency and the shared lock pool; '
        'name assignment can further limit concurrency. The observed gap includes '
        'client scheduling, networking, lock handoff and uneven load; '
        'it does not isolate daemon overhead. Zero-duration work has no finite '
        'work-derived ceiling.</p>'
        + table(
            [
                'Percentile',
                'Raw acquisition wait, ms',
                'Ideal workload ceiling reached (fleet)',
                'Wait / protected-work period',
            ],
            [
                [
                    name,
                    f'{aggregate["merged_latency_upper_bound_us"][name] / 1000:.3f}',
                    ceiling_reached,
                    f'{ratio:.4g}x' if ratio is not None else 'not applicable',
                ]
                for name, ratio in context['latency_upper_bound_work_periods'].items()
            ],
        )
        + '<p>Raw waits and work-period ratios use merged histogram bucket upper '
        'bounds. The ceiling percentage applies to the whole fleet, not each '
        'percentile. Raw waits are '
        'the delays clients experienced, including contention. The ratio expresses '
        'that wait in units of configured protected work. It is not a measured queue '
        'depth or a lockd-only latency; no estimated queueing delay is subtracted.</p>'
    )


def generate(path: Path, state: dict) -> Path:
    """Export results and limitations without inventing a common measurement window."""
    destination = output_directory(state['run_id'], final=True)
    destination.mkdir(parents=True, exist_ok=True)
    summary = {
        'run_id': state['run_id'],
        'outcome': state['outcome'],
        'image': state.get('image'),
        'source_sha256': state['config']['source_sha256'],
        'machines': [],
        'warnings': [],
        'valid_measurement': False,
        'profiled': state['config'].get('profile_daemon', False),
    }
    histograms: Counter[int] = Counter()
    results = []
    figures = []
    machine_samples: dict[str, list[dict]] = {}
    for record in state['resources']:
        role = record['role']
        machine = {
            key: record.get(key)
            for key in ('role', 'vcpu_count', 'ram', 'options', 'logging')
        }
        # Public reports need the hardware plan and image, not the account's VPC,
        # firewall and SSH-key IDs or the tags used to authorize cleanup.
        machine['request'] = {
            key: value
            for key, value in record.get('request', {}).items()
            if key in ('region', 'plan', 'os_id', 'label')
        }
        try:
            metrics = path.parent / (role + '-metrics.jsonl')
            if not metrics.exists():
                metrics = path.parent / (role + '-stream.jsonl')
            samples = read_samples(metrics, record.get('vcpu_count'))
            machine['network_diagnostics'] = network_summary(metrics, samples)
            machine_samples[role] = samples
            machine['resources'] = resources(samples)
            cpu_count = machine['resources']['cpu_capacity_vcpus']
            figures.append(
                '<h2>'
                + html.escape(role)
                + '</h2>'
                + ''.join(
                    plot(
                        samples,
                        key,
                        label,
                        divisor,
                        percentage=key == 'cpu_capacity_percent',
                    )
                    for key, label, divisor in (
                        ('rss_bytes', 'RSS (MiB)', 1024**2),
                        (
                            'cpu_capacity_percent',
                            f'Process CPU (% of VM capacity; {cpu_count} vCPUs)',
                            1,
                        ),
                        ('threads', 'Threads', 1),
                        ('established', 'Established process sockets', 1),
                    )
                )
            )
            if role != 'daemon':
                csv_path = path.parent / (role + '-result.csv')
                machine['setup'] = read_setup(csv_path)
                result = read_result(csv_path)
                if (
                    result['metadata'].get('require_all_clients') == '1'
                    or (record.get('options') or {}).get('require_all_clients', False)
                ) and not result['client_coverage_complete']:
                    summary['warnings'].append(
                        role + ' measured client coverage is incomplete'
                    )
                machine['result'] = result
                histograms.update(result['histogram'])
                results.append(result)
        except (OSError, ValueError, KeyError) as error:
            summary['warnings'].append(role + ': ' + str(error))
        summary['machines'].append(machine)
    if summary['profiled']:
        try:
            profile = object_value(
                json.loads(
                    (path.parent / 'daemon-profile.json').read_text(encoding='utf-8')
                )
            )
            summary['daemon_profile'] = {
                key: profile[key]
                for key in ('status', 'event', 'frequency_hz', 'call_graph', 'scope')
            }
            if profile['status'] != 'completed':
                raise ValueError('Daemon CPU profile was not completed')
            for name in ('daemon-perf.data', 'daemon-perf-report.txt'):
                if not (path.parent / name).is_file():
                    raise ValueError('Missing local profile artifact: ' + name)
        except (OSError, ValueError, TypeError, KeyError) as error:
            summary['warnings'].append('Daemon profiling: ' + str(error))
    if results:
        signatures = {
            tuple(
                result['metadata'].get(key)
                for key in (
                    'engine',
                    'workload',
                    'lock_prefix',
                    'hold_ms',
                    'lock_count',
                )
            )
            for result in results
        }
        summary['aggregate'] = {
            'generators_reported': len(results),
            'requested_connections': sum(result['connections'] for result in results),
            'native_worker_threads': sum(result['threads'] for result in results),
            'clients_completed': sum(result['clients_completed'] for result in results)
            if all(result['clients_completed'] is not None for result in results)
            else None,
            'client_coverage_complete': all(
                result['client_coverage_complete'] for result in results
            )
            if all(result['client_coverage_complete'] is not None for result in results)
            else None,
            'cycles': sum(result['cycles'] for result in results),
            'timeouts': sum(result['timeouts'] for result in results),
            'errors': sum(result['errors'] for result in results),
            'sum_individual_cycles_per_second': sum(
                x['cycles_per_second'] for x in results
            ),
            'estimated_clock_uncorrected_overlap_seconds': max(
                0,
                min(x['end_unix'] for x in results)
                - max(x['start_unix'] for x in results),
            ),
            'merged_latency_upper_bound_us': {
                f'p{percent}': percentile(histograms, percent)
                for percent in (50, 95, 99)
            },
        }
        if len(signatures) != 1:
            summary['warnings'].append(
                "Workloads or engines differ; do not publish a combined workload result"
            )
        summary['valid_measurement'] = (
            state['outcome'] == 'completed'
            and len(results) == state['config']['worker_count']
            and not summary['warnings']
            and not any(x['errors'] for x in results)
        )
        if summary['valid_measurement']:
            summary['workload_context'] = workload_context(results)
    summary['cpu_measurement_window'] = None
    if len(results) == state['config']['worker_count']:
        start = max(result['start_unix'] for result in results)
        end = min(result['end_unix'] for result in results)
        if end > start:
            summary['cpu_measurement_window'] = {
                'start_unix': start,
                'end_unix': end,
                'seconds': end - start,
            }
            for machine in summary['machines']:
                samples = machine_samples.get(machine['role'])
                if samples:
                    try:
                        machine['measurement_cpu'] = measurement_cpu(
                            samples, start, end
                        )
                    except (ValueError, KeyError) as error:
                        summary['warnings'].append(
                            machine['role'] + ' CPU window: ' + str(error)
                        )
    summary['warnings'].extend(
        [
            (
                'Generators share a launch barrier, '
                'not an exact native measurement barrier.'
            ),
            (
                'Clock offsets between VMs are unverified; '
                'overlap uses their reported wall clocks.'
            ),
            (
                'Summed individual rates are not throughput '
                'measured over one common window.'
            ),
            (
                'Resource summaries cover setup, warm-up, measurement and drain; '
                'peaks are sampled.'
            ),
            (
                'Displayed CPU averages use only the shared generator '
                'measurement window. Boundary intervals are proportionally weighted; '
                'partial coverage averages the observed intervals only, without '
                'extrapolating across missing time, and is labeled partial. '
                'Lifetime CPU averages remain in the raw resource summary.'
            ),
            (
                'CPU graphs and tables show process usage as a percentage of total '
                'VM CPU capacity: 100% uses all guest vCPUs. '
                'This is not total host utilization. '
                'Raw one-core CPU fields retain their original units.'
            ),
            'VmHWM is observed through the last sample; exit-time peaks can be missed.',
            (
                'VmHWM includes pre-measurement allocations. '
                'Socket counts include the gate.'
            ),
        ]
    )
    if summary['profiled']:
        summary['warnings'].append(
            'CPU profiling adds overhead. These are profiled measurements, '
            'not an unprofiled throughput baseline. Sampling covers connection '
            'setup, warm-up, measurement and drain after gate release. '
            'CPU samples do not measure time spent blocked on I/O or locks.'
        )
    for source in path.parent.iterdir():
        if source.is_file() and (
            source.suffix in ('.csv', '.jsonl') or source.name.endswith('-process.log')
        ):
            shutil.copy2(source, destination / source.name)
    build = path.parent / 'build.json'
    if build.exists():
        shutil.copy2(build, destination / build.name)
    encoded = json.dumps(summary, indent=2)
    (destination / 'summary.json').write_text(encoded + '\n', encoding='utf-8')
    resource_rows = []
    cpu_rows = []
    worker_rows = []
    setup_rows = []
    network_rows = []
    counter_rows = []
    for machine in summary['machines']:
        setup = machine.get('setup')
        if setup:
            setup_rows.append(
                [
                    machine['role'],
                    setup['requested'],
                    setup['tcp_connected'],
                    setup['greetings_received'],
                ]
            )
        diagnostics = machine.get('network_diagnostics', {})
        if diagnostics:
            limits = diagnostics['limits']
            network_rows.append(
                [
                    machine['role'],
                    diagnostics['peak_fd_count']
                    if diagnostics['peak_fd_count'] is not None
                    else 'unavailable',
                    diagnostics['nofile'] or 'unavailable',
                    limits.get('net/core/somaxconn', 'unavailable'),
                    limits.get('net/ipv4/tcp_max_syn_backlog', 'unavailable'),
                    diagnostics['peak_gauges'].get('nf_conntrack_count', 'unavailable'),
                    limits.get('net/netfilter/nf_conntrack_max', 'unavailable'),
                ]
            )
            for name, values in diagnostics['counter_deltas'].items():
                counter_rows.append(
                    [
                        machine['role'],
                        name,
                        values['baseline'],
                        values['last'],
                        values['increase']
                        if values['increase'] is not None
                        else 'unavailable',
                    ]
                )
        measured = machine.get('resources')
        if measured:
            window_cpu = machine.get('measurement_cpu', {})
            average = window_cpu.get('cpu_capacity_average_percent')
            cpu_rows.append(
                [
                    machine['role'],
                    measured['cpu_capacity_vcpus'] or 'unknown',
                    (
                        f'{average:.2f}'
                        + (' (partial)' if not window_cpu['coverage_complete'] else '')
                    )
                    if average is not None
                    else 'unavailable',
                    f'{window_cpu["covered_seconds"]:.2f} / '
                    f'{window_cpu["window_seconds"]:.2f}'
                    if window_cpu
                    else 'unavailable',
                ]
            )
            peak = measured['cpu_capacity_peak_sample_percent']
            resource_rows.append(
                [
                    machine['role'],
                    machine['request'].get('plan', ''),
                    measured['cpu_capacity_vcpus'] or 'unknown',
                    f'{peak:.1f}' if peak is not None else 'unavailable',
                    f'{measured["peak_sampled_rss_mib"]:.2f}',
                    f'{measured["largest_observed_hwm_rss_mib"]:.2f}',
                    measured['peak_threads'],
                    measured['peak_established_process_sockets'],
                ]
            )
        result = machine.get('result')
        if result:
            worker_rows.append(
                [
                    machine['role'],
                    result['metadata']['workload'],
                    result['metadata']['engine'],
                    result['connections'],
                    result['clients_completed']
                    if result['clients_completed'] is not None
                    else 'not recorded',
                    f'{result["cycles_per_second"]:.1f}',
                    result['timeouts'],
                    result['errors'],
                    *[
                        percentile(result['histogram'], percent)
                        for percent in (50, 95, 99)
                    ],
                ]
            )
    worker_table = '<h2>Generator results</h2>' + table(
        [
            'Machine',
            'Workload',
            'Engine',
            'Connections',
            'Clients with measured cycles',
            'Cycles/s',
            'Timeouts',
            'Errors',
            'p50 us',
            'p95 us',
            'p99 us',
        ],
        worker_rows,
    )
    window = summary['cpu_measurement_window']
    measured_figures = []
    if window:
        for machine in summary['machines']:
            role = machine['role']
            average = machine.get('measurement_cpu', {}).get(
                'cpu_capacity_average_percent'
            )
            if average is None:
                continue
            coverage = machine['measurement_cpu']['coverage_complete']
            figure = (
                f'<h3>{html.escape(role)}</h3><p><strong>'
                f'CPU average: {average:.3f}% of VM capacity'
                + (' (partial coverage)' if not coverage else '')
                + '</strong></p>'
                + plot(
                    machine_samples[role],
                    'cpu_capacity_percent',
                    'CPU during shared measurement (% of VM capacity)',
                    percentage=True,
                    window=(window['start_unix'], window['end_unix']),
                )
            )
            measured_figures.append(
                figure
                if role == 'daemon'
                else '<details><summary>'
                + html.escape(role)
                + ' measurement CPU</summary>'
                + figure
                + '</details>'
            )
    tables = (
        workload_tables(summary)
        + '<h2>CPU during the shared measurement window</h2><p>'
        + (
            f'{window["seconds"]:.2f} seconds when all generators were measuring. '
            'VM clock offsets are unverified.'
            if window
            else 'Unavailable: a positive overlap and valid results '
            'from every generator are required.'
        )
        + ' Setup, warm-up and teardown are excluded; all activity inside the '
        'window is retained. Partial CPU averages use only the recorded portion; '
        'the coverage column shows how much was observed. '
        'Missing time is not counted as idle.</p>'
        + ''.join(measured_figures)
        + table(
            ['Machine', 'vCPUs', 'CPU avg % of VM', 'Covered / window seconds'],
            cpu_rows,
        )
        + worker_table
    )
    tables += '<h2>Resources over the observed process lifetime</h2>' + table(
        [
            'Machine',
            'Plan',
            'vCPUs',
            'CPU peak % of VM',
            'Sampled RSS peak MiB',
            'Observed VmHWM MiB',
            'Peak threads',
            'Peak established sockets',
        ],
        resource_rows,
    )
    tables += '<h2>Logging</h2>' + table(
        ['Machine', 'Settings'],
        [
            [machine['role'], machine.get('logging') or 'not recorded']
            for machine in summary['machines']
        ],
    )
    tables += (
        '<h2>Initial connection setup</h2><p>'
        'Counts exclude churn reconnects and remain available for aborted setup. '
        'Older generators do not record them.</p>'
        + table(
            ['Machine', 'Requested', 'TCP connected', 'Greetings received'],
            setup_rows,
        )
    )
    tables += (
        '<h2>Network diagnostics</h2><p>'
        'Read-only samples cover setup through shutdown. Kernel TCP counters '
        'cover the network namespace, including SSH and other processes. '
        'Missing or reset counters are unavailable, not zero. Queue and descriptor '
        'peaks can miss brief spikes. somaxconn caps the application backlog; '
        'it is not a measurement of that backlog.</p>'
        + table(
            [
                'Machine',
                'Peak FDs',
                'FD soft/hard limit',
                'somaxconn',
                'tcp_max_syn_backlog',
                'Peak conntrack entries',
                'Conntrack limit',
            ],
            network_rows,
        )
    )
    listener_rows = [
        [
            machine['role'],
            name,
            values['accept_queue_peak'],
            values['syn_recv_on_port_peak'],
        ]
        for machine in summary['machines']
        for name, values in machine.get('network_diagnostics', {})
        .get('listeners', {})
        .items()
    ]
    tables += table(
        [
            'Machine',
            'Listener family:port',
            'Peak accept queue',
            'Peak SYN_RECV on namespace port',
        ],
        listener_rows,
    )
    tables += (
        '<details><summary>Kernel TCP counter increases</summary>'
        + table(
            ['Machine', 'Counter', 'Before launch', 'Last sample', 'Increase'],
            counter_rows,
        )
        + '</details><p>Raw metrics and summary include socket states, '
        'TCP memory gauges, '
        'kernel settings and unavailable diagnostic names.</p>'
    )
    if summary['profiled']:
        tables += (
            '<h2>Daemon CPU profiling</h2><p>'
            'Read daemon-perf-report.txt in the local recovery journal for hot '
            'functions and call stacks. daemon-perf.data and the matching '
            'bundle.tar.gz are retained there for further analysis. '
            'Raw stacks, addresses and profiler diagnostics are excluded from '
            'this public report. Check daemon-perf.log for recorder warnings '
            'and lost samples; unresolved symbols and truncated stacks limit '
            'what the profile can establish.</p>'
        )
    body = (
        '<!doctype html><html lang="en"><meta charset="utf-8">'
        '<meta name="viewport" content="width=device-width,initial-scale=1">'
        '<title>lockd benchmark</title><style>'
        'body{font:16px system-ui;margin:2rem auto;max-width:1000px;padding:0 1rem}'
        'svg{width:100%;max-width:680px}'
        'pre{white-space:pre-wrap;overflow-wrap:anywhere}'
        '.table{overflow:auto}table{border-collapse:collapse;width:100%}'
        'th,td{text-align:left;border-bottom:1px solid #ccc;padding:.6rem}'
        '</style><h1>lockd benchmark</h1><p>'
        + (
            (
                'Completed profiled measurement.'
                if summary['profiled']
                else 'Completed measurement.'
            )
            if summary['valid_measurement']
            else 'INCOMPLETE OR INVALID: do not use this run for a performance claim.'
        )
        + '</p>'
        + tables
        + '<h2>Interpretation</h2><ul>'
        + ''.join('<li>' + html.escape(item) + '</li>' for item in summary['warnings'])
        + '</ul>'
        + '<details><summary>Raw summary and configuration</summary><pre>'
        + html.escape(encoded)
        + '</pre></details>'
        + '<details><summary>Full process lifetime, including setup and shutdown'
        '</summary>' + ''.join(figures) + '</details>' + '</html>'
    )
    (destination / 'report.html').write_text(body, encoding='utf-8')
    print('Report: ' + str(destination / 'report.html'))
    return destination

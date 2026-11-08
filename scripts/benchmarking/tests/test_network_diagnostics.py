"""Verify read-only Linux diagnostics using isolated proc snapshots."""

import json
import os
import socket
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from scripts.benchmarking import agent, config, report


class NetworkDiagnosticsTests(unittest.TestCase):
    """Keep optional observations separate from zero counters and tuning."""

    def setUp(self) -> None:
        """Create only local proc fixtures, never contact a host or cloud API."""
        root = config.ROOT / 'output/obj' / config.host_architecture()
        root = root / 'opt/shared/verifications/benchmarking'
        root.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=root)
        self.addCleanup(self.temporary.cleanup)
        self.proc = Path(self.temporary.name)
        self.net = self.proc / 'net'
        self.net.mkdir()

    def write_counters(self) -> None:
        """Provide extra unrelated groups and representative optional gauges."""
        (self.net / 'netstat').write_text(
            'Other: A\nOther: 99\n'
            'TcpExt: ListenOverflows ListenDrops SyncookiesSent\n'
            'TcpExt: 4 7 2\n'
        )
        (self.net / 'snmp').write_text('Tcp: RetransSegs OutRsts\nTcp: 17 3\n')
        (self.net / 'sockstat').write_text(
            'sockets: used 19\nTCP: inuse 12 orphan 2 tw 3 alloc 17 mem 4\n'
        )
        path = self.proc / 'sys/net/core'
        path.mkdir(parents=True)
        (path / 'somaxconn').write_text('4096\n')

    def test_optional_kernel_counters_are_named_and_read_only(self) -> None:
        """Read existing values; absent conntrack isn't reported as zero usage."""
        self.write_counters()
        before = {
            str(path): path.read_bytes()
            for path in self.proc.rglob('*')
            if path.is_file()
        }
        with mock.patch.object(agent.subprocess, 'run') as commands:
            data = agent.kernel_network(self.proc, self.net, limits=True)
        commands.assert_not_called()
        self.assertEqual(data['counters']['TcpExt.ListenDrops'], 7)
        self.assertEqual(data['counters']['Tcp.RetransSegs'], 17)
        self.assertEqual(data['gauges']['TCP.mem'], 4)
        self.assertEqual(data['limits']['net/core/somaxconn'], 4096)
        self.assertNotIn('nf_conntrack_count', data['gauges'])
        self.assertIn('nf_conntrack_count', data['unavailable'])
        self.assertIn('TcpExt.TCPReqQFullDrop', data['unavailable'])
        self.assertEqual(
            before,
            {
                str(path): path.read_bytes()
                for path in self.proc.rglob('*')
                if path.is_file()
            },
        )

    def test_malformed_or_denied_optional_files_do_not_abort(self) -> None:
        """An unreadable diagnostic is explicit while other observations survive."""
        self.write_counters()
        for contents in (
            'TcpExt: A B\nTcpExt: 1\n',
            'TcpExt: A\nTcpExt: invalid\n',
            'TcpExt: A\n',
        ):
            with self.subTest(contents=contents):
                (self.net / 'netstat').write_text(contents)
                data = agent.kernel_network(self.proc, self.net)
                self.assertIn('TcpExt', data['unavailable'])
                self.assertEqual(data['counters']['Tcp.OutRsts'], 3)
        with mock.patch.object(
            Path, 'read_text', side_effect=PermissionError('denied')
        ):
            data = agent.kernel_network(self.proc, self.net, limits=True)
        self.assertEqual(data['counters'], {})
        self.assertIn('PermissionError', data['unavailable']['Tcp'])

    @staticmethod
    def tcp_row(state: str, inode: str, port: int, queue: int = 0) -> str:
        """Build the documented proc TCP fields, including inode-less SYN rows."""
        return (
            f'0: 00000000:{port:04X} 00000000:0000 {state} '
            f'00000000:{queue:08X} 00:00000000 00000000 0 0 {inode}\n'
        )

    def test_listener_queues_and_syn_rows_are_not_owned_connections(self) -> None:
        """Match listener ports per family and count owned states by inode."""
        (self.net / 'tcp').write_text(
            'header\n'
            + self.tcp_row('0A', '10', 7198, 17)
            + self.tcp_row('03', '0', 7198)
            + self.tcp_row('03', '0', 80)
            + self.tcp_row('01', '11', 7198)
            + self.tcp_row('01', '99', 7198)
            + self.tcp_row('02', '12', 30000)
        )
        (self.net / 'tcp6').write_text(
            'header\n'
            + self.tcp_row('0A', '13', 7198, 4)
            + self.tcp_row('03', '0', 7198) * 2
        )
        data = agent.tcp_snapshot(self.net, {'10', '11', '12', '13'})
        self.assertEqual(
            data['socket_states'],
            {
                'LISTEN': 2,
                'ESTABLISHED': 1,
                'SYN_SENT': 1,
            },
        )
        self.assertEqual(
            data['listeners'],
            [
                {
                    'family': 'IPv4',
                    'port': 7198,
                    'accept_queue': 17,
                    'syn_recv_on_port': 1,
                },
                {
                    'family': 'IPv6',
                    'port': 7198,
                    'accept_queue': 4,
                    'syn_recv_on_port': 2,
                },
            ],
        )
        (self.net / 'tcp6').write_text('header\ntruncated\n')
        self.assertIn('tcp6', agent.tcp_snapshot(self.net, set())['unavailable'])

    @unittest.skipUnless(sys.platform == 'linux', 'Real Linux proc socket tables')
    def test_real_listener_reports_an_unaccepted_connection(self) -> None:
        """Observe a completed handshake still queued before application accept."""
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            listener.listen(4)
            port = listener.getsockname()[1]
            with socket.create_connection(('127.0.0.1', port)):
                data = agent.sample(os.getpid())
        queue = next(
            item
            for item in data['tcp']['listeners']
            if item['family'] == 'IPv4' and item['port'] == port
        )
        self.assertGreaterEqual(queue['accept_queue'], 1)
        self.assertGreater(data['fd_count'], 0)
        self.assertIn('LISTEN', data['tcp']['socket_states'])
        self.assertIn('TcpExt.ListenDrops', data['kernel_network']['counters'])

    def test_report_deltas_require_baseline_and_no_resets(self) -> None:
        """Subtract cumulative counters, but take peaks of gauges and queues."""
        baseline = {
            'counters': {'drops': 10, 'reset': 10, 'missing': 2},
            'limits': {'net/core/somaxconn': 4096},
        }
        samples = [
            {
                'event': 'sample',
                'fd_count': 15,
                'kernel_network': {
                    'counters': {'drops': 12, 'reset': 1},
                    'gauges': {'nf_conntrack_count': 22},
                },
                'tcp': {
                    'socket_states': {'SYN_SENT': 4},
                    'listeners': [
                        {
                            'family': 'IPv4',
                            'port': 7198,
                            'accept_queue': 8,
                            'syn_recv_on_port': 3,
                        }
                    ],
                },
            },
            {
                'event': 'sample',
                'fd_count': 12,
                'kernel_network': {
                    'counters': {'drops': 16, 'reset': 3},
                    'gauges': {'nf_conntrack_count': 20},
                },
                'tcp': {'socket_states': {'SYN_SENT': 2}, 'listeners': []},
            },
        ]
        events = [
            {'event': 'host', 'nofile': [100, 200], 'kernel_network': baseline},
            *samples,
        ]
        path = self.proc / 'metrics.jsonl'
        path.write_text('\n'.join(json.dumps(event) for event in events))
        data = report.network_summary(path, samples)
        self.assertEqual(data['counter_deltas']['drops']['increase'], 6)
        self.assertIsNone(data['counter_deltas']['reset']['increase'])
        self.assertIsNone(data['counter_deltas']['missing']['increase'])
        self.assertEqual(data['peak_fd_count'], 15)
        self.assertEqual(data['peak_gauges']['nf_conntrack_count'], 22)
        self.assertEqual(data['listeners']['IPv4:7198']['accept_queue_peak'], 8)
        self.assertEqual(data['peak_owned_socket_states']['SYN_SENT'], 4)
        self.assertEqual(report.network_summary(path, [])['peak_fd_count'], None)


if __name__ == '__main__':
    unittest.main()

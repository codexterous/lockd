"""Exercise provisioning safety and report contracts without contacting Vultr or SSH."""

import argparse
import contextlib
import copy
import datetime
import hashlib
import io
import ipaddress
import json
import os
import shlex
import socket
import subprocess
import sys
import tarfile
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
import uuid
from pathlib import Path
from unittest import mock

from scripts.benchmarking import __main__ as controller
from scripts.benchmarking import agent, config, report, vultr
from scripts.benchmarking.remote import Remote, Session

WORK = (
    config.ROOT
    / 'output/obj'
    / config.host_architecture()
    / 'opt/shared/verifications/benchmarking'
)


class FakeApi:
    """Model individual Vultr resources and explicit not-found replies offline."""

    def __init__(self) -> None:
        self.instances: dict[str, dict] = {}
        self.deleted: list[str] = []
        self.listed = 0
        self.journal: Path | None = None
        self.fail_create = False
        self.fail_delete = False
        self.before_delete = None

    def instance(self, instance_id: str) -> dict:
        """Return a detached snapshot so confirmation races can be simulated."""
        if instance_id not in self.instances:
            raise vultr.ApiError(404, 'GET', '/instances/' + instance_id)
        return copy.deepcopy(self.instances[instance_id])

    def items(self, path: str, key: str) -> list[dict]:
        """Make broad discovery observable; known IDs should never use it."""
        if (path, key) != ('/instances', 'instances'):
            raise AssertionError("Unexpected API listing")
        self.listed += 1
        return copy.deepcopy(list(self.instances.values()))

    def request(self, method: str, path: str, data: dict | None = None) -> dict:
        """Create VMs only after their intent is on disk, or delete one exact UUID."""
        if method == 'POST' and path == '/instances':
            saved = vultr.load(self.journal)
            if saved['resources'][-1]['status'] != 'pending':
                raise AssertionError("Creation was not journaled first")
            instance_id = str(uuid.uuid4())
            instance = data | {
                'id': instance_id,
                'main_ip': '192.0.2.10',
                'internal_ip': '10.20.0.2',
                'date_created': datetime.datetime.now(datetime.UTC).isoformat(),
                'default_password': 'MUST_NOT_BE_SAVED',
                'status': 'active',
                'power_status': 'running',
            }
            self.instances[instance_id] = instance
            if self.fail_create:
                raise RuntimeError("Create response lost")
            return {'instance': copy.deepcopy(instance)}
        if method == 'DELETE' and path.startswith('/instances/'):
            instance_id = path.removeprefix('/instances/')
            uuid.UUID(instance_id)
            if self.fail_delete:
                raise vultr.ApiError(500, method, path)
            if self.before_delete:
                self.before_delete()
            self.deleted.append(instance_id)
            del self.instances[instance_id]
            return {}
        raise AssertionError(f"Unexpected API mutation {method} {path}")


class BenchmarkTest(unittest.TestCase):
    """Put every temporary file under the project's verification output tree."""

    def setUp(self) -> None:
        """Create an isolated journal and fake account for each transition test."""
        WORK.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=WORK)
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.path = self.directory / 'manifest.json'
        self.settings = config.example() | {
            'region': 'ewr',
            'os_id': 1234,
            'daemon_plan': 'daemon-plan',
            'worker_plan': 'worker-plan',
            'vpc_id': str(uuid.uuid4()),
            'ssh_key_id': str(uuid.uuid4()),
            'firewall_group_id': str(uuid.uuid4()),
        }
        self.plan = {
            'id': 'test-plan',
            'type': 'vhp',
            'disk': 100,
            'storage_type': 'local_storage',
        }
        self.selection = {
            'image': {},
            'plans': {
                self.settings[role + '_plan']: self.plan
                | {'id': self.settings[role + '_plan']}
                for role in ('daemon', 'worker')
            },
        }
        self.state = {
            'schema': 1,
            'run_id': str(uuid.uuid4()),
            'config': self.settings,
            'resources': [],
            'outcome': 'incomplete',
        }
        self.api = FakeApi()
        self.api.journal = self.path
        self.output = io.StringIO()
        redirect = contextlib.redirect_stdout(self.output)
        redirect.__enter__()
        self.addCleanup(redirect.__exit__, None, None, None)

    def create(self, role: str = 'daemon') -> dict:
        """Create a real journal entry against the external-boundary fake."""
        return vultr.create(
            self.api, self.path, self.state, self.settings, role, self.plan
        )

    def cleanup_vms(self) -> list[str]:
        """Call production automatic cleanup against the recorded fake resources."""
        return vultr.cleanup(self.api, self.path, self.state)


class ResumeTests(BenchmarkTest):
    """Check recorded cleanup and configuration identity without cloud access."""

    def setUp(self) -> None:
        """Write valid local inputs so resume uses the real configuration loader."""
        super().setUp()
        archive = self.directory / 'source.tar.gz'
        archive.write_bytes(b'local fixture')
        key = self.directory / 'ssh_key'
        key.write_bytes(b'not a real private key')
        self.settings.update(
            source_archive=str(archive),
            source_sha256=hashlib.sha256(archive.read_bytes()).hexdigest(),
            ssh_private_key=str(key),
        )
        self.configuration = self.directory / 'profile.json'
        self.configuration.write_text(json.dumps(self.settings), encoding='utf-8')
        self.assertEqual(config.load(self.configuration), self.settings)

    def test_resume_requires_recorded_cleanup_and_matching_attempt(self) -> None:
        """Unresolved resources, another configuration or another run cannot resume."""
        record = self.create()
        start = datetime.datetime.fromtimestamp(
            record['intent_unix'] - 1, datetime.UTC
        ).isoformat()
        end = datetime.datetime.fromtimestamp(
            record['intent_unix'] + 1, datetime.UTC
        ).isoformat()
        for status in ('pending', 'created', 'deleting'):
            with self.subTest(status=status):
                record['status'] = status
                vultr.save(self.path, self.state)
                with self.assertRaisesRegex(ValueError, 'unresolved resources'):
                    controller.check_resume(self.path, self.configuration, start, end)
        for status in ('deleted', 'rejected'):
            with self.subTest(status=status):
                record['status'] = status
                vultr.save(self.path, self.state)
                self.assertEqual(
                    controller.check_resume(self.path, self.configuration, start, end),
                    self.state['run_id'],
                )
                self.assertEqual(
                    controller.check_resume(self.path, self.configuration, start, '-'),
                    self.state['run_id'],
                )
        with self.assertRaisesRegex(ValueError, 'creation times'):
            controller.check_resume(self.path, self.configuration, end, end)
        self.settings['daemon_threads'] += 1
        self.configuration.write_text(json.dumps(self.settings), encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'configuration differs'):
            controller.check_resume(self.path, self.configuration, start, end)
        self.assertFalse(self.api.deleted)

    def test_round_run_id_is_journaled_and_existing_directories_are_rejected(
        self,
    ) -> None:
        """The runner's UUID selects a fresh recovery journal before provisioning."""
        run_id = str(uuid.uuid4())
        location = self.directory / 'round-run'
        with (
            mock.patch.object(controller.shutil, 'which', return_value='ssh'),
            mock.patch.object(
                controller, 'preflight', return_value=self.selection
            ) as preflight,
            mock.patch.object(
                config, 'output_directory', return_value=location
            ) as output,
            mock.patch.object(controller, 'execute_run'),
            mock.patch.object(report, 'generate'),
        ):
            controller.run(self.api, self.settings, run_id)
            output.assert_called_with(run_id)
            self.assertEqual(vultr.load(location / 'manifest.json')['run_id'], run_id)
            preflight.reset_mock()
            with self.assertRaisesRegex(ValueError, 'already exists'):
                controller.run(self.api, self.settings, run_id)
            preflight.assert_not_called()
            with self.assertRaises(ValueError):
                controller.run(self.api, self.settings, 'invalid-uuid')
            preflight.assert_not_called()
        self.assertFalse(self.api.instances)


class CleanupTests(BenchmarkTest):
    """
    Protect unrelated machines and retain resources whenever ownership is uncertain.
    """

    def test_cleanup_hint_quotes_manifest_paths_for_each_shell(self) -> None:
        """Copyable commands preserve spaces and apostrophes in the manifest path."""
        manifest = self.directory / "owner's files" / 'manifest.json'
        for platform in ('nt', 'posix'):
            with (
                self.subTest(platform=platform),
                mock.patch.object(controller.os, 'name', platform),
            ):
                command = controller.cleanup_command(manifest)
                self.assertTrue(
                    command.startswith('python -m scripts.benchmarking cleanup ')
                )
                if platform == 'nt':
                    self.assertTrue(
                        command.endswith("'" + str(manifest).replace("'", "''") + "'")
                    )
                else:
                    self.assertEqual(shlex.split(command)[-1], str(manifest))

    def test_cleanup_cli_failure_prints_the_retry_command(self) -> None:
        """Explicit cleanup must provide the same command when resources remain."""
        self.create()
        self.api.fail_delete = True
        with (
            mock.patch.object(vultr, 'Api', return_value=self.api),
            mock.patch.object(sys, 'argv', ['benchmarking', 'cleanup', str(self.path)]),
            self.assertRaisesRegex(RuntimeError, 'Cleanup incomplete') as failure,
        ):
            controller.main()
        self.assertIn(controller.cleanup_command(self.path), str(failure.exception))
        self.assertFalse(self.api.deleted)

    def test_run_failure_prints_cleanup_command_when_resources_remain(self) -> None:
        """Automatic cleanup failures must explain how to retry before round resume."""
        location = self.directory / 'failed-run'

        def execute(api: FakeApi, path: Path, state: dict) -> None:
            """Leave an owned instance whose deletion the provider rejects."""
            api.journal = path
            vultr.create(api, path, state, self.settings, 'daemon', self.plan)
            api.fail_delete = True
            raise RuntimeError('Execution failed')

        with (
            mock.patch.object(controller.shutil, 'which', return_value='ssh'),
            mock.patch.object(controller, 'preflight', return_value=self.selection),
            mock.patch.object(config, 'output_directory', return_value=location),
            mock.patch.object(controller, 'execute_run', side_effect=execute),
            mock.patch.object(report, 'generate'),
            self.assertRaisesRegex(RuntimeError, 'Execution failed'),
        ):
            controller.run(self.api, self.settings)
        self.assertIn(
            controller.cleanup_command(location / 'manifest.json'),
            self.output.getvalue(),
        )
        self.assertFalse(self.api.deleted)

    def test_automatic_cleanup_deletes_only_journal_owned_uuids(self) -> None:
        """Delete both recorded VMs without prompting or touching an unrelated VM."""
        first = self.create()
        second = self.create('worker-0')
        unrelated = str(uuid.uuid4())
        self.api.instances[unrelated] = self.api.instances[first['id']] | {
            'id': unrelated,
            'tags': [],
            'label': 'unrelated-machine',
        }
        with mock.patch('builtins.input', side_effect=AssertionError) as prompt:
            remaining = self.cleanup_vms()
        prompt.assert_not_called()
        self.assertEqual(self.api.deleted, [first['id'], second['id']])
        self.assertIn(unrelated, self.api.instances)
        self.assertFalse(remaining)
        self.assertEqual(self.api.listed, 0)
        self.assertNotIn('MUST_NOT_BE_SAVED', self.output.getvalue())
        self.assertNotIn('MUST_NOT_BE_SAVED', self.path.read_text())

    def test_changed_ownership_fields_refuse_deletion(self) -> None:
        """
        Copied labels, wrong tags, changed plans, and different creation dates are
        unsafe.
        """
        record = self.create()
        original = copy.deepcopy(self.api.instances[record['id']])
        record['identity'] = {key: original[key] for key in vultr.IDENTITY}
        for key, value in (
            ('label', 'other-machine'),
            ('tags', []),
            ('plan', 'other-plan'),
            ('os_id', 999),
            ('region', 'other-region'),
            ('date_created', '2020-01-01T00:00:00Z'),
        ):
            with self.subTest(key=key):
                self.api.instances[record['id']] = original | {key: value}
                self.assertTrue(self.cleanup_vms())
        self.assertFalse(self.api.deleted)

    def test_identity_is_rechecked_immediately_before_deletion(self) -> None:
        """Refuse a changed instance even when its initial ownership lookup passed."""
        record = self.create()
        original = self.api.instance
        calls = 0

        def changed(instance_id: str) -> dict:
            """Change the provider snapshot between the two read-only checks."""
            nonlocal calls
            calls += 1
            if calls == 2:
                self.api.instances[record['id']]['main_ip'] = '192.0.2.20'
            return original(instance_id)

        with mock.patch.object(self.api, 'instance', side_effect=changed):
            self.assertTrue(self.cleanup_vms())
        self.assertFalse(self.api.deleted)

    def test_lost_create_response_recovers_only_exact_intent(self) -> None:
        """
        Recover a unique request-tagged VM without deleting an unrelated account VM.
        """
        self.api.fail_create = True
        with self.assertRaisesRegex(RuntimeError, 'response lost'):
            self.create()
        self.state = vultr.load(self.path)
        self.assertEqual(self.state['resources'][0]['status'], 'pending')
        actual = next(iter(self.api.instances))
        unrelated_id = str(uuid.uuid4())
        self.api.instances[unrelated_id] = self.api.instances[actual] | {
            'id': unrelated_id,
            'tags': [],
        }
        self.assertFalse(self.cleanup_vms())
        self.assertEqual(self.api.deleted, [actual])
        self.assertIn(unrelated_id, self.api.instances)

    def test_ambiguous_reconciliation_does_not_guess(self) -> None:
        """
        Two candidates with copied request tags require manual review, not deletion.
        """
        self.api.fail_create = True
        with self.assertRaises(RuntimeError):
            self.create()
        original = next(iter(self.api.instances.values()))
        other = str(uuid.uuid4())
        self.api.instances[other] = original | {'id': other}
        self.assertTrue(self.cleanup_vms())
        self.assertFalse(self.api.deleted)

    def test_deleted_journal_is_idempotent(self) -> None:
        """
        Verified absence is saved and subsequent cleanup performs no second deletion.
        """
        record = self.create()
        self.assertFalse(self.cleanup_vms())
        self.state = vultr.load(self.path)
        self.assertEqual(self.state['resources'][0]['status'], 'deleted')
        self.assertFalse(self.cleanup_vms())
        self.assertEqual(self.api.deleted, [record['id']])

    def test_api_failure_does_not_mark_deletion_complete(self) -> None:
        """
        A failed deletion remains retryable and does not stop later per-VM cleanup.
        """
        record = self.create()
        self.api.fail_delete = True
        self.assertTrue(self.cleanup_vms())
        self.assertEqual(vultr.load(self.path)['resources'][0]['status'], 'deleting')
        self.assertIn(record['id'], self.api.instances)
        self.api.fail_delete = False
        self.assertFalse(self.cleanup_vms())

    def test_deleting_status_is_saved_before_api_delete(self) -> None:
        """A controller crash during DELETE leaves a recoverable transition on disk."""
        self.create()
        self.api.before_delete = lambda: self.assertEqual(
            vultr.load(self.path)['resources'][0]['status'], 'deleting'
        )
        self.assertFalse(self.cleanup_vms())

    def test_unknown_journal_schema_is_rejected(self) -> None:
        """Cleanup must fail closed when given a file from another tool or schema."""
        self.create()
        vultr.save(self.path, self.state | {'schema': 42})
        with self.assertRaises(ValueError):
            vultr.load(self.path)

    def test_explicit_creation_rejections_are_saved_without_cleanup_candidates(
        self,
    ) -> None:
        """A rejected request needs no deletion prompt and must never be retried."""
        for status in vultr.REJECTED_CREATE_STATUSES:
            with self.subTest(status=status):
                self.state['resources'] = []
                failure = vultr.ApiError(
                    status, 'POST', '/instances', 'Request rejected'
                )
                with (
                    mock.patch.object(
                        self.api, 'request', side_effect=failure
                    ) as request,
                    self.assertRaises(vultr.ApiError),
                ):
                    self.create()
                request.assert_called_once()
                self.state = vultr.load(self.path)
                self.assertEqual(self.state['resources'][0]['status'], 'rejected')
                self.assertEqual(
                    self.state['resources'][0]['creation_error']['status'], status
                )
                self.assertFalse(self.cleanup_vms())
                self.assertEqual(self.api.listed, 0)
                self.assertFalse(self.api.deleted)

    def test_server_errors_remain_pending_when_creation_might_have_succeeded(
        self,
    ) -> None:
        """
        A 5xx response can follow creation; recover only its unique owned instance.
        """
        original = self.api.request

        def lost_response(method: str, path: str, data: dict | None = None) -> dict:
            """Create at the provider boundary, then lose its successful response."""
            original(method, path, data)
            raise vultr.ApiError(503, method, path, 'Service unavailable')

        with (
            mock.patch.object(self.api, 'request', side_effect=lost_response),
            self.assertRaises(vultr.ApiError),
        ):
            self.create()
        self.state = vultr.load(self.path)
        self.assertEqual(self.state['resources'][0]['status'], 'pending')
        identifier = next(iter(self.api.instances))
        self.assertFalse(self.cleanup_vms())
        self.assertEqual(self.api.deleted, [identifier])

    def test_creation_explicitly_uses_local_storage(self) -> None:
        """Ordinary local-storage plans do not request separately managed volumes."""
        record = self.create()
        self.assertNotIn('block_devices', record['request'])

    def test_vx1_creation_selects_the_local_boot_device(self) -> None:
        """VX1 local boot needs an explicit local device, without buying a volume."""
        self.plan.update(type='vx1', storage_type='local_and_block_storage')
        record = self.create()
        self.assertEqual(
            record['request']['block_devices'],
            [{'block_id': 'local', 'bootable': True}],
        )
        self.assertEqual(
            vultr.load(self.path)['resources'][0]['request'], record['request']
        )

    def test_create_refuses_block_only_metadata_before_journaling_or_post(self) -> None:
        """
        Reject unsupported storage at the mutation boundary even without preflight.
        """
        self.plan.update(disk=1, type='vx1', storage_type='block_storage')
        with self.assertRaisesRegex(ValueError, 'local storage'):
            self.create()
        self.assertFalse(self.api.instances)
        self.assertFalse(self.path.exists())

    def test_rejected_run_does_not_claim_that_billable_instances_exist(self) -> None:
        """
        Report the provider rejection, preserve its journal, and skip resource deletion.
        """
        run_id = str(uuid.uuid4())
        location = self.directory / 'rejected-run'
        failure = vultr.ApiError(400, 'POST', '/instances', 'Invalid instance plan')
        with (
            mock.patch.object(controller.shutil, 'which', return_value='ssh'),
            mock.patch.object(controller, 'preflight', return_value=self.selection),
            mock.patch.object(
                controller.uuid, 'uuid4', side_effect=[uuid.UUID(run_id), uuid.uuid4()]
            ),
            mock.patch.object(config, 'output_directory', return_value=location),
            mock.patch.object(report, 'generate'),
            mock.patch.object(self.api, 'request', side_effect=failure) as request,
            mock.patch('builtins.input', side_effect=AssertionError) as prompt,
            self.assertRaises(vultr.ApiError),
        ):
            controller.run(self.api, self.settings)
        self.assertEqual(request.call_count, 1)
        prompt.assert_not_called()
        journal = vultr.load(location / 'manifest.json')
        self.assertEqual(journal['resources'][0]['status'], 'rejected')
        self.assertIn('nothing to delete', self.output.getvalue())
        self.assertNotIn('Machines remain billable', self.output.getvalue())
        self.assertNotIn('NOT REMOVED', self.output.getvalue())

    def test_run_failure_and_report_failure_still_clean_up(self) -> None:
        """Execution and rendering failures must still remove journal-owned VMs."""
        run_id = str(uuid.uuid4())

        def execute(api, path, state) -> None:
            """Fail after a billable creation, where cleanup matters most."""
            api.journal = path
            vultr.create(api, path, state, self.settings, 'daemon', self.plan)
            raise RuntimeError("Execution failed")

        with (
            mock.patch.object(controller.shutil, 'which', return_value='ssh'),
            mock.patch.object(controller, 'preflight', return_value=self.selection),
            mock.patch.object(
                controller.uuid,
                'uuid4',
                side_effect=[uuid.UUID(run_id), uuid.uuid4(), uuid.uuid4()],
            ),
            mock.patch.object(
                config, 'output_directory', return_value=self.directory / 'run'
            ),
            mock.patch.object(controller, 'execute_run', side_effect=execute),
            mock.patch.object(
                report, 'generate', side_effect=ValueError('report failed')
            ),
            mock.patch('builtins.input', side_effect=AssertionError),
            self.assertRaisesRegex(RuntimeError, 'Execution failed'),
        ):
            controller.run(self.api, self.settings)
        self.assertNotIn('NOT REMOVED:', self.output.getvalue())
        self.assertFalse(self.api.instances)
        self.assertEqual(len(self.api.deleted), 1)


class VisibilityTests(BenchmarkTest):
    """Distinguish temporary API absence from acknowledged resource deletion."""

    def test_connect_waits_for_the_created_uuid_before_opening_ssh(self) -> None:
        """Two initial 404s must not abandon creation or submit a duplicate POST."""
        record = self.create()
        self.api.instances[record['id']]['main_ip'] = '8.8.8.8'
        instance = self.api.instance(record['id'])
        missing = vultr.ApiError(404, 'GET', '/instances/' + record['id'])
        remote = mock.Mock(spec=Remote)
        remote.work = '/root/test-run'
        with (
            mock.patch.object(
                self.api, 'instance', side_effect=[missing, missing, instance]
            ) as lookup,
            mock.patch.object(
                self.api,
                'items',
                return_value=[
                    {
                        'id': self.settings['vpc_id'],
                        'ip_address': '10.20.0.2',
                        'mac_address': 'aa:bb:cc:dd:ee:ff',
                    }
                ],
            ),
            mock.patch.object(controller, 'remote_for', return_value=remote),
            mock.patch.object(vultr.time, 'monotonic', return_value=0),
            mock.patch.object(vultr.time, 'sleep') as sleep,
        ):
            self.assertIs(
                controller.connect(self.api, self.path, self.state, record), remote
            )
        self.assertEqual(lookup.call_args_list, [mock.call(record['id'])] * 3)
        self.assertEqual(sleep.call_count, 2)
        self.assertEqual(len(self.api.instances), 1)
        remote.wait.assert_called_once()
        self.assertEqual(vultr.load(self.path)['resources'][0]['public_ip'], '8.8.8.8')

    def test_cleanup_waits_for_visibility_then_deletes_only_the_same_uuid(self) -> None:
        """Initial cleanup 404s must not be recorded as successful deletion."""
        record = self.create()
        original = self.api.instance
        lookups = 0

        def delayed(instance_id: str) -> dict:
            """Expose the provider instance only after two transient missing reads."""
            nonlocal lookups
            lookups += 1
            if lookups <= 2:
                raise vultr.ApiError(404, 'GET', '/instances/' + instance_id)
            return original(instance_id)

        with (
            mock.patch.object(self.api, 'instance', side_effect=delayed),
            mock.patch.object(vultr.time, 'monotonic', return_value=0),
            mock.patch.object(vultr.time, 'sleep'),
        ):
            self.assertFalse(self.cleanup_vms())
        saved = vultr.load(self.path)['resources'][0]
        self.assertEqual(self.api.deleted, [record['id']])
        self.assertTrue(saved['delete_acknowledged'])
        self.assertEqual(saved['status'], 'deleted')
        self.assertEqual(self.api.listed, 0)

    def test_permanent_visibility_failure_preserves_the_cleanup_journal(self) -> None:
        """An unobserved UUID remains unresolved even if every GET returns 404."""
        record = self.create()
        with (
            mock.patch.object(
                self.api,
                'instance',
                side_effect=vultr.ApiError(404, 'GET', '/instances/' + record['id']),
            ),
            mock.patch.object(vultr.time, 'monotonic', side_effect=[0, 121]),
            mock.patch.object(vultr.time, 'sleep'),
        ):
            remaining = self.cleanup_vms()
        self.assertEqual(len(remaining), 1)
        self.assertIn(record['id'], remaining[0])
        self.assertEqual(vultr.load(self.path)['resources'][0]['status'], 'created')
        self.assertNotIn('delete_acknowledged', record)
        self.assertFalse(self.api.deleted)

    def test_connect_visibility_timeout_never_attempts_ssh(self) -> None:
        """Keep failed provisioning separate from SSH readiness and VM deletion."""
        record = self.create()
        with (
            mock.patch.object(
                self.api,
                'instance',
                side_effect=vultr.ApiError(404, 'GET', '/instances/' + record['id']),
            ),
            mock.patch.object(vultr.time, 'monotonic', side_effect=[0, 0, 0, 121]),
            mock.patch.object(controller, 'remote_for') as remote,
            self.assertRaisesRegex(
                TimeoutError, 'creation and deletion remain unconfirmed'
            ),
        ):
            controller.connect(self.api, self.path, self.state, record)
        remote.assert_not_called()
        self.assertEqual(record['status'], 'created')

    def test_visibility_wait_does_not_retry_authorization_errors(self) -> None:
        """
        Only not-found responses are visibility retries; other API errors remain clear.
        """
        failure = vultr.ApiError(403, 'GET', '/instances/test')
        with (
            mock.patch.object(self.api, 'instance', side_effect=failure) as lookup,
            mock.patch.object(vultr.time, 'sleep') as sleep,
            self.assertRaises(vultr.ApiError) as caught,
        ):
            vultr.wait_for_instance(self.api, 'test', 120)
        self.assertIs(caught.exception, failure)
        lookup.assert_called_once()
        sleep.assert_not_called()

    def test_delete_404_is_not_an_acknowledged_deletion(self) -> None:
        """A stale not-found DELETE response must not hide a still-existing VM."""
        record = self.create()
        with mock.patch.object(
            self.api,
            'request',
            side_effect=vultr.ApiError(404, 'DELETE', '/instances/' + record['id']),
        ):
            self.assertTrue(self.cleanup_vms())
        self.assertEqual(vultr.load(self.path)['resources'][0]['status'], 'deleting')
        self.assertNotIn('delete_acknowledged', record)
        self.assertIn(record['id'], self.api.instances)
        self.assertFalse(self.cleanup_vms())

    def test_acknowledged_delete_recovery_polls_without_reissuing_delete(self) -> None:
        """Recover after a crash following DELETE without sending another mutation."""
        record = self.create()
        record.update(status='deleting', delete_acknowledged=True)
        del self.api.instances[record['id']]
        with mock.patch.object(
            self.api, 'request', side_effect=AssertionError
        ) as request:
            self.assertFalse(self.cleanup_vms())
        request.assert_not_called()
        self.assertEqual(record['status'], 'deleted')

    def test_absence_cannot_be_confirmed_without_a_delete_acknowledgment(self) -> None:
        """Prevent accidental reuse of deletion polling for pre-creation absence."""
        record = self.create()
        with self.assertRaisesRegex(ValueError, 'acknowledged DELETE'):
            vultr.confirm_deletion(self.api, self.path, self.state, record)
        self.assertEqual(record['status'], 'created')


class StartupTests(BenchmarkTest):
    """Preserve setup failures and collect only files belonging to started agents."""

    def test_remaining_workers_prepare_in_parallel_with_valid_journals(self) -> None:
        """
        Overlap two SSH preparations after the canary without racing journal writes.
        """
        self.state['plans'] = self.selection['plans']
        self.settings['worker_count'] = 3
        entered = {role: threading.Event() for role in ('worker-1', 'worker-2')}
        sessions = []
        remote = mock.Mock(spec=Remote)
        remote.work = '/root/test-run'
        original_save = vultr.save
        written = []

        def save(path, state) -> None:
            """Read every published journal back through its production validator."""
            original_save(path, state)
            if path == self.path:
                written.append(vultr.load(path))

        def connect(api, path, state, record, **kwargs) -> Remote:
            """Require both parallel tasks to enter instead of relying on durations."""
            with kwargs.get('journal_guard', contextlib.nullcontext()):
                record.update(public_ip='192.0.2.10', private_ip='10.20.0.2')
                vultr.save(path, state)
            role = record['role']
            if role in entered:
                self.assertTrue(sessions[1].wait_ready.called)
                entered[role].set()
                other = 'worker-2' if role == 'worker-1' else 'worker-1'
                self.assertTrue(
                    entered[other].wait(3), 'Remaining workers must overlap'
                )
            return remote

        def session(transport, directory, role):
            """
            Supply successful monitored children without executing remote commands.
            """
            result = mock.Mock()
            result.role = role
            result.has_launched_event = role == 'daemon'
            result.process.poll.return_value = None if role == 'daemon' else 0
            result.finish.return_value = 0
            sessions.append(result)
            return result

        with (
            mock.patch.object(controller, 'connect', side_effect=connect),
            mock.patch.object(controller, 'build_bundle'),
            mock.patch.object(controller, 'Session', side_effect=session),
            mock.patch.object(controller, 'remote_for', return_value=remote),
            mock.patch.object(vultr, 'save', side_effect=save),
        ):
            controller.execute_run(self.api, self.path, self.state)
        self.assertTrue(all(event.is_set() for event in entered.values()))
        self.assertTrue(written)
        self.assertEqual(self.state['outcome'], 'completed')
        final = vultr.load(self.path)
        self.assertEqual(len(final['resources']), 4)
        self.assertTrue(all(record.get('private_ip') for record in final['resources']))
        sessions[0].release.assert_called_once_with(40)

    def test_parallel_failure_stops_other_tasks_before_cleanup(self) -> None:
        """Join active setup before closing sessions or deleting any recorded VM."""
        self.state['plans'] = self.selection['plans']
        self.settings['worker_count'] = 3
        entered = {role: threading.Event() for role in ('worker-1', 'worker-2')}
        stopped = threading.Event()
        sessions = []
        remote = mock.Mock(spec=Remote)
        remote.work = '/root/test-run'

        def connect(api, path, state, record, **kwargs) -> Remote:
            """Fail one task and keep another active until cancellation is observed."""
            with kwargs.get('journal_guard', contextlib.nullcontext()):
                record.update(public_ip='192.0.2.10', private_ip='10.20.0.2')
            role = record['role']
            if role in entered:
                entered[role].set()
                other = 'worker-2' if role == 'worker-1' else 'worker-1'
                self.assertTrue(entered[other].wait(3))
                if role == 'worker-1':
                    raise RuntimeError('Worker preparation failed')
                self.assertTrue(kwargs['cancelled'].wait(3))
                stopped.set()
                raise InterruptedError('Other worker cancelled')
            return remote

        def session(transport, directory, role):
            """Require every session close to happen after active setup has stopped."""
            result = mock.Mock()
            result.role = role
            result.has_launched_event = role == 'daemon'
            result.close.side_effect = lambda: self.assertTrue(stopped.is_set())
            sessions.append(result)
            return result

        with (
            mock.patch.object(controller, 'connect', side_effect=connect),
            mock.patch.object(controller, 'build_bundle'),
            mock.patch.object(controller, 'Session', side_effect=session),
            mock.patch.object(controller, 'remote_for', return_value=remote),
            self.assertRaisesRegex(RuntimeError, 'Worker preparation failed'),
        ):
            controller.execute_run(self.api, self.path, self.state)
        self.assertTrue(stopped.is_set())
        for started in sessions:
            started.close.assert_called_once()
        self.assertFalse(self.api.deleted)
        self.assertEqual(len(vultr.load(self.path)['resources']), 4)
        self.assertFalse(self.cleanup_vms())
        self.assertEqual(len(self.api.deleted), 4)

    def test_remaining_worker_preparations_reach_but_never_exceed_ten(self) -> None:
        """
        Keep the first-worker check, then allow ten expensive preparations at once.
        """
        self.state['plans'] = self.selection['plans']
        self.settings['worker_count'] = 12
        remote = mock.Mock(spec=Remote)
        remote.work = '/root/test-run'
        sessions = []
        lock = threading.Lock()
        full = threading.Event()
        active = peak = 0

        def connect(api, path, state, record, **kwargs) -> Remote:
            """Hold the first ten remaining tasks until all ten have entered."""
            nonlocal active, peak
            with kwargs.get('journal_guard', contextlib.nullcontext()):
                record.update(public_ip='192.0.2.10', private_ip='10.20.0.2')
            if record['role'] not in ('daemon', 'worker-0'):
                self.assertTrue(sessions[1].wait_ready.called)
                with lock:
                    active += 1
                    peak = max(peak, active)
                    if active == 10:
                        full.set()
                try:
                    self.assertTrue(full.wait(5), 'Ten tasks must be able to overlap')
                finally:
                    with lock:
                        active -= 1
            return remote

        def session(transport, directory, role):
            """Supply successful child monitoring without opening any SSH connection."""
            result = mock.Mock()
            result.role = role
            result.has_launched_event = role == 'daemon'
            result.process.poll.return_value = None if role == 'daemon' else 0
            result.finish.return_value = 0
            sessions.append(result)
            return result

        with (
            mock.patch.object(controller, 'connect', side_effect=connect),
            mock.patch.object(controller, 'build_bundle'),
            mock.patch.object(controller, 'Session', side_effect=session),
            mock.patch.object(controller, 'remote_for', return_value=remote),
        ):
            controller.execute_run(self.api, self.path, self.state)
        self.assertEqual(peak, 10)
        self.assertEqual(active, 0)
        self.assertEqual(self.state['outcome'], 'completed')

    def test_cancelled_preparation_never_opens_another_remote_connection(self) -> None:
        """A task cancelled before it starts must not contact or modify its VM."""
        cancelled = threading.Event()
        cancelled.set()
        with (
            mock.patch.object(controller, 'connect') as connect,
            self.assertRaises(InterruptedError),
        ):
            controller.prepare_worker(
                self.api, self.path, self.state, {}, [], threading.RLock(), cancelled
            )
        connect.assert_not_called()

    def test_cancelled_ssh_wait_never_launches_another_ssh_attempt(self) -> None:
        """A parallel failure must stop a different worker's repeated SSH boot wait."""
        cancelled = threading.Event()
        cancelled.set()
        remote = Remote(
            '192.0.2.10', self.directory / 'key', self.directory, '/root/run'
        )
        with (
            mock.patch.object(remote, 'call') as call,
            self.assertRaises(InterruptedError),
        ):
            remote.wait(300, self.directory / 'setup.log', cancelled=cancelled)
        call.assert_not_called()

    def test_first_unreachable_worker_prevents_creating_the_rest(self) -> None:
        """
        Stop after the daemon and first worker if real readiness cannot be reached.
        """
        self.state['plans'] = self.selection['plans']
        self.settings['worker_count'] = 10
        remote = mock.Mock(spec=Remote)
        remote.work = '/root/test-run'
        sessions = []

        def connect(api, path, state, record, **kwargs) -> Remote:
            """Substitute only the SSH boundary and assigned instance addresses."""
            record.update(public_ip='192.0.2.10', private_ip='10.20.0.2')
            return remote

        def session(transport, directory, role):
            """Let the daemon listen, then reproduce the first worker's TCP failure."""
            result = mock.Mock()
            result.role = role
            result.has_launched_event = role == 'daemon'
            if role != 'daemon':
                result.wait_ready.side_effect = RuntimeError(
                    'Cannot connect to daemon 10.20.0.2:7198'
                )
            sessions.append(result)
            return result

        with (
            mock.patch.object(controller, 'connect', side_effect=connect),
            mock.patch.object(controller, 'build_bundle'),
            mock.patch.object(controller, 'Session', side_effect=session),
            mock.patch.object(controller, 'remote_for', return_value=remote),
            self.assertRaisesRegex(RuntimeError, 'Cannot connect to daemon'),
        ):
            controller.execute_run(self.api, self.path, self.state)
        self.assertEqual(
            [record['role'] for record in self.state['resources']],
            ['daemon', 'worker-0'],
        )
        self.assertEqual(len(self.api.instances), 2)
        self.assertEqual(
            [record['process_started'] for record in self.state['resources']],
            [True, False],
        )
        self.assertIn(
            'BENCHMARK FAILED: Cannot connect to daemon', self.output.getvalue()
        )
        self.assertFalse(
            any(call.args[1] == 'result.csv' for call in remote.copy.call_args_list)
        )
        for started in sessions:
            started.close.assert_called_once()

    def test_canary_is_ready_before_expansion_and_all_workers_before_release(
        self,
    ) -> None:
        """
        Verify the canary before expansion and every parallel worker before release.
        """
        self.state['plans'] = self.selection['plans']
        self.settings['worker_count'] = 3
        remote = mock.Mock(spec=Remote)
        remote.work = '/root/test-run'
        ready = []
        sessions = []

        def connect(api, path, state, record, **kwargs) -> Remote:
            """Give production orchestration a fake transport and private address."""
            with kwargs.get('journal_guard', contextlib.nullcontext()):
                record.update(public_ip='192.0.2.10', private_ip='10.20.0.2')
            return remote

        def session(transport, directory, role):
            """Check creation order at readiness and release only after all workers."""
            if role not in ('daemon', 'worker-0'):
                self.assertIn('worker-0', ready)
            result = mock.Mock()
            result.role = role
            result.has_launched_event = role == 'daemon'
            result.process.poll.return_value = None if role == 'daemon' else 0
            result.finish.return_value = 0
            result.wait_ready.side_effect = lambda timeout: ready.append(role)
            result.release.side_effect = lambda timeout: self.assertEqual(len(ready), 4)
            sessions.append(result)
            return result

        with (
            mock.patch.object(controller, 'connect', side_effect=connect),
            mock.patch.object(controller, 'build_bundle'),
            mock.patch.object(controller, 'Session', side_effect=session),
            mock.patch.object(controller, 'remote_for', return_value=remote),
        ):
            controller.execute_run(self.api, self.path, self.state)
        self.assertEqual(ready[:2], ['daemon', 'worker-0'])
        self.assertCountEqual(ready[2:], ['worker-1', 'worker-2'])
        sessions[0].release.assert_called_once_with(40)
        self.assertEqual(self.state['outcome'], 'completed')

    def test_build_failure_is_visible_before_automatic_cleanup(self) -> None:
        """
        Show the apt error before cleanup, keep its log, and skip absent runtime logs.
        """
        run_id = str(uuid.uuid4())
        location = self.directory / 'failed-build'
        self.api.journal = location / 'manifest.json'
        remote = mock.Mock(spec=Remote)
        remote.work = '/root/benchmark-test'

        def connect(api, path, state, record) -> Remote:
            """Record successful SSH setup without contacting a real machine."""
            record['public_ip'] = '192.0.2.10'
            return remote

        def fail_build(arguments, timeout, log) -> None:
            """Reproduce the provider image's apt failure in the local build log."""
            log.write_text(
                'Early build output\n' * 100
                + 'E: Unable to locate package libboost-system-dev\n',
                encoding='utf-8',
            )
            raise subprocess.CalledProcessError(100, ['ssh', 'agent.py', 'build'])

        def before_delete() -> None:
            """Require diagnostics to be visible before deleting the failed VM."""
            output = self.output.getvalue()
            self.assertIn('BUILD FAILED:', output)
            self.assertIn('E: Unable to locate package libboost-system-dev', output)
            self.assertIn(str(location / 'build.log'), output)
            self.assertEqual(output.count('Early build output'), 39)
            self.assertNotIn('COLLECTION FAILED:', output)

        self.api.before_delete = before_delete

        with (
            mock.patch.object(controller.shutil, 'which', return_value='ssh'),
            mock.patch.object(controller, 'preflight', return_value=self.selection),
            mock.patch.object(controller.uuid, 'uuid4', return_value=uuid.UUID(run_id)),
            mock.patch.object(config, 'output_directory', return_value=location),
            mock.patch.object(controller, 'connect', side_effect=connect),
            mock.patch.object(remote, 'call', side_effect=fail_build),
            mock.patch.object(report, 'generate'),
            mock.patch('builtins.input', side_effect=AssertionError) as prompt,
            self.assertRaisesRegex(
                RuntimeError, 'Daemon build failed; see .*build.log'
            ),
        ):
            controller.run(self.api, self.settings)
        prompt.assert_not_called()
        remote.copy.assert_called_once_with(
            Path(self.settings['source_archive']), 'source.tar.gz'
        )
        self.assertTrue((location / 'build.log').is_file())
        self.assertFalse(self.api.instances)
        self.assertEqual(len(self.api.deleted), 1)

    def test_build_success_downloads_the_bundle_and_provenance(self) -> None:
        """Retrieve built artifacts only after the remote build command succeeds."""
        remote = mock.Mock(spec=Remote)
        remote.work = '/root/benchmark-test'
        controller.build_bundle(remote, self.settings, self.directory)
        self.assertEqual(
            remote.copy.call_args_list,
            [
                mock.call(Path(self.settings['source_archive']), 'source.tar.gz'),
                mock.call(
                    self.directory / 'bundle.tar.gz', 'bundle.tar.gz', download=True
                ),
                mock.call(
                    self.directory / 'build.json', 'bundle/build.json', download=True
                ),
            ],
        )
        self.assertIn(str(self.directory / 'build.log'), self.output.getvalue())

    def test_async_override_requires_native_support_before_worker_creation(
        self,
    ) -> None:
        """Check the actual built parser if even one generator selects async mode."""
        remote = mock.Mock(spec=Remote)
        remote.work = '/root/benchmark-test'
        self.settings['worker_count'] = 2
        self.settings['worker_overrides'] = {'1': ['--engine=async']}
        controller.build_bundle(remote, self.settings, self.directory)
        self.assertIn('--require-async', remote.call.call_args.args[0])

    def test_unreadable_build_log_does_not_replace_the_command_failure(self) -> None:
        """
        Preserve command and timeout failures when SSH could not create a build log.
        """
        failures = [
            subprocess.CalledProcessError(255, ['ssh', 'build']),
            subprocess.TimeoutExpired(['ssh', 'build'], 60),
        ]
        for failure in failures:
            with self.subTest(failure=type(failure).__name__):
                remote = mock.Mock(spec=Remote)
                remote.work = '/root/benchmark-test'
                remote.call.side_effect = failure
                with self.assertRaisesRegex(
                    RuntimeError, 'Daemon build failed'
                ) as caught:
                    controller.build_bundle(remote, self.settings, self.directory)
                self.assertIs(caught.exception.__cause__, failure)
                self.assertIn('Could not read build log:', self.output.getvalue())
                remote.copy.assert_called_once_with(
                    Path(self.settings['source_archive']), 'source.tar.gz'
                )

    def test_collection_skips_machines_without_started_monitoring(self) -> None:
        """Successful SSH setup alone does not imply that runtime files exist."""
        record = self.create()
        record['public_ip'] = '192.0.2.10'
        with mock.patch.object(controller, 'remote_for') as remote_for:
            self.assertEqual(controller.collect(self.state, self.directory), [])
        remote_for.assert_not_called()

    def test_collection_keeps_recovering_other_logs_after_a_download_failure(
        self,
    ) -> None:
        """Once monitoring starts, missing files remain visible and others are tried."""
        record = self.create()
        record.update(public_ip='192.0.2.10', monitoring_started=True)
        remote = mock.Mock(spec=Remote)
        remote.copy.side_effect = [
            subprocess.CalledProcessError(1, ['scp']),
            None,
            None,
        ]
        with mock.patch.object(controller, 'remote_for', return_value=remote):
            failures = controller.collect(self.state, self.directory)
        self.assertEqual(len(failures), 1)
        self.assertIn('daemon/metrics.jsonl', failures[0])
        self.assertEqual(
            [call.args[1] for call in remote.copy.call_args_list],
            ['metrics.jsonl', 'process.log', 'daemon.log'],
        )

    def test_collection_recognizes_monitoring_from_older_journals(self) -> None:
        """An existing agent stream preserves recovery without the new journal flag."""
        record = self.create()
        record['public_ip'] = '192.0.2.10'
        (self.directory / 'daemon-stream.jsonl').write_text('', encoding='utf-8')
        remote = mock.Mock(spec=Remote)
        with mock.patch.object(controller, 'remote_for', return_value=remote):
            self.assertEqual(controller.collect(self.state, self.directory), [])
        self.assertEqual(remote.copy.call_count, 3)


class ConfigurationTests(BenchmarkTest):
    """Reject invalid or reserved arguments before any remote work."""

    def test_native_defaults_and_overrides_remain_explicit(self) -> None:
        """
        Automatic native thread count stays automatic and contention options forward.
        """
        options = config.worker_options(
            ['-t', '32', '--connections=100', '--workload=contended', '--lock-count=4']
        )
        self.assertIn('--threads=32', config.benchmark_arguments(options))
        self.assertIn('--lock-count=4', config.benchmark_arguments(options))
        default = config.benchmark_arguments(config.worker_options([]))
        self.assertFalse(any(x.startswith('--threads=') for x in default))
        self.assertFalse(any(x.startswith('--lock-count=') for x in default))
        self.assertFalse(any(x.startswith('--engine=') for x in default))
        self.assertIn(
            '--engine=async',
            config.benchmark_arguments(config.worker_options(['--engine=async'])),
        )

    def test_reserved_and_invalid_worker_options_fail(self) -> None:
        """
        The controller owns host, port and result paths; invalid load cannot create VMs.
        """
        for arguments in (
            ['--interface=127.0.0.1'],
            ['--results=/etc/passwd'],
            ['--port=1'],
            ['--dur=1'],
            ['--run_test=all'],
            ['--duration=-1'],
            ['--connections=0'],
            ['--threads=0'],
            ['--workload=idle', '--hold-ms=1'],
            ['--lock-prefix=é'],
            ['--lock-count=2'],
            ['--workload=unknown'],
            ['--engine=unknown'],
        ):
            with (
                self.subTest(arguments=arguments),
                self.assertRaises((ValueError, argparse.ArgumentError)),
            ):
                config.worker_options(arguments)

    def test_source_hash_is_checked_before_planning(self) -> None:
        """
        A configuration with a substituted source archive cannot proceed to
        provisioning.
        """
        archive = self.directory / 'source.tar.gz'
        archive.write_bytes(b'source')
        key = self.directory / 'key'
        key.write_text('test-only placeholder')
        settings = self.settings | {
            'source_archive': str(archive),
            'ssh_private_key': str(key),
            'source_sha256': config.file_digest(archive),
        }
        destination = self.directory / 'config.json'
        vultr.save(destination, settings)
        self.assertEqual(
            config.load(destination)['source_sha256'], settings['source_sha256']
        )
        vultr.save(
            destination, settings | {'source_sha256': settings['source_sha256'].upper()}
        )
        self.assertEqual(
            config.load(destination)['source_sha256'], settings['source_sha256']
        )
        archive.write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'source_sha256'):
            config.load(destination)

    def test_ssh_quotes_remote_arguments_and_keeps_host_key_checking(self) -> None:
        """
        Shell metacharacters stay literal and trust checks cannot silently be disabled.
        """
        remote = Remote(
            '192.0.2.1',
            self.directory / 'key with spaces',
            self.directory,
            '/root/benchmark',
        )
        arguments = ['python3', 'path with spaces', '$(touch /tmp/WRONG)']
        command = remote.command(arguments)
        self.assertEqual(shlex.split(command[-1]), arguments)
        self.assertIn('StrictHostKeyChecking=accept-new', command)
        self.assertNotIn('StrictHostKeyChecking=no', command)

    def test_api_refuses_redirects_with_bearer_credentials(self) -> None:
        """An API redirect must not forward authorization to another host."""
        request = urllib.request.Request('https://api.vultr.com/v2/instances')
        with self.assertRaises(vultr.ApiError):
            vultr.NoRedirect().redirect_request(
                request, None, 302, '', {}, 'https://other.test'
            )

    def test_api_error_keeps_provider_reason_and_redacts_sensitive_values(self) -> None:
        """
        Retain useful diagnostics while excluding tokens, IDs and unrelated response
        data.
        """
        token = 'TEST_API_TOKEN'
        private = ['TEST_SSH_ID', 'TEST_VPC_ID', 'TEST_USER_DATA']
        body = json.dumps(
            {
                'error': 'Invalid public-plan\n' + token + ' ' + ' '.join(private),
                'default_password': 'DO_NOT_LOG_PASSWORD',
            }
        ).encode()
        failure = urllib.error.HTTPError(
            'https://api.vultr.com/v2/instances',
            400,
            'Bad Request',
            {},
            io.BytesIO(body),
        )
        api = vultr.Api(token)
        with (
            mock.patch.object(api._opener, 'open', side_effect=failure),
            self.assertRaises(vultr.ApiError) as caught,
        ):
            api.request(
                'POST',
                '/instances',
                {
                    'plan': 'public-plan',
                    'sshkey_id': [private[0]],
                    'attach_vpc': [private[1]],
                    'user_data': private[2],
                },
            )
        message = str(caught.exception)
        self.assertIn('HTTP 400: Invalid public-plan', message)
        self.assertIn('[redacted]', message)
        for secret in [token, *private, 'DO_NOT_LOG_PASSWORD']:
            self.assertNotIn(secret, message)
        self.assertNotIn('\n', message)

    def test_api_error_detail_is_bounded_and_ignores_non_json_bodies(self) -> None:
        """
        Proxy HTML and unrelated fields stay out of logs; JSON reasons have a size
        limit.
        """
        self.assertIsNone(
            vultr.provider_error(b'<html>private proxy page</html>', 'token', None)
        )
        self.assertIsNone(
            vultr.provider_error(b'{"default_password":"secret"}', 'token', None)
        )
        detail = vultr.provider_error(
            json.dumps({'error': 'x' * 1000}).encode(), 'token', None
        )
        self.assertEqual(len(detail), vultr.MAX_ERROR_DETAIL_CHARS)

    def test_pagination_visits_later_pages_without_following_absolute_urls(
        self,
    ) -> None:
        """Recovery cannot silently miss a resource on the second account page."""
        api = vultr.Api('test-placeholder')
        with mock.patch.object(
            api,
            'request',
            side_effect=[
                {
                    'instances': [{'id': 'one'}],
                    'meta': {'links': {'next': 'cursor/+?'}},
                },
                {'instances': [{'id': 'two'}], 'meta': {'links': {'next': ''}}},
            ],
        ) as request:
            self.assertEqual(
                api.items('/instances', 'instances'), [{'id': 'one'}, {'id': 'two'}]
            )
        self.assertIn('cursor=cursor%2F%2B%3F', request.call_args.args[1])

    def test_os_catalog_omits_an_empty_initial_cursor(self) -> None:
        """
        OS listings reject cursor=, while the first page works without that parameter.
        """
        api = vultr.Api('test-placeholder')

        def request(method: str, path: str) -> dict:
            """Model the HTTP 400 returned by Vultr for an empty cursor value."""
            if 'cursor=' in path:
                raise vultr.ApiError(400, method, path)
            return {
                'os': [{'id': 1234, 'name': 'Ubuntu'}],
                'meta': {'links': {'next': ''}},
            }

        with mock.patch.object(api, 'request', side_effect=request) as call:
            self.assertEqual(api.items('/os', 'os'), [{'id': 1234, 'name': 'Ubuntu'}])
        call.assert_called_once_with('GET', '/os?per_page=100')


class PreflightTests(BenchmarkTest):
    """Reject unsupported or unavailable plans before billable requests."""

    def provider(
        self,
        available: set[str] | None = None,
        disk: int = 100,
        storage_type: str = 'local_storage',
    ) -> mock.Mock:
        """Model the read-only provider metadata used by production preflight checks."""
        api = mock.Mock(spec=vultr.Api)
        plans = [
            {
                'id': self.settings[role + '_plan'],
                'disk': disk,
                'type': 'vhp',
                'storage_type': storage_type,
            }
            for role in ('daemon', 'worker')
        ]
        image = {
            'id': self.settings['os_id'],
            'name': 'Ubuntu',
            'family': 'ubuntu',
            'arch': 'x64',
        }
        collections = {('/plans', 'plans'): plans, ('/os', 'os'): [image]}
        api.items.side_effect = lambda path, key: collections[path, key]
        available = {plan['id'] for plan in plans} if available is None else available
        responses = {
            '/regions/ewr/availability': {'available_plans': sorted(available)},
            '/vpcs/' + self.settings['vpc_id']: {
                'vpc': {
                    'region': 'ewr',
                    'v4_subnet': '10.20.0.0',
                    'v4_subnet_mask': 24,
                }
            },
            '/firewalls/' + self.settings['firewall_group_id']: {},
            '/ssh-keys/' + self.settings['ssh_key_id']: {},
        }

        def request(method: str, path: str) -> dict:
            """Refuse any mutation rather than silently accepting it in the test."""
            self.assertEqual(method, 'GET')
            return responses[path]

        api.request.side_effect = request
        return api

    def test_preflight_checks_both_plans_and_only_reads_metadata(self) -> None:
        """
        Available local-storage plans pass while shared resource checks remain read-
        only.
        """
        api = self.provider()
        selection = controller.preflight(api, self.settings)
        self.assertEqual(
            selection['image'],
            {'id': self.settings['os_id'], 'name': 'Ubuntu', 'arch': 'x64'},
        )
        self.assertEqual(set(selection['plans']), {'daemon-plan', 'worker-plan'})
        self.assertTrue(
            all(call.args[0] == 'GET' for call in api.request.call_args_list)
        )

    def test_unavailable_daemon_or_worker_stops_before_creation(self) -> None:
        """
        A catalog entry does not imply regional capacity for either selected VM plan.
        """
        for role, other in (('daemon', 'worker'), ('worker', 'daemon')):
            with self.subTest(role=role):
                api = self.provider({self.settings[other + '_plan']})
                with (
                    mock.patch.object(controller.shutil, 'which', return_value='ssh'),
                    mock.patch('builtins.input', side_effect=AssertionError) as prompt,
                    mock.patch.object(
                        vultr, 'create', side_effect=AssertionError
                    ) as create,
                    self.assertRaisesRegex(ValueError, role + ' plan .* unavailable'),
                ):
                    controller.run(api, self.settings)
                prompt.assert_not_called()
                create.assert_not_called()

    def test_block_only_plans_are_rejected_without_provisioning_volumes(self) -> None:
        """
        The benchmark supports local disks and must not implicitly buy block storage.
        """
        with self.assertRaisesRegex(ValueError, 'local storage'):
            controller.preflight(self.provider(disk=0), self.settings)

    def test_block_storage_with_positive_disk_stops_before_creation(self) -> None:
        """
        VX1 block-only plans report disk=1; storage_type must prevent creation.
        """
        for role in ('daemon', 'worker'):
            with self.subTest(role=role):
                api = self.provider()
                plans = api.items('/plans', 'plans')
                plan = next(
                    p for p in plans if p['id'] == self.settings[role + '_plan']
                )
                plan.update(disk=1, type='vx1', storage_type='block_storage')
                with (
                    mock.patch.object(controller.shutil, 'which', return_value='ssh'),
                    mock.patch('builtins.input', side_effect=AssertionError) as prompt,
                    mock.patch.object(
                        vultr, 'create', side_effect=AssertionError
                    ) as create,
                    self.assertRaisesRegex(ValueError, role + ' plan .*local storage'),
                ):
                    controller.run(api, self.settings)
                prompt.assert_not_called()
                create.assert_not_called()

    def test_vx1_local_metadata_reaches_the_creation_request(self) -> None:
        """
        Keep the verified plan classification through run state and local boot POST.
        """
        api = self.provider(storage_type='local_and_block_storage', disk=960)
        plans = api.items('/plans', 'plans')
        plans[0]['type'] = 'vx1'
        selection = controller.preflight(api, self.settings)
        run_id = str(uuid.uuid4())
        location = self.directory / 'local-run'
        self.api.journal = location / 'manifest.json'
        with (
            mock.patch.object(controller.shutil, 'which', return_value='ssh'),
            mock.patch.object(controller, 'preflight', return_value=selection),
            mock.patch.object(controller.uuid, 'uuid4', return_value=uuid.UUID(run_id)),
            mock.patch.object(config, 'output_directory', return_value=location),
            mock.patch.object(
                controller, 'connect', side_effect=RuntimeError('Stop before SSH')
            ),
            mock.patch.object(report, 'generate'),
            mock.patch('builtins.input', return_value=''),
            self.assertRaisesRegex(RuntimeError, 'Stop before SSH'),
        ):
            controller.run(self.api, self.settings)
        state = vultr.load(location / 'manifest.json')
        self.assertEqual(state['plans'], selection['plans'])
        self.assertEqual(
            state['resources'][0]['request']['block_devices'],
            [{'block_id': 'local', 'bootable': True}],
        )
        self.assertEqual(self.api.deleted, [state['resources'][0]['id']])

    def test_unknown_storage_classification_is_not_treated_as_local(self) -> None:
        """Missing or new storage types need review instead of relying on disk size."""
        for storage_type in (None, '', 'unfamiliar_storage'):
            with self.subTest(storage_type=storage_type):
                api = self.provider(storage_type=storage_type)
                if storage_type is None:
                    for plan in api.items('/plans', 'plans'):
                        del plan['storage_type']
                with self.assertRaisesRegex(ValueError, 'local storage'):
                    controller.preflight(api, self.settings)


class CatalogTests(BenchmarkTest):
    """Check the catalog CLI's query construction, price ordering and table output."""

    def setUp(self) -> None:
        """Leave provisioning inputs incomplete while choosing regional resources."""
        super().setUp()
        self.configuration = self.directory / 'benchmark.json'
        self.configuration.write_text('{"region": "ewr"}', encoding='utf-8')

    def arguments(self, kind: str) -> list[str]:
        """Supply the same configuration path to every catalog CLI invocation."""
        return ['benchmarking', 'catalog', kind, str(self.configuration)]

    def test_firewall_catalog_reads_the_paginated_firewall_groups_envelope(
        self,
    ) -> None:
        """Read Vultr's firewall_groups field through the actual catalog CLI path."""
        api = vultr.Api('test-placeholder')
        first = {'id': str(uuid.uuid4()), 'description': 'First benchmark firewall'}
        second = {'id': str(uuid.uuid4()), 'description': 'Second benchmark firewall'}
        with (
            mock.patch.object(
                api,
                'request',
                side_effect=[
                    {
                        'firewall_groups': [first],
                        'meta': {'links': {'next': 'page-two'}},
                    },
                    {'firewall_groups': [second], 'meta': {'links': {'next': ''}}},
                ],
            ) as request,
            mock.patch.object(controller.vultr, 'Api', return_value=api),
            mock.patch.object(sys, 'argv', self.arguments('firewalls')),
        ):
            controller.main()
        output = self.output.getvalue()
        self.assertIn('firewalls catalog is global', output)
        for item in (first, second):
            self.assertIn(item['id'], output)
            self.assertIn(item['description'], output)
        request.assert_has_calls(
            [
                mock.call('GET', '/firewalls?per_page=100'),
                mock.call('GET', '/firewalls?per_page=100&cursor=page-two'),
            ]
        )

    def test_supported_catalogs_extract_their_documented_collections(self) -> None:
        """Exercise every catalog using API-shaped responses, including empty lists."""
        cases = (
            ('regions', 'regions', {'id': 'ewr'}),
            ('plans', 'plans', self.plans()[0]),
            ('os', 'os', {'id': 1234, 'name': 'Ubuntu', 'arch': 'x64'}),
            (
                'vpcs',
                'vpcs',
                {
                    'id': str(uuid.uuid4()),
                    'region': 'ewr',
                    'description': 'Benchmark VPC',
                },
            ),
            (
                'firewalls',
                'firewall_groups',
                {'id': str(uuid.uuid4()), 'description': 'Benchmark firewall'},
            ),
            (
                'ssh-keys',
                'ssh_keys',
                {'id': str(uuid.uuid4()), 'name': 'Benchmark key'},
            ),
        )
        for kind, collection, item in cases:
            for populated in (False, True):
                with self.subTest(kind=kind, populated=populated):
                    self.output.seek(0)
                    self.output.truncate()
                    items = [item] if populated else []
                    api = vultr.Api('test-placeholder')
                    with (
                        mock.patch.object(
                            api,
                            'request',
                            side_effect=[
                                {
                                    collection: items,
                                    'meta': {'links': {'next': ''}},
                                },
                                {'available_plans': [item['id']]},
                            ],
                        ) as request,
                        mock.patch.object(controller.vultr, 'Api', return_value=api),
                        mock.patch.object(sys, 'argv', self.arguments(kind)),
                    ):
                        controller.main()
                    calls = [mock.call('GET', '/' + kind + '?per_page=100')]
                    if kind == 'plans':
                        calls.append(mock.call('GET', '/regions/ewr/availability'))
                    self.assertEqual(request.call_args_list, calls)
                    output = self.output.getvalue()
                    self.assertIn('ewr', output)
                    self.assertIn('---', output)
                    if populated:
                        self.assertIn(str(item['id']), output)
                    else:
                        self.assertIn('(no matching entries)', output)

    @staticmethod
    def plans() -> list[dict]:
        """Provide prices that sort differently as text, including a tie and zero."""
        return [
            {
                'id': identifier,
                'vcpu_count': 1,
                'ram': 512,
                'monthly_cost': cost,
                'disk': 100,
                'type': 'vhp',
                'storage_type': 'local_storage',
            }
            for identifier, cost in (
                ('expensive', 100),
                ('cheap-b', 3.5),
                ('middle', 12),
                ('cheap-a', 3.5),
                ('free', 0),
                ('small', '2.50'),
            )
        ]

    def test_plans_table_sorts_numeric_prices_and_formats_columns(self) -> None:
        """Sort by cost across numeric representations and use IDs to order ties."""
        lines = controller.format_plans(self.plans()).splitlines()
        self.assertIn('RAM (MiB)', lines[0])
        self.assertIn('Monthly cost', lines[0])
        rows = [line.split() for line in lines[2:]]
        self.assertEqual(
            [row[0] for row in rows],
            ['free', 'small', 'cheap-a', 'cheap-b', 'middle', 'expensive'],
        )
        self.assertEqual(
            [row[-1] for row in rows],
            ['0.00', '2.50', '3.50', '3.50', '12.00', '100.00'],
        )
        self.assertTrue(all(row[1:3] == ['1', '512'] for row in rows))

    def test_plans_cli_sorts_the_entire_paginated_catalog(self) -> None:
        """A cheaper plan from a later API page must appear first in the CLI table."""
        api = vultr.Api('test-placeholder')
        plans = self.plans()
        with (
            mock.patch.object(
                api,
                'request',
                side_effect=[
                    {'plans': plans[:3], 'meta': {'links': {'next': 'next-page'}}},
                    {'plans': plans[3:], 'meta': {'links': {'next': ''}}},
                    {'available_plans': [plan['id'] for plan in plans]},
                ],
            ) as request,
            mock.patch.object(controller.vultr, 'Api', return_value=api),
            mock.patch.object(sys, 'argv', self.arguments('plans')),
        ):
            controller.main()
        output = self.output.getvalue()
        self.assertEqual(
            output,
            'Region: ewr\n'
            'Only plans with local boot storage are shown; '
            'no separate volumes are created.\n'
            + controller.format_plans(plans)
            + '\n',
        )
        request.assert_has_calls(
            [
                mock.call('GET', '/plans?per_page=100'),
                mock.call('GET', '/plans?per_page=100&cursor=next-page'),
                mock.call('GET', '/regions/ewr/availability'),
            ]
        )

    def test_global_os_table_filters_unlisted_response_fields(self) -> None:
        """
        Display the OS ID and architecture without dumping unrelated API fields.
        """
        api = mock.Mock()
        api.items.return_value = [
            {
                'id': 1234,
                'name': 'Ubuntu',
                'arch': 'x64',
                'private_material': 'not-for-display',
            }
        ]
        with (
            mock.patch.object(controller.vultr, 'Api', return_value=api),
            mock.patch.object(sys, 'argv', self.arguments('os')),
        ):
            controller.main()
        output = self.output.getvalue()
        for value in ('1234', 'Ubuntu', 'x64', 'os catalog is global', 'Architecture'):
            self.assertIn(value, output)
        self.assertNotIn('not-for-display', output)
        self.assertNotIn('private_material', output)
        api.items.assert_called_once_with('/os', 'os')

    def test_empty_plan_catalog_is_explicit(self) -> None:
        """
        Keep the column headings and explain when the regional result has no rows.
        """
        output = controller.format_plans([])
        self.assertIn('Plan', output)
        self.assertIn('Monthly cost', output)
        self.assertIn('(no matching entries)', output)

    def test_invalid_plan_prices_are_not_presented_as_free(self) -> None:
        """
        Invalid or non-finite costs cannot acquire a misleading position in the table.
        """
        for cost in (-1, 'NaN', 'Infinity', 'invalid', None):
            with self.subTest(cost=cost), self.assertRaises(ValueError):
                controller.format_plans([self.plans()[0] | {'monthly_cost': cost}])

    def test_regional_plans_catalog_filters_current_availability(self) -> None:
        """
        Use the JSON region and live capacity rather than static plan locations.
        """
        api = mock.Mock(spec=vultr.Api)
        plans = self.plans()
        self.configuration.write_text('{"region": "ams"}', encoding='utf-8')
        api.items.return_value = plans
        api.request.return_value = {'available_plans': ['cheap-a', 'small']}
        with (
            mock.patch.object(controller.vultr, 'Api', return_value=api),
            mock.patch.object(sys, 'argv', self.arguments('plans')),
        ):
            controller.main()
        expected = [plan for plan in plans if plan['id'] in ('cheap-a', 'small')]
        self.assertEqual(
            self.output.getvalue(),
            'Region: ams\n'
            'Only plans with local boot storage are shown; '
            'no separate volumes are created.\n'
            + controller.format_plans(expected)
            + '\n',
        )
        api.request.assert_called_once_with('GET', '/regions/ams/availability')

    def test_plans_catalog_excludes_block_only_and_unknown_storage(self) -> None:
        """Available block-only disk=1 must not be offered as a runnable plan."""
        plans = self.plans()
        plans[0].update(disk=1, storage_type='block_storage')
        del plans[1]['storage_type']
        plans[2].update(type='vx1', storage_type='local_and_block_storage', disk=960)
        api = mock.Mock(spec=vultr.Api)
        api.items.return_value = plans
        api.request.return_value = {'available_plans': [plan['id'] for plan in plans]}
        with (
            mock.patch.object(controller.vultr, 'Api', return_value=api),
            mock.patch.object(sys, 'argv', self.arguments('plans')),
        ):
            controller.main()
        output = self.output.getvalue()
        self.assertNotIn('expensive', output)
        self.assertNotIn('cheap-b', output)
        self.assertIn('middle', output)
        self.assertIn('960', output)
        self.assertIn('Local disk (GB)', output)

    def test_vpc_table_filters_all_pages_to_the_configured_region(self) -> None:
        """Exclude another region's VPC even when it precedes a matching later page."""
        self.configuration.write_text('{"region": "ams"}', encoding='utf-8')
        api = vultr.Api('test-placeholder')
        with (
            mock.patch.object(
                api,
                'request',
                side_effect=[
                    {
                        'vpcs': [{'id': 'outside', 'region': 'ewr'}],
                        'meta': {'links': {'next': 'next-page'}},
                    },
                    {
                        'vpcs': [
                            {
                                'id': 'matching',
                                'region': 'ams',
                                'description': 'Benchmark network',
                                'v4_subnet': '10.0.0.0',
                                'v4_subnet_mask': 24,
                            }
                        ],
                        'meta': {'links': {'next': ''}},
                    },
                ],
            ) as request,
            mock.patch.object(controller.vultr, 'Api', return_value=api),
            mock.patch.object(sys, 'argv', self.arguments('vpcs')),
        ):
            controller.main()
        output = self.output.getvalue()
        for value in ('Region: ams', 'matching', 'Benchmark network', '10.0.0.0', '24'):
            self.assertIn(value, output)
        self.assertNotIn('outside', output)
        self.assertEqual(
            request.call_args_list,
            [
                mock.call('GET', '/vpcs?per_page=100'),
                mock.call('GET', '/vpcs?per_page=100&cursor=next-page'),
            ],
        )

    def test_region_table_shows_only_the_configured_region(self) -> None:
        """Region details must describe the JSON choice, not unrelated locations."""
        api = mock.Mock(spec=vultr.Api)
        api.items.return_value = [
            {'id': 'ams', 'city': 'Amsterdam', 'country': 'NL'},
            {'id': 'ewr', 'city': 'Newark', 'country': 'US'},
        ]
        with (
            mock.patch.object(controller.vultr, 'Api', return_value=api),
            mock.patch.object(sys, 'argv', self.arguments('regions')),
        ):
            controller.main()
        self.assertIn('Newark', self.output.getvalue())
        self.assertNotIn('Amsterdam', self.output.getvalue())

    def test_invalid_region_configuration_makes_no_api_calls(self) -> None:
        """
        Fail locally for missing or invalid regions without requiring a full config.
        """
        for settings in ({}, {'region': ''}, {'region': 1}, {'region': ' ewr'}, []):
            with self.subTest(settings=settings):
                self.configuration.write_text(json.dumps(settings), encoding='utf-8')
                with (
                    mock.patch.object(controller.vultr, 'Api') as api,
                    mock.patch.object(sys, 'argv', self.arguments('os')),
                    self.assertRaises((TypeError, ValueError)),
                ):
                    controller.main()
                api.assert_not_called()

    def test_example_placeholders_do_not_block_catalog_selection(self) -> None:
        """
        An unfinished example must list resources without touching SSH or archives.
        """
        self.configuration.write_text(json.dumps(config.example()), encoding='utf-8')
        api = mock.Mock(spec=vultr.Api)
        api.items.return_value = []
        with (
            mock.patch.object(controller.vultr, 'Api', return_value=api),
            mock.patch.object(sys, 'argv', self.arguments('os')),
        ):
            controller.main()
        api.items.assert_called_once_with('/os', 'os')
        api.request.assert_not_called()

    def test_availability_errors_cannot_fall_back_to_unfiltered_plans(self) -> None:
        """
        A failed capacity lookup must not advertise unavailable plans as regional.
        """
        api = mock.Mock(spec=vultr.Api)
        api.items.return_value = self.plans()
        api.request.side_effect = vultr.ApiError(
            400, 'GET', '/regions/ewr/availability'
        )
        with (
            mock.patch.object(controller.vultr, 'Api', return_value=api),
            mock.patch.object(sys, 'argv', self.arguments('plans')),
            self.assertRaises(vultr.ApiError),
        ):
            controller.main()
        self.assertEqual(self.output.getvalue(), '')

    def test_ssh_key_table_keeps_labels_on_one_row_without_showing_key_material(
        self,
    ) -> None:
        """
        Flatten label controls and allowlist fields instead of displaying key bodies.
        """
        api = mock.Mock(spec=vultr.Api)
        api.items.return_value = [
            {
                'id': 'key-id',
                'name': 'Benchmark\nkey\tname',
                'ssh_key': 'SECRET-SENTINEL',
            }
        ]
        with (
            mock.patch.object(controller.vultr, 'Api', return_value=api),
            mock.patch.object(sys, 'argv', self.arguments('ssh-keys')),
        ):
            controller.main()
        output = self.output.getvalue()
        self.assertIn('Benchmark key name', output)
        self.assertIn('ssh-keys catalog is global', output)
        self.assertNotIn('SECRET-SENTINEL', output)


class SessionProgressTests(BenchmarkTest):
    """Keep live CPU output on the same whole-VM scale as exported report graphs."""

    def read_session(self, events: list[dict], role: str = 'worker-0') -> Session:
        """Feed recorded agent events through the real session reader and formatter."""
        remote = mock.Mock(spec=Remote)
        remote.work = '/unused'
        process = mock.Mock()
        process.stdout = io.StringIO(
            ''.join(json.dumps(event) + '\n' for event in events)
        )
        process.stdin = io.StringIO()
        process.wait.return_value = 0
        process.poll.return_value = 0
        with mock.patch(
            'scripts.benchmarking.remote.subprocess.Popen', return_value=process
        ):
            session = Session(remote, self.directory, role)
        self.addCleanup(session.close)
        self.assertEqual(session.finish(3), 0)
        return session

    def test_live_cpu_uses_all_guest_cpus_for_both_roles(self) -> None:
        """Eight busy cores are 25% on a 32-vCPU VM, regardless of process threads."""
        for role, cpus, expected in (('daemon', 32, 25), ('worker-0', 8, 100)):
            with self.subTest(role=role):
                samples = [ReportTests.metric(1, 100), ReportTests.metric(3, 1700)]
                for sample in samples:
                    sample['threads'] = 9
                events = [{'event': 'host', 'cpu_count': cpus}, *samples]
                session = self.read_session(events, role)
                self.assertEqual(session.last_sample['cpu_percent'], 800)
                self.assertEqual(session.last_sample['cpu_capacity_percent'], expected)
                self.assertIn(f'CPU {expected:.1f}% of VM RSS', session.progress())
                recorded = [
                    json.loads(line) for line in session.output.read_text().splitlines()
                ]
                self.assertEqual(recorded, events)
                report_sample = report.read_samples(session.output)[-1]
                self.assertEqual(report_sample['cpu_capacity_percent'], expected)

    def test_missing_or_invalid_guest_cpu_count_is_unavailable(self) -> None:
        """Show unavailable CPU when the VM's capacity is unknown."""
        for cpus in (None, 0, -1, True, '8'):
            with self.subTest(cpus=cpus):
                samples = [ReportTests.metric(1, 100), ReportTests.metric(3, 1700)]
                events = (
                    samples
                    if cpus is None
                    else [{'event': 'host', 'cpu_count': cpus}, *samples]
                )
                session = self.read_session(events)
                self.assertNotIn('cpu_capacity_percent', session.last_sample)
                self.assertIn('CPU unavailable RSS', session.progress())

    def test_first_sample_has_no_cpu_interval_and_overshoot_is_not_clamped(
        self,
    ) -> None:
        """Keep initial CPU unknown and retain observations above VM capacity."""
        first = ReportTests.metric(1, 100)
        session = self.read_session([{'event': 'host', 'cpu_count': 8}, first])
        self.assertIn('CPU unavailable RSS', session.progress())
        session = self.read_session(
            [
                {'event': 'host', 'cpu_count': 8},
                first,
                ReportTests.metric(3, 1860),
            ]
        )
        self.assertIn('CPU 110.0% of VM RSS', session.progress())


class ReportTests(BenchmarkTest):
    """Check count-based latency aggregation and refusal of partial measurements."""

    def csv(
        self,
        role: str = 'worker-0',
        *,
        cycles: int = 2,
        samples: int = 2,
        aborted: int = 0,
    ) -> Path:
        """Write a small native-schema result with a known percentile distribution."""
        path = self.directory / (role + '-result.csv')
        path.write_text(
            'metric,worker,value,count\n'
            'workload,all,independent,0\nrun_id,all,native-id,0\n'
            'threads,all,1,0\nconnections_per_worker,all,100,0\n'
            f'aborted,all,{aborted},0\nmeasurement_seconds,all,10,0\n'
            'start_unix_ms,all,1000000,0\n'
            f'cycles,0,{cycles},0\ntimeouts,0,1,0\nerrors,0,0,0\n'
            f'acquisition_us,0,100,{samples}\n',
            encoding='utf-8',
        )
        return path

    def test_percentiles_use_merged_counts_not_average_percentiles(self) -> None:
        """A tiny slow generator must not dominate a large fast generator's p99."""
        histogram = {100: 10000, 100000: 1}
        self.assertEqual(report.percentile(histogram, 99), 100)
        self.assertEqual(report.percentile({}, 99), 0)

    def test_work_context_uses_global_locks_and_engine_concurrency(self) -> None:
        """Sync rotates connections; async can keep every connection underway."""
        path = self.csv()
        path.write_text(
            path.read_text() + 'hold_ms,all,250,0\nlock_count,all,64,0\n'
            'lock_prefix,all,shared,0\n'
        )
        first = report.read_result(path)
        second = copy.deepcopy(first)
        # Give the small slow generator one observation. Merge counts before
        # dividing by work duration, just as we do for the raw percentiles.
        first['histogram'] = {100: 10000}
        second['histogram'] = {1000000: 1}
        for engine, workload, expected in (
            ('sync', 'independent', 2),
            ('sync', 'contended', 2),
            ('async', 'independent', 200),
            ('async', 'contended', 64),
        ):
            with self.subTest(engine=engine, workload=workload):
                for result in (first, second):
                    result['metadata'].update(engine=engine, workload=workload)
                context = report.workload_context([first, second])
                self.assertEqual(context['maximum_concurrent_holders'], expected)
                self.assertEqual(
                    context['ideal_work_ceiling_cycles_per_second'], expected * 4
                )
                self.assertAlmostEqual(
                    context['work_ceiling_reached_percent'],
                    100 * 0.4 / (expected * 4),
                )
                self.assertEqual(
                    context['latency_upper_bound_work_periods']['p99'], 0.0004
                )
                self.assertEqual(
                    context['shared_lock_count'],
                    64 if workload == 'contended' else None,
                )

    def test_work_context_does_not_invent_work_for_old_or_zero_hold_runs(self) -> None:
        """Zero work has no finite ceiling; historical missing metadata stays absent."""
        result = report.read_result(self.csv())
        self.assertIsNone(report.workload_context([result]))
        result['metadata'].update(hold_ms='0', lock_count='64', lock_prefix='shared')
        context = report.workload_context([result])
        self.assertEqual(context['hold_ms'], 0)
        self.assertIsNone(context['ideal_work_ceiling_cycles_per_second'])
        self.assertIsNone(context['work_ceiling_reached_percent'])
        self.assertTrue(
            all(x is None for x in context['latency_upper_bound_work_periods'].values())
        )
        rendered = report.workload_tables(
            {
                'workload_context': context,
                'aggregate': {
                    'sum_individual_cycles_per_second': result['cycles_per_second'],
                    'merged_latency_upper_bound_us': {
                        f'p{p}': 100 for p in (50, 95, 99)
                    },
                },
            }
        )
        self.assertIn('Ideal workload ceiling reached (fleet)', rendered)
        self.assertIn('<td>0.100</td><td>not applicable</td>', rendered)
        for changes in (
            {'hold_ms': '-1'},
            {'hold_ms': 'unknown'},
            {'lock_count': '0'},
            {'workload': 'idle'},
        ):
            with self.subTest(changes=changes):
                altered = copy.deepcopy(result)
                altered['metadata'].update(changes)
                self.assertIsNone(report.workload_context([altered]))

    def test_native_csv_round_trip_and_timeout_reporting(self) -> None:
        """
        Successful cycles contribute samples while timeout outcomes remain separate.
        """
        result = report.read_result(self.csv())
        self.assertEqual(result['cycles_per_second'], 0.2)
        self.assertEqual(result['timeouts'], 1)
        self.assertEqual(result['connections'], 100)
        self.assertEqual(result['histogram'], {100: 2})

    def test_aborted_setup_keeps_handshake_counts_without_a_throughput_result(
        self,
    ) -> None:
        """Retain partial handshakes in the report while rejecting aborted rates."""
        self.create()
        self.create('worker-0')
        self.state['config']['worker_count'] = 1
        path = self.csv(aborted=1)
        original = path.read_text()
        path.write_text(original + 'tcp_connected,0,90,0\ngreetings_received,0,71,0\n')
        self.assertEqual(
            report.read_setup(path),
            {
                'requested': 100,
                'tcp_connected': 90,
                'greetings_received': 71,
            },
        )
        with self.assertRaises(ValueError):
            report.read_result(path)
        for role in ('daemon', 'worker-0'):
            (self.directory / (role + '-metrics.jsonl')).write_text(
                '\n'.join(
                    json.dumps(event)
                    for event in [
                        {
                            'event': 'host',
                            'cpu_count': 4,
                            'nofile': [1024, 2048],
                            'kernel_network': {
                                'counters': {'TcpExt.ListenDrops': 3},
                                'limits': {'net/core/somaxconn': 128},
                            },
                        },
                        self.metric(1, 100)
                        | {
                            'fd_count': 50,
                            'kernel_network': {'counters': {'TcpExt.ListenDrops': 7}},
                        },
                        self.metric(3, 200)
                        | {
                            'fd_count': 55,
                            'kernel_network': {'counters': {'TcpExt.ListenDrops': 9}},
                        },
                    ]
                )
            )
        with mock.patch.object(
            report, 'output_directory', return_value=self.directory / 'report'
        ):
            destination = report.generate(self.path, self.state)
        summary = json.loads((destination / 'summary.json').read_text())
        self.assertFalse(summary['valid_measurement'])
        worker = next(
            machine for machine in summary['machines'] if machine['role'] == 'worker-0'
        )
        self.assertEqual(worker['setup']['greetings_received'], 71)
        self.assertNotIn('result', worker)
        self.assertEqual(
            worker['network_diagnostics']['counter_deltas']['TcpExt.ListenDrops'][
                'increase'
            ],
            6,
        )
        rendered = (destination / 'report.html').read_text()
        self.assertIn('Initial connection setup', rendered)
        self.assertIn('Kernel TCP counter increases', rendered)
        self.assertIn('INCOMPLETE OR INVALID', rendered)

    def test_setup_rejects_inconsistent_or_missing_counts(self) -> None:
        """Do not invent complete setup for old CSVs or malformed partial rows."""
        path = self.csv()
        original = path.read_text()
        self.assertIsNone(report.read_setup(path))
        for extra in (
            'tcp_connected,0,3,0\n',
            'tcp_connected,0,3,0\ngreetings_received,0,4,0\n',
            'tcp_connected,0,101,0\ngreetings_received,0,2,0\n',
            'tcp_connected,0,-1,0\ngreetings_received,0,0,0\n',
            'tcp_connected,0,3,0\ntcp_connected,0,3,0\ngreetings_received,0,2,0\n',
        ):
            with self.subTest(extra=extra), self.assertRaises(ValueError):
                path.write_text(original + extra)
                report.read_setup(path)

    def test_aborted_or_mismatched_results_are_rejected(self) -> None:
        """A failed run or missing histogram rows cannot become a throughput claim."""
        for options in ({'aborted': 1}, {'cycles': 3, 'samples': 2}):
            with self.subTest(options=options), self.assertRaises(ValueError):
                report.read_result(self.csv(**options))

    def test_resource_cpu_is_per_core_and_high_water_is_distinct(self) -> None:
        """Two fully busy cores are 200%, and VmHWM is not a sampled RSS maximum."""
        samples = [self.metric(1, 100), self.metric(3, 500)]
        metrics = self.directory / 'metrics.jsonl'
        metrics.write_text('\n'.join(json.dumps(item) for item in samples))
        parsed = report.read_samples(metrics)
        resource = report.resources(parsed)
        self.assertEqual(resource['cpu_average_percent'], 200)
        self.assertEqual(resource['cpu_peak_sample_percent'], 200)
        self.assertEqual(resource['peak_sampled_rss_mib'], 2)
        self.assertEqual(resource['largest_observed_hwm_rss_mib'], 4)

    def test_cpu_capacity_uses_vm_cpus_instead_of_daemon_worker_count(self) -> None:
        """Eight busy workers on a 32-vCPU guest use 25% of the VM's CPU capacity."""
        samples = [self.metric(1, 100), self.metric(3, 1700)]
        for sample in samples:
            sample['threads'] = 9
        path = self.directory / 'metrics.jsonl'
        events = [{'event': 'host', 'cpu_count': 32}, *samples]
        path.write_text('\n'.join(json.dumps(event) for event in events))
        parsed = report.read_samples(path, cpu_count=8)
        measured = report.resources(parsed)
        self.assertEqual(parsed[-1]['cpu_percent'], 800)
        self.assertEqual(parsed[-1]['cpu_capacity_percent'], 25)
        self.assertEqual(measured['cpu_capacity_vcpus'], 32)
        self.assertEqual(measured['cpu_capacity_average_percent'], 25)
        self.assertEqual(measured['cpu_capacity_peak_sample_percent'], 25)
        self.assertEqual(measured['cpu_average_percent'], 800)

    def test_cpu_capacity_falls_back_to_metadata_without_inventing_one_cpu(
        self,
    ) -> None:
        """Use known VM metadata, while missing or invalid CPU counts stay unknown."""
        path = self.directory / 'metrics.jsonl'
        path.write_text(
            '\n'.join(
                json.dumps(sample) for sample in (self.metric(), self.metric(3, 500))
            )
        )
        for count, expected in ((8, 25), (32, 6.25)):
            samples = report.read_samples(path, cpu_count=count)
            self.assertEqual(samples[-1]['cpu_capacity_percent'], expected)
        for count in (None, 0, -1, True, '32'):
            with self.subTest(count=count):
                samples = report.read_samples(path, cpu_count=count)
                self.assertNotIn('cpu_capacity_percent', samples[-1])
                self.assertIsNone(
                    report.resources(samples)['cpu_capacity_average_percent']
                )
                self.assertEqual(
                    report.plot(
                        samples,
                        'cpu_capacity_percent',
                        'CPU capacity',
                        percentage=True,
                    ),
                    '',
                )

    def test_cpu_charts_share_a_full_capacity_scale_without_clipping_overshoot(
        self,
    ) -> None:
        """A quiet run stays on 0-100%; a sample above capacity remains visible."""
        samples = [
            {'monotonic': 1, 'cpu_capacity_percent': 20},
            {'monotonic': 2, 'cpu_capacity_percent': 25},
        ]
        chart = report.plot(samples, 'cpu_capacity_percent', 'CPU', percentage=True)
        self.assertIn('scale 0-100.00', chart)
        self.assertIn('>100%</text>', chart)
        self.assertIn('40.00,142.00 650.00,135.00', chart)
        samples[-1]['cpu_capacity_percent'] = 110
        chart = report.plot(samples, 'cpu_capacity_percent', 'CPU', percentage=True)
        self.assertIn('scale 0-110.00', chart)
        self.assertIn('650.00,30.00', chart)

    def test_measurement_cpu_excludes_idle_phases_and_clips_boundary_intervals(
        self,
    ) -> None:
        """Clip both edges of a busy interval, excluding idle setup and drain."""
        path = self.directory / 'metrics.jsonl'
        events = [
            {'event': 'host', 'cpu_count': 8},
            self.metric(1, 0),
            self.metric(11, 0),
            self.metric(15, 3200),
            self.metric(25, 3200),
        ]
        path.write_text('\n'.join(json.dumps(event) for event in events))
        samples = report.read_samples(path)
        measured = report.measurement_cpu(samples, 1011, 1013)
        self.assertEqual(measured['covered_seconds'], 2)
        self.assertTrue(measured['coverage_complete'])
        self.assertEqual(measured['cpu_capacity_average_percent'], 100)
        self.assertLess(report.resources(samples)['cpu_capacity_average_percent'], 20)
        measured = report.measurement_cpu(samples, 1009, 1015)
        self.assertAlmostEqual(measured['cpu_capacity_average_percent'], 100 * 4 / 6)

    def test_cpu_graph_clips_intervals_without_removing_in_window_spikes(self) -> None:
        """Keep overlapping rates, even when their interval ends outside the window."""
        samples = [
            {'monotonic': index, 'unix': 1000 + index, 'cpu_capacity_percent': value}
            for index, value in enumerate((0, 500, 10, 40, 20, 500))
        ]
        chart = report.plot(
            samples, 'cpu_capacity_percent', 'CPU', percentage=True, window=(1001, 1004)
        )
        self.assertIn('scale 0-100.00', chart)
        self.assertIn(
            'points="40.00,156.00 243.33,156.00 243.33,114.00 '
            '446.67,114.00 446.67,142.00 650.00,142.00"',
            chart,
        )
        self.assertIn(
            'scale 0-500.00',
            report.plot(
                samples, 'cpu_capacity_percent', 'Lifetime CPU', percentage=True
            ),
        )
        chart = report.plot(
            samples,
            'cpu_capacity_percent',
            'CPU',
            percentage=True,
            window=(1002.5, 1003.5),
        )
        self.assertIn(
            'points="40.00,114.00 345.00,114.00 345.00,142.00 650.00,142.00"',
            chart,
        )
        # Missing beginning/end coverage must not be drawn as zero CPU.
        chart = report.plot(
            samples[2:4],
            'cpu_capacity_percent',
            'CPU',
            percentage=True,
            window=(1001, 1004),
        )
        self.assertIn('points="243.33,114.00 446.67,114.00"', chart)
        self.assertEqual(
            report.plot(samples, 'cpu_capacity_percent', 'CPU', window=(2000, 2010)), ''
        )

    def test_measurement_cpu_weights_observed_intervals_and_reports_coverage(
        self,
    ) -> None:
        """Show partial means without counting missing time as idle or extrapolating."""
        path = self.directory / 'metrics.jsonl'
        events = [
            {'event': 'host', 'cpu_count': 8},
            self.metric(1, 0),
            self.metric(2, 100),
            self.metric(11, 1900),
        ]
        path.write_text('\n'.join(json.dumps(event) for event in events))
        samples = report.read_samples(path)
        measured = report.measurement_cpu(samples, 1000, 1010)
        self.assertAlmostEqual(measured['cpu_capacity_average_percent'], 23.75)
        for start, end, covered in ((999, 1010, 10), (1000, 1011, 10), (1020, 1030, 0)):
            with self.subTest(start=start, end=end):
                measured = report.measurement_cpu(samples, start, end)
                self.assertEqual(measured['covered_seconds'], covered)
                self.assertFalse(measured['coverage_complete'])
                self.assertAlmostEqual(
                    measured['coverage_percent'], 100 * covered / (end - start)
                )
                if covered:
                    self.assertAlmostEqual(
                        measured['cpu_capacity_average_percent'], 23.75
                    )
                else:
                    self.assertIsNone(measured['cpu_capacity_average_percent'])
        for start, end in ((1000, 1000), (1001, 1000)):
            with self.assertRaises(ValueError):
                report.measurement_cpu(samples, start, end)
        samples[-1]['unix'] = samples[-2]['unix']
        with self.assertRaisesRegex(ValueError, 'wall clock'):
            report.measurement_cpu(samples, 1000, 1010)

    def test_measured_client_coverage_rejects_counts_inferred_from_total_cycles(
        self,
    ) -> None:
        """Many cycles by one client don't prove all connected clients participated."""
        path = self.csv(cycles=200, samples=200)
        original = path.read_text()
        self.assertIsNone(report.read_result(path)['clients_completed'])
        path.write_text(
            original + 'require_all_clients,all,1,0\nclients_completed,0,1,0\n'
        )
        result = report.read_result(path)
        self.assertEqual(result['clients_completed'], 1)
        self.assertFalse(result['client_coverage_complete'])
        path.write_text(
            original + 'require_all_clients,all,1,0\nclients_completed,0,100,0\n'
        )
        self.assertTrue(report.read_result(path)['client_coverage_complete'])
        for rows in (
            'require_all_clients,all,1,0\n',
            'clients_completed,0,101,0\n',
            'clients_completed,0,-1,0\n',
            'clients_completed,1,1,0\n',
        ):
            with self.subTest(rows=rows), self.assertRaises(ValueError):
                path.write_text(original + rows)
                report.read_result(path)
        path = self.csv()
        path.write_text(path.read_text() + 'clients_completed,0,3,0\n')
        with self.assertRaisesRegex(ValueError, 'client coverage'):
            report.read_result(path)

    def test_partial_cpu_can_be_zero_without_being_unavailable(self) -> None:
        """Zero observed CPU and absent sampling coverage have distinct meanings."""
        samples = [self.metric(1, 0), self.metric(3, 0)]
        for sample in samples:
            sample['cpu_capacity_vcpus'] = 8
            sample['cpu_percent'] = 0
        result = report.measurement_cpu(samples, 1000, 1003)
        self.assertFalse(result['coverage_complete'])
        self.assertEqual(result['cpu_capacity_average_percent'], 0)
        self.assertAlmostEqual(result['coverage_percent'], 200 / 3)

    def test_report_labels_partial_cpu_and_incomplete_client_participation(
        self,
    ) -> None:
        """Export observed CPU while refusing a complete claim for missing clients."""
        self.create()
        self.create('worker-0')
        self.state['config']['worker_count'] = 1
        self.state['outcome'] = 'completed'
        path = self.csv(cycles=200, samples=200)
        original = path.read_text() + 'require_all_clients,all,1,0\n'
        path.write_text(original + 'clients_completed,0,99,0\n')
        for role, end in (('daemon', 11), ('worker-0', 10)):
            events = [
                {'event': 'host', 'cpu_count': 8},
                self.metric(1, 0),
                self.metric(end, (end - 1) * 100),
            ]
            (self.directory / (role + '-metrics.jsonl')).write_text(
                '\n'.join(json.dumps(event) for event in events)
            )
        with mock.patch.object(
            report, 'output_directory', return_value=self.directory / 'report'
        ):
            destination = report.generate(self.path, self.state)
            summary = json.loads((destination / 'summary.json').read_text())
            rendered = (destination / 'report.html').read_text()
            self.assertFalse(summary['valid_measurement'])
            self.assertFalse(summary['aggregate']['client_coverage_complete'])
            self.assertEqual(summary['aggregate']['clients_completed'], 99)
            self.assertIn('12.50 (partial)', rendered)
            self.assertIn('9.00 / 10.00', rendered)
            self.assertIn('Clients with measured cycles', rendered)
            path.write_text(original + 'clients_completed,0,100,0\n')
            report.generate(self.path, self.state)
            summary = json.loads((destination / 'summary.json').read_text())
            self.assertTrue(summary['valid_measurement'])
            self.assertTrue(summary['aggregate']['client_coverage_complete'])

    @staticmethod
    def metric(monotonic: float = 1, ticks: int = 100) -> dict:
        """
        Return deterministic process samples with a fixed identity and known units.
        """
        return {
            'event': 'sample',
            'pid': 42,
            'start_ticks': 12,
            'monotonic': monotonic,
            'unix': monotonic + 999,
            'cpu_ticks': ticks,
            'clock_ticks': 100,
            'rss_bytes': 2 * 1024**2,
            'hwm_bytes': 4 * 1024**2,
            'threads': 32,
            'established': 100,
            'host_available_bytes': 100 * 1024**2,
        }

    def test_pid_reuse_rejects_cpu_aggregation(self) -> None:
        """Counter subtraction across different process identities is meaningless."""
        path = self.directory / 'metrics.jsonl'
        path.write_text(
            '\n'.join(
                json.dumps(item)
                for item in (self.metric(), self.metric(2) | {'start_ticks': 15})
            )
        )
        with self.assertRaises(ValueError):
            report.read_samples(path)

    def test_report_marks_missing_workers_and_escapes_html(self) -> None:
        """Missing metrics stay visibly incomplete, and report data is HTML text."""
        self.state['resources'] = [
            {'role': 'worker-0', 'request': {'label': '<script>x</script>'}}
        ]
        self.csv()
        with mock.patch.object(
            report, 'output_directory', return_value=self.directory / 'report'
        ):
            result = report.generate(self.path, self.state)
        rendered = (result / 'report.html').read_text()
        self.assertIn('INCOMPLETE OR INVALID', rendered)
        self.assertNotIn('<script>x</script>', rendered)
        self.assertIn('&lt;script&gt;', rendered)
        self.assertTrue((result / 'worker-0-result.csv').exists())

    def test_complete_report_keeps_resource_and_latency_results_separate(self) -> None:
        """Export tables, merged counts, raw files and explicit clock caveats."""
        self.create()
        self.create('worker-0')
        self.state['config']['worker_count'] = 1
        self.state['outcome'] = 'completed'
        for role in ('daemon', 'worker-0'):
            (self.directory / (role + '-metrics.jsonl')).write_text(
                '\n'.join(
                    json.dumps(sample)
                    for sample in (
                        {'event': 'host', 'cpu_count': 32 if role == 'daemon' else 8},
                        self.metric(),
                        self.metric(3, 500),
                        self.metric(11, 2100),
                    )
                )
            )
        self.csv()
        with mock.patch.object(
            report, 'output_directory', return_value=self.directory / 'report'
        ):
            destination = report.generate(self.path, self.state)
        summary = json.loads((destination / 'summary.json').read_text())
        self.assertTrue(summary['valid_measurement'])
        self.assertEqual(
            summary['aggregate']['merged_latency_upper_bound_us']['p99'], 100
        )
        self.assertEqual(summary['aggregate']['requested_connections'], 100)
        self.assertEqual(
            summary['machines'][0]['resources']['largest_observed_hwm_rss_mib'], 4
        )
        rendered = (destination / 'report.html').read_text()
        self.assertIn('<table>', rendered)
        self.assertIn('not an exact native measurement barrier', rendered)
        self.assertIn('Observed VmHWM', rendered)
        self.assertIn('CPU avg % of VM', rendered)
        self.assertIn('CPU during the shared measurement window', rendered)
        self.assertEqual(summary['cpu_measurement_window']['seconds'], 10)
        self.assertIn('VM capacity; 32 vCPUs', rendered)
        self.assertIn('VM capacity; 8 vCPUs', rendered)
        self.assertEqual(
            summary['machines'][0]['resources']['cpu_capacity_average_percent'], 6.25
        )
        self.assertEqual(
            summary['machines'][1]['resources']['cpu_capacity_average_percent'], 25
        )
        self.assertEqual(
            summary['machines'][0]['measurement_cpu']['cpu_capacity_average_percent'],
            6.25,
        )
        for role in ('daemon', 'worker-0'):
            name = role + '-metrics.jsonl'
            self.assertEqual(
                (destination / name).read_bytes(), (self.directory / name).read_bytes()
            )

    def test_report_shows_work_context_and_measured_cpu_before_lifetime_graphs(
        self,
    ) -> None:
        """Preserve raw waits and lifetime spikes while foregrounding workload CPU."""
        self.create()
        self.create('worker-0')
        self.state['config']['worker_count'] = 1
        self.state['outcome'] = 'completed'
        path = self.csv()
        path.write_text(
            path.read_text().replace(
                'workload,all,independent', 'workload,all,contended'
            )
            + 'hold_ms,all,250,0\nlock_count,all,64,0\nlock_prefix,all,shared,0\n'
        )
        original = path.read_bytes()
        for role in ('daemon', 'worker-0'):
            events = [
                {'event': 'host', 'cpu_count': 8},
                self.metric(0, 0),
                self.metric(1, 800),
                self.metric(11, 900),
                self.metric(12, 1700),
            ]
            (self.directory / (role + '-metrics.jsonl')).write_text(
                '\n'.join(json.dumps(event) for event in events)
            )
        with mock.patch.object(
            report, 'output_directory', return_value=self.directory / 'report'
        ):
            destination = report.generate(self.path, self.state)
        summary = json.loads((destination / 'summary.json').read_text())
        self.assertEqual(summary['workload_context']['work_ceiling_reached_percent'], 5)
        self.assertEqual(
            summary['aggregate']['merged_latency_upper_bound_us']['p99'], 100
        )
        daemon = summary['machines'][0]
        self.assertEqual(
            daemon['measurement_cpu']['cpu_capacity_average_percent'], 1.25
        )
        self.assertEqual(daemon['resources']['cpu_capacity_peak_sample_percent'], 100)
        rendered = (destination / 'report.html').read_text()
        self.assertIn('CPU average: 1.250% of VM capacity', rendered)
        self.assertIn('Raw acquisition wait, ms', rendered)
        self.assertIn('Ideal workload ceiling reached (fleet)', rendered)
        self.assertEqual(
            rendered.count('<td>0.100</td><td>5.00%</td><td>0.0004x</td>'), 3
        )
        self.assertIn('no estimated queueing delay is subtracted', rendered)
        self.assertLess(
            rendered.index('CPU average: 1.250%'), rendered.index('Generator results')
        )
        self.assertIn(
            '<details><summary>Full process lifetime, including setup and shutdown',
            rendered,
        )
        self.assertEqual(path.read_bytes(), original)
        self.assertEqual((destination / path.name).read_bytes(), original)

    def test_report_cpu_uses_all_generator_windows_without_a_lifetime_fallback(
        self,
    ) -> None:
        """Require every generator's result and a positive shared interval."""
        self.create()
        self.state['config']['worker_count'] = 2
        self.state['outcome'] = 'completed'
        for role in ('daemon', 'worker-0', 'worker-1'):
            if role != 'daemon':
                self.create(role)
                csv = self.csv(role)
                csv.write_text(
                    csv.read_text().replace(
                        'start_unix_ms,all,1000000,0',
                        'start_unix_ms,all,'
                        + ('1004000' if role == 'worker-0' else '1007000')
                        + ',0',
                    )
                )
            events = [
                {'event': 'host', 'cpu_count': 32 if role == 'daemon' else 8},
                self.metric(1, 0),
                self.metric(6, 0),
                self.metric(16, 8000),
                self.metric(30, 8000),
            ]
            (self.directory / (role + '-metrics.jsonl')).write_text(
                '\n'.join(json.dumps(event) for event in events)
            )

        def generate() -> dict:
            """Render the report after changing external result files."""
            with mock.patch.object(
                report, 'output_directory', return_value=self.directory / 'report'
            ):
                destination = report.generate(self.path, self.state)
            return json.loads((destination / 'summary.json').read_text())

        summary = generate()
        self.assertEqual(
            summary['cpu_measurement_window'],
            {
                'start_unix': 1007,
                'end_unix': 1014,
                'seconds': 7,
            },
        )
        daemon = summary['machines'][0]
        self.assertEqual(daemon['measurement_cpu']['cpu_capacity_average_percent'], 25)
        self.assertLess(daemon['resources']['cpu_capacity_average_percent'], 10)
        csv = self.directory / 'worker-1-result.csv'
        csv.write_text(csv.read_text().replace('1007000', '2007000'))
        summary = generate()
        self.assertIsNone(summary['cpu_measurement_window'])
        self.assertNotIn('measurement_cpu', summary['machines'][0])
        csv.unlink()
        summary = generate()
        self.assertIsNone(summary['cpu_measurement_window'])
        self.assertNotIn('measurement_cpu', summary['machines'][0])
        rendered = (self.directory / 'report/report.html').read_text()
        self.assertIn('CPU avg % of VM', rendered)
        self.assertIn('unavailable', rendered)

    def test_report_keeps_engine_results_separate(self) -> None:
        """Older files are sync, and mixing them with async invalidates aggregation."""
        self.create()
        self.state['config']['worker_count'] = 2
        self.state['outcome'] = 'completed'
        for role in ('daemon', 'worker-0', 'worker-1'):
            if role != 'daemon':
                self.create(role)
                csv = self.csv(role)
                csv.write_text(
                    csv.read_text() + 'hold_ms,all,250,0\nlock_count,all,64,0\n'
                    'lock_prefix,all,shared,0\n'
                )
                if role == 'worker-1':
                    csv.write_text(csv.read_text() + 'engine,all,async,0\n')
            (self.directory / (role + '-metrics.jsonl')).write_text(
                '\n'.join(
                    json.dumps(sample)
                    for sample in (self.metric(), self.metric(3, 500))
                )
            )
        self.assertEqual(
            report.read_result(self.directory / 'worker-0-result.csv')['metadata'][
                'engine'
            ],
            'sync',
        )
        with mock.patch.object(
            report, 'output_directory', return_value=self.directory / 'report'
        ):
            destination = report.generate(self.path, self.state)
        summary = json.loads((destination / 'summary.json').read_text())
        self.assertFalse(summary['valid_measurement'])
        self.assertTrue(
            any('engines differ' in warning for warning in summary['warnings'])
        )
        self.assertNotIn('workload_context', summary)
        rendered = (destination / 'report.html').read_text()
        self.assertIn('<th>Engine</th>', rendered)
        self.assertIn('<td>async</td>', rendered)


class AgentTests(BenchmarkTest):
    """Check the Linux agent's protocol and archive handling on any controller OS."""

    def setUp(self) -> None:
        """Never discover or invoke the controller machine's real firewall tools."""
        super().setUp()
        boundary = mock.patch.object(agent.shutil, 'which', return_value=None)
        boundary.start()
        self.addCleanup(boundary.stop)

    def test_active_guest_ufw_allows_only_the_private_daemon_endpoint(self) -> None:
        """Allow VPC TCP ingress without disabling UFW or opening the public NIC."""
        address = ipaddress.IPv4Interface('10.1.96.3/20')
        with (
            mock.patch.object(agent.shutil, 'which', return_value='/usr/sbin/ufw'),
            mock.patch.object(
                agent,
                'execute',
                side_effect=[
                    'Status: active\nDefault: deny (incoming), allow (outgoing)\n',
                    'Rule added\n',
                    'Status: active\n',
                ],
            ) as execute,
        ):
            agent.configure_guest_firewall('enp8s0', address, 7198)
        self.assertEqual(
            [call.args[0] for call in execute.call_args_list],
            [
                ['/usr/sbin/ufw', 'status', 'verbose'],
                [
                    '/usr/sbin/ufw',
                    'insert',
                    '1',
                    'allow',
                    'in',
                    'on',
                    'enp8s0',
                    'proto',
                    'tcp',
                    'from',
                    '10.1.96.0/20',
                    'to',
                    '10.1.96.3',
                    'port',
                    '7198',
                    'comment',
                    'lockd-benchmark',
                ],
                ['/usr/sbin/ufw', 'status', 'verbose'],
            ],
        )
        self.assertTrue(
            all(call.kwargs['env']['LC_ALL'] == 'C' for call in execute.call_args_list)
        )
        self.assertIn('Guest UFW after configuration:', self.output.getvalue())

    def test_inactive_or_missing_ufw_is_never_enabled(self) -> None:
        """Record absent or inactive firewalls without changing the image policy."""
        address = ipaddress.IPv4Interface('10.1.96.3/20')
        with mock.patch.object(agent, 'execute') as execute:
            agent.configure_guest_firewall('enp8s0', address, 7198)
        execute.assert_not_called()
        with (
            mock.patch.object(agent.shutil, 'which', return_value='/usr/sbin/ufw'),
            mock.patch.object(
                agent, 'execute', return_value='Status: inactive\n'
            ) as execute,
        ):
            agent.configure_guest_firewall('enp8s0', address, 7198)
        self.assertEqual(execute.call_count, 1)

    def test_worker_ufw_status_is_recorded_without_opening_an_inbound_port(
        self,
    ) -> None:
        """Workers initiate connections and require no new inbound listening rule."""
        with (
            mock.patch.object(agent.shutil, 'which', return_value='/usr/sbin/ufw'),
            mock.patch.object(
                agent, 'execute', return_value='Status: active\n'
            ) as execute,
        ):
            agent.configure_guest_firewall(
                'enp8s0', ipaddress.IPv4Interface('10.1.96.4/20'), None
            )
        self.assertEqual(execute.call_count, 1)

    def test_unknown_ufw_status_stops_before_changing_rules(self) -> None:
        """
        Unexpected status output must not be treated as an active or inactive firewall.
        """
        with (
            mock.patch.object(agent.shutil, 'which', return_value='/usr/sbin/ufw'),
            mock.patch.object(
                agent, 'execute', return_value='Unexpected status\n'
            ) as execute,
            self.assertRaisesRegex(ValueError, 'Cannot determine guest UFW status'),
        ):
            agent.configure_guest_firewall(
                'enp8s0', ipaddress.IPv4Interface('10.1.96.3/20'), 7198
            )
        self.assertEqual(execute.call_count, 1)

    def test_configured_vpc_interface_is_used_for_the_daemon_firewall_rule(
        self,
    ) -> None:
        """Use the API-matched private NIC even when its address already exists."""
        network = {
            'address': '10.1.96.3',
            'prefix': 20,
            'mac_address': 'aa:bb:cc:dd:ee:ff',
            'listen_port': 7198,
        }
        interfaces = [
            {
                'ifname': 'enp8s0',
                'address': network['mac_address'],
                'addr_info': [
                    {'family': 'inet', 'local': '10.1.96.3', 'prefixlen': 20}
                ],
            }
        ]
        with (
            mock.patch.object(
                agent,
                'execute',
                side_effect=[json.dumps(interfaces), '', json.dumps(interfaces), '[]'],
            ),
            mock.patch.object(agent, 'configure_guest_firewall') as firewall,
        ):
            agent.configure_address(network)
        firewall.assert_called_once_with(
            'enp8s0', ipaddress.IPv4Interface('10.1.96.3/20'), 7198
        )

    def test_stat_parser_handles_spaces_and_parentheses_in_process_name(self) -> None:
        """
        The comm field may contain spaces and closing parentheses; offsets stay correct.
        """
        fields = ['S'] + ['0'] * 21
        fields[11], fields[12], fields[19] = '40', '2', '99'
        self.assertEqual(
            agent.process_stat('42 (name with ) spaces) ' + ' '.join(fields)), (42, 99)
        )

    def test_vpc_setup_uses_only_the_api_identified_interface(self) -> None:
        """
        Bring up the matching VPC MAC while leaving the management interface untouched.
        """
        network = {
            'address': '10.20.0.2',
            'prefix': 24,
            'mac_address': 'aa:bb:cc:dd:ee:ff',
        }
        interfaces = [
            {'ifname': 'eth0', 'address': '00:11:22:33:44:55'},
            {'ifname': 'eth1', 'address': network['mac_address'], 'addr_info': []},
        ]
        with mock.patch.object(
            agent, 'execute', side_effect=[json.dumps(interfaces), '', '', '[]', '[]']
        ) as execute:
            agent.configure_address(network)
        self.assertEqual(
            execute.call_args_list[1].args[0],
            ['ip', 'address', 'add', '10.20.0.2/24', 'dev', 'eth1'],
        )
        self.assertEqual(
            execute.call_args_list[2].args[0],
            ['ip', 'link', 'set', 'dev', 'eth1', 'up'],
        )

    def test_vpc_setup_never_overwrites_existing_configuration(self) -> None:
        """Bring up a matching NIC without replacing its address; reject conflicts."""
        network = {
            'address': '10.20.0.2',
            'prefix': 24,
            'mac_address': 'aa:bb:cc:dd:ee:ff',
        }
        interface = {
            'ifname': 'eth1',
            'address': network['mac_address'],
            'addr_info': [{'family': 'inet', 'local': '10.20.0.2', 'prefixlen': 24}],
        }
        with mock.patch.object(
            agent, 'execute', return_value=json.dumps([interface])
        ) as execute:
            agent.configure_address(network)
            execute.assert_any_call(['ip', 'link', 'set', 'dev', 'eth1', 'up'])
            self.assertFalse(
                any(
                    call.args[0][:3] == ['ip', 'address', 'add']
                    for call in execute.call_args_list
                )
            )
        interface['addr_info'][0]['local'] = '10.20.0.3'
        with (
            mock.patch.object(
                agent, 'execute', return_value=json.dumps([interface])
            ) as execute,
            self.assertRaises(ValueError),
        ):
            agent.configure_address(network)
        self.assertEqual(execute.call_count, 1)

    def test_vpc_route_to_daemon_uses_the_private_interface_and_source(self) -> None:
        """Reject a public default route or wrong source before attempting a worker."""
        network = {
            'address': '10.20.0.4',
            'prefix': 24,
            'mac_address': 'aa:bb:cc:dd:ee:ff',
            'peer': '10.20.0.3',
        }
        interfaces = [
            {
                'ifname': 'eth1',
                'address': network['mac_address'],
                'flags': ['BROADCAST'],
                'addr_info': [
                    {'family': 'inet', 'local': network['address'], 'prefixlen': 24}
                ],
            }
        ]
        for device, source, accepted in (
            ('eth1', '10.20.0.4', True),
            ('eth0', '192.0.2.10', False),
            ('eth1', '10.20.0.5', False),
        ):
            with self.subTest(device=device, source=source):
                route = [{'dev': device, 'prefsrc': source}]
                with mock.patch.object(
                    agent,
                    'execute',
                    side_effect=[
                        json.dumps(interfaces),
                        '',
                        json.dumps(interfaces),
                        '[]',
                        json.dumps(route),
                    ],
                ) as execute:
                    if accepted:
                        agent.configure_address(network)
                    else:
                        with self.assertRaisesRegex(
                            ValueError, 'must use VPC interface'
                        ):
                            agent.configure_address(network)
                execute.assert_any_call(['ip', 'link', 'set', 'dev', 'eth1', 'up'])
                execute.assert_any_call(['ip', '-j', 'route', 'get', network['peer']])

    def test_archive_rejects_links_and_path_escape(self) -> None:
        """A source archive cannot write outside its dedicated extraction directory."""
        for index, name in enumerate(('../escape', '/absolute', 'root/link')):
            with self.subTest(name=name):
                archive = self.directory / f'unsafe-{index}.tar'
                member = tarfile.TarInfo(name)
                if name == 'root/link':
                    member.type = tarfile.SYMTYPE
                    member.linkname = '/etc'
                with tarfile.open(archive, 'w') as stream:
                    stream.addfile(member, io.BytesIO())
                with self.assertRaises(ValueError):
                    agent.extract(archive, self.directory / f'extract-{index}')
        self.assertFalse((self.directory / 'escape').exists())

    def test_regular_archive_retains_executable_permission(self) -> None:
        """The same safe extractor supports source and executable deployment bundles."""
        path = self.directory / 'valid.tar'
        member = tarfile.TarInfo('bundle/lockd')
        member.mode, member.size = 0o755, 4
        with tarfile.open(path, 'w') as stream:
            stream.addfile(member, io.BytesIO(b'test'))
        destination = self.directory / 'extracted'
        agent.extract(path, destination)
        self.assertEqual((destination / 'bundle/lockd').read_bytes(), b'test')

    def test_barrier_waits_for_grant_then_explicitly_releases(self) -> None:
        """
        A queued generator cannot proceed before LOCK_ACQUIRED and must acknowledge
        release.
        """
        listener = socket.socket()
        listener.bind(('127.0.0.1', 0))
        listener.listen()
        self.addCleanup(listener.close)
        queued = threading.Event()
        grant = threading.Event()
        observed = []
        failures = []

        def server() -> None:
            """Control grant delivery through events rather than timing assumptions."""
            try:
                with listener.accept()[0] as connection:
                    connection.settimeout(3)
                    with connection.makefile('rwb', buffering=0) as stream:
                        stream.write(b'LOCKD\r\n')
                        observed.append(stream.readline())
                        stream.write(b'LOCK_ACQUIRING\r\n')
                        queued.set()
                        if not grant.wait(3):
                            raise TimeoutError("Test did not grant barrier")
                        stream.write(b'LOCK_ACQUIRED\r\n')
                        observed.append(stream.readline())
                        stream.write(b'LOCK_RELEASED\r\n')
            except OSError as error:
                failures.append(error)

        thread = threading.Thread(target=server, daemon=True)
        thread.start()
        barrier = agent.Barrier(
            '127.0.0.1', listener.getsockname()[1], 'benchmark_gate', 3
        )
        self.addCleanup(barrier.close)
        self.assertTrue(queued.wait(3))
        grant.set()
        barrier.expect('LOCK_ACQUIRED')
        barrier.release()
        thread.join(3)
        self.assertFalse(thread.is_alive())
        self.assertFalse(failures)
        self.assertEqual(observed, [b'LOCK benchmark_gate\n', b'UNLOCK\n'])

    def test_barrier_retries_transient_connection_failures(self) -> None:
        """Allow brief network convergence without retrying any benchmark operation."""
        connection = mock.Mock()
        connection.recv.side_effect = [
            bytes([byte]) for byte in b'LOCKD\nLOCK_ACQUIRING\n'
        ]
        with (
            mock.patch.object(
                agent.socket,
                'create_connection',
                side_effect=[TimeoutError(), connection],
            ) as connect,
            mock.patch.object(agent.time, 'sleep'),
        ):
            barrier = agent.Barrier('10.20.0.2', 7198, 'gate', 30)
        self.assertEqual(connect.call_count, 2)
        connection.sendall.assert_called_once_with(b'LOCK gate\n')
        barrier.close()

    def test_barrier_connection_deadline_names_the_unreachable_endpoint(self) -> None:
        """Bound retries and report the private address without waiting in the test."""
        with (
            mock.patch.object(
                agent.socket, 'create_connection', side_effect=TimeoutError()
            ),
            mock.patch.object(agent.time, 'monotonic', side_effect=[0, 0, 31]),
            mock.patch.object(agent.time, 'sleep'),
            self.assertRaisesRegex(TimeoutError, '10.20.0.2:7198.*interface state'),
        ):
            agent.Barrier('10.20.0.2', 7198, 'gate', 100)

    def test_cancelled_barrier_never_attempts_a_connection(self) -> None:
        """Controller loss before connecting must not open a coordination socket."""
        cancelled = threading.Event()
        cancelled.set()
        with (
            mock.patch.object(agent.socket, 'create_connection') as connect,
            self.assertRaises(InterruptedError),
        ):
            agent.Barrier('10.20.0.2', 7198, 'gate', 30, cancelled)
        connect.assert_not_called()

    def test_session_reports_agent_network_errors_before_readiness(self) -> None:
        """
        An explicit agent error must reach the user even before a child is launched.
        """
        remote = mock.Mock(work='/unused')
        remote.command.return_value = [
            sys.executable,
            '-c',
            (
                'import json; print(json.dumps({"event":"error", '
                '"message":"Cannot connect to 10.20.0.2:7198"}), flush=True)'
            ),
        ]
        session = Session(remote, self.directory, 'worker-0')
        self.addCleanup(session.close)
        with self.assertRaisesRegex(RuntimeError, 'Cannot connect to 10.20.0.2:7198'):
            session.wait_ready(3)
        self.assertFalse(session.has_launched_event)

    @unittest.skipUnless(sys.platform == 'linux', 'Linux /proc monitoring')
    def test_live_proc_sample_has_own_pid_memory_and_threads(self) -> None:
        """
        Read actual kernel counters for this process without changing its configuration.
        """
        sampled = agent.sample(os.getpid())
        self.assertEqual(sampled['pid'], os.getpid())
        self.assertGreater(sampled['rss_bytes'], 0)
        self.assertGreaterEqual(sampled['threads'], 1)
        self.assertGreater(sampled['host_total_bytes'], sampled['host_available_bytes'])

    def test_session_eof_is_not_a_ready_event(self) -> None:
        """
        An SSH command exiting successfully without a ready message must fail startup.
        """
        remote = mock.Mock(work='/unused')
        remote.command.return_value = [sys.executable, '-c', 'pass']
        session = Session(remote, self.directory, 'worker-0')
        self.addCleanup(session.close)
        with self.assertRaises(RuntimeError):
            session.wait_ready(3)


if __name__ == '__main__':
    unittest.main()

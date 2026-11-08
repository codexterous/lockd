"""Check installer deployment with isolated service, registry and firewall boundaries."""

import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

import output_layout
import package


# WSL can expose powershell.exe on PATH, but these fixtures pass native Windows paths.
@unittest.skipUnless(
    os.name == 'nt' and shutil.which('powershell.exe'),
    'Native Windows PowerShell is required',
)
class InstallerOptionsTests(unittest.TestCase):
    """Validate silent options through the same helper used by the NSIS wizard."""

    def test_deployment_delegates_registration_and_preserves_installer_choices(
        self,
    ) -> None:
        """Pass native arguments intact, retain SCM metadata, and recover from failures."""
        root = (
            output_layout.directories(package.ROOT, output_layout.architecture())[
                'verifications'
            ]
            / 'installer'
        )
        root.mkdir(parents=True, exist_ok=True)
        fixture = package.ROOT / 'scripts/release/installer_deployment_fixture.ps1'
        with tempfile.TemporaryDirectory(dir=root) as temporary:
            work = Path(temporary)
            payload = work / 'payload'
            payload.mkdir()
            compile_result = subprocess.run(
                [
                    'powershell.exe',
                    '-NoProfile',
                    '-NonInteractive',
                    '-File',
                    str(fixture),
                    '-BuildProbe',
                    '-Payload',
                    str(payload),
                ],
                capture_output=True,
                text=True,
                timeout=30,
                check=False,
            )
            self.assertEqual(
                compile_result.returncode,
                0,
                compile_result.stdout + compile_result.stderr,
            )
            for name in (
                'lockd_test.exe',
                'license.txt',
                'boost_license.txt',
                'uninstall.exe',
            ):
                (payload / name).write_text(
                    'isolated installer payload', encoding='utf-8'
                )
            for label, scenario, changes, expected in (
                ('defaults', 'Success', {}, 0),
                (
                    'manual',
                    'Success',
                    {
                        'Automatic': '0',
                        'Start': '0',
                        'Threads': '2',
                        'Address': '::1',
                        'EventLog': '0',
                    },
                    0,
                ),
                ('ipv4', 'Success', {'Address': '127.0.0.1', 'Threads': '3'}, 0),
                (
                    'ARM64 registration',
                    'Success',
                    {'Address': '0.0.0.0', 'Firewall': '1'},
                    0,
                ),
                ('explicit worker override', 'Success', {'Threads': '9'}, 0),
                (
                    'private firewall',
                    'Success',
                    {'Address': '0.0.0.0', 'Firewall': '1'},
                    0,
                ),
                ('remote without firewall', 'Success', {'Address': '::'}, 0),
                (
                    'firewall failure',
                    'FirewallFailure',
                    {'Address': '::', 'Firewall': '1'},
                    1,
                ),
                (
                    'registry failure',
                    'RegistryFailure',
                    {'Address': '::', 'Firewall': '1'},
                    1,
                ),
                (
                    'firewall start rollback',
                    'StartFailure',
                    {'Address': '::', 'Firewall': '1'},
                    1,
                ),
                ('duplicate', 'Duplicate', {}, 1),
                ('native failure', 'NativeFailure', {}, 1),
                ('startup failure', 'StartupFailure', {}, 1),
                ('start failure', 'StartFailure', {}, 1),
                ('upgrade', 'Upgrade', {}, 0),
                ('CLI reconfiguration', 'UpgradeReconfigured', {}, 0),
                (
                    'CLI reconfiguration beside native System.dll',
                    'UpgradeReconfigured',
                    {},
                    0,
                ),
                ('firewall upgrade', 'UpgradeFirewall', {}, 0),
                ('removed firewall stays removed', 'UpgradeFirewallMissing', {}, 0),
                ('files only', 'Success', {'Service': '0'}, 0),
            ):
                with self.subTest(scenario=label):
                    case = work / label
                    case.mkdir()
                    if label == 'CLI reconfiguration beside native System.dll':
                        # NSIS extracts a native System.dll into the helper's working
                        # directory. Any native DLL with that name reproduces the
                        # framework reference collision without requiring NSIS here.
                        shutil.copyfile(
                            Path(os.environ['SystemRoot']) / 'System32/kernel32.dll',
                            case / 'System.dll',
                        )
                    target = case / 'installed application'
                    trace = case / 'state.json'
                    arguments_trace = case / 'native arguments.txt'
                    options = case / 'options.ini'
                    settings = {
                        'Service': '1',
                        'Name': 'lockd_selected_name',
                        'Address': 'localhost',
                        'Port': '7199',
                        'Threads': '',
                        'Level': 'debug',
                        'Log': str(case / "log directory/a log's file.txt"),
                        'EventLog': '1',
                        'Automatic': '1',
                        'Start': '1',
                        'Firewall': '0',
                    } | changes
                    options.write_text(
                        '[Service]\n'
                        + ''.join(f'{k}={v}\n' for k, v in settings.items()),
                        encoding='utf-16',
                    )
                    if scenario.startswith('Upgrade'):
                        target.mkdir()
                        for file in payload.iterdir():
                            if file.is_file():
                                shutil.copyfile(file, target / file.name)
                    result = subprocess.run(
                        [
                            'powershell.exe',
                            '-NoProfile',
                            '-NonInteractive',
                            '-File',
                            str(fixture),
                            '-DeployScript',
                            str(package.ROOT / 'platform/win32/installer/deploy.ps1'),
                            '-Directory',
                            str(target),
                            '-Payload',
                            str(payload),
                            '-Options',
                            str(options),
                            '-Trace',
                            str(trace),
                            '-ServiceName',
                            settings['Name'],
                            '-Architecture',
                            'arm64' if label == 'ARM64 registration' else 'x86_64',
                            '-Scenario',
                            scenario,
                        ],
                        cwd=case,
                        env=dict(
                            os.environ,
                            LOCKD_INSTALLER_PROBE_TRACE=str(arguments_trace),
                            LOCKD_INSTALLER_PROBE_EXIT='7'
                            if scenario == 'NativeFailure'
                            else '0',
                        ),
                        capture_output=True,
                        text=True,
                        timeout=30,
                        check=False,
                    )
                    self.assertEqual(
                        result.returncode, expected, result.stdout + result.stderr
                    )
                    state = json.loads(trace.read_text(encoding='utf-8-sig'))
                    invoked = (
                        scenario != 'Duplicate'
                        and not scenario.startswith('Upgrade')
                        and settings['Service'] == '1'
                    )
                    self.assertEqual(arguments_trace.exists(), invoked)
                    created_rule = settings['Firewall'] == '1' and invoked
                    self.assertEqual(len(state['FirewallCreated']), int(created_rule))
                    self.assertEqual(
                        len(state['FirewallDeleted']),
                        int(created_rule and expected != 0),
                    )
                    if created_rule:
                        rule = state['FirewallCreated'][0]
                        self.assertEqual(rule['PolicyStore'], 'PersistentStore')
                        self.assertEqual(rule['Program'], str(target / 'lockd.exe'))
                        self.assertEqual(rule['LocalPort'], settings['Port'])
                        self.assertEqual(rule['Protocol'], 'TCP')
                        self.assertEqual(rule['Profile'], 'Private')
                        self.assertEqual(rule['Direction'], 'Inbound')
                        self.assertEqual(rule['Action'], 'Allow')
                        self.assertEqual(rule['Enabled'], 'True')
                        self.assertEqual(rule['EdgeTraversalPolicy'], 'Block')
                        if not expected:
                            self.assertEqual(
                                state['Registry']['FirewallRule'], rule['Name']
                            )
                    self.assertIn(
                        'unrelated-rule', [r['Name'] for r in state['FirewallRules']]
                    )
                    if scenario in ('UpgradeFirewall', 'UpgradeFirewallMissing'):
                        self.assertEqual(
                            state['Registry']['FirewallRule'],
                            'lockd-x86_64-11111111-2222-3333-4444-555555555555',
                        )
                    elif not created_rule:
                        self.assertNotIn('FirewallRule', state['Registry'])
                    if label == 'ARM64 registration':
                        self.assertEqual(
                            state['Registry']['DisplayName'], 'lockd (arm64)'
                        )
                        self.assertTrue(
                            state['Registry']['FirewallRule'].startswith('lockd-arm64-')
                        )
                    if invoked:
                        arguments = [
                            '--install-service',
                            '--service-name',
                            settings['Name'],
                            '--interface',
                            settings['Address'],
                            '--port',
                            settings['Port'],
                            '--log-level',
                            settings['Level'],
                            '--log-file',
                            settings['Log'],
                        ]
                        if settings['Threads']:
                            arguments += ['--threads', settings['Threads']]
                        if settings['EventLog'] == '0':
                            arguments += ['--no-event-log']
                        self.assertEqual(
                            arguments_trace.read_text(
                                encoding='utf-8-sig'
                            ).splitlines(),
                            arguments,
                        )
                    if expected:
                        self.assertFalse(state['BinaryExists'])
                        self.assertEqual(
                            state['Deleted'],
                            [settings['Name']]
                            if scenario
                            in (
                                'StartupFailure',
                                'StartFailure',
                                'FirewallFailure',
                                'RegistryFailure',
                            )
                            else [],
                        )
                        self.assertEqual(state['Registry'], {})
                        if scenario == 'Duplicate':
                            self.assertIn(
                                'service name is already in use', result.stdout
                            )
                        if scenario == 'NativeFailure':
                            self.assertIn('lockd.exe returned 7', result.stdout)
                        continue
                    self.assertTrue(state['BinaryExists'])
                    cleanup = json.loads(state['Registry']['CleanupState'])
                    self.assertEqual(
                        str(target) in cleanup['Directories'],
                        not scenario.startswith('Upgrade'),
                    )
                    if not scenario.startswith('Upgrade'):
                        self.assertEqual(
                            cleanup['LogFiles'],
                            [settings['Log']] if settings['Service'] == '1' else [],
                        )
                        self.assertEqual(
                            cleanup['EventSources'],
                            [settings['Name']] if settings['Service'] == '1' else [],
                        )
                    if settings['Service'] == '0':
                        self.assertEqual(state['Registry']['ServiceName'], '')
                        self.assertEqual(state['Registry']['ServiceCommand'], '')
                        self.assertEqual(state['Startup'], [])
                        self.assertEqual(state['Started'], [])
                    else:
                        self.assertEqual(
                            state['Registry']['ServiceName'], settings['Name']
                        )
                        self.assertEqual(
                            state['Registry']['ServiceCommand'],
                            f'"{target / "lockd.exe"}" --as-service --updated-options'
                            if scenario == 'UpgradeReconfigured'
                            else '"command supplied by lockd.exe" --daemon-owned-options',
                        )
                        startup = (
                            []
                            if scenario.startswith('Upgrade')
                            else [
                                {
                                    'Name': settings['Name'],
                                    'Mode': 'Automatic'
                                    if settings['Automatic'] == '1'
                                    else 'Manual',
                                }
                            ]
                        )
                        self.assertEqual(state['Startup'], startup)
                        self.assertEqual(
                            state['Started'],
                            [settings['Name']] if settings['Start'] == '1' else [],
                        )
                        self.assertEqual(
                            state['Stopped'],
                            [settings['Name']]
                            if scenario.startswith('Upgrade')
                            else [],
                        )

    def test_uninstall_cleans_owned_resources_and_preserves_unrelated_data(
        self,
    ) -> None:
        """Exercise log consent, registration ownership, path guards and cleanup failures."""
        root = (
            output_layout.directories(package.ROOT, output_layout.architecture())[
                'verifications'
            ]
            / 'installer'
        )
        root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=root) as temporary:
            for scenario, remove_logs, expected in (
                ('OwnedEventSource', '0', 0),
                ('OwnedEventSource', '1', 0),
                ('PreexistingEventSource', '1', 0),
                ('UnrelatedFiles', '1', 0),
                ('ForeignEventSource', '1', 1),
                ('SourceReadFailure', '1', 1),
                ('LockedLog', '1', 1),
                ('LockedLogRetry', '1', 0),
                ('RedirectedLogs', '1', 1),
                ('InvalidDirectory', '1', 1),
                ('PruneFailureEventSource', '1', 1),
                ('PruneRetryEventSource', '1', 0),
            ):
                with self.subTest(scenario=scenario, remove_logs=remove_logs):
                    case = Path(temporary) / (scenario + remove_logs)
                    target = case / 'app1'
                    logs = case / 'logs'
                    target.mkdir(parents=True)
                    logs.mkdir()
                    for name in (
                        'lockd.exe',
                        'lockd_test.exe',
                        'license.txt',
                        'boost_license.txt',
                        'uninstall.exe',
                    ):
                        (target / name).write_bytes(b'installed fixture')
                    log = logs / 'a [log].txt'
                    archive = log.with_name(log.name + '.00001')
                    for file in (log, archive):
                        file.write_bytes(b'log fixture')
                    extra = target / 'notes.txt'
                    external_archive = log.with_name(log.name + '.old')
                    if scenario == 'UnrelatedFiles':
                        extra.write_bytes(b'user data')
                        external_archive.write_bytes(b'external rotation')
                    preexisting = scenario == 'PreexistingEventSource'
                    cleanup = {
                        'Directories': [] if preexisting else [str(target), str(logs)],
                        'LogFiles': [str(log)],
                        'EventSources': [] if preexisting else ['lockd_cleanup_test'],
                    }
                    outside = case / 'unrelated-empty-directory'
                    if scenario == 'InvalidDirectory':
                        outside.mkdir()
                        cleanup['Directories'].append(str(outside))
                    metadata = case / 'cleanup.json'
                    metadata.write_text(json.dumps(cleanup), encoding='utf-8')
                    trace = case / 'state.json'
                    arguments = [
                        'powershell.exe',
                        '-NoProfile',
                        '-NonInteractive',
                        '-File',
                        str(
                            package.ROOT
                            / 'scripts/release/installer_deployment_fixture.ps1'
                        ),
                        '-DeployScript',
                        str(package.ROOT / 'platform/win32/installer/deploy.ps1'),
                        '-Operation',
                        'Uninstall',
                        '-Directory',
                        str(target),
                        '-Trace',
                        str(trace),
                        '-ServiceName',
                        'lockd_cleanup_test',
                        '-Scenario',
                        scenario,
                        '-CleanupStateFile',
                        str(metadata),
                        '-RemoveLogs',
                        remove_logs,
                    ]
                    if scenario in ('LockedLogRetry', 'PruneRetryEventSource'):
                        arguments.append('-RetryCleanup')
                    result = subprocess.run(
                        arguments,
                        capture_output=True,
                        text=True,
                        timeout=30,
                        check=False,
                    )
                    self.assertEqual(
                        result.returncode, expected, result.stdout + result.stderr
                    )
                    state = json.loads(trace.read_text(encoding='utf-8-sig'))
                    self.assertEqual(state['RegistryExists'], bool(expected))
                    if scenario in ('LockedLogRetry', 'PruneRetryEventSource'):
                        self.assertEqual(state['CleanupAttempts'], [1, 0])
                        self.assertEqual(state['Deleted'], ['lockd_cleanup_test'])
                    if expected:
                        if scenario in ('ForeignEventSource', 'SourceReadFailure'):
                            self.assertEqual(state['Deleted'], [])
                            self.assertEqual(state['EventSourcesDeleted'], [])
                            self.assertTrue((target / 'lockd.exe').is_file())
                        if scenario == 'InvalidDirectory':
                            self.assertTrue(outside.is_dir())
                            self.assertIn('outside this installation', result.stdout)
                        elif scenario != 'PruneFailureEventSource':
                            self.assertTrue(log.is_file())
                            self.assertTrue(archive.is_file())
                        self.assertTrue((target / 'uninstall.exe').is_file())
                        self.assertEqual(
                            (target / 'uninstall.exe').read_bytes(),
                            b'installed fixture',
                        )
                        continue
                    self.assertFalse((target / 'uninstall.exe').exists())
                    self.assertEqual(log.exists(), remove_logs == '0')
                    self.assertEqual(archive.exists(), remove_logs == '0')
                    if scenario == 'UnrelatedFiles':
                        self.assertEqual(extra.read_bytes(), b'user data')
                        self.assertEqual(
                            external_archive.read_bytes(), b'external rotation'
                        )
                    self.assertEqual(
                        target.exists(), preexisting or scenario == 'UnrelatedFiles'
                    )
                    self.assertEqual(
                        logs.exists(),
                        preexisting
                        or scenario == 'UnrelatedFiles'
                        or remove_logs == '0',
                    )
                    self.assertEqual(bool(state['EventSources']), preexisting)

    def test_options_are_validated_before_installation(self) -> None:
        """Reject malformed addresses, ports and commands without creating a target."""
        root: Path = (
            package.ROOT
            / f'output/obj/{output_layout.architecture()}/opt/shared/verifications/installer'
        )
        root.mkdir(parents=True, exist_ok=True)
        settings: dict[str, str] = {
            'Service': '1',
            'Name': 'lockd_validation_test',
            'Address': '127.0.0.1',
            'Port': '7198',
            'Threads': '',
            'Level': 'info',
            'Log': str(root / 'a log.txt'),
            'EventLog': '1',
            'Automatic': '1',
            'Start': '1',
            'Firewall': '0',
        }
        with tempfile.TemporaryDirectory(dir=root) as temporary:
            work = Path(temporary)
            target = work / 'installation'
            options = work / 'options.ini'
            scenarios: list[tuple[dict[str, str], int]] = [
                ({}, 0),
                ({'Service': '0'}, 0),
                ({'Address': '::1'}, 0),
                ({'Address': 'localhost'}, 0),
                ({'Address': 'lockd.example.com'}, 0),
                ({'Address': 'host" --extra'}, 1),
                ({'Port': '0'}, 1),
                ({'Port': '65536'}, 1),
                ({'Threads': '-1'}, 1),
                ({'Address': 'not an address'}, 1),
                ({'Name': 'name" --extra'}, 1),
                ({'Log': 'relative.log'}, 1),
                ({'Level': 'invalid'}, 1),
                ({'Automatic': 'yes'}, 1),
                ({'Firewall': 'yes'}, 1),
                ({'Service': '0', 'Firewall': '1'}, 1),
                ({'Address': '0.0.0.0', 'Firewall': '1'}, 0),
                ({'Address': '::', 'Firewall': '1'}, 0),
                ({'Address': 'lockd.example.com', 'Firewall': '1'}, 0),
            ]
            loopbacks = (
                'localhost',
                'LOCALHOST',
                'localhost.',
                '127.0.0.1',
                '127.12.34.56',
                '::1',
                '::1%1',
                '0:0:0:0:0:0:0:1',
                '::ffff:127.0.0.1',
            )
            scenarios += [
                ({'Address': address, 'Firewall': '1'}, 1) for address in loopbacks
            ]
            operations = [
                ('Validate', changes, expected) for changes, expected in scenarios
            ]
            operations += [
                ('FirewallAvailable', {'Address': address}, 2) for address in loopbacks
            ]
            operations += [
                ('FirewallAvailable', {'Address': address}, 0)
                for address in ('0.0.0.0', '::', '192.0.2.1', 'lockd.example.com')
            ]
            for operation, changes, expected in operations:
                with self.subTest(operation=operation, changes=changes):
                    values = settings | changes
                    options.write_text(
                        '[Service]\n'
                        + ''.join(f'{key}={value}\n' for key, value in values.items()),
                        encoding='utf-16',
                    )
                    result = subprocess.run(
                        [
                            'powershell.exe',
                            '-NoProfile',
                            '-NonInteractive',
                            '-ExecutionPolicy',
                            'Bypass',
                            '-File',
                            str(
                                package.ROOT
                                / 'scripts/release/installer_deployment_fixture.ps1'
                            ),
                            '-DeployScript',
                            str(package.ROOT / 'platform/win32/installer/deploy.ps1'),
                            '-Operation',
                            operation,
                            '-Directory',
                            str(target),
                            '-Options',
                            str(options),
                            '-Trace',
                            str(work / 'validation-state.json'),
                            '-ServiceName',
                            settings['Name'],
                        ],
                        env=dict(
                            os.environ,
                            LOCKD_INSTALLER_PROBE_TRACE=str(work / 'unused-probe.txt'),
                        ),
                        check=False,
                        capture_output=True,
                        text=True,
                        timeout=30,
                    )
                    self.assertEqual(
                        result.returncode, expected, result.stdout + result.stderr
                    )
                    self.assertFalse(target.exists())

    def test_firewall_cleanup_uses_only_the_recorded_installation_rule(self) -> None:
        """Remove owned rules on uninstall and reject a rule replaced by another owner."""
        root = (
            output_layout.directories(package.ROOT, output_layout.architecture())[
                'verifications'
            ]
            / 'installer'
        )
        root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=root) as temporary:
            for scenario, operation, expected in (
                ('UninstallFirewall', 'Uninstall', 0),
                ('Success', 'Uninstall', 0),
                ('UpgradeFirewallForeign', 'Install', 1),
                ('UpgradeForeignCommand', 'Install', 1),
            ):
                with self.subTest(scenario=scenario):
                    case = Path(temporary) / scenario
                    target = case / 'installation'
                    target.mkdir(parents=True)
                    for name in (
                        'lockd.exe',
                        'lockd_test.exe',
                        'license.txt',
                        'boost_license.txt',
                        'uninstall.exe',
                    ):
                        (target / name).write_text(
                            'existing installation', encoding='utf-8'
                        )
                    options = case / 'options.ini'
                    options.write_text('[Service]\nService=0\n', encoding='utf-16')
                    trace = case / 'state.json'
                    result = subprocess.run(
                        [
                            'powershell.exe',
                            '-NoProfile',
                            '-NonInteractive',
                            '-File',
                            str(
                                package.ROOT
                                / 'scripts/release/installer_deployment_fixture.ps1'
                            ),
                            '-DeployScript',
                            str(package.ROOT / 'platform/win32/installer/deploy.ps1'),
                            '-Operation',
                            operation,
                            '-Directory',
                            str(target),
                            '-Options',
                            str(options),
                            '-Trace',
                            str(trace),
                            '-ServiceName',
                            'lockd_cleanup_test',
                            '-Scenario',
                            scenario,
                        ],
                        capture_output=True,
                        text=True,
                        timeout=30,
                        check=False,
                    )
                    self.assertEqual(
                        result.returncode, expected, result.stdout + result.stderr
                    )
                    state = json.loads(trace.read_text(encoding='utf-8-sig'))
                    self.assertIn(
                        'unrelated-rule', [r['Name'] for r in state['FirewallRules']]
                    )
                    self.assertEqual(state['FirewallCreated'], [])
                    self.assertEqual(
                        state['FirewallDeleted'],
                        ['lockd-x86_64-11111111-2222-3333-4444-555555555555']
                        if scenario == 'UninstallFirewall'
                        else [],
                    )
                    self.assertEqual(state['BinaryExists'], bool(expected))
                    self.assertEqual(
                        state['Deleted'], ['lockd_cleanup_test'] if not expected else []
                    )

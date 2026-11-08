"""Check Windows wheel ABI selection without downloading or compiling Boost."""

import json
import os
import shutil
import subprocess
import tempfile
import unittest
import zipfile
from pathlib import Path

import output_layout
import package


@unittest.skipUnless(
    os.name == 'nt' and shutil.which('powershell.exe'),
    'Native Windows PowerShell is required',
)
class WindowsWheelTests(unittest.TestCase):
    """Drive Windows wheel build and test scripts with external processes replaced."""

    def test_wheels_select_a_runnable_packaged_daemon(self) -> None:
        """ARM64 must use its native daemon; x64 may also verify the x86 package."""
        work = output_layout.directories(package.ROOT, output_layout.architecture())[
            'verifications'
        ]
        work.mkdir(parents=True, exist_ok=True)
        for architecture, skip_native, expected in (
            ('arm64', True, ['arm64']),
            ('arm64', False, ['arm64']),
            ('x86_64', True, ['x86_64']),
            ('x86_64', False, ['x86', 'x86_64']),
            ('x86', True, ['x86_64']),
        ):
            with (
                self.subTest(architecture=architecture, skip_native=skip_native),
                tempfile.TemporaryDirectory(dir=work) as temporary,
            ):
                root = Path(temporary)
                distributions = root / 'packages'
                distributions.mkdir()
                for arch in ('x86', 'x86_64', 'arm64'):
                    archive = distributions / f'lockd-1.2.3-windows-{arch}-portable.zip'
                    with zipfile.ZipFile(archive, 'w') as files:
                        for name in ('lockd.exe', 'lockd_test.exe'):
                            files.writestr(name, f'fixture for {arch}')
                platform = package.WINDOWS_WHEEL_PLATFORMS[architecture]
                (distributions / f'pylockd-1.2.3-cp314-cp314-{platform}.whl').touch()
                trace = root / 'trace.json'
                fixture = root / 'wheel_test.ps1'
                fixture.write_text(
                    'param($Script, $Packages, [switch] $SkipNative)\n'
                    '$global:wheelTestCommands = @()\n'
                    'function python { param($Source)\n'
                    '  $global:LASTEXITCODE = 0\n'
                    '  if ($Source.EndsWith("package.py")) { $env:WHEEL_DETAILS }\n'
                    '  else { @{ verifications = $env:WHEEL_WORK } | ConvertTo-Json }\n'
                    '}\n'
                    'function Invoke-MockedCommand { param($Command, $Arguments)\n'
                    '  $global:wheelTestCommands += @{ command = $Command; arguments = @($Arguments) }\n'
                    '}\n'
                    # Aliases take precedence over the production function. Only the
                    # external process boundary is replaced; selection and extraction run.
                    'Set-Alias Invoke-Checked Invoke-MockedCommand\n'
                    '& $Script -DistributionDirectory $Packages -SkipNativeTests:$SkipNative\n'
                    '@{ daemon = $env:LOCKD_TEST_DAEMON; commands = $global:wheelTestCommands } | '
                    'ConvertTo-Json -Depth 5 | Set-Content $env:WHEEL_TRACE -Encoding UTF8\n',
                    encoding='utf-8',
                )
                details = {
                    'version': '3.14',
                    'tag': 'cp314',
                    'architecture': architecture,
                    'wheel_platform': platform,
                }
                result = subprocess.run(
                    [
                        'powershell.exe',
                        '-NoProfile',
                        '-File',
                        str(fixture),
                        '-Script',
                        str(package.ROOT / 'scripts/release/test_windows.ps1'),
                        '-Packages',
                        str(distributions),
                        *(['-SkipNative'] if skip_native else []),
                    ],
                    cwd=root,
                    env=dict(
                        os.environ,
                        WHEEL_DETAILS=json.dumps(details),
                        WHEEL_WORK=str(root / 'work'),
                        WHEEL_TRACE=str(trace),
                    ),
                    capture_output=True,
                    text=True,
                    check=False,
                    timeout=30,
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                actual = json.loads(trace.read_text(encoding='utf-8-sig'))
                runtime = root / 'work/release_distributions/cp314'
                daemon_arch = 'arm64' if architecture == 'arm64' else 'x86_64'
                self.assertEqual(
                    Path(actual['daemon']),
                    runtime / f'portable/{daemon_arch}/lockd.exe',
                )
                self.assertEqual(
                    sorted(path.name for path in (runtime / 'portable').iterdir()),
                    sorted(expected),
                )
                commands = [Path(item['command']) for item in actual['commands']]
                for arch in expected:
                    test = runtime / f'portable/{arch}/lockd_test.exe'
                    self.assertEqual(test in commands, not skip_native, actual)

    def test_boost_build_matches_the_selected_interpreter(self) -> None:
        """Use the target's compiler, address width, compact paths, and SDK directories."""
        work = output_layout.directories(package.ROOT, output_layout.architecture())[
            'verifications'
        ]
        work.mkdir(parents=True, exist_ok=True)
        for architecture, processor, bits, suffix in (
            ('x86_64', 'x86', 64, ''),
            ('x86', 'x86', 32, '_x86'),
            ('arm64', 'arm', 64, '_arm64'),
        ):
            with (
                self.subTest(architecture=architecture),
                tempfile.TemporaryDirectory(dir=work) as temporary,
            ):
                root = Path(temporary)
                scripts = root / 'scripts/release'
                scripts.mkdir(parents=True)
                shutil.copyfile(
                    package.ROOT / 'scripts/release/build_boost_python.ps1',
                    scripts / 'build_boost_python.ps1',
                )
                dependencies = root / 'dependencies'
                source = dependencies / 'release/boost_1_88_0'
                source.mkdir(parents=True)
                (source / 'b2.exe').touch()
                (dependencies / 'release/boost_1_88_0.tar.gz').touch()
                trace = root / 'commands.json'
                (scripts / 'windows_environment.ps1').write_text(
                    'param($Architecture)\n'
                    'if ($Architecture -ne $env:WHEEL_TARGET) { throw "Wrong compiler target" }\n'
                    '$layout = @{ dependencies = (Join-Path $env:WHEEL_ROOT "dependencies") }\n'
                    'function Get-Command { param($Name, $ErrorAction)\n'
                    '  @{ Source = (Join-Path $env:WHEEL_ROOT "compiler/$Name") }\n'
                    '}\n'
                    'function Get-Item { param($LiteralPath)\n'
                    '  @{ VersionInfo = @{ FileMajorPart = 19; FileMinorPart = 44 } }\n'
                    '}\n'
                    'function Get-FileHash { param($LiteralPath, $Algorithm)\n'
                    '  @{ Hash = "3621533e820dcab1e8012afd583c0c73cf0f77694952b81352bf38c1488f9cb4" }\n'
                    '}\n'
                    'function Invoke-Checked { param($Command, $Arguments)\n'
                    '  if ($Command -ne "./b2.exe") { throw "Unexpected build command" }\n'
                    '  ConvertTo-Json -InputObject @($Arguments) | Set-Content $env:WHEEL_TRACE\n'
                    '}\n',
                    encoding='utf-8',
                )
                details = {
                    'version': '3.14',
                    'tag': 'cp314',
                    'architecture': architecture,
                    'boost_name': 'boost_python314' + suffix,
                    'executable': str(root / 'python/python.exe'),
                    'include': str(root / 'python/include'),
                    'prefix': str(root / 'python'),
                }
                result = subprocess.run(
                    [
                        'powershell.exe',
                        '-NoProfile',
                        '-Command',
                        (
                            'function python { $global:LASTEXITCODE = 0; $env:WHEEL_DETAILS }; '
                            '& ./scripts/release/build_boost_python.ps1'
                        ),
                    ],
                    cwd=root,
                    env=dict(
                        os.environ,
                        LOCKD_BOOST_CACHE_HIT='',
                        WHEEL_TARGET=architecture,
                        WHEEL_ROOT=str(root),
                        WHEEL_TRACE=str(trace),
                        WHEEL_DETAILS=json.dumps(details),
                    ),
                    capture_output=True,
                    text=True,
                    check=False,
                    timeout=30,
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                arguments = json.loads(trace.read_text(encoding='utf-8-sig'))
                self.assertIn(f'architecture={processor}', arguments)
                self.assertIn(f'address-model={bits}', arguments)
                self.assertIn('--hash', arguments)
                paths = {
                    key: Path(value)
                    for argument in arguments
                    if argument.startswith(('--prefix=', '--build-dir='))
                    for key, value in [argument.split('=', 1)]
                }
                self.assertEqual(
                    paths['--prefix'], dependencies / f'release/boost_python314{suffix}'
                )
                self.assertEqual(
                    paths['--build-dir'],
                    dependencies
                    / f'release/boost_python_build/boost_python314{suffix}',
                )
                self.assertTrue((dependencies / 'release/.boost-cache-ready').is_file())


if __name__ == '__main__':
    unittest.main()

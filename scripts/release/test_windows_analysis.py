"""Exercise Windows analysis orchestration without installing dependencies."""

import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import output_layout
import package


# WSL can expose powershell.exe on PATH, but these fixtures pass native Windows paths.
@unittest.skipUnless(
    os.name == 'nt' and shutil.which('powershell.exe'),
    'Native Windows PowerShell is required',
)
@patch.dict(os.environ, LOCKD_HOST_BUILD_DIR='')
class WindowsAnalysisTests(unittest.TestCase):
    """Keep native and Python analysis separate from dependency installation."""

    def test_developer_shell_preserves_selected_vcpkg_installation(self) -> None:
        """Compiler initialization must not replace the caller's or runner's vcpkg."""
        work = output_layout.directories(package.ROOT, output_layout.architecture())[
            'verifications'
        ]
        work.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=work) as temporary:
            root = Path(temporary)
            script = root / 'scripts/release/windows_environment.ps1'
            script.parent.mkdir(parents=True)
            shutil.copyfile(
                package.ROOT / 'scripts/release/windows_environment.ps1', script
            )
            program_files = root / 'program-files'
            vswhere = program_files / 'Microsoft Visual Studio/Installer/vswhere.exe'
            vswhere.parent.mkdir(parents=True)
            fixture = root / 'developer_shell.ps1'
            fixture.write_text(
                'param($VsWhere, $EnvironmentScript)\n'
                'if (!(Test-Path -LiteralPath $VsWhere)) {\n'
                '  Add-Type -OutputAssembly $VsWhere -OutputType ConsoleApplication '
                '-TypeDefinition @\'\n'
                'using System;\n'
                'public static class VsWhereFixture {\n'
                '  public static void Main(string[] args) {\n'
                '    Console.WriteLine(Environment.GetEnvironmentVariable("LOCKD_TEST_VS"));\n'
                '  }\n'
                '}\n'
                "'@\n"
                '}\n'
                '$script:entered = $false\n'
                'function Import-Module { param($Name) }\n'
                'function Enter-VsDevShell {\n'
                '  param($VsInstallPath, [switch] $SkipAutomaticLocation, $DevCmdArguments)\n'
                '  if ($VsInstallPath -ne $env:LOCKD_TEST_VS) { throw "Wrong compiler" }\n'
                '  $env:VCPKG_ROOT = $env:LOCKD_TEST_BUNDLED_VCPKG\n'
                '  $script:entered = $true\n'
                '  $script:arguments = $DevCmdArguments\n'
                '}\n'
                '. $EnvironmentScript -Architecture $env:LOCKD_TEST_ARCH\n'
                '@{ root = $env:VCPKG_ROOT; entered = $script:entered; arguments = $script:arguments } | ConvertTo-Json\n',
                encoding='utf-8',
            )
            user = str(root / 'user-vcpkg')
            runner = str(root / 'runner-vcpkg')
            bundled = str(root / 'visual-studio/VC/vcpkg')
            path_root = root / 'path-vcpkg'
            path_root.mkdir()
            (path_root / 'vcpkg.exe').touch()
            # Keep package-manager discovery independent of the host's PATH.
            fixture_path = os.pathsep.join(
                entry
                for entry in os.environ['PATH'].split(os.pathsep)
                if not (Path(entry) / 'vcpkg.exe').is_file()
            )
            for architecture, compiler_target, host in (
                ('x86', 'x86', 'AMD64'),
                ('x86_64', 'x64', 'AMD64'),
                ('arm64', 'arm64', 'ARM64'),
            ):
                for initialized in (False, True):
                    for selected in ('user', 'runner', 'path', 'unspecified'):
                        with self.subTest(
                            architecture=architecture,
                            initialized=initialized,
                            selected=selected,
                        ):
                            environment = dict(os.environ)
                            environment.pop('VCPKG_ROOT', None)
                            environment.pop('VCPKG_INSTALLATION_ROOT', None)
                            environment.pop('PROCESSOR_ARCHITEW6432', None)
                            environment['PROCESSOR_ARCHITECTURE'] = host
                            environment['LOCKD_MSVC_TOOLSET_VERSION'] = (
                                '14.44' if architecture == 'arm64' else ''
                            )
                            environment['VCToolsVersion'] = (
                                '14.44.35207' if initialized else '14.51.36247'
                            )
                            if selected == 'user':
                                environment['VCPKG_ROOT'] = user
                            environment['PATH'] = fixture_path
                            if selected != 'unspecified':
                                environment['PATH'] = (
                                    str(path_root) + os.pathsep + fixture_path
                                )
                            if selected in ('user', 'runner'):
                                environment['VCPKG_INSTALLATION_ROOT'] = runner
                            environment.update(
                                {
                                    'ProgramFiles(x86)': str(program_files),
                                    'VSCMD_ARG_TGT_ARCH': (
                                        compiler_target if initialized else 'other'
                                    ),
                                    'LOCKD_TEST_ARCH': architecture,
                                    'LOCKD_TEST_VS': str(root / 'visual-studio'),
                                    'LOCKD_TEST_BUNDLED_VCPKG': bundled,
                                }
                            )
                            result = subprocess.run(
                                [
                                    'powershell.exe',
                                    '-NoProfile',
                                    '-File',
                                    str(fixture),
                                    '-VsWhere',
                                    str(vswhere),
                                    '-EnvironmentScript',
                                    str(script),
                                ],
                                env=environment,
                                capture_output=True,
                                text=True,
                                check=False,
                                timeout=30,
                            )
                            self.assertEqual(result.returncode, 0, result.stderr)
                            actual = json.loads(result.stdout)
                            expected = {
                                'user': user,
                                'runner': runner,
                                'path': str(path_root),
                            }.get(selected, '' if initialized else bundled)
                            self.assertEqual(actual['root'] or '', expected)
                            self.assertEqual(actual['entered'], not initialized)
                            if not initialized:
                                compiler_host = 'arm64' if host == 'ARM64' else 'x64'
                                expected_arguments = f'-arch={compiler_target} -host_arch={compiler_host}'
                                if architecture == 'arm64':
                                    expected_arguments += ' -vcvars_ver=14.44'
                                self.assertEqual(
                                    actual['arguments'], expected_arguments
                                )

    def test_tool_caches_follow_the_host_from_an_x86_shell(self) -> None:
        """Changing the compiler target must reuse the host's existing tool cache."""
        source = (package.ROOT / 'scripts/release/windows_environment.ps1').read_text()
        prefix = source.split('# Discover the installed compiler;', 1)[0]
        temporary_root = output_layout.directories(
            package.ROOT, output_layout.architecture()
        )['verifications']
        temporary_root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=temporary_root) as temporary:
            root = Path(temporary)
            script = root / 'scripts/release/windows_environment.ps1'
            script.parent.mkdir(parents=True)
            script.write_text(
                prefix + '\n$layout | ConvertTo-Json -Compress\n', encoding='utf-8'
            )
            for machine, wow64, expected in (
                ('AMD64', '', 'x86_64'),
                ('x86', 'AMD64', 'x86_64'),
                ('ARM64', '', 'arm64'),
                ('x86', '', 'x86'),
            ):
                with self.subTest(machine=machine, wow64=wow64):
                    environment = dict(os.environ, PROCESSOR_ARCHITECTURE=machine)
                    environment.pop('PROCESSOR_ARCHITEW6432', None)
                    environment.pop('LOCKD_HOST_BUILD_DIR', None)
                    if wow64:
                        environment['PROCESSOR_ARCHITEW6432'] = wow64
                    result = subprocess.run(
                        ['powershell.exe', '-NoProfile', '-File', str(script)],
                        env=environment,
                        capture_output=True,
                        text=True,
                        check=False,
                        timeout=30,
                    )
                    self.assertEqual(result.returncode, 0, result.stderr)
                    layout = json.loads(result.stdout)
                    self.assertEqual(
                        Path(layout['dependencies']),
                        root / f'output/obj/{expected}/opt/shared/dependencies',
                    )

    def test_extracted_source_reuses_the_explicit_host_build_directory(self) -> None:
        """PowerShell and Python must select the same short dependency path in CI."""
        source = (package.ROOT / 'scripts/release/windows_environment.ps1').read_text()
        prefix = source.split('# Discover the installed compiler;', 1)[0]
        work = output_layout.directories(package.ROOT, output_layout.architecture())[
            'verifications'
        ]
        work.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=work) as temporary:
            workspace = Path(temporary) / 'output/obj/x86_64/opt/shared'
            script = (
                workspace / 'source/lockd-test/scripts/release/windows_environment.ps1'
            )
            script.parent.mkdir(parents=True)
            script.write_text(prefix + '\n$layout | ConvertTo-Json -Compress\n')
            environment = dict(os.environ, LOCKD_HOST_BUILD_DIR=str(workspace))
            result = subprocess.run(
                ['powershell.exe', '-NoProfile', '-File', str(script)],
                env=environment,
                capture_output=True,
                text=True,
                check=False,
                timeout=30,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(
                Path(json.loads(result.stdout)['dependencies']),
                workspace / 'dependencies',
            )

    def test_compiled_boost_hit_never_prepares_source(self) -> None:
        """A valid installed prefix needs neither the source archive nor build tools."""
        temporary_root: Path = (
            package.ROOT
            / f'output/obj/{output_layout.architecture()}/opt/shared/verifications/windows_analysis'
        )
        temporary_root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=temporary_root) as temporary:
            root: Path = Path(temporary)
            script: Path = root / 'build_boost_python.ps1'
            shutil.copyfile(
                package.ROOT / 'scripts/release/build_boost_python.ps1', script
            )
            (root / 'package.py').write_text(
                'print(\'{"version":"3.14","architecture":"x86_64",'
                '"boost_name":"boost_python314"}\')\n',
                encoding='utf-8',
            )
            (root / 'windows_environment.ps1').write_text(
                "$ErrorActionPreference = 'Stop'\n"
                '$repositoryRoot = $PSScriptRoot\n'
                '$layout = @{ dependencies = (Join-Path $repositoryRoot '
                "'output/obj/x86_64/opt/shared/dependencies') }\n"
                '$global:LASTEXITCODE = 0\n'
                "function Invoke-WebRequest { throw 'Unexpected download' }\n"
                "function Invoke-Checked { throw 'Unexpected external command' }\n",
                encoding='utf-8',
            )
            prefix: Path = (
                root
                / 'output/obj/x86_64/opt/shared/dependencies/release/boost_python314'
            )
            for name in (
                'include/boost-1_88/boost/version.hpp',
                'lib/boost_python314.lib',
                'lib/boost_python314.dll',
            ):
                path: Path = prefix / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()
            for incomplete in (False, True):
                if incomplete:
                    (prefix / 'lib/boost_python314.dll').unlink()
                result = subprocess.run(
                    [
                        'powershell.exe',
                        '-NoProfile',
                        '-ExecutionPolicy',
                        'Bypass',
                        '-File',
                        str(script),
                    ],
                    check=False,
                    capture_output=True,
                    text=True,
                    env=dict(os.environ, LOCKD_BOOST_CACHE_HIT='true'),
                )
                self.assertEqual(
                    result.returncode == 0,
                    not incomplete,
                    result.stdout + result.stderr,
                )
                if incomplete:
                    self.assertIn('cache is incomplete', result.stderr)
                self.assertFalse((prefix.parent / 'boost_1_88_0.tar.gz').exists())
                self.assertFalse((prefix.parent / 'boost_1_88_0').exists())

    def test_boost_cache_markers_follow_checksum_and_install_results(self) -> None:
        """Keep verified sources after build failure and clear stale success markers."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            script = root / 'build_boost_python.ps1'
            shutil.copyfile(
                package.ROOT / 'scripts/release/build_boost_python.ps1', script
            )
            dependencies = root / 'output/obj/x86_64/opt/shared/dependencies/release'
            source = dependencies / 'boost_1_88_0'
            source.mkdir(parents=True)
            (source / 'b2.exe').touch()
            (dependencies / 'boost_1_88_0.tar.gz').touch()
            (root / 'package.py').write_text(
                'print(\'{"version":"3.14","executable":"python",'
                '"include":"include","prefix":"prefix","tag":"cp314",'
                '"architecture":"x86_64","boost_name":"boost_python314"}\')\n',
                encoding='utf-8',
            )
            (root / 'windows_environment.ps1').write_text(
                "$ErrorActionPreference = 'Stop'\n"
                '$repositoryRoot = $PSScriptRoot\n'
                '$layout = @{ dependencies = (Join-Path $repositoryRoot '
                "'output/obj/x86_64/opt/shared/dependencies') }\n"
                '$global:LASTEXITCODE = 0\n'
                'Import-Module Microsoft.PowerShell.Utility\n'
                'function Get-Command($Name) {\n'
                '    $directory = if ($Name -eq "link.exe") {\n'
                '        $env:LINKER_DIRECTORY\n'
                '    } else { "C:/fixture" }\n'
                '    @{ Source = "$directory/$Name" }\n'
                '}\n'
                'function Get-Item { @{ VersionInfo = @{ '
                'FileMajorPart = 19; FileMinorPart = [int]$env:COMPILER_MINOR } } }\n'
                'function Get-FileHash { @{Hash = $env:ARCHIVE_HASH} }\n'
                'function Invoke-Checked {\n'
                '    param($Command, $Arguments)\n'
                '    ConvertTo-Json -Compress -InputObject $Arguments |\n'
                '        Set-Content -LiteralPath $env:BUILD_TRACE\n'
                "    if ($env:FAIL_BUILD -eq 'true') { throw 'Build failed' }\n"
                '}\n',
                encoding='utf-8',
            )
            for fail_build, corrupt, minor, linker in (
                (False, False, '36', 'C:/fixture'),
                (True, False, '36', 'C:/fixture'),
                (False, True, '36', 'C:/fixture'),
                (False, False, '0', 'C:/fixture'),
                (False, False, '36', 'C:/other'),
            ):
                trace = root / 'build.json'
                trace.unlink(missing_ok=True)
                invalid_compiler = minor == '0' or linker != 'C:/fixture'
                result = subprocess.run(
                    [
                        'powershell.exe',
                        '-NoProfile',
                        '-ExecutionPolicy',
                        'Bypass',
                        '-File',
                        str(script),
                    ],
                    capture_output=True,
                    text=True,
                    check=False,
                    env=dict(
                        os.environ,
                        LOCKD_BOOST_CACHE_HIT='',
                        FAIL_BUILD=str(fail_build).lower(),
                        COMPILER_MINOR=minor,
                        LINKER_DIRECTORY=linker,
                        BUILD_TRACE=str(trace),
                        ARCHIVE_HASH=(
                            'corrupt'
                            if corrupt
                            else '3621533e820dcab1e8012afd583c0c73cf0f77694952b81352bf38c1488f9cb4'
                        ),
                    ),
                )
                self.assertEqual(
                    result.returncode == 0,
                    not (fail_build or corrupt or invalid_compiler),
                    result.stdout + result.stderr,
                )
                self.assertEqual(
                    (dependencies / '.boost-source-verified').exists(), not corrupt
                )
                self.assertEqual(
                    (dependencies / '.boost-cache-ready').exists(),
                    not (fail_build or corrupt or invalid_compiler),
                )
                self.assertEqual(trace.exists(), not (corrupt or invalid_compiler))
                if trace.exists():
                    arguments = json.loads(trace.read_text(encoding='utf-8-sig'))
                    self.assertIn('toolset=msvc-14.3', arguments)
                    self.assertIn('architecture=x86', arguments)
                    self.assertIn('address-model=64', arguments)
                    self.assertIn('--project-config=', arguments)
                    self.assertIn('--ignore-site-config', arguments)
                    self.assertIn(
                        'using msvc : 14.3 : "C:/fixture/cl.exe" ;',
                        (dependencies / 'boost_python314.jam').read_text(
                            encoding='utf-8'
                        ),
                    )

    def test_analysis_configures_python_and_propagates_each_failure(self) -> None:
        """A configure, scan, or regression-test failure must fail the analysis job."""
        temporary_root: Path = (
            package.ROOT
            / f'output/obj/{output_layout.architecture()}/opt/shared/verifications/windows_analysis'
        )
        temporary_root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=temporary_root) as temporary:
            root: Path = Path(temporary)
            script: Path = root / 'analyze_windows.ps1'
            shutil.copyfile(
                package.ROOT / 'scripts/release/analyze_windows.ps1', script
            )

            # Record only the external command boundary. Execute the real orchestration.
            (root / 'windows_environment.ps1').write_text(
                "$ErrorActionPreference = 'Stop'\n"
                '$repositoryRoot = $PSScriptRoot\n'
                '$layout = @{ dependencies = (Join-Path $repositoryRoot '
                "'output/obj/x86_64/opt/shared/dependencies') }\n"
                'function python {}\n'
                '$script:commandCount = 0\n'
                'function Invoke-Checked {\n'
                '    param([string] $Command, [string[]] $Arguments)\n'
                '    $script:commandCount++\n'
                '    ConvertTo-Json -Compress -InputObject (@($Command) + $Arguments) |\n'
                '        Add-Content -LiteralPath $env:TRACE\n'
                '    if ($script:commandCount -eq [int]$env:FAIL_COMMAND) {\n'
                "        throw 'Recorded command failure'\n"
                '    }\n'
                '}\n',
                encoding='utf-8',
            )
            for fail_command in range(5):
                trace: Path = root / f'trace-{fail_command}.jsonl'
                environment: dict[str, str] = dict(
                    os.environ,
                    TRACE=str(trace),
                    FAIL_COMMAND=str(fail_command),
                    LOCKD_CI_CLANG_TIDY='C:/analysis tools/clang-tidy.exe',
                )
                result = subprocess.run(
                    [
                        'powershell.exe',
                        '-NoProfile',
                        '-ExecutionPolicy',
                        'Bypass',
                        '-File',
                        str(script),
                    ],
                    env=environment,
                    check=False,
                    capture_output=True,
                    text=True,
                )
                commands: list[list[str]] = [
                    json.loads(line)
                    for line in trace.read_text(encoding='utf-8-sig').splitlines()
                ]
                self.assertEqual(len(commands), fail_command or 4)
                self.assertEqual(
                    result.returncode == 0,
                    fail_command == 0,
                    result.stdout + result.stderr,
                )
                self.assertIn('-DLOCKD_DEPENDENCY_PROVIDER=SYSTEM', commands[0])
                python_build = (
                    'lib/pylockd/output/obj/cp314-cp314-win_amd64/opt/static/analysis'
                )
                self.assertEqual(commands[0][commands[0].index('-B') + 1], python_build)
                self.assertIn('-DLOCKD_BUILD_PYTHON=ON', commands[0])
                self.assertIn('-DSKBUILD=ON', commands[0])
                self.assertIn(
                    '-DLOCKD_CLANG_TIDY_EXECUTABLE=C:/analysis tools/clang-tidy.exe',
                    commands[0],
                )
                self.assertIn('-DLOCKD_CLANG_TIDY_WARNINGS_AS_ERRORS=ON', commands[0])
                if not fail_command:
                    self.assertEqual(commands[1][-2:], ['--target', 'clang_tidy'])
                    self.assertEqual(commands[2][-2:], ['--target', 'clang_tidy'])
                    self.assertNotEqual(commands[1][2], commands[2][2])
                    self.assertEqual(commands[2][2], python_build)
                    self.assertEqual(commands[3][0], 'ctest')

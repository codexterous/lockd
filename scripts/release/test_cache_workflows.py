"""Check that dependency caches survive failed jobs without publishing failed wheels."""

import os
import re
import shutil
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path

import output_layout
import package


class CacheWorkflowTests(unittest.TestCase):
    """Keep all existing caches independent of downstream validation results."""

    def test_wheel_uploads_still_require_success(self) -> None:
        """Failure-independent dependency saves must not enable failed wheel uploads."""
        for name in ('release_python.yml', 'release_python_windows.yml'):
            text = (package.ROOT / '.github/workflows' / name).read_text()
            for step in re.split(r'(?m)^\s*- name: ', text)[1:]:
                if 'uses: actions/upload-artifact@' not in step or '.whl' not in step:
                    continue
                self.assertNotIn('always()', step, name)
                self.assertNotIn('failure()', step, name)

    @unittest.skipUnless(
        os.name == 'nt' and shutil.which('powershell.exe'),
        'Native Windows PowerShell is required',
    )
    def test_windows_wheel_cache_restores_a_reusable_host_installation(self) -> None:
        """Preserve and restore real workflow paths, then require the build to skip b2."""
        workflow = (
            package.ROOT / '.github/workflows/release_python_windows.yml'
        ).read_text()
        steps = re.split(r'(?m)^\s*- name: ', workflow)[1:]
        scripts: dict[str, str] = {}
        for action in ('Preserve', 'Restore'):
            step = next(
                s
                for s in steps
                if s.startswith(f'{action} compiled Windows wheel dependencies\n')
            )
            scripts[action] = textwrap.dedent(step.split('        run: |\n', 1)[1])
            scripts[action] = scripts[action].replace(
                '${{ env.OUTPUT_ARCH }}', 'x86_64'
            )
        work = output_layout.directories(package.ROOT, output_layout.architecture())[
            'verifications'
        ]
        work.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=work) as temporary:
            workspace = Path(temporary)
            host = workspace / 'output/obj/x86_64/opt/shared'
            source = host / 'source/lockd-1.0.35'
            release = host / 'dependencies/release'
            prefix = release / 'boost_python314'
            for name in (
                'include/boost-1_88/boost/version.hpp',
                'lib/boost_python314.lib',
                'lib/boost_python314.dll',
            ):
                file = prefix / name
                file.parent.mkdir(parents=True, exist_ok=True)
                file.write_text('cached dependency')
            scripts_dir = source / 'scripts/release'
            scripts_dir.mkdir(parents=True)
            shutil.copyfile(
                package.ROOT / 'scripts/release/build_boost_python.ps1',
                scripts_dir / 'build_boost_python.ps1',
            )
            (scripts_dir / 'windows_environment.ps1').write_text(
                '$ErrorActionPreference = "Stop"\n'
                '$layout = @{ dependencies = (Join-Path $env:LOCKD_HOST_BUILD_DIR "dependencies") }\n'
                '$global:LASTEXITCODE = 0\n'
                'function python { \'{"version":"3.14", "architecture":"x86_64", '
                '"boost_name":"boost_python314"}\' }\n'
                'function Invoke-WebRequest { throw "Unexpected download" }\n'
                'function Invoke-Checked { throw "Unexpected Boost build" }\n'
            )
            environment = dict(
                os.environ,
                LOCKD_HOST_BUILD_DIR=str(host),
                PYTHON_VERSION='3.14',
                LOCKD_BOOST_CACHE_HIT='true',
                WHEEL_ARCH='x86_64',
            )
            for action in ('Preserve', 'Restore', 'Build'):
                if action == 'Restore':
                    prefix.rename(prefix.with_name('original-installation'))
                script = (
                    scripts[action]
                    if action != 'Build'
                    else '. ./scripts/release/windows_environment.ps1\n'
                    './scripts/release/build_boost_python.ps1'
                )
                result = subprocess.run(
                    [
                        'powershell.exe',
                        '-NoProfile',
                        '-Command',
                        '$ErrorActionPreference = "Stop"\n' + script,
                    ],
                    cwd=source if action == 'Build' else workspace,
                    env=environment,
                    capture_output=True,
                    text=True,
                    check=False,
                    timeout=30,
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                if action == 'Build':
                    self.assertIn('Using cached Boost.Python', result.stdout)
            self.assertEqual(
                (prefix / 'lib/boost_python314.dll').read_text(), 'cached dependency'
            )
            self.assertFalse((release / 'boost_1_88_0').exists())

    @unittest.skipUnless(
        os.name == 'nt' and shutil.which('powershell.exe'),
        'Native Windows PowerShell is required',
    )
    def test_windows_wheel_isolation_hides_host_dependencies_and_restores_on_failure(
        self,
    ) -> None:
        """Run the workflow isolation step from extracted sources on success and failure."""
        text = (
            package.ROOT / '.github/workflows/release_python_windows.yml'
        ).read_text()
        step = next(
            s
            for s in re.split(r'(?m)^\s*- name: ', text)[1:]
            if s.startswith('Test Windows Python ')
        )
        script = textwrap.dedent(step.split('        run: |\n', 1)[1])
        script = script.replace('${{ env.OUTPUT_ARCH }}', 'x86_64')
        script = script.replace('${{ matrix.python }}', '3.11')
        script = script.replace('${{ matrix.arch }}', 'x86_64')
        work = output_layout.directories(package.ROOT, output_layout.architecture())[
            'verifications'
        ]
        work.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=work) as temporary:
            host = Path(temporary) / 'output/obj/x86_64/opt/shared'
            source = host / 'source/lockd-1.0.35'
            dependencies = host / 'dependencies/release'
            builds = source / 'lib/pylockd/output/obj'
            for directory in (dependencies, builds):
                directory.mkdir(parents=True)
                (directory / 'sentinel').write_text('retained')
            test_script = source / 'scripts/release/test_windows.ps1'
            test_script.parent.mkdir(parents=True)
            test_script.write_text(
                'param($DistributionDirectory, $WheelDirectory, [switch] $SkipNativeTests)\n'
                '$dependencies = Join-Path $env:LOCKD_HOST_BUILD_DIR "dependencies/release"\n'
                'if (Test-Path -LiteralPath $dependencies) { throw "Boost is still visible" }\n'
                'if (Test-Path -LiteralPath "lib/pylockd/output/obj") '
                '{ throw "Extensions are still visible" }\n'
                'if (!$SkipNativeTests) { throw "Native selection was lost" }\n'
                'if ($env:FAIL_TEST -eq "true") { throw "Test failed deliberately" }\n'
            )
            for fail in (False, True):
                result = subprocess.run(
                    ['powershell.exe', '-NoProfile', '-Command', script],
                    cwd=source,
                    env=dict(
                        os.environ,
                        LOCKD_HOST_BUILD_DIR=str(host),
                        FAIL_TEST=str(fail).lower(),
                    ),
                    capture_output=True,
                    text=True,
                    check=False,
                    timeout=30,
                )
                self.assertEqual(
                    result.returncode == 0, not fail, result.stdout + result.stderr
                )
                if fail:
                    self.assertIn('Test failed deliberately', result.stderr)
                for directory in (dependencies, builds):
                    self.assertEqual((directory / 'sentinel').read_text(), 'retained')
                    self.assertFalse(Path(str(directory) + '-unavailable').exists())

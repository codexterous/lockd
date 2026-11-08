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

    def test_every_cache_has_an_explicit_failure_independent_save(self) -> None:
        """Every restored tool cache has a guarded save that bypasses success()."""
        paths: list[Path] = list((package.ROOT / '.github').rglob('*.yml'))
        restores = 0
        for path in paths:
            text = path.read_text(encoding='utf-8')
            self.assertNotIn('uses: actions/cache@', text, str(path))
            steps = re.split(r'(?m)^\s*- name: ', text)[1:]
            for step in steps:
                if 'uses: actions/cache/restore@' not in step:
                    continue
                restores += 1
                identity = re.search(r'(?m)^\s+id: (\w+)$', step).group(1)
                key = 'steps.' + identity + '.outputs.cache-primary-key'
                saves = [
                    candidate
                    for candidate in steps
                    if 'uses: actions/cache/save@' in candidate and key in candidate
                ]
                self.assertEqual(len(saves), 1, (path, identity))
                condition = re.search(r'(?m)^\s+if: (.+)$', saves[0]).group(1)
                self.assertIn('always()', condition, (path, identity))
                self.assertTrue(
                    'outcome' in condition or 'hashFiles(' in condition,
                    (path, identity),
                )
        self.assertGreater(restores, 0)

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

    def test_native_cache_can_save_new_packages_after_a_restore(self) -> None:
        """Keep rebuilt ABI variants after cache hits without resaving unchanged data."""
        for relative in (
            '.github/workflows/release_windows.yml',
            '.github/actions/windows_analysis/action.yml',
        ):
            with self.subTest(workflow=relative):
                text = (package.ROOT / relative).read_text()
                steps = re.split(r'(?m)^\s*- name: ', text)[1:]
                restored = next(s for s in steps if 'id: native_cache\n' in s)
                baseline = next(s for s in steps if 'id: native_cache_state\n' in s)
                collected = next(s for s in steps if 'id: collect_native\n' in s)
                saved = next(
                    s
                    for s in steps
                    if 'uses: actions/cache/save@' in s
                    and 'steps.native_cache.outputs.cache-primary-key' in s
                )

                # A new run or retry needs a writable key; immutable restored snapshots must
                # remain candidates so this change does not discard existing compiled packages.
                key = re.search(r'(?m)^\s+key: (.+)$', restored).group(1)
                self.assertIn('github.run_id', key)
                self.assertIn('github.run_attempt', key)
                self.assertIn('restore-keys: |', restored)
                self.assertIn('native-windows-v2-', restored)
                toolchain = next(
                    s
                    for s in steps
                    if 'id: toolchain\n' in s or 'id: native_toolchain\n' in s
                )
                identity = re.search(r'(?m)^\s+\$identity = (.+)$', toolchain).group(1)
                for value in (
                    '$cmakeVersion',
                    '$powerShellVersion',
                    '$env:ImageVersion',
                ):
                    self.assertIn(value, identity)
                self.assertLess(steps.index(baseline), steps.index(collected))
                digest = "hashFiles('output/obj/*/opt/shared/cache/native/**')"
                self.assertIn(f'digest=${{{{ {digest} }}}}', baseline)

                # Completed packages must survive a failed configure even when restoration
                # succeeded. Save only when collection changes the cached contents.
                condition = re.search(r'(?m)^\s+if: (.+)$', collected).group(1)
                self.assertIn('always()', condition)
                self.assertNotIn('cache-hit', condition)
                condition = re.search(r'(?m)^\s+if: (.+)$', saved).group(1)
                self.assertIn('always()', condition)
                self.assertIn("steps.collect_native.outcome == 'success'", condition)
                self.assertIn(
                    f'{digest} != steps.native_cache_state.outputs.digest', condition
                )

    @unittest.skipUnless(
        os.name == 'nt' and shutil.which('powershell.exe'),
        'Native Windows PowerShell is required',
    )
    def test_native_collection_merges_restored_and_new_binary_packages(self) -> None:
        """Run the workflow's collector twice and retain old and newly completed ABIs."""
        text = (package.ROOT / '.github/workflows/release_windows.yml').read_text()
        step = next(
            s
            for s in re.split(r'(?m)^\s*- name: ', text)[1:]
            if 'id: collect_native\n' in s
        )
        script = '$ErrorActionPreference = "Stop"\n' + textwrap.dedent(
            step.split('        run: |\n', 1)[1]
        )
        script = script.replace('${{ env.OUTPUT_ARCH }}', 'x86_64')
        root = (
            package.ROOT
            / f'output/obj/{output_layout.architecture()}/opt/shared/verifications/native_cache'
        )
        root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=root) as directory:
            workspace = Path(directory)
            checkout = workspace / 'source/lockd-test'
            helper = checkout / 'scripts/release/windows_vcpkg_cache.ps1'
            helper.parent.mkdir(parents=True)
            shutil.copyfile(
                package.ROOT / 'scripts/release/windows_vcpkg_cache.ps1', helper
            )
            cache = workspace / 'output/obj/x86_64/opt/shared/cache/native'
            dependencies = workspace / 'output/obj/x86_64/opt/shared/dependencies'
            restored = cache / 'binary-cache/aa/old.zip'
            restored.parent.mkdir(parents=True)
            restored.write_bytes(b'restored package')
            for name, content in (
                ('binary-cache/aa/old.zip', b'restored package'),
                ('binary-cache/bb/new.zip', b'new ABI package'),
                ('vcpkg-downloads/source.tar.gz', b'completed download'),
            ):
                file = dependencies / name
                file.parent.mkdir(parents=True, exist_ok=True)
                file.write_bytes(content)
            environment = dict(
                os.environ,
                GITHUB_WORKSPACE=str(workspace),
                BUILD_ROOT=str(checkout),
                LOCKD_HOST_BUILD_DIR=str(dependencies.parent),
                VCPKG_ROOT=str(workspace / 'vcpkg'),
                VCPKG_DEFAULT_BINARY_CACHE=str(dependencies / 'binary-cache'),
                VCPKG_DOWNLOADS=str(dependencies / 'vcpkg-downloads'),
            )
            for attempt in range(2):
                with self.subTest(collection=attempt):
                    result = subprocess.run(
                        ['powershell.exe', '-NoProfile', '-Command', script],
                        env=environment,
                        capture_output=True,
                        text=True,
                        check=False,
                        timeout=30,
                    )
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(
                        sorted(
                            p.relative_to(cache).as_posix()
                            for p in cache.rglob('*')
                            if p.is_file()
                        ),
                        [
                            'binary-cache/aa/old.zip',
                            'binary-cache/bb/new.zip',
                            'vcpkg-downloads/source.tar.gz',
                        ],
                    )
                    self.assertEqual(restored.read_bytes(), b'restored package')
                    self.assertEqual(
                        (cache / 'binary-cache/bb/new.zip').read_bytes(),
                        b'new ABI package',
                    )

            # Restore into the defaults too, using a fixture profile so the
            # developer's real caches cannot be touched by this offline test.
            environment.pop('VCPKG_DEFAULT_BINARY_CACHE')
            environment.pop('VCPKG_DOWNLOADS')
            environment['LOCALAPPDATA'] = str(workspace / 'user-local')
            result = subprocess.run(
                [
                    'powershell.exe',
                    '-NoProfile',
                    '-File',
                    str(helper),
                    '-Operation',
                    'Restore',
                    '-CacheDirectory',
                    str(cache),
                ],
                env=environment,
                capture_output=True,
                text=True,
                check=False,
                timeout=30,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(
                (workspace / 'user-local/vcpkg/archives/bb/new.zip').read_bytes(),
                b'new ABI package',
            )
            self.assertEqual(
                (workspace / 'vcpkg/downloads/source.tar.gz').read_bytes(),
                b'completed download',
            )

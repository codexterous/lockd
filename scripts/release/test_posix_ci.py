"""Check compatibility orchestration without installing packages or running builds."""

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

import output_layout

ROOT = Path(__file__).resolve().parents[2]
SHELL = shutil.which('sh')
if SHELL is None and os.name == 'nt':
    git_shell = (
        Path(os.environ.get('ProgramFiles', 'C:/Program Files')) / 'Git/bin/sh.exe'
    )
    if git_shell.is_file():
        SHELL = str(git_shell)


@unittest.skipUnless(SHELL, 'A POSIX shell is needed to exercise the CI scripts')
class PosixCiTests(unittest.TestCase):
    """Verify shared builds and propagation of build and test failures."""

    def setUp(self) -> None:
        """Create a checkout-shaped fixture and recording tools beneath output/."""
        temporary_root = (
            ROOT
            / f'output/obj/{output_layout.architecture()}/opt/shared/verifications/posix_ci'
        )
        temporary_root.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=temporary_root)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        scripts = self.root / 'scripts/release'
        scripts.mkdir(parents=True)
        self.script = scripts / 'test_posix.sh'
        shutil.copyfile(ROOT / 'scripts/release/test_posix.sh', self.script)
        self.bin = self.root / 'bin'
        self.bin.mkdir()
        self.trace = self.root / 'trace'
        for name in ('cmake', 'ctest', 'c++', 'dnf', 'zypper'):
            tool = self.bin / name
            tool.write_text(
                '#!/bin/sh\n'
                'command="$(basename "$0") $*"\n'
                'printf "%s\\n" "$command" >> "$TRACE"\n'
                'if [ -n "${FAIL_MATCH:-}" ]; then\n'
                '    case "$command" in *"$FAIL_MATCH"*) exit 23 ;; esac\n'
                'fi\n',
                encoding='utf-8',
            )
            tool.chmod(0o755)

        uname = self.bin / 'uname'
        uname.write_text('#!/bin/sh\nprintf "%s\\n" "$TEST_ARCH"\n', encoding='utf-8')
        uname.chmod(0o755)

    def run_checks(
        self, failure: str = '', arch: str = 'x86_64'
    ) -> subprocess.CompletedProcess[str]:
        """Run the real orchestration with recording external tool boundaries."""
        environment = os.environ.copy()
        environment.update(
            TRACE=self.trace.as_posix(),
            FAIL_MATCH=failure,
            TEST_ARCH=arch,
            TEST_BIN=self.bin.as_posix(),
            CXX='c++',
            PATH=str(self.bin) + os.pathsep + environment['PATH'],
        )

        # Git for Windows prepends /usr/bin at startup. Put the recording tools
        # first inside the shell too, so uname does not bypass the ARM64 fixture.
        return subprocess.run(
            [
                SHELL,
                '-c',
                'PATH="$(cd "$TEST_BIN" && pwd):$PATH"; export PATH; . "$0"',
                str(self.script),
            ],
            cwd=self.root,
            env=environment,
            text=True,
            capture_output=True,
            check=False,
        )

    def test_shared_release_is_built_twice_without_repeating_analysis(self) -> None:
        """
        Trace the POSIX job with fake tools; require two shared builds and native tests,
        with analysis left to its separate job.
        """
        result = self.run_checks()
        self.assertEqual(result.returncode, 0, result.stderr)
        commands = self.trace.read_text(encoding='utf-8').splitlines()
        build = 'output/obj/x86_64/opt/shared'
        self.assertEqual(commands.count(f'cmake --build {build} --parallel 2'), 2)
        self.assertIn(
            f'ctest --test-dir {build} --output-on-failure '
            '--no-tests=error --parallel 2 -E ^lockd_clang_tidy_',
            commands,
        )
        configure = next(
            line
            for line in commands
            if line.startswith('cmake --preset x86_64-opt-shared ')
        )
        self.assertIn('-DLOCKD_BUILD_PYTHON=OFF', configure)
        self.assertIn('-DLOCKD_STATIC_RUNTIME=OFF', configure)
        self.assertIn('-DLOCKD_CLANG_TIDY_WARNINGS_AS_ERRORS=ON', configure)
        self.assertNotIn('/static', '\n'.join(commands))
        self.assertNotIn('-opt-static', '\n'.join(commands))
        self.assertFalse(any('--target clang_tidy' in line for line in commands))

    def test_configure_build_and_test_failures_stop_the_job(self) -> None:
        """Fail each tool stage in turn; require its exit code and no later commands."""
        for failure in ('--preset', '--parallel 2', 'ctest '):
            with self.subTest(failure=failure):
                self.trace.unlink(missing_ok=True)
                result = self.run_checks(failure)
                self.assertEqual(result.returncode, 23, result.stderr)
                commands = self.trace.read_text(encoding='utf-8').splitlines()
                self.assertIn(failure, commands[-1])

    def test_arm64_uses_native_presets_for_build_and_tests(self) -> None:
        """
        Trace an aarch64 job; require arm64 shared builds and tests without x86 paths
        or a duplicate analysis scan.
        """
        result = self.run_checks(arch='aarch64')
        self.assertEqual(result.returncode, 0, result.stderr)
        commands = self.trace.read_text(encoding='utf-8').splitlines()
        self.assertTrue(
            any(
                line.startswith('cmake --preset arm64-opt-shared ') for line in commands
            )
        )
        build = 'output/obj/arm64/opt/shared'
        self.assertEqual(commands.count(f'cmake --build {build} --parallel 2'), 2)
        self.assertIn(
            f'ctest --test-dir {build} --output-on-failure '
            '--no-tests=error --parallel 2 -E ^lockd_clang_tidy_',
            commands,
        )
        self.assertNotIn('/static', '\n'.join(commands))
        self.assertFalse(any('--target clang_tidy' in line for line in commands))
        self.assertNotIn('x86_64', '\n'.join(commands))

    def test_package_installer_rejects_non_ci_execution(self) -> None:
        """
        Run the installer without the CI marker; require rejection before it can
        change the local package environment.
        """
        environment = os.environ.copy()
        environment.pop('GITHUB_ACTIONS', None)
        result = subprocess.run(
            [SHELL, str(ROOT / 'scripts/release/install_linux_ci.sh'), 'apt'],
            env=environment,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('only for disposable CI containers', result.stderr)

    def test_analysis_requires_full_coverage_and_propagates_failures(self) -> None:
        """The separate gate includes Python and stops before later checks on failure."""
        shutil.copyfile(ROOT / 'scripts/release/analyze_linux.sh', self.script)
        result = self.run_checks()
        self.assertEqual(result.returncode, 0, result.stderr)
        commands = self.trace.read_text(encoding='utf-8').splitlines()
        self.assertEqual(len(commands), 3)
        for option in (
            '-DLOCKD_CLANG_TIDY_EXECUTABLE=/usr/bin/clang-tidy-23',
            '-DBUILD_TESTING=ON',
            '-DLOCKD_BUILD_PYTHON=ON',
            '-DLOCKD_CLANG_TIDY=OFF',
            '-DLOCKD_CLANG_TIDY_WARNINGS_AS_ERRORS=ON',
        ):
            self.assertIn(option, commands[0])
        self.assertEqual(
            commands[1],
            'cmake --build output/obj/x86_64/opt/shared/analysis --target clang_tidy_full',
        )
        self.assertIn('-R ^lockd_clang_tidy_', commands[2])
        for failure in ('cmake -S', '--target clang_tidy_full', 'ctest '):
            with self.subTest(failure=failure):
                self.trace.unlink()
                result = self.run_checks(failure)
                self.assertEqual(result.returncode, 23, result.stderr)
                commands = self.trace.read_text(encoding='utf-8').splitlines()
                self.assertIn(failure, commands[-1])

    def test_opensuse_installs_required_boost_library_development_packages(
        self,
    ) -> None:
        """The base Boost package alone does not provide the component libraries."""
        environment = os.environ.copy()
        environment.update(
            GITHUB_ACTIONS='true',
            TRACE=self.trace.as_posix(),
            PATH=str(self.bin) + os.pathsep + environment['PATH'],
        )
        result = subprocess.run(
            [SHELL, str(ROOT / 'scripts/release/install_linux_ci.sh'), 'zypper'],
            env=environment,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        commands = self.trace.read_text(encoding='utf-8').splitlines()
        self.assertEqual(commands[0], 'zypper --non-interactive refresh')
        for package in (
            'libboost_log-devel',
            'libboost_program_options-devel',
            'libboost_filesystem-devel',
            'libboost_thread-devel',
        ):
            self.assertIn(package, commands[1].split())

    def test_enterprise_packages_enable_crb_without_changing_fedora(self) -> None:
        """Record package transactions without changing this machine's packages."""
        environment = os.environ.copy()
        environment.update(
            GITHUB_ACTIONS='true',
            TRACE=self.trace.as_posix(),
            PATH=str(self.bin) + os.pathsep + environment['PATH'],
        )
        for manager in ('dnf', 'dnf-crb'):
            with self.subTest(manager=manager):
                self.trace.unlink(missing_ok=True)
                result = subprocess.run(
                    [SHELL, str(ROOT / 'scripts/release/install_linux_ci.sh'), manager],
                    env=environment,
                    text=True,
                    capture_output=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                command = self.trace.read_text(encoding='utf-8')
                self.assertIn('ninja-build', command)
                self.assertEqual('--enablerepo=crb' in command, manager == 'dnf-crb')


if __name__ == '__main__':
    unittest.main()

"""Exercise CMake version refresh using local Git repositories, without network access."""

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

import output_layout
import package

CMAKE = os.environ.get('CMAKE_COMMAND') or shutil.which('cmake')
NINJA = os.environ.get('CMAKE_MAKE_PROGRAM') or shutil.which('ninja')


@unittest.skipUnless(CMAKE and NINJA, 'CMake and Ninja are required for version checks')
class CMakeVersionTests(unittest.TestCase):
    """Check assignments, development changes, and offline configuration together."""

    def test_configure_refreshes_versions_without_changing_source(self) -> None:
        """Reloads fetch numbers; clean assigned commits also stay development builds."""
        temporary_root: Path = (
            package.ROOT
            / f'output/obj/{output_layout.architecture()}/opt/shared/verifications'
        )
        temporary_root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=temporary_root) as temporary:
            root: Path = Path(temporary)
            upstream: Path = root / 'upstream'
            checkout: Path = root / 'checkout'
            upstream.mkdir()

            def run(*arguments: str, cwd: Path = upstream) -> str:
                """Run the actual Git or CMake command and retain useful failure output."""
                result = subprocess.run(
                    arguments, cwd=cwd, capture_output=True, text=True, check=False
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                return result.stdout + result.stderr

            def commit(cwd: Path) -> None:
                """Give fixture commits an identity without modifying Git configuration."""
                run('git', 'add', '.', cwd=cwd)
                run(
                    'git',
                    '-c',
                    'core.hooksPath=',
                    '-c',
                    'user.name=Version test',
                    '-c',
                    'user.email=version-test@example.invalid',
                    '-c',
                    'commit.gpgsign=false',
                    'commit',
                    '-m',
                    'fixture',
                    cwd=cwd,
                )

            (upstream / 'cmake').mkdir()
            for name in ('lockd_version.cmake', 'lockd_build_version.hpp.in'):
                shutil.copyfile(
                    package.ROOT / 'cmake' / name, upstream / 'cmake' / name
                )
            (upstream / 'cmake/version.txt').write_text('1.0.1.dev0\n')
            (upstream / 'tracked.txt').write_text('original\n')
            (upstream / '.gitignore').write_text('/build/\n')
            (upstream / 'CMakeLists.txt').write_text(
                'cmake_minimum_required(VERSION 3.24)\n'
                'include(cmake/lockd_version.cmake)\n'
                'project(version_fixture VERSION ${Lockd_VERSION} LANGUAGES NONE)\n'
                'configure_file(cmake/lockd_build_version.hpp.in '
                'generated/lockd_build_version.hpp @ONLY)\n'
                'file(WRITE "${CMAKE_BINARY_DIR}/project_version.txt" "${PROJECT_VERSION}")\n'
                'add_custom_target(check ALL COMMAND "${CMAKE_COMMAND}" -E true)\n'
            )
            run('git', 'init')
            commit(upstream)
            run('git', 'clone', upstream.as_posix(), checkout.as_posix())
            cmake: str = CMAKE
            build: Path = checkout / 'build'

            def configure(expected: str, *options: str) -> str:
                """Check the header and CMake project metadata agree after a reload."""
                output: str = run(
                    cmake,
                    '-S',
                    str(checkout),
                    '-B',
                    str(build),
                    '-G',
                    'Ninja',
                    f'-DCMAKE_MAKE_PROGRAM={NINJA}',
                    *options,
                    cwd=checkout,
                )
                header: str = (build / 'generated/lockd_build_version.hpp').read_text()
                native_version: str = expected.replace('.dev0', '-dev')
                self.assertIn(
                    f'#define LOCKD_VERSION_STRING "{native_version}"', header
                )
                self.assertEqual(
                    (build / 'project_version.txt').read_text(),
                    expected.removesuffix('.dev0'),
                )
                self.assertEqual(
                    (checkout / 'cmake/version.txt').read_text(), '1.0.1.dev0\n'
                )
                return output

            configure('1.0.1.dev0')
            run('git', 'tag', 'build/v1.0.7')
            configure('1.0.7.dev0')
            header: Path = build / 'generated/lockd_build_version.hpp'
            timestamp: int = header.stat().st_mtime_ns
            run(cmake, '--build', str(build), cwd=checkout)
            configure('1.0.7.dev0')
            run(cmake, '--build', str(build), cwd=checkout)
            self.assertEqual(header.stat().st_mtime_ns, timestamp)
            self.assertEqual(run('git', 'status', '--porcelain', cwd=checkout), '')

            # A newer assignment on a different commit must not label this checkout.
            (upstream / 'tracked.txt').write_text('remote change\n')
            commit(upstream)
            run('git', 'tag', 'build/v1.0.8')
            configure('1.0.7.dev0')
            (checkout / 'tracked.txt').write_text('local change\n')
            configure('1.0.7.dev0')
            commit(checkout)
            configure('1.0.7.dev0')

            # Offline configuration retains a fetched assignment without touching source.
            run(
                'git',
                'remote',
                'set-url',
                'origin',
                str(root / 'missing'),
                cwd=checkout,
            )
            output: str = configure('1.0.7.dev0')
            self.assertIn('using local assignments', output)
            output = configure('1.0.7.dev0', '-DLOCKD_REFRESH_VERSION=OFF')
            self.assertNotIn('refresh unavailable', output)

            # The Python backend reads version.txt before invoking CMake.
            configure('1.0.1.dev0', '-DSKBUILD=ON')

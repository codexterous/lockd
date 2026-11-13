"""Check Linux dependency isolation and portable-wheel release gates offline."""

import hashlib
import shlex
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import build_boost_linux
import build_linux_wheel
import output_layout
import package
from test_linux_wheel import check_dependencies


class LinuxPackagingTests(unittest.TestCase):
    """Reject missing or external Boost libraries rather than just testing imports."""

    def test_i686_wheel_tests_use_the_x86_daemon_and_restore_hidden_sdk(self) -> None:
        """Keep the new 32-bit wheel isolated, including when installed-wheel tests fail."""
        work = output_layout.directories(package.ROOT, output_layout.architecture())[
            'verifications'
        ]
        work.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=work) as temporary:
            root = Path(temporary)
            prefix = root / 'boost_python314'
            (prefix / 'lib').mkdir(parents=True)
            for name in ('libboost_log.so', 'libboost_python314.so'):
                (prefix / 'lib' / name).write_bytes(b'fixture')

            def inspect_isolation(distributions: Path, launcher: Path) -> None:
                """Inspect the real staging state at the external test boundary."""
                self.assertEqual(
                    distributions, root / 'lib/pylockd/output/x86/opt/static'
                )
                self.assertFalse(prefix.exists())
                self.assertTrue(prefix.with_name(prefix.name + '_unavailable').is_dir())
                self.assertEqual(
                    shlex.split(launcher.read_text().splitlines()[-1]),
                    ['exec', str(root / 'output/x86/opt/shared/lockd'), '$@'],
                )
                self.assertTrue(
                    (launcher.parent / 'wheel_daemon_libs/libboost_log.so').is_file()
                )
                self.assertFalse(
                    (
                        launcher.parent / 'wheel_daemon_libs/libboost_python314.so'
                    ).exists()
                )
                raise RuntimeError('Wheel test failed')

            with (
                patch.object(package, 'ROOT', root),
                patch.object(
                    package,
                    'interpreter',
                    return_value={
                        'wheel_directory': 'lib/pylockd/output/x86/opt/static',
                    },
                ),
                patch.object(output_layout, 'architecture', return_value='x86'),
                patch.dict(output_layout.os.environ, LOCKD_HOST_BUILD_DIR=''),
                patch.object(
                    build_linux_wheel.test_linux_wheel,
                    'test_wheel',
                    side_effect=inspect_isolation,
                ),
                self.assertRaisesRegex(RuntimeError, 'Wheel test failed'),
            ):
                build_linux_wheel.test_built_wheel(prefix, 'i686')
            self.assertTrue((prefix / 'lib/libboost_python314.so').is_file())
            self.assertFalse(prefix.with_name(prefix.name + '_unavailable').exists())

    def test_compiled_boost_cache_skips_build_commands(self) -> None:
        """An exact cache hit supplies both the binding and test-daemon libraries."""
        temporary_root: Path = (
            package.ROOT
            / f'output/obj/{output_layout.architecture()}/opt/shared/verifications'
        )
        temporary_root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=temporary_root) as temporary:
            prefix: Path = Path(temporary)
            for name in (
                'LICENSE_1_0.txt',
                'include/boost/version.hpp',
                'lib/libboost_python314.so',
                'lib/libboost_log.so',
                'lib/libboost_program_options.so',
                'lib/libboost_filesystem.so',
            ):
                path: Path = prefix / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('cached fixture', encoding='utf-8')
            with (
                patch.dict(
                    build_boost_linux.os.environ, {'LOCKD_BOOST_CACHE_HIT': 'true'}
                ),
                patch.object(build_boost_linux, 'boost_source') as source,
                patch.object(build_boost_linux, 'run') as run,
            ):
                build_boost_linux.build_boost(prefix, '3.14')
                run.assert_not_called()
                source.assert_not_called()
                (prefix / 'lib/libboost_python314.so').unlink()
                with self.assertRaisesRegex(RuntimeError, 'cache is incomplete'):
                    build_boost_linux.build_boost(prefix, '3.14')
                run.assert_not_called()
                source.assert_not_called()

    def test_cache_miss_builds_only_requested_boost_components(self) -> None:
        """A miss still builds the Python binding and the test daemon's dependencies."""
        temporary_root: Path = (
            package.ROOT
            / f'output/obj/{output_layout.architecture()}/opt/shared/verifications'
        )
        temporary_root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=temporary_root) as temporary:
            root: Path = Path(temporary)
            (root / 'LICENSE_1_0.txt').write_text('license fixture', encoding='utf-8')
            (root / 'prefix').mkdir()
            with (
                patch.dict(build_boost_linux.os.environ, {'LOCKD_BOOST_CACHE_HIT': ''}),
                patch.object(build_boost_linux, 'boost_source', return_value=root),
                patch.object(build_boost_linux, 'run') as run,
            ):
                build_boost_linux.build_boost(root / 'prefix', '3.14')
            self.assertEqual(run.call_count, 2)
            components: list[str] = [
                argument
                for argument in run.call_args.args
                if argument.startswith('--with-')
            ]
            self.assertEqual(
                components,
                [
                    '--with-python',
                    '--with-log',
                    '--with-program_options',
                    '--with-filesystem',
                ],
            )

    def test_failed_rebuild_clears_completed_cache_marker(self) -> None:
        """Only a completed installation remains eligible after a rebuild fails."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            prefix = root / 'prefix'
            prefix.mkdir()
            (root / 'LICENSE_1_0.txt').write_text('license', encoding='utf-8')
            marker = root / '.boost-cache-ready'
            with (
                patch.dict(build_boost_linux.os.environ, {'LOCKD_BOOST_CACHE_HIT': ''}),
                patch.object(build_boost_linux, 'boost_source', return_value=root),
                patch.object(build_boost_linux, 'run') as run,
            ):
                build_boost_linux.build_boost(prefix, '3.14')
                self.assertTrue(marker.is_file())
                run.side_effect = RuntimeError('Build failed')
                with self.assertRaisesRegex(RuntimeError, 'Build failed'):
                    build_boost_linux.build_boost(prefix, '3.14')
                self.assertFalse(marker.exists())

    def test_verified_archive_survives_extraction_failure(self) -> None:
        """Save a verified download after extraction fails; reject corrupted retries."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = root / 'boost_1_88_0.tar.gz'
            archive.write_bytes(b'archive fixture')
            marker = root / '.boost-source-verified'
            with (
                patch.object(build_boost_linux, 'DEPENDENCIES', root),
                patch.object(
                    build_boost_linux,
                    'BOOST_HASH',
                    hashlib.sha256(archive.read_bytes()).hexdigest(),
                ),
                patch.object(
                    build_boost_linux.tarfile,
                    'open',
                    side_effect=RuntimeError('Extraction failed'),
                ),
            ):
                with self.assertRaisesRegex(RuntimeError, 'Extraction failed'):
                    build_boost_linux.boost_source()
                self.assertTrue(marker.is_file())
                archive.write_bytes(b'corrupted')
                with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
                    build_boost_linux.boost_source()
                self.assertFalse(marker.exists())

    def test_bundled_boost_and_system_runtime_are_accepted(self) -> None:
        """System libc remains external; Boost must resolve inside the environment."""
        environment: Path = (
            package.ROOT
            / f'output/obj/{output_layout.architecture()}/opt/shared/verifications/wheel_env'
        )
        library: Path = environment / 'lib/pylockd.libs/libboost_python311-hash.so'
        check_dependencies(
            f'libboost_python311-hash.so => {library} (0x123)\n'
            'libc.so.6 => /lib/libc.so.6 (0x456)',
            environment,
        )

    def test_missing_and_external_dependencies_are_rejected(self) -> None:
        """A working system Boost installation must not hide incomplete packaging."""
        environment: Path = (
            package.ROOT
            / f'output/obj/{output_layout.architecture()}/opt/shared/verifications/wheel_env'
        )
        for output in (
            'libboost_python311.so => not found',
            'libboost_python311.so => /usr/local/lib/boost.so (0x123)',
            'libliblockd.so.1 => /src/output/libliblockd.so.1 (0x123)',
            'liblockd.so.1 => /src/output/liblockd.so.1 (0x123)',
            'liblockd_dll.so.1 => /src/output/liblockd_dll.so.1 (0x123)',
        ):
            with self.subTest(output=output), self.assertRaises(RuntimeError):
                check_dependencies(output, environment)

    def test_multiple_manylinux_tags_keep_the_required_baseline(self) -> None:
        """Auditwheel may advertise additional compatible platforms in one filename."""
        name: str = 'pylockd-1.0.0-cp311-cp311-manylinux_2_28_x86_64'
        self.assertEqual(
            package.release_asset_name(name + '.manylinux_2_17_x86_64.whl'),
            name + '.whl',
        )
        self.assertNotEqual(
            package.release_asset_name(
                'pylockd-1.0.0-cp311-cp311-manylinux_2_34_x86_64.whl'
            ),
            name + '.whl',
        )

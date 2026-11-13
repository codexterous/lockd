"""Check architecture names and the wheel paths used by release packaging."""

import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import build_linux_wheel
import output_layout
import package


@patch.dict(os.environ, LOCKD_HOST_BUILD_DIR='')
class OutputLayoutTests(unittest.TestCase):
    """Keep interpreter ABIs separate without replacing the packaging backend."""

    def test_architecture_matches_process_and_normalizes_aliases(self) -> None:
        """A 32-bit interpreter needs x86 outputs even when Windows reports AMD64."""
        for machine, size, expected in (
            ('AMD64', 8, 'x86_64'),
            ('AMD64', 4, 'x86'),
            ('x86_64', 8, 'x86_64'),
            ('i686', 4, 'x86'),
            ('aarch64', 8, 'arm64'),
            ('ARM64', 8, 'arm64'),
        ):
            with self.subTest(machine=machine, size=size):
                self.assertEqual(output_layout.architecture(machine, size), expected)
        with self.assertRaises(ValueError):
            output_layout.architecture('aarch64', 4)
        with self.assertRaises(ValueError):
            output_layout.architecture('unknown', 8)

    def test_extracted_source_uses_the_outer_workspace_for_shared_tools(self) -> None:
        """Avoid nesting dependencies beneath a release source's second output tree."""
        workspace = package.ROOT / 'output/obj/x86_64/opt/shared'
        source = workspace / 'source/lockd-test'
        with patch.dict(os.environ, LOCKD_HOST_BUILD_DIR=str(workspace)):
            paths = output_layout.directories(source, 'x86', 'opt', 'static')
        self.assertEqual(paths['dependencies'], workspace / 'dependencies')
        self.assertEqual(paths['staging'], workspace / 'release')
        self.assertEqual(paths['build'], source / 'output/obj/x86/opt/static')
        self.assertEqual(paths['release'], source / 'output/x86/opt/static')

    def test_wheel_metadata_uses_backend_placeholders_and_component_outputs(
        self,
    ) -> None:
        """Release builds retain the backend's ABI expansion and native arch names."""
        for arch in output_layout.ARCHITECTURES:
            with (
                self.subTest(arch=arch),
                patch.object(package.output_layout, 'architecture', return_value=arch),
                patch.object(
                    package.sysconfig,
                    'get_platform',
                    return_value=package.WINDOWS_WHEEL_PLATFORMS[arch],
                ),
            ):
                details = package.interpreter()
                self.assertEqual(
                    details['build_directory'],
                    'lib/pylockd/output/obj/{wheel_tag}/opt/static/release',
                )
                self.assertEqual(
                    details['wheel_directory'],
                    f'lib/pylockd/output/{arch}/opt/static',
                )

    def test_linux_builder_keeps_raw_wheels_and_backend_caches_separate(self) -> None:
        """Only build inputs use the common work tree; the backend owns its ABI tree."""
        work = output_layout.directories(package.ROOT, output_layout.architecture())[
            'verifications'
        ]
        work.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=work) as temporary:
            root = Path(temporary)
            (root / 'pyproject.toml').write_bytes(
                (package.ROOT / 'pyproject.toml').read_bytes()
            )
            prefix = root / 'boost'
            prefix.mkdir()
            (prefix / 'LICENSE_1_0.txt').write_text('license fixture', encoding='utf-8')

            def run(*arguments: str) -> None:
                """Stand in for external tools and create their raw wheel output."""
                if '--outdir' in arguments:
                    raw = Path(arguments[arguments.index('--outdir') + 1])
                    raw.mkdir(parents=True)
                    (raw / 'pylockd-fixture.whl').touch()

            with (
                patch.object(package, 'ROOT', root),
                patch.object(build_linux_wheel, 'run', side_effect=run) as command,
                patch.dict(build_linux_wheel.os.environ),
            ):
                wheel = build_linux_wheel.build_wheel(prefix, '3.14')
            self.assertTrue(wheel.is_relative_to(root / 'output/obj'))
            arguments = command.call_args.args
            self.assertIn(
                '-Cbuild-dir=lib/pylockd/output/obj/{wheel_tag}/opt/static/release',
                arguments,
            )
            self.assertEqual(
                arguments[arguments.index('--outdir') + 1], str(wheel.parent)
            )

    def test_wheel_uploads_keep_architecture_out_of_the_wildcard(self) -> None:
        """Artifact uploads must stay flat so package collection receives wheel files."""
        for name in ('release_python.yml', 'release_python_windows.yml'):
            text = (package.ROOT / '.github/workflows' / name).read_text()
            paths = [
                line.strip().removeprefix('path: ')
                for line in text.splitlines()
                if 'path:' in line and line.endswith('/*.whl')
            ]
            self.assertEqual(len(paths), 1, name)
            self.assertEqual(paths[0].count('*'), 1, paths[0])
            self.assertIn('/lib/pylockd/output/', paths[0])

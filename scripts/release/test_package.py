"""Check release version gates, source contents, and required distribution assets."""

import gzip
import subprocess
import tarfile
import tempfile
import tomllib
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

import output_layout
import package


class PackageTests(unittest.TestCase):
    """Exercise packaging without a network, native compiler, or GitHub account."""

    def setUp(self) -> None:
        """Keep even temporary test repositories inside the project's output directory."""
        temporary_root: Path = (
            package.ROOT
            / f'output/obj/{output_layout.architecture()}/opt/shared/verifications/release'
        )
        temporary_root.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=temporary_root)
        self.addCleanup(self.temporary.cleanup)
        self.root: Path = Path(self.temporary.name)
        self.write('.gitattributes', '* text=auto eol=lf\n')
        self.write(
            'source/version.cpp',
            '#define MAJOR_VERSION 1\n#define MINOR_VERSION 2\n#define BUILD_NUMBER 3\n',
        )
        self.write('cmake/version.txt', '1.2.3\n')
        self.write('pyproject.toml', '[project]\ndynamic = ["version"]\n')

    def write(self, name: str, content: str) -> None:
        """Create a fixture file with deterministic LF text."""
        path: Path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding='utf-8', newline='\n')

    def git(self, *arguments: str) -> None:
        """Run Git only in the temporary repository, with no hooks or signing."""
        subprocess.run(
            [
                'git',
                '-c',
                'core.hooksPath=',
                '-c',
                'core.autocrlf=false',
                '-c',
                'commit.gpgsign=false',
                '-c',
                'user.name=Packaging test',
                '-c',
                'user.email=test@example.invalid',
                *arguments,
            ],
            cwd=self.root,
            check=True,
            capture_output=True,
        )

    def test_versions_and_tag_gate(self) -> None:
        """All packages share one version; a mismatched release tag fails."""
        self.assertEqual(package.versions(self.root), ('1.2.3', '1.2.3'))
        package.validate_tag('1.2.3', 'v1.2.3')
        package.validate_tag('1.2.3', None)
        for tag in ('v4.5.6', '1.2.3', 'v1.2.3-rc1'):
            with self.assertRaises(ValueError):
                package.validate_tag('1.2.3', tag)

    def test_supported_interpreters(self) -> None:
        """Every release ABI is accepted; other versions and free-threaded ABIs fail."""
        self.assertEqual(
            package.PYTHON_VERSIONS, ('3.11', '3.12', '3.13', '3.14', '3.15')
        )
        with (
            patch.object(package.struct, 'calcsize', return_value=8),
            patch.object(package.sysconfig, 'get_path', return_value='/python/include'),
            patch.object(package.sysconfig, 'get_config_var', return_value=0),
        ):
            for minor in range(11, 16):
                with (
                    self.subTest(minor=minor),
                    patch.object(package.sys, 'version_info', (3, minor)),
                ):
                    self.assertEqual(package.interpreter()['tag'], f'cp3{minor}')
            for version in ((3, 10), (3, 16)):
                with (
                    patch.object(package.sys, 'version_info', version),
                    self.assertRaises(ValueError),
                ):
                    package.interpreter()
            with (
                patch.object(package.sysconfig, 'get_config_var', return_value=1),
                self.assertRaises(ValueError),
            ):
                package.interpreter()
            with (
                patch.object(package.struct, 'calcsize', return_value=16),
                self.assertRaises(ValueError),
            ):
                package.interpreter()

    def test_windows_wheel_abis_have_separate_boost_installations(self) -> None:
        """Keep x64 SDKs reusable while x86 and ARM64 select their own ABI and SDK."""
        for machine, size, platform, architecture, suffix in (
            ('AMD64', 8, 'win-amd64', 'x86_64', ''),
            ('AMD64', 4, 'win32', 'x86', '_x86'),
            ('ARM64', 8, 'win-arm64', 'arm64', '_arm64'),
            ('ARM64', 8, 'win-amd64', 'x86_64', ''),
            ('ARM64', 4, 'win32', 'x86', '_x86'),
        ):
            with (
                self.subTest(architecture=architecture),
                patch.object(package.sys, 'platform', 'win32'),
                patch.object(package.sys, 'version_info', (3, 14)),
                patch.object(package.sysconfig, 'get_config_var', return_value=0),
                patch.object(package.sysconfig, 'get_platform', return_value=platform),
                patch.object(
                    package.sysconfig, 'get_path', return_value='/python/include'
                ),
                patch.object(package.struct, 'calcsize', return_value=size),
                patch.object(output_layout.platform, 'machine', return_value=machine),
            ):
                details = package.interpreter()
                self.assertEqual(details['architecture'], architecture)
                self.assertEqual(details['boost_name'], 'boost_python314' + suffix)
                self.assertEqual(details['wheel_platform'], platform.replace('-', '_'))
                self.assertEqual(
                    details['wheel_directory'],
                    f'lib/pylockd/output/{architecture}/opt/static',
                )

    def test_package_python_support_matches_release_matrix(self) -> None:
        """Package metadata must advertise the same Python versions we release."""
        project = tomllib.loads((package.ROOT / 'pyproject.toml').read_text())[
            'project'
        ]
        self.assertEqual(project['name'], 'pylockd')
        self.assertEqual(package.python_wheel_name(), 'pylockd')
        self.assertEqual(project['requires-python'], '>=3.11,<3.16')
        advertised = {
            value.removeprefix('Programming Language :: Python :: ')
            for value in project['classifiers']
            if value.startswith('Programming Language :: Python :: 3.')
        }
        self.assertEqual(advertised, set(package.PYTHON_VERSIONS))

    def test_wheels_include_the_source_distribution_documentation(self) -> None:
        """Shipping docs in the source archive must also make them available in wheels."""
        settings = tomllib.loads((package.ROOT / 'pyproject.toml').read_text())['tool'][
            'scikit-build'
        ]
        documents = {
            name for name in settings['sdist']['include'] if name.endswith('.md')
        }
        self.assertIn('PYTHON.md', documents)
        self.assertIn('README.md', documents)
        included = settings['wheel']['force-include']
        for name in documents:
            with self.subTest(document=name):
                self.assertTrue((package.ROOT / name).is_file())
                self.assertEqual(
                    included[name], f'${{SKBUILD_DATA_DIR}}/share/doc/pylockd/{name}'
                )

    def test_source_is_committed_and_excludes_generated_files(self) -> None:
        """Even accidentally tracked private/build files must not enter the archive."""
        self.write('include/a file.hpp', '// Committed source\n')
        for name in (
            'output/build.log',
            'lib/pylockd/output/module.pyd',
            '.private/notes.txt',
            'platform/freebsd/dist/old.tar.gz',
            'platform/freebsd/ports/net/lockd/distinfo',
        ):
            self.write(name, 'Excluded\n')
        self.git('init')
        self.git('add', '.')
        self.git('commit', '-m', 'fixture')
        self.write('include/a file.hpp', '// Uncommitted edit\n')
        self.write('untracked.txt', 'Untracked\n')
        with patch.object(
            package.subprocess, 'run', wraps=package.subprocess.run
        ) as export:
            archive: Path = package.source_archive(
                self.root, self.root / 'output/x86_64/opt/static', '1.2.3'
            )
        temporary: Path = Path(
            next(
                argument.removeprefix('--output=')
                for argument in export.call_args.args[0]
                if argument.startswith('--output=')
            )
        )
        self.assertTrue(temporary.is_relative_to(self.root / 'output/obj'))
        self.assertFalse(temporary.exists())
        with tarfile.open(archive) as source:
            files: set[str] = {
                member.name for member in source.getmembers() if member.isfile()
            }
            self.assertEqual(
                files,
                {
                    'lockd-1.2.3/.gitattributes',
                    'lockd-1.2.3/source/version.cpp',
                    'lockd-1.2.3/pyproject.toml',
                    'lockd-1.2.3/cmake/version.txt',
                    'lockd-1.2.3/include/a file.hpp',
                },
            )
            self.assertEqual(
                source.extractfile('lockd-1.2.3/include/a file.hpp').read(),
                b'// Committed source\n',
            )
        original: bytes = archive.read_bytes()
        package.source_archive(self.root, archive.parent, '1.2.3')
        self.assertEqual(archive.read_bytes(), original)
        self.assertTrue(gzip.decompress(original))

    def test_assigned_version_survives_source_extraction(self) -> None:
        """Only version metadata is stamped; rebuilding the archive needs no Git."""
        self.git('init')
        self.git('add', '.')
        self.git('commit', '-m', 'fixture')
        self.write('cmake/version.txt', '9.0.0\n')
        archive: Path = package.source_archive(self.root, self.root / 'output', '1.2.4')
        extracted: Path = self.root / 'output/extracted'
        with tarfile.open(archive) as source:
            source.extractall(extracted, filter='data')
        tree: Path = extracted / 'lockd-1.2.4'
        self.assertEqual(package.versions(tree), ('1.2.4', '1.2.4'))
        self.assertFalse((tree / '.git').exists())
        self.assertEqual((self.root / 'cmake/version.txt').read_text(), '9.0.0\n')
        self.assertEqual(
            (tree / 'source/version.cpp').read_text(),
            (self.root / 'source/version.cpp').read_text(),
        )

    def test_windows_names_and_licenses(self) -> None:
        """The portable ZIP contains stable executable names and both license notices."""
        self.write('lib/pylockd/license.txt', 'MIT')
        for architecture, triplet_arch in (
            ('x86', 'x86'),
            ('x86_64', 'x64'),
            ('arm64', 'arm64'),
        ):
            with self.subTest(architecture=architecture):
                self.write(f'output/{architecture}/opt/static/lockd.exe', architecture)
                self.write(
                    f'utility/lockd_test/output/{architecture}/opt/static/'
                    'lockd_test.exe',
                    'tests',
                )
                installed = self.root / 'user installation outside build trees'
                triplet = f'{triplet_arch}-windows-static'
                self.write(
                    f'output/obj/{architecture}/opt/static/CMakeCache.txt',
                    f'VCPKG_INSTALLED_DIR:PATH={installed.as_posix()}\n'
                    f'VCPKG_TARGET_TRIPLET:STRING={triplet}\n',
                )
                self.write(
                    f'user installation outside build trees/{triplet}/'
                    'share/boost-asio/copyright',
                    'Boost',
                )
                archive = package.windows_archive(
                    self.root, self.root / 'output', '1.2.3', architecture
                )
                self.assertEqual(
                    archive.name, f'lockd-1.2.3-windows-{architecture}-portable.zip'
                )
                with zipfile.ZipFile(archive) as files:
                    self.assertEqual(
                        set(files.namelist()),
                        {
                            'lockd.exe',
                            'lockd_test.exe',
                            'license.txt',
                            'boost_license.txt',
                        },
                    )
                    self.assertEqual(files.read('lockd.exe').decode(), architecture)
                    self.assertEqual(files.read('boost_license.txt').decode(), 'Boost')

    def test_missing_or_extra_assets_prevent_checksums(self) -> None:
        """Ignore the checkout placeholder, preserving complete and unique asset checks."""
        destination: Path = self.root / 'output/x86_64/opt/static'
        destination.mkdir(parents=True)
        (destination / '.gitkeep').touch()
        with self.assertRaises(ValueError):
            package.checksums(destination, '1.2.3', '4.5.6')
        names = ['lockd-1.2.3.tar.gz']
        for arch in ('x86', 'x86_64', 'arm64'):
            names.extend(
                (
                    f'lockd-1.2.3-windows-{arch}-portable.zip',
                    f'lockd-1.2.3-windows-{arch}-setup.exe',
                )
            )
        names.extend(
            f'pylockd-4.5.6-cp3{minor}-cp3{minor}-{platform}.whl'
            for minor in range(11, 16)
            for platform in ('win32', 'win_amd64', 'win_arm64')
        )
        names.extend(
            f'pylockd-4.5.6-cp3{minor}-cp3{minor}-manylinux_2_28_{arch}.whl'
            for minor in range(11, 16)
            for arch in ('x86_64', 'i686', 'aarch64')
        )
        for name in names:
            (destination / name).write_bytes(b'fixture')
        package.checksums(destination, '1.2.3', '4.5.6')
        manifest: Path = destination / 'SHA256SUMS'
        contents: str = manifest.read_text()
        self.assertEqual(
            [line.split('  ', 1)[1] for line in contents.splitlines()], sorted(names)
        )
        self.assertTrue((destination / '.gitkeep').is_file())
        package.checksums(destination, '1.2.3', '4.5.6')
        self.assertEqual(manifest.read_text(), contents)
        for name in names:
            with self.subTest(missing=name):
                (destination / name).unlink()
                with self.assertRaises(ValueError):
                    package.checksums(destination, '1.2.3', '4.5.6')
                (destination / name).write_bytes(b'fixture')
        duplicate: Path = destination / (
            'pylockd-4.5.6-cp311-cp311-manylinux_2_28_x86_64.manylinux_2_17_x86_64.whl'
        )
        duplicate.touch()
        with self.assertRaises(ValueError):
            package.checksums(destination, '1.2.3', '4.5.6')
        duplicate.unlink()
        for name in ('unexpected.whl', '.unexpected'):
            with self.subTest(extra=name):
                extra: Path = destination / name
                extra.touch()
                with self.assertRaises(ValueError):
                    package.checksums(destination, '1.2.3', '4.5.6')
                extra.unlink()


if __name__ == '__main__':
    unittest.main()

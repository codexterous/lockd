"""Check native wheel dependency selection through the production CMake helper."""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import output_layout
import package

CMAKE = os.environ.get('CMAKE_COMMAND') or shutil.which('cmake')


@unittest.skipUnless(
    os.name == 'nt' and CMAKE and shutil.which('powershell.exe'),
    'Windows CMake and PowerShell are required',
)
class PythonPackageTests(unittest.TestCase):
    """Keep cached SDKs reusable without reconciling the native vcpkg installation."""

    def setUp(self) -> None:
        """Replace the external SDK builder and wheel frontend with offline recorders."""
        work = (
            output_layout.directories(package.ROOT, output_layout.architecture())[
                'verifications'
            ]
            / 'python_package'
        )
        work.mkdir(parents=True, exist_ok=True)
        temporary = tempfile.TemporaryDirectory(dir=work)
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        modules = self.root / 'cmake'
        modules.mkdir()
        for name in ('lockd_python_package.cmake', 'lockd_output_layout.cmake'):
            shutil.copyfile(package.ROOT / 'cmake' / name, modules / name)
        frontend = self.root / 'build'
        frontend.mkdir()
        (frontend / '__init__.py').touch()
        (frontend / '__main__.py').write_text(
            'import json, os, sys\n'
            'from pathlib import Path\n'
            "Path(os.environ['WHEEL_TRACE']).write_text(json.dumps({\n"
            "'executable': sys.executable,\n"
            "'arguments': sys.argv[1:], 'generator': os.environ.get('CMAKE_GENERATOR')}))\n",
            encoding='utf-8',
        )
        self.native = self.root / 'output/obj/x86_64/opt/shared'
        self.native.mkdir(parents=True)
        suffix = f'{sys.version_info.major}{sys.version_info.minor}'
        self.sdk = self.native / f'dependencies/release/boost_python{suffix}'
        scripts = self.root / 'scripts/release'
        scripts.mkdir(parents=True)
        (scripts / 'build_boost_python.ps1').write_text(
            'param([string] $Python)\n'
            '$ErrorActionPreference = "Stop"\n'
            'Add-Content -LiteralPath $env:SDK_BUILD_TRACE -Value $Python\n'
            'foreach ($name in @("include/boost-1_88/boost/version.hpp",\n'
            f'    "lib/boost_python{suffix}-fixture.lib", '
            f'"lib/boost_python{suffix}-fixture.dll")) {{\n'
            '    $file = Join-Path $env:SDK_PREFIX $name\n'
            '    New-Item -ItemType Directory -Force (Split-Path -Parent $file) | Out-Null\n'
            '    Set-Content -LiteralPath $file -Value "SDK fixture"\n'
            '}\n',
            encoding='utf-8',
        )
        self.native_python = self.root / f'native-sdk/boost_python{suffix}'
        self.cache = {
            'CMAKE_HOME_DIRECTORY': self.root.as_posix(),
            'CMAKE_GENERATOR': 'Ninja',
            'CMAKE_MAKE_PROGRAM': (self.root / 'native-ninja.exe').as_posix(),
            'CMAKE_TOOLCHAIN_FILE': (self.root / 'native-vcpkg.cmake').as_posix(),
            'CMAKE_C_COMPILER': (self.root / 'native-cl.exe').as_posix(),
            'CMAKE_CXX_COMPILER': (self.root / 'native-cl.exe').as_posix(),
            'LOCKD_TARGET_ARCH': 'x86_64',
            'LOCKD_STATIC_RUNTIME': 'OFF',
            'Boost_INCLUDE_DIR': (self.root / 'native-boost/include').as_posix(),
            f'boost_python{suffix}_DIR': self.native_python.as_posix(),
        }
        self.trace = self.root / 'frontend.json'
        self.builds = self.root / 'sdk-builds.txt'

    def _configure(self, directory: Path, settings: dict[str, str]) -> None:
        """Create a native cache without configuring or installing any real packages."""
        directory.mkdir(parents=True, exist_ok=True)
        (directory / 'CMakeCache.txt').write_text(
            ''.join(f'{key}:STRING={value}\n' for key, value in settings.items()),
            encoding='utf-8',
        )

    def _run(
        self, directory: Path, *, interpreter: str | None = sys.executable
    ) -> subprocess.CompletedProcess[str]:
        """Run production selection while the external frontend only records its inputs."""
        arguments = [CMAKE, f'-DLOCKD_PACKAGE_BUILD_DIR={directory}']
        if interpreter is not None:
            arguments.append(f'-DLOCKD_PACKAGE_PYTHON={interpreter}')
        arguments += ['-P', str(self.root / 'cmake/lockd_python_package.cmake')]
        return subprocess.run(
            arguments,
            env=dict(
                os.environ,
                LOCKD_HOST_BUILD_DIR=str(self.native),
                WHEEL_TRACE=str(self.trace),
                SDK_BUILD_TRACE=str(self.builds),
                SDK_PREFIX=str(self.sdk),
                CMAKE_GENERATOR='NMake Makefiles',
            ),
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )

    def _virtual_environment(self) -> Path:
        """Create an offline interpreter fixture with no installed frontend packages."""
        subprocess.run(
            [sys.executable, '-m', 'venv', '--without-pip', str(self.root / '.venv')],
            capture_output=True,
            text=True,
            check=True,
            timeout=30,
        )
        return self.root / '.venv/Scripts/python.exe'

    def test_build_only_defaults_to_project_virtual_environment(self) -> None:
        """Use .venv without shell activation so IDE builds find its frontend tools."""
        interpreter = self._virtual_environment()
        self._configure(self.native, self.cache)
        result = self._run(self.native, interpreter=None)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        trace = json.loads(self.trace.read_text())
        self.assertEqual(Path(trace['executable']), interpreter)
        self.assertEqual(Path(self.builds.read_text().strip()), interpreter)

    def test_explicit_interpreter_overrides_project_virtual_environment(self) -> None:
        """Keep build-only wheels selectable for another interpreter despite .venv."""
        self._virtual_environment()
        self._configure(self.native, self.cache)
        result = self._run(self.native)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(
            Path(json.loads(self.trace.read_text())['executable']), Path(sys.executable)
        )

    def test_missing_python_sdk_is_built_once_and_reused(self) -> None:
        """Build the missing interpreter SDK, then reuse it with matching Ninja settings."""
        self._configure(self.native, self.cache)
        previous = None
        for _ in range(2):
            result = self._run(self.native)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            trace = json.loads(self.trace.read_text())
            arguments = trace['arguments']
            self.assertEqual(trace['generator'], 'Ninja')
            self.assertIn(f'-Ccmake.define.Boost_ROOT={self.sdk.as_posix()}', arguments)
            self.assertIn('-Ccmake.define.Boost_NO_BOOST_CMAKE=ON', arguments)
            self.assertIn(
                f'-Ccmake.define.CMAKE_MAKE_PROGRAM={self.cache["CMAKE_MAKE_PROGRAM"]}',
                arguments,
            )
            self.assertFalse(any('CMAKE_TOOLCHAIN_FILE=' in item for item in arguments))
            self.assertFalse(any('Boost_INCLUDE_DIR=' in item for item in arguments))
            self.assertEqual(self.builds.read_text().splitlines(), [sys.executable])
            if previous is not None:
                self.assertEqual(arguments, previous)
            previous = arguments

    def test_existing_native_python_sdk_remains_read_only(self) -> None:
        """Reuse the native SDK and retain its manifest/classic selection for wheels."""
        self.native_python.mkdir(parents=True)
        for mode in ('ON', 'OFF'):
            with self.subTest(mode=mode):
                self._configure(self.native, self.cache | {'VCPKG_MANIFEST_MODE': mode})
                result = self._run(self.native)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                arguments = json.loads(self.trace.read_text())['arguments']
                self.assertIn('-Ccmake.define.VCPKG_MANIFEST_INSTALL=OFF', arguments)
                self.assertIn(f'-Ccmake.define.VCPKG_MANIFEST_MODE={mode}', arguments)
                self.assertIn(
                    '-Ccmake.define.CMAKE_TOOLCHAIN_FILE='
                    + self.cache['CMAKE_TOOLCHAIN_FILE'],
                    arguments,
                )
                self.assertFalse(self.builds.exists())

    def test_static_profile_uses_matching_shared_python_sdk(self) -> None:
        """A static runtime must borrow shared-runtime dependencies for the extension."""
        self.native_python.mkdir(parents=True)
        self._configure(self.native, self.cache)
        static = self.native.parent / 'static'
        self._configure(
            static,
            self.cache
            | {'LOCKD_STATIC_RUNTIME': 'ON', 'CMAKE_GENERATOR': 'NMake Makefiles'},
        )
        result = self._run(static)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(json.loads(self.trace.read_text())['generator'], 'Ninja')
        self.assertFalse(self.builds.exists())

    def test_architecture_mismatch_fails_before_building_dependencies(self) -> None:
        """Reject an x86 profile with a 64-bit interpreter before any external build."""
        self._configure(self.native, self.cache | {'LOCKD_TARGET_ARCH': 'x86'})
        result = self._run(self.native)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Use a matching native profile', result.stdout + result.stderr)
        self.assertFalse(self.builds.exists())
        self.assertFalse(self.trace.exists())

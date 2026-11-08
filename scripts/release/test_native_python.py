"""Check optional native Python discovery without installing or compiling Boost."""

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import output_layout
import package

CMAKE = os.environ.get('CMAKE_COMMAND') or shutil.which('cmake')
NINJA = shutil.which('ninja')


@unittest.skipUnless(CMAKE and NINJA, 'CMake and Ninja are required')
class NativePythonTests(unittest.TestCase):
    """Use the cached package only with matching native headers and build variants."""

    def setUp(self) -> None:
        """Provide external Boost targets while running the real discovery helper."""
        work = output_layout.directories(package.ROOT, output_layout.architecture())[
            'verifications'
        ]
        work.mkdir(parents=True, exist_ok=True)
        temporary = tempfile.TemporaryDirectory(dir=work)
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        modules = self.root / 'cmake'
        modules.mkdir()
        for name in ('lockd_boost.cmake', 'lockd_output_layout.cmake'):
            shutil.copyfile(package.ROOT / 'cmake' / name, modules / name)
        self.native = self.root / 'native/boost/version.hpp'
        self.native.parent.mkdir(parents=True)
        self.native.write_text('#define BOOST_VERSION 108800\n')
        self.prefix = (
            self.root
            / f'output/obj/{output_layout.architecture()}/opt/shared'
            / 'dependencies/release/boost_python314'
        )
        header = self.prefix / 'include/boost-1_88/boost/version.hpp'
        header.parent.mkdir(parents=True)
        header.write_text('#define BOOST_VERSION 108800\n')
        config = self.prefix / 'lib/cmake/boost_python-1.88.0'
        config.mkdir(parents=True)
        (config / 'boost_python-config-version.cmake').write_text(
            'set(PACKAGE_VERSION 1.88.0)\n'
            'set(PACKAGE_VERSION_EXACT TRUE)\n'
            'set(PACKAGE_VERSION_COMPATIBLE TRUE)\n'
        )
        (config / 'boost_python-config.cmake').write_text(
            'add_library(Boost::python SHARED IMPORTED GLOBAL)\n'
            'set_target_properties(Boost::python PROPERTIES\n'
            '  IMPORTED_IMPLIB_RELEASE "${CMAKE_CURRENT_LIST_DIR}/../../python.lib"\n'
            '  IMPORTED_IMPLIB_DEBUG "${CMAKE_CURRENT_LIST_DIR}/../../python-d.lib")\n'
        )
        for name in ('python.lib', 'python-d.lib'):
            (self.prefix / 'lib' / name).touch()
        (self.root / 'CMakeLists.txt').write_text(
            'cmake_minimum_required(VERSION 3.24)\n'
            'project(native_python NONE)\n'
            'set(WIN32 TRUE)\nset(MSVC TRUE)\n'
            'set(Python3_VERSION_MAJOR 3)\nset(Python3_VERSION_MINOR 14)\n'
            'set(Lockd_BOOST_PYTHON_COMPONENT python314)\n'
            'add_library(Boost::headers INTERFACE IMPORTED GLOBAL)\n'
            'set_target_properties(Boost::headers PROPERTIES\n'
            '  INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/native")\n'
            'include(cmake/lockd_boost.cmake)\n'
            'lockd_probe_cached_python(found)\n'
            'if (NOT "${found}" STREQUAL "${EXPECTED}")\n'
            '  message(FATAL_ERROR "Unexpected Python discovery: ${found}")\n'
            'endif()\n'
            'if (found AND NOT TARGET Boost::python314)\n'
            '  message(FATAL_ERROR "Missing interpreter-specific target")\n'
            'endif()\n'
            'get_target_property(headers Boost::headers INTERFACE_INCLUDE_DIRECTORIES)\n'
            'if (NOT headers STREQUAL "${CMAKE_CURRENT_SOURCE_DIR}/native")\n'
            '  message(FATAL_ERROR "Native Boost headers changed")\n'
            'endif()\n'
        )

    def _configure(
        self, expected: bool, config: str = 'Release', arch: str = 'x86_64'
    ) -> None:
        """Check discovery with a fresh target namespace for every scenario."""
        with patch.dict(os.environ, LOCKD_HOST_BUILD_DIR=''):
            result = subprocess.run(
                [
                    CMAKE,
                    '-G',
                    'Ninja',
                    f'-DCMAKE_MAKE_PROGRAM={NINJA}',
                    '-S',
                    str(self.root),
                    '-B',
                    str(self.root / f'build-{config}-{arch}'),
                    f'-DEXPECTED={"TRUE" if expected else "FALSE"}',
                    f'-DCMAKE_BUILD_TYPE={config}',
                    f'-DLockd_ARCH={arch}',
                ],
                check=False,
                capture_output=True,
                text=True,
                timeout=30,
            )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_matching_sdk_supports_both_configurations(self) -> None:
        """Import the real package contract without replacing native header targets."""
        self._configure(True)
        self._configure(True, 'Debug')

    def test_different_boost_version_is_rejected(self) -> None:
        """A directory name alone cannot make mismatched Boost headers acceptable."""
        (self.prefix / 'include/boost-1_88/boost/version.hpp').write_text(
            '#define BOOST_VERSION 108200\n'
        )
        self._configure(False)

    def test_missing_selected_library_is_rejected(self) -> None:
        """A release-only SDK must not silently supply a Debug native build."""
        (self.prefix / 'lib/python-d.lib').unlink()
        self._configure(True)
        self._configure(False, 'Debug')

    def test_other_architecture_cannot_import_x64_sdk(self) -> None:
        """An x86 profile must not link the Windows x64 wheel dependency cache."""
        self._configure(False, arch='x86')

"""Check vcpkg selection and user settings without installing any packages."""

import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import output_layout

ROOT = Path(__file__).resolve().parents[2]
CMAKE = os.environ.get('CMAKE_COMMAND') or shutil.which('cmake')
NINJA = os.environ.get('CMAKE_MAKE_PROGRAM') or shutil.which('ninja')


@unittest.skipUnless(
    CMAKE and NINJA, 'CMake and Ninja are needed to test the toolchain'
)
@patch.dict(os.environ, LOCKD_HOST_BUILD_DIR='')
class ToolchainTests(unittest.TestCase):
    def test_install_options_preserve_cmake_and_environment_precedence(self) -> None:
        """User install arguments must reach vcpkg without changing project defaults."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            driver = root / 'options.cmake'
            driver.write_text(
                'set(CMAKE_HOST_WIN32 TRUE)\n'
                'function(cmake_host_system_information)\n'
                '  cmake_parse_arguments(reg "" "RESULT;QUERY;VALUE;VIEW;ERROR_VARIABLE" "" ${ARGN})\n'
                '  if(NOT reg_VALUE STREQUAL "VCPKG_INSTALL_OPTIONS")\n'
                '    message(FATAL_ERROR "Unexpected registry value")\n'
                '  endif()\n'
                '  if(reg_UNPARSED_ARGUMENTS STREQUAL "HKCU/Environment")\n'
                '    set(value "$ENV{LOCKD_TEST_USER_OPTIONS}")\n'
                '  else()\n'
                '    set(value "$ENV{LOCKD_TEST_SYSTEM_OPTIONS}")\n'
                '  endif()\n'
                '  set(${reg_RESULT} "${value}" PARENT_SCOPE)\n'
                '  set(${reg_ERROR_VARIABLE} "" PARENT_SCOPE)\n'
                'endfunction()\n'
                f'include("{(ROOT / "cmake/lockd_vcpkg_settings.cmake").as_posix()}")\n'
                'lockd_select_vcpkg_install_options(options)\n'
                'file(WRITE "${CMAKE_CURRENT_LIST_DIR}/options.txt" "${options}")\n',
                encoding='utf-8',
            )
            user = '--x-buildtrees-root=X:/user build;--no-downloads'
            system = '--x-buildtrees-root=X:/system'
            process = '--x-buildtrees-root=X:/process'
            cases = (
                (None, None, '', '', ''),
                (None, None, user, system, user),
                (None, None, '', system, system),
                (None, process, user, system, process),
                ('--no-downloads', process, user, system, '--no-downloads'),
                ('', process, user, system, ''),
            )
            for cmake, current, saved, machine, expected in cases:
                with self.subTest(cmake=cmake, process=current, user=saved):
                    environment = dict(
                        os.environ,
                        LOCKD_TEST_USER_OPTIONS=saved,
                        LOCKD_TEST_SYSTEM_OPTIONS=machine,
                    )
                    environment.pop('VCPKG_INSTALL_OPTIONS', None)
                    if current is not None:
                        environment['VCPKG_INSTALL_OPTIONS'] = current
                    result = subprocess.run(
                        [
                            CMAKE,
                            *(
                                [f'-DVCPKG_INSTALL_OPTIONS={cmake}']
                                if cmake is not None
                                else []
                            ),
                            '-P',
                            str(driver),
                        ],
                        env=environment,
                        capture_output=True,
                        text=True,
                        check=False,
                        timeout=30,
                    )
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual((root / 'options.txt').read_text(), expected)

    def test_windows_root_selection_preserves_saved_overrides(self) -> None:
        """A developer shell's bundled default must not replace Windows user settings."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            bundled = root / 'visual-studio/VC/vcpkg'
            configured = root / 'user-vcpkg'
            system = root / 'system-vcpkg'
            process = root / 'process-vcpkg'
            explicit = root / 'explicit-vcpkg'
            for directory in (bundled, configured, system, process, explicit):
                directory.mkdir(parents=True)
            driver = root / 'select.cmake'
            driver.write_text(
                'set(CMAKE_HOST_WIN32 TRUE)\n'
                # Only the registry read is replaced. Run the production root
                # selector, including path comparison and precedence decisions.
                'function(cmake_host_system_information)\n'
                '  cmake_parse_arguments(reg "" "RESULT;QUERY;VALUE;VIEW;ERROR_VARIABLE" "" ${ARGN})\n'
                '  if(NOT reg_QUERY STREQUAL "WINDOWS_REGISTRY" OR\n'
                '      NOT reg_VALUE STREQUAL "VCPKG_ROOT" OR NOT reg_VIEW STREQUAL "HOST")\n'
                '    message(FATAL_ERROR "Unexpected registry request: ${ARGN}")\n'
                '  endif()\n'
                '  if(reg_UNPARSED_ARGUMENTS STREQUAL "HKCU/Environment")\n'
                '    set(value "$ENV{LOCKD_TEST_USER_ROOT}")\n'
                '  elseif(reg_UNPARSED_ARGUMENTS STREQUAL\n'
                '      "HKLM/SYSTEM/CurrentControlSet/Control/Session Manager/Environment")\n'
                '    set(value "$ENV{LOCKD_TEST_SYSTEM_ROOT}")\n'
                '  else()\n'
                '    message(FATAL_ERROR "Unexpected registry key")\n'
                '  endif()\n'
                '  set(${reg_RESULT} "${value}" PARENT_SCOPE)\n'
                '  set(${reg_ERROR_VARIABLE} "" PARENT_SCOPE)\n'
                'endfunction()\n'
                f'include("{(ROOT / "cmake/lockd_vcpkg_settings.cmake").as_posix()}")\n'
                'lockd_select_vcpkg_root(selected)\n'
                'file(WRITE "${CMAKE_CURRENT_LIST_DIR}/selected.txt" "${selected}")\n',
                encoding='utf-8',
            )
            scenarios = (
                ('user', '', bundled, configured, system, configured),
                ('system', '', bundled, '', system, system),
                ('bundled', '', bundled, '', '', bundled),
                ('process', '', process, configured, system, process),
                ('cmake', explicit, bundled, configured, system, explicit),
                ('new-environment', '', '', configured, system, configured),
            )
            for name, selected, current, user, machine, expected in scenarios:
                with self.subTest(name=name):
                    result = subprocess.run(
                        [
                            CMAKE,
                            f'-DLOCKD_VCPKG_ROOT={selected}',
                            '-P',
                            str(driver),
                        ],
                        env=dict(
                            os.environ,
                            VCPKG_ROOT=str(current),
                            VSINSTALLDIR=str(root / 'visual-studio'),
                            LOCKD_TEST_USER_ROOT=str(user),
                            LOCKD_TEST_SYSTEM_ROOT=str(machine),
                        ),
                        capture_output=True,
                        text=True,
                        check=False,
                        timeout=30,
                    )
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(
                        Path((root / 'selected.txt').read_text()), expected
                    )

    @unittest.skipUnless(os.name == 'nt', 'Windows selects vcpkg automatically')
    def test_native_configuration_preserves_automatic_installation_and_user_mode(
        self,
    ) -> None:
        """Default configuration installs missing dependencies and respects an explicit mode."""
        work = output_layout.directories(ROOT, output_layout.architecture())[
            'verifications'
        ]
        work.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=work) as temporary:
            root = Path(temporary)
            modules = root / 'cmake'
            modules.mkdir()
            for name in (
                'lockd_architecture.cmake',
                'lockd_build_options.cmake',
                'lockd_output_layout.cmake',
                'lockd_toolchain.cmake',
                'lockd_vcpkg_settings.cmake',
            ):
                shutil.copyfile(ROOT / 'cmake' / name, modules / name)
            checkout = root / 'user-vcpkg'
            backend = checkout / 'scripts/buildsystems/vcpkg.cmake'
            backend.parent.mkdir(parents=True)
            # Replace only the package manager. Leave its automatic installation
            # default enabled and verify that explicit user choices survive the
            # production wrapper. No real vcpkg is run.
            backend.write_text(
                'option(VCPKG_MANIFEST_MODE "Use manifests" ON)\n'
                'option(VCPKG_MANIFEST_INSTALL "Install dependencies" ON)\n'
                'if(VCPKG_MANIFEST_MODE AND VCPKG_MANIFEST_INSTALL)\n'
                '  file(WRITE "${CMAKE_BINARY_DIR}/installation-enabled" "yes")\n'
                'endif()\n',
                encoding='utf-8',
            )
            (root / 'CMakeLists.txt').write_text(
                'cmake_minimum_required(VERSION 3.24)\n'
                'include(cmake/lockd_build_options.cmake NO_POLICY_SCOPE)\n'
                'project(native_dependencies NONE)\n',
                encoding='utf-8',
            )
            for mode in ('', 'ON', 'OFF'):
                with self.subTest(mode=mode):
                    build = root / ('build-' + (mode or 'default'))
                    for attempt in range(2):
                        result = subprocess.run(
                            [
                                CMAKE,
                                '-S',
                                str(root),
                                '-B',
                                str(build),
                                '-G',
                                'Ninja',
                                f'-DCMAKE_MAKE_PROGRAM={NINJA}',
                                *([f'-DVCPKG_MANIFEST_MODE={mode}'] if mode else []),
                            ],
                            capture_output=True,
                            text=True,
                            check=False,
                            timeout=30,
                            env=dict(os.environ, VCPKG_ROOT=str(checkout)),
                        )
                        self.assertEqual(
                            result.returncode, 0, result.stdout + result.stderr
                        )
                        self.assertEqual(
                            (build / 'installation-enabled').exists(), mode != 'OFF'
                        )
                        cache = (build / 'CMakeCache.txt').read_text()
                        self.assertIn('LOCKD_DEPENDENCY_PROVIDER:STRING=AUTO', cache)
                        self.assertIn(f'VCPKG_MANIFEST_MODE:BOOL={mode or "ON"}', cache)
                        self.assertNotIn('VCPKG_INSTALLED_DIR:', cache)
                        self.assertNotIn('LOCKD_VCPKG_ROOT:', cache)

    def test_multi_configuration_generator_fails_before_dependency_setup(self) -> None:
        """Reject a mixed object tree before cloning or building dependencies."""
        work = output_layout.directories(ROOT, output_layout.architecture())[
            'verifications'
        ]
        work.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=work) as temporary:
            root = Path(temporary)
            (root / 'CMakeLists.txt').write_text(
                'cmake_minimum_required(VERSION 3.24)\n'
                f'include("{(ROOT / "cmake/lockd_build_options.cmake").as_posix()}")\n'
                'project(layout_rejection NONE)\n'
                'message(FATAL_ERROR "Dependency_setup_reached")\n',
                encoding='utf-8',
            )
            result = subprocess.run(
                [
                    CMAKE,
                    '-S',
                    str(root),
                    '-B',
                    str(root / 'build'),
                    '-G',
                    'Ninja Multi-Config',
                    f'-DCMAKE_MAKE_PROGRAM={NINJA}',
                ],
                capture_output=True,
                text=True,
                check=False,
                timeout=30,
            )
            output = result.stdout + result.stderr
            self.assertNotEqual(result.returncode, 0, output)
            self.assertIn('Use a single-configuration generator', output)
            self.assertNotIn('Dependency_setup_reached', output)

    def test_analysis_fixtures_share_the_outer_host_tree(self) -> None:
        """Nested release profiles need short, separate compiler-probe directories."""
        work = output_layout.directories(ROOT, output_layout.architecture())[
            'verifications'
        ]
        work.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=work) as temporary:
            workspace = Path(temporary)
            host = workspace / 'output/obj/x86_64/opt/shared'
            source = host / 'source/lockd-1.0.35'
            modules = source / 'cmake'
            modules.mkdir(parents=True)
            for name in ('lockd_clang_tidy.cmake', 'lockd_output_layout.cmake'):
                shutil.copyfile(ROOT / 'cmake' / name, modules / name)
            (source / 'CMakeLists.txt').write_text(
                'cmake_minimum_required(VERSION 3.24)\n'
                'project(analysis_paths NONE)\n'
                'enable_testing()\n'
                'set(BUILD_TESTING ON)\n'
                'include(cmake/lockd_clang_tidy.cmake)\n'
                'lockd_configure_clang_tidy()\n'
            )
            selected: set[Path] = set()
            for profile in ('static', 'shared'):
                build = source / 'output/obj/x86_64/opt' / profile
                result = subprocess.run(
                    [
                        CMAKE,
                        '-S',
                        str(source),
                        '-B',
                        str(build),
                        '-G',
                        'Ninja',
                        f'-DCMAKE_MAKE_PROGRAM={NINJA}',
                        '-DCMAKE_BUILD_TYPE=Release',
                        f'-DLOCKD_CLANG_TIDY_EXECUTABLE={CMAKE}',
                    ],
                    env=dict(os.environ, LOCKD_HOST_BUILD_DIR=str(host)),
                    capture_output=True,
                    text=True,
                    check=False,
                    timeout=30,
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                tests = (build / 'CTestTestfile.cmake').read_text()
                directories = re.findall(r'-DLOCKD_TIDY_TEST_DIR=([^"\n]+)', tests)
                self.assertEqual(len(directories), 2, tests)
                self.assertEqual(directories[0], directories[1])
                directory = Path(directories[0])
                self.assertEqual(directory.parent, host / 'verifications/clang_tidy')
                self.assertFalse(directory.is_relative_to(source))
                selected.add(directory)
            self.assertEqual(
                len(selected), 2, 'Profiles must not share mutable fixtures'
            )

    def test_cmake_and_python_share_dependencies_outside_extracted_sources(
        self,
    ) -> None:
        """Run the real CMake layout selector from a nested source tree on both runs."""
        work = output_layout.directories(ROOT, output_layout.architecture())[
            'verifications'
        ]
        work.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=work) as temporary:
            workspace = Path(temporary) / 'output/obj/x86_64/opt/shared'
            source = workspace / 'source/lockd-test'
            (source / 'cmake').mkdir(parents=True)
            shutil.copyfile(
                ROOT / 'cmake/lockd_output_layout.cmake',
                source / 'cmake/lockd_output_layout.cmake',
            )
            driver = source / 'layout.cmake'
            driver.write_text(
                'include("${CMAKE_CURRENT_LIST_DIR}/cmake/lockd_output_layout.cmake")\n'
                'file(WRITE "${CMAKE_CURRENT_LIST_DIR}/selected.txt" '
                '"${Lockd_HOST_BUILD_DIR}")\n'
            )
            for shared in (False, True):
                environment = dict(os.environ)
                environment.pop('LOCKD_HOST_BUILD_DIR', None)
                if shared:
                    environment['LOCKD_HOST_BUILD_DIR'] = str(workspace)
                for attempt in range(2):
                    with self.subTest(shared=shared, attempt=attempt):
                        subprocess.run(
                            [CMAKE, '-P', str(driver)],
                            env=environment,
                            check=True,
                            capture_output=True,
                            timeout=30,
                        )
                        with patch.dict(os.environ, environment, clear=True):
                            expected = output_layout.directories(source, 'x86')[
                                'dependencies'
                            ]
                        self.assertEqual(
                            Path((source / 'selected.txt').read_text()), expected.parent
                        )

    def test_vcpkg_preserves_user_storage_and_mode_settings(self) -> None:
        """Use the selected checkout without replacing caches or installed packages."""
        work = output_layout.directories(ROOT, output_layout.architecture())[
            'verifications'
        ]
        work.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=work) as temporary:
            root = Path(temporary)
            checkout = root / 'user-vcpkg'
            backend = checkout / 'scripts/buildsystems/vcpkg.cmake'
            backend.parent.mkdir(parents=True)
            settings = (
                'VCPKG_ROOT',
                'VCPKG_DOWNLOADS',
                'VCPKG_DEFAULT_BINARY_CACHE',
                'VCPKG_BINARY_SOURCES',
                'X_VCPKG_REGISTRIES_CACHE',
                'VCPKG_DISABLE_METRICS',
            )
            backend.write_text(
                '\n'.join(
                    f'file(APPEND "${{CMAKE_BINARY_DIR}}/selected.txt" '
                    f'"{name}=$ENV{{{name}}}\\n")'
                    for name in settings
                )
                + '\nfile(APPEND "${CMAKE_BINARY_DIR}/selected.txt" '
                '"mode=${VCPKG_MANIFEST_MODE}\\n'
                'installed=${VCPKG_INSTALLED_DIR}\\n'
                'options=${VCPKG_INSTALL_OPTIONS}\\n'
                'triplet=${VCPKG_TARGET_TRIPLET}\\n")\n'
            )
            driver = root / 'driver.cmake'
            driver.write_text(
                f'include("{(ROOT / "cmake/lockd_toolchain.cmake").as_posix()}")\n'
            )
            environment = dict(os.environ)
            environment.update({name: f'user-{name}' for name in settings})
            environment['VCPKG_ROOT'] = str(checkout)
            environment['VCPKG_INSTALL_OPTIONS'] = '--x-buildtrees-root=user-builds'
            for mode in ('ON', 'OFF', ''):
                with self.subTest(mode=mode):
                    if not mode:
                        for name in settings:
                            environment.pop(name, None)
                        environment['VCPKG_ROOT'] = str(checkout)
                    result = subprocess.run(
                        [
                            CMAKE,
                            *([f'-DVCPKG_MANIFEST_MODE={mode}'] if mode else []),
                            '-DVCPKG_INSTALLED_DIR=user-installation',
                            '-DVCPKG_TARGET_TRIPLET=user-triplet',
                            '-P',
                            str(driver),
                        ],
                        cwd=root,
                        env=environment,
                        capture_output=True,
                        text=True,
                        check=False,
                        timeout=30,
                    )
                    self.assertEqual(result.returncode, 0, result.stderr)
                    selected = (root / 'selected.txt').read_text().splitlines()
                    for name in settings:
                        self.assertIn(f'{name}={environment.get(name, "")}', selected)
                    self.assertIn(f'mode={mode}', selected)
                    self.assertIn('installed=user-installation', selected)
                    self.assertIn('options=--x-buildtrees-root=user-builds', selected)
                    self.assertIn('triplet=user-triplet', selected)
                    (root / 'selected.txt').unlink()
            self.assertFalse((root / 'output').exists())

    def test_missing_vcpkg_reports_setup_without_cloning(self) -> None:
        """Missing prerequisites must not silently install another package manager."""
        work = output_layout.directories(ROOT, output_layout.architecture())[
            'verifications'
        ]
        work.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=work) as temporary:
            root = Path(temporary)
            driver = root / 'driver.cmake'
            driver.write_text(
                f'include("{(ROOT / "cmake/lockd_toolchain.cmake").as_posix()}")\n'
            )
            result = subprocess.run(
                [
                    CMAKE,
                    f'-DLOCKD_VCPKG_ROOT={root / "not-installed"}',
                    '-P',
                    str(driver),
                ],
                cwd=root,
                capture_output=True,
                text=True,
                check=False,
                timeout=30,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('Select an existing vcpkg installation', result.stderr)
            self.assertFalse((root / 'not-installed').exists())
            self.assertFalse((root / 'output').exists())

    @unittest.skipUnless(
        shutil.which('cl') or shutil.which('c++') or shutil.which('clang++'),
        'A C++ compiler is needed to test the UUID probe',
    )
    def test_uuid_failure_reports_compiler_output_and_disabled_installation(
        self,
    ) -> None:
        """Expose probe errors and explain when a cached flag blocks vcpkg installs."""
        temporary_root = (
            ROOT
            / f'output/obj/{output_layout.architecture()}/opt/shared/verifications/toolchain'
        )
        temporary_root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=temporary_root) as directory:
            root = Path(directory)
            headers = root / 'headers/boost/uuid'
            headers.mkdir(parents=True)

            # Fail inside a selected package header even when the system also has
            # working Boost headers. The real CMake probe must show that failure.
            (headers / 'random_generator.hpp').write_text(
                '#error Broken_UUID_fixture\n', encoding='utf-8'
            )
            (headers / 'uuid.hpp').write_text('', encoding='utf-8')
            (root / 'CMakeLists.txt').write_text(
                'cmake_minimum_required(VERSION 3.24)\n'
                'project(uuid_probe CXX)\n'
                'add_library(Boost::headers INTERFACE IMPORTED)\n'
                'set_target_properties(Boost::headers PROPERTIES '
                'INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/headers")\n'
                f'include("{(ROOT / "cmake/lockd_boost.cmake").as_posix()}")\n'
                'lockd_check_uuid()\n',
                encoding='utf-8',
            )
            for mode, install in (('ON', 'OFF'), ('ON', 'ON'), ('OFF', 'OFF')):
                with self.subTest(mode=mode, install=install):
                    result = subprocess.run(
                        [
                            CMAKE,
                            '-S',
                            str(root),
                            '-B',
                            str(root / 'build'),
                            '-G',
                            'Ninja',
                            f'-DCMAKE_MAKE_PROGRAM={NINJA}',
                            f'-DVCPKG_MANIFEST_MODE={mode}',
                            f'-DVCPKG_MANIFEST_INSTALL={install}',
                        ],
                        capture_output=True,
                        text=True,
                        check=False,
                        timeout=60,
                    )
                    output = result.stdout + result.stderr
                    self.assertNotEqual(result.returncode, 0, output)
                    self.assertIn('Broken_UUID_fixture', output)
                    self.assertEqual(
                        'VCPKG_MANIFEST_INSTALL=OFF' in output,
                        mode == 'ON' and install == 'OFF',
                        output,
                    )


if __name__ == '__main__':
    unittest.main()

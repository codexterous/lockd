"""Compile C++17 exception source paths in sources, headers, and installed SDKs."""

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

import output_layout

ROOT = Path(__file__).resolve().parents[2]
CMAKE = os.environ.get('CMAKE_COMMAND') or shutil.which('cmake')
NINJA = os.environ.get('CMAKE_MAKE_PROGRAM') or shutil.which('ninja')
COMPILER = shutil.which('cl' if os.name == 'nt' else 'c++')


@unittest.skipUnless(CMAKE and NINJA and COMPILER, 'CMake, Ninja, and C++ are required')
class SourcePathTests(unittest.TestCase):
    """Exercise real header throws, multiple translation units, and unchanged rebuilds."""

    def test_source_paths_follow_the_throwing_header_in_sources_and_sdks(self) -> None:
        """Header and source throws retain their own names across source and SDK builds."""
        work = (
            output_layout.directories(ROOT, output_layout.architecture())[
                'verifications'
            ]
            / 'source_paths'
        )
        work.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(
            prefix='checkout with spaces \u798f ', dir=work
        ) as tmp:
            source = Path(tmp) / 'source'
            build = Path(tmp) / 'build'
            include = source / 'lib/liblockd/include'
            sdk = Path(tmp) / 'sdk/include'
            for directory in (include, sdk):
                (directory / 'liblockd/detail').mkdir(parents=True)
                for name in ('exceptions.hpp', 'detail/source_path.hpp'):
                    original = ROOT / 'lib/liblockd/include/liblockd' / name
                    (directory / 'liblockd' / name).write_bytes(original.read_bytes())
            (source / 'nested').mkdir()
            (source / 'include').mkdir()
            (source / 'nested/CMakeLists.txt').write_text(
                'add_library(paths STATIC extra.cpp)\n'
                f'target_include_directories(paths PRIVATE "{include.as_posix()}"\n'
                '    "${PROJECT_SOURCE_DIR}/include")\n'
                'target_compile_definitions(paths PRIVATE\n'
                '    LOCKD_SOURCE_ROOT=\\"${PROJECT_SOURCE_DIR}/\\")\n',
                encoding='utf-8',
            )
            header = (
                '#include <liblockd/detail/source_path.hpp>\n'
                '#include <liblockd/exceptions.hpp>\n'
                'inline const char* header_source() { return LOCKD_SOURCE_FILE; }\n'
                'template<class T> void throw_from_header() {\n'
                '    throw lld::exception(lld::exception::et_logic_error,\n'
                '        LOCKD_SOURCE_FILE, __LINE__, __FUNCTION__, "header");\n'
                '}\n'
            )
            (source / 'include/thrower.hpp').write_text(header, encoding='utf-8')
            (sdk / 'liblockd/example.hpp').write_text(header, encoding='utf-8')
            (source / 'nested/extra.cpp').write_text(
                '#include <thrower.hpp>\n'
                'const char* cpp_source() { return LOCKD_SOURCE_FILE; }\n'
                'const char* second_header_source() { return header_source(); }\n'
                'void second_header_throw() { throw_from_header<double>(); }\n',
                encoding='utf-8',
            )
            (source / 'main.cpp').write_text(
                '#include <filesystem>\n'
                '#include <string>\n'
                '#include <string_view>\n'
                '#include <thrower.hpp>\n'
                'static_assert(std::string_view(lld::detail::relative_source_path(\n'
                '    "C:/checkout/include/test.hpp", "C:/checkout/")) == "include/test.hpp");\n'
                'static_assert(std::string_view(lld::detail::relative_source_path(\n'
                '    "C:/checkout-other/test.hpp", "C:/checkout/")) == "test.hpp");\n'
                'static_assert(std::string_view(lld::detail::relative_source_path(\n'
                '    "C:/checkout/source/../include/test.hpp", "C:/checkout/")) ==\n'
                '    "source/../include/test.hpp");\n'
                'std::string portable(const char* path) {\n'
                '    return std::filesystem::path(path).generic_string();\n'
                '}\n'
                'const char* cpp_source();\n'
                'const char* second_header_source();\n'
                'void second_header_throw();\n'
                'int main() {\n'
                '    if (portable(cpp_source()) != "nested/extra.cpp" ||\n'
                '        portable(LOCKD_SOURCE_FILE) != "main.cpp" ||\n'
                '        portable(header_source()) != "include/thrower.hpp" ||\n'
                '        portable(second_header_source()) != "include/thrower.hpp") return 1;\n'
                '    try { throw_from_header<int>(); return 5; }\n'
                '    catch (const lld::exception& error) {\n'
                '        if (portable(error.file().c_str()) != "include/thrower.hpp") return 2;\n'
                '    }\n'
                '    try { second_header_throw(); }\n'
                '    catch (const lld::exception& error) {\n'
                '        return portable(error.file().c_str()) == "include/thrower.hpp" ? 0 : 3;\n'
                '    }\n'
                '    return 4;\n'
                '}\n',
                encoding='utf-8',
            )
            (source / 'sdk_main.cpp').write_text(
                '#include <liblockd/example.hpp>\n'
                'int main() {\n'
                '    try { throw_from_header<int>(); }\n'
                '    catch (const lld::exception& error) {\n'
                '        return error.file() == "example.hpp" ? 0 : 1;\n'
                '    }\n'
                '    return 2;\n'
                '}\n',
                encoding='utf-8',
            )
            (source / 'constant_check.cpp').write_text(
                '#include <liblockd/detail/source_path.hpp>\n'
                'const char* source_name() { return LOCKD_SOURCE_FILE; }\n'
                'int main() { return source_name()[0] == 0; }\n',
                encoding='utf-8',
            )
            (source / 'CMakeLists.txt').write_text(
                'cmake_minimum_required(VERSION 3.24)\n'
                'project(source_paths LANGUAGES CXX)\n'
                'set(CMAKE_CXX_STANDARD 17)\n'
                'set(CMAKE_CXX_STANDARD_REQUIRED ON)\n'
                'set(CMAKE_CXX_EXTENSIONS OFF)\n'
                'if(MSVC)\n'
                '    add_compile_options(/utf-8 /WX)\n'
                'endif()\n'
                'add_subdirectory(nested "${CMAKE_BINARY_DIR}/separate objects")\n'
                'add_executable(check main.cpp)\n'
                f'target_include_directories(check PRIVATE "{include.as_posix()}"\n'
                '    "${PROJECT_SOURCE_DIR}/include")\n'
                'target_compile_definitions(check PRIVATE\n'
                '    LOCKD_SOURCE_ROOT=\\"${PROJECT_SOURCE_DIR}/\\")\n'
                'target_link_libraries(check PRIVATE paths)\n'
                'add_executable(sdk_check sdk_main.cpp)\n'
                f'target_include_directories(sdk_check PRIVATE "{sdk.as_posix()}")\n'
                'add_executable(constant_check constant_check.cpp)\n'
                f'target_include_directories(constant_check PRIVATE "{include.as_posix()}")\n',
                encoding='utf-8',
            )

            def run(*arguments: str) -> None:
                """Bound each compiler command and expose its diagnostics on failure."""
                result = subprocess.run(
                    arguments,
                    capture_output=True,
                    text=True,
                    encoding='utf-8',
                    errors='replace',
                    timeout=120,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

            configure = (
                CMAKE,
                '-S',
                str(source),
                '-B',
                str(build),
                '-G',
                'Ninja',
                f'-DCMAKE_MAKE_PROGRAM={NINJA}',
                '-DCMAKE_TOOLCHAIN_FILE=',
            )
            executable = build / ('check.exe' if os.name == 'nt' else 'check')
            sdk_executable = build / (
                'sdk_check.exe' if os.name == 'nt' else 'sdk_check'
            )
            for configuration in ('Debug', 'Release'):
                with self.subTest(configuration=configuration):
                    settings = (*configure, f'-DCMAKE_BUILD_TYPE={configuration}')
                    run(*settings)
                    run(CMAKE, '--build', str(build))
                    run(str(executable))
                    run(str(sdk_executable))
                    objects = {
                        path: path.stat().st_mtime_ns
                        for path in build.rglob('*')
                        if path.suffix in ('.obj', '.o')
                        and path.name.startswith(
                            (
                                'main.cpp.',
                                'sdk_main.cpp.',
                                'extra.cpp.',
                                'constant_check.cpp.',
                            )
                        )
                    }
                    self.assertEqual(len(objects), 4)
                    run(*settings)
                    run(CMAKE, '--build', str(build))
                    run(str(executable))
                    run(str(sdk_executable))
                    self.assertEqual(
                        objects, {path: path.stat().st_mtime_ns for path in objects}
                    )

            # The production macro must require constant evaluation, independently of
            # optimizer settings. Removing constexpr from the copied helper must fail;
            # restoring it must let the same target compile again, regardless of how
            # the compiler words its diagnostic.
            copied = include / 'liblockd/detail/source_path.hpp'
            original = copied.read_text(encoding='utf-8')
            changed = original.replace(
                'constexpr const char* relative_source_path(',
                'const char* relative_source_path(',
            )
            self.assertNotEqual(original, changed)
            for configuration in ('Debug', 'Release'):
                with self.subTest(runtime_helper=configuration):
                    run(*configure, f'-DCMAKE_BUILD_TYPE={configuration}')
                    copied.write_text(changed, encoding='utf-8')
                    failed = subprocess.run(
                        (CMAKE, '--build', str(build), '--target', 'constant_check'),
                        capture_output=True,
                        text=True,
                        encoding='utf-8',
                        errors='replace',
                        timeout=120,
                        check=False,
                    )
                    self.assertNotEqual(
                        failed.returncode,
                        0,
                        "A runtime-only filename helper must fail the constexpr initializer",
                    )
                    copied.write_text(original, encoding='utf-8')
                    run(CMAKE, '--build', str(build), '--target', 'constant_check')

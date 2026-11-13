# Native build contracts run through CTest, independently of Python release tooling.
if (NOT DEFINED Lockd_BUILD_TEST_SOURCE)
    set(Lockd_BUILD_TEST_SOURCE "${PROJECT_SOURCE_DIR}")
endif ()
include("${CMAKE_CURRENT_LIST_DIR}/lockd_output_layout.cmake")
string(SHA256 Lockd_BUILD_TEST_ID "${PROJECT_BINARY_DIR}")
string(SUBSTRING "${Lockd_BUILD_TEST_ID}" 0 12 Lockd_BUILD_TEST_ID)
set(Lockd_BUILD_TEST_ROOT
        "${Lockd_HOST_BUILD_DIR}/verifications/build/${Lockd_BUILD_TEST_ID}")

function(lockd_set_build_test_properties name)
    set_tests_properties("${name}" PROPERTIES TIMEOUT 300 LABELS native_build)
    if (MSVC)
        # A compiler path alone cannot reproduce the selected MSVC toolchain. CTest may
        # start outside the developer environment used by CMake, including from an IDE.
        # Preserve only the tool/include/library search paths, not unrelated environment data.
        set(environment)
        foreach(variable IN ITEMS INCLUDE LIB LIBPATH PATH)
            string(REPLACE ";" "\\;" value "$ENV{${variable}}")
            list(APPEND environment "${variable}=${value}")
        endforeach ()
        set_property(TEST "${name}" PROPERTY ENVIRONMENT "${environment}")
    endif ()
endfunction()

function(lockd_add_build_test name script)
    set(command "${CMAKE_COMMAND}"
            "-DLOCKD_TEST_SOURCE=${Lockd_BUILD_TEST_SOURCE}"
            "-DLOCKD_TEST_ROOT=${Lockd_BUILD_TEST_ROOT}"
            "-DLOCKD_TEST_WORK=${Lockd_BUILD_TEST_ROOT}/${name}"
            "-DLOCKD_TEST_GENERATOR=${CMAKE_GENERATOR}"
            "-DLOCKD_TEST_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
            "-DLOCKD_TEST_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
            "-DLOCKD_TEST_CXX_COMPILER_ID=${CMAKE_CXX_COMPILER_ID}"
            "-DLOCKD_TEST_HOST_ARCH=${Lockd_HOST_ARCH}"
            -P "${Lockd_BUILD_TEST_SOURCE}/cmake/tests/${script}")
    if (MSVC AND name STREQUAL "lockd_source_paths")
        # /showIncludes uses the console output code page, independently of /utf-8.
        # Give this Unicode-path fixture its own UTF-8 console so dependency paths
        # survive both IDE launches and shell launches without changing their consoles.
        find_program(powershell NAMES powershell.exe REQUIRED)
        set(invocation "&")
        foreach(argument IN LISTS command)
            string(REPLACE "'" "''" argument "${argument}")
            string(APPEND invocation " '${argument}'")
        endforeach ()
        set(launcher "${Lockd_BUILD_TEST_ROOT}/${name}.ps1")
        # Windows PowerShell needs a BOM to read UTF-8 paths in a script file.
        string(ASCII 239 187 191 utf8_bom)
        file(GENERATE OUTPUT "${launcher}" CONTENT "${utf8_bom}${invocation}\nexit $LASTEXITCODE\n")
        set(command "${powershell}" -NoLogo -NoProfile -NonInteractive
                -ExecutionPolicy Bypass -File
                "${Lockd_BUILD_TEST_SOURCE}/cmake/tests/windows/utf8_console.ps1"
                -Log "${Lockd_BUILD_TEST_ROOT}/${name}.log" -Script "${launcher}")
    endif ()
    add_test(NAME "${name}" COMMAND ${command})
    lockd_set_build_test_properties("${name}")
endfunction()

lockd_add_build_test(lockd_source_paths source_paths_test.cmake)
lockd_add_build_test(lockd_version_fields version_test.cmake)
lockd_add_build_test(lockd_vcpkg_settings vcpkg_settings_test.cmake)
lockd_add_build_test(lockd_toolchain toolchain_test.cmake)
lockd_add_build_test(lockd_python_sdk python_sdk_test.cmake)
lockd_add_build_test(lockd_native_presets native_presets_test.cmake)
if (EXISTS "${Lockd_BUILD_TEST_SOURCE}/.github/workflows/release_windows.yml")
    lockd_add_build_test(lockd_native_cache_workflow native_cache_workflow_test.cmake)
    lockd_add_build_test(lockd_workflows workflow_test.cmake)
endif ()
if (UNIX)
    lockd_add_build_test(lockd_posix_ci posix_ci_test.cmake)
endif ()
find_package(Git QUIET)
if (Git_FOUND)
    lockd_add_build_test(lockd_version_refresh version_refresh_test.cmake)
endif ()
if (MSVC)
    lockd_add_build_test(lockd_build_environment build_environment_test.cmake)
endif ()

if (WIN32)
    find_program(Lockd_TEST_POWERSHELL powershell.exe)
    if (Lockd_TEST_POWERSHELL)
        foreach(script IN ITEMS installer windows_analysis windows_signing native_cache)
            set(name "lockd_${script}")
            add_test(NAME "${name}" COMMAND "${CMAKE_COMMAND}"
                    "-DLOCKD_TEST_SOURCE=${Lockd_BUILD_TEST_SOURCE}"
                    "-DLOCKD_TEST_ROOT=${Lockd_BUILD_TEST_ROOT}"
                    "-DLOCKD_TEST_WORK=${Lockd_BUILD_TEST_ROOT}/${name}"
                    "-DLOCKD_TEST_POWERSHELL=${Lockd_TEST_POWERSHELL}"
                    "-DLOCKD_TEST_SCRIPT=${script}"
                    -P "${Lockd_BUILD_TEST_SOURCE}/cmake/tests/windows_test.cmake")
            lockd_set_build_test_properties("${name}")
        endforeach ()
        find_package(Python3 3.11...<3.16 QUIET COMPONENTS Interpreter)
        if (Python3_Interpreter_FOUND)
            execute_process(COMMAND "${Python3_EXECUTABLE}" -c
                    "import platform,struct; print(platform.machine().lower(),struct.calcsize('P'))"
                    OUTPUT_VARIABLE python_arch OUTPUT_STRIP_TRAILING_WHITESPACE
                    RESULT_VARIABLE python_result)
            # These fixtures model the Windows x64 wheel SDK fallback. Python remains
            # optional for native builds and other interpreter architectures skip this case.
            if (python_result STREQUAL "0" AND python_arch MATCHES "^(amd64|x86_64) 8$")
                add_test(NAME lockd_python_package_windows_x64 COMMAND "${CMAKE_COMMAND}"
                        "-DLOCKD_TEST_SOURCE=${Lockd_BUILD_TEST_SOURCE}"
                        "-DLOCKD_TEST_ROOT=${Lockd_BUILD_TEST_ROOT}"
                        "-DLOCKD_TEST_WORK=${Lockd_BUILD_TEST_ROOT}/python_package_windows"
                        "-DLOCKD_TEST_PYTHON=${Python3_EXECUTABLE}"
                        -P "${Lockd_BUILD_TEST_SOURCE}/cmake/tests/python_package_windows_test.cmake")
                lockd_set_build_test_properties(lockd_python_package_windows_x64)
            endif ()
        endif ()
    endif ()
endif ()

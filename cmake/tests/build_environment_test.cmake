cmake_minimum_required(VERSION 3.24)
include("${CMAKE_CURRENT_LIST_DIR}/test_helpers.cmake")
lockd_test_prepare()

# Configure the real build tests with this toolchain, then invoke their CTest definitions
# without a developer shell. Use a separate build tree so concurrent outer tests keep their files.
set(build "${LOCKD_TEST_WORK}/build")
lockd_test_run(COMMAND "${CMAKE_COMMAND}" -S "${LOCKD_TEST_SOURCE}/cmake/tests" -B "${build}"
        -G "${LOCKD_TEST_GENERATOR}" "-DCMAKE_MAKE_PROGRAM=${LOCKD_TEST_MAKE_PROGRAM}"
        "-DCMAKE_CXX_COMPILER=${LOCKD_TEST_CXX_COMPILER}")
get_filename_component(cmake_directory "${CMAKE_COMMAND}" DIRECTORY)
find_program(ctest NAMES ctest PATHS "${cmake_directory}" NO_DEFAULT_PATH REQUIRED)
lockd_test_run(COMMAND "${CMAKE_COMMAND}" -E env
        --unset=INCLUDE --unset=LIB --unset=LIBPATH --unset=PATH
        "${ctest}" --test-dir "${build}" --output-on-failure --no-tests=error
        -R "^lockd_(source_paths|toolchain)$" --parallel 2)

# A console launcher must preserve failure status and diagnostics, otherwise a failed
# compiler fixture could appear to pass. Exercise its process boundary independently.
set(failure "${LOCKD_TEST_WORK}/expected_failure.ps1")
file(WRITE "${failure}" "Write-Output 'console-launcher-failure'\nexit 37\n")
find_program(powershell NAMES powershell.exe REQUIRED)
lockd_test_run(FAILS RESULT result OUTPUT output COMMAND "${powershell}"
        -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File
        "${LOCKD_TEST_SOURCE}/cmake/tests/windows/utf8_console.ps1"
        -Script "${failure}" -Log "${LOCKD_TEST_WORK}/expected_failure.log")
lockd_test_equal("${result}" "37")
lockd_test_contains("${output}" "console-launcher-failure")
lockd_test_run(COMMAND "${powershell}" -NoLogo -NoProfile -NonInteractive
        -ExecutionPolicy Bypass -File
        "${LOCKD_TEST_SOURCE}/cmake/tests/windows/console_lifetime.ps1"
        -Repository "${LOCKD_TEST_SOURCE}" -WorkDirectory "${LOCKD_TEST_WORK}/console_lifetime")

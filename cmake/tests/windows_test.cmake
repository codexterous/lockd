cmake_minimum_required(VERSION 3.24)
include("${CMAKE_CURRENT_LIST_DIR}/test_helpers.cmake")
lockd_test_prepare()
lockd_test_run(TIMEOUT 270 COMMAND "${LOCKD_TEST_POWERSHELL}" -NoProfile -NonInteractive
        -ExecutionPolicy Bypass -File "${LOCKD_TEST_SOURCE}/cmake/tests/windows/${LOCKD_TEST_SCRIPT}.ps1"
        -Repository "${LOCKD_TEST_SOURCE}" -WorkDirectory "${LOCKD_TEST_WORK}")

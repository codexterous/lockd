cmake_minimum_required(VERSION 3.24)

# A repeated lib prefix changes both linker filenames and the ABI-versioned runtime names.
if (NOT LOCKD_CLIENT_NAME STREQUAL LOCKD_EXPECTED_CLIENT OR
        NOT LOCKD_C_NAME STREQUAL LOCKD_EXPECTED_C)
    message(FATAL_ERROR
            "Unexpected client library filenames: ${LOCKD_CLIENT_NAME}, ${LOCKD_C_NAME}; "
            "expected ${LOCKD_EXPECTED_CLIENT}, ${LOCKD_EXPECTED_C}")
endif ()

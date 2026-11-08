cmake_minimum_required(VERSION 3.24)

# Build a deliberately lint-invalid component through the production module. Debug must reject
# it in strict mode, including targets declared in subdirectories. Release must compile it,
# while an explicit Release scan still rejects it.
set(fixture "${LOCKD_TIDY_TEST_DIR}/build fixture")
file(MAKE_DIRECTORY "${fixture}/component")
file(WRITE "${fixture}/component/CMakeLists.txt"
        "add_library(fixture OBJECT ../warning.cpp)\n")
file(WRITE "${fixture}/warning.cpp" [=[
#include "event_log_messages.h"
#if defined(__clang__)
# if defined(_MSVC_LANG)
static_assert(_MSVC_LANG >= 201703L);
# else
static_assert(__cplusplus >= 201703L);
# endif
#endif
int* pointer() { return 0; }
]=])
file(WRITE "${fixture}/.clang-tidy" "Checks: '-*,modernize-use-nullptr'\nWarningsAsErrors: ''\n")
file(WRITE "${fixture}/message_header.in" "// Generated message declarations.\n")
get_filename_component(module "${CMAKE_CURRENT_LIST_DIR}/../lockd_clang_tidy.cmake" ABSOLUTE)
set(project [=[
cmake_minimum_required(VERSION 3.24)
project(clang_tidy_build_test LANGUAGES CXX)
set(BUILD_TESTING OFF)
include("@module@")
add_subdirectory(component)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/event_log_messages.h"
        COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_SOURCE_DIR}/message_header.in"
                "${CMAKE_CURRENT_BINARY_DIR}/event_log_messages.h"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/message_header.in" VERBATIM)
add_custom_target(lockd_event_resources DEPENDS
        "${CMAKE_CURRENT_BINARY_DIR}/event_log_messages.h")
add_dependencies(fixture lockd_event_resources)
target_include_directories(fixture PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_features(fixture PRIVATE cxx_std_17)
lockd_configure_clang_tidy()
]=])
string(CONFIGURE "${project}" project @ONLY)
file(WRITE "${fixture}/CMakeLists.txt" "${project}")

foreach(configuration IN ITEMS Release Debug)
    if (configuration STREQUAL "Debug")
        set(profile dbg)
    else ()
        set(profile opt)
    endif ()
    set(build "${fixture}/${profile}")

    # CTest can use a different compiler path or generator than a previous fixture run.
    # A fresh fixture cache keeps CMake from resetting the supplied probe settings while
    # switching compilers. Only this standalone test project's cache is replaced.
    execute_process(COMMAND "${CMAKE_COMMAND}" --fresh -S "${fixture}" -B "${build}"
            -G "${LOCKD_TIDY_TEST_GENERATOR}"
            "-DCMAKE_MAKE_PROGRAM=${LOCKD_TIDY_TEST_MAKE_PROGRAM}"
            "-DCMAKE_CXX_COMPILER=${LOCKD_TIDY_TEST_COMPILER}"
            -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY
            "-DCMAKE_BUILD_TYPE=${configuration}"
            "-DLOCKD_CLANG_TIDY_EXECUTABLE=${LOCKD_TIDY_TEST_EXECUTABLE}"
            -DLOCKD_CLANG_TIDY=ON -DLOCKD_CLANG_TIDY_WARNINGS_AS_ERRORS=ON
            RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
    if (NOT result STREQUAL "0")
        message(FATAL_ERROR "${configuration} fixture configuration failed:\n${output}${errors}")
    endif ()

    # Force a compilation even when this regression test has run previously.
    file(TOUCH "${fixture}/warning.cpp")
    execute_process(COMMAND "${CMAKE_COMMAND}" --build "${build}" --config "${configuration}"
            --target fixture RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
    if (configuration STREQUAL "Debug")
        if ("${output}${errors}" MATCHES "static assertion failed")
            message(FATAL_ERROR "Debug analysis did not use C++17:\n${output}${errors}")
        endif ()
        if (result STREQUAL "0" OR NOT "${output}${errors}" MATCHES "modernize-use-nullptr")
            message(FATAL_ERROR "Debug did not enforce clang-tidy:\n${output}${errors}")
        endif ()
    else ()
        if (NOT result STREQUAL "0" OR "${output}${errors}" MATCHES "modernize-use-nullptr")
            message(FATAL_ERROR "Release ran automatic clang-tidy or failed:\n${output}${errors}")
        endif ()
        execute_process(COMMAND "${CMAKE_COMMAND}" --build "${build}" --config Release
                --target fixture RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
        if (NOT result STREQUAL "0" OR "${output}${errors}" MATCHES "modernize-use-nullptr")
            message(FATAL_ERROR "Unchanged Release build failed:\n${output}${errors}")
        endif ()

        # An explicit scan must generate its own prerequisites, even if no native
        # build has left the message header behind.
        file(REMOVE "${build}/event_log_messages.h")
        execute_process(COMMAND "${CMAKE_COMMAND}" --build "${build}" --config Release
                --target clang_tidy RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
        if (NOT EXISTS "${build}/event_log_messages.h" OR
                "${output}${errors}" MATCHES "file not found")
            message(FATAL_ERROR "Explicit analysis missed its generated header:\n${output}${errors}")
        endif ()
        if ("${output}${errors}" MATCHES "static assertion failed")
            message(FATAL_ERROR "Explicit analysis did not use C++17:\n${output}${errors}")
        endif ()
        if (result STREQUAL "0" OR NOT "${output}${errors}" MATCHES "modernize-use-nullptr")
            message(FATAL_ERROR "Explicit Release scan did not enforce clang-tidy:\n${output}${errors}")
        endif ()
    endif ()
endforeach ()

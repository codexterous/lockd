# Keep analysis on project targets, rather than propagating it into dependency builds.
# Wheels can still opt in, but installing the Python package does not require LLVM tools.
if (SKBUILD)
    set(Lockd_TIDY_DEFAULT OFF)
else ()
    set(Lockd_TIDY_DEFAULT AUTO)
endif ()
set(LOCKD_CLANG_TIDY "${Lockd_TIDY_DEFAULT}" CACHE STRING
        "Run clang-tidy during Debug compilation: AUTO, ON, or OFF")
set_property(CACHE LOCKD_CLANG_TIDY PROPERTY STRINGS AUTO ON OFF)
option(LOCKD_CLANG_TIDY_WARNINGS_AS_ERRORS "Fail clang-tidy on enabled check warnings" OFF)
set(LOCKD_CLANG_TIDY_JOBS 2 CACHE STRING "Concurrent processes for explicit clang-tidy scans")
if (NOT LOCKD_CLANG_TIDY_JOBS MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "LOCKD_CLANG_TIDY_JOBS must be a positive integer.")
endif ()
if (NOT LOCKD_CLANG_TIDY MATCHES "^(AUTO|ON|OFF)$")
    message(FATAL_ERROR "LOCKD_CLANG_TIDY must be AUTO, ON, or OFF.")
endif ()

# Visual Studio puts its bundled LLVM on PATH in developer environments. Prefer a
# standalone Windows installation first, while respecting an explicit or cached choice.
if (CMAKE_HOST_WIN32)
    set(Lockd_LLVM_PATHS)
    foreach(program_files IN ITEMS ProgramW6432 ProgramFiles)
        if (DEFINED ENV{${program_files}})
            list(APPEND Lockd_LLVM_PATHS "$ENV{${program_files}}/LLVM/bin")
        endif ()
    endforeach()
    find_program(LOCKD_CLANG_TIDY_EXECUTABLE NAMES clang-tidy
            PATHS ${Lockd_LLVM_PATHS} NO_DEFAULT_PATH)
endif ()
find_program(LOCKD_CLANG_TIDY_EXECUTABLE NAMES clang-tidy-23 clang-tidy
        clang-tidy-22 clang-tidy-21 clang-tidy-20 clang-tidy-19
        clang-tidy-18 clang-tidy-17 clang-tidy-16 clang-tidy-15 clang-tidy-14)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Component targets live in their own binary directories. Include every directory
# when applying automatic checks; inspecting only the root silently misses libraries.
function(lockd_collect_targets directory result)
    get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(child IN LISTS children)
        lockd_collect_targets("${child}" child_targets)
        list(APPEND targets ${child_targets})
    endforeach ()
    set(${result} "${targets}" PARENT_SCOPE)
endfunction()

function(lockd_configure_clang_tidy)
    string(TOUPPER "${CMAKE_BUILD_TYPE}" build_type)
    set(debug_configuration FALSE)
    if (CMAKE_CONFIGURATION_TYPES OR build_type STREQUAL "DEBUG")
        set(debug_configuration TRUE)
    endif ()
    set(supported_generator FALSE)
    if (CMAKE_GENERATOR MATCHES "Ninja|Makefiles" AND
            CMAKE_VERSION VERSION_GREATER_EQUAL 3.25 AND
            (NOT CMAKE_CONFIGURATION_TYPES OR CMAKE_VERSION VERSION_GREATER_EQUAL 3.27))
        set(supported_generator TRUE)
    endif ()
    if (debug_configuration AND LOCKD_CLANG_TIDY STREQUAL "ON" AND
            (NOT LOCKD_CLANG_TIDY_EXECUTABLE OR NOT supported_generator))
        message(FATAL_ERROR "Clang-tidy Debug builds require clang-tidy on PATH (or "
                "LOCKD_CLANG_TIDY_EXECUTABLE), CMake 3.25+ (3.27+ for multi-configuration "
                "builds), and a Ninja or Makefiles generator.")
    endif ()

    # Hide suppression statistics, while preserving diagnostics and failure status.
    set(arguments --quiet "--config-file=${PROJECT_SOURCE_DIR}/.clang-tidy")

    # Release builds and explicitly disabled analysis do not require a compatible tool.
    # The explicit scan runner checks the version when analysis is actually requested.
    if (LOCKD_CLANG_TIDY_EXECUTABLE AND debug_configuration AND
            NOT LOCKD_CLANG_TIDY STREQUAL "OFF")
        include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/lockd_clang_tidy_arguments.cmake")
        lockd_clang_tidy_arguments("${LOCKD_CLANG_TIDY_EXECUTABLE}" compatibility_arguments)
        list(APPEND arguments ${compatibility_arguments})
    endif ()
    if (LOCKD_CLANG_TIDY_WARNINGS_AS_ERRORS)
        list(APPEND arguments "--warnings-as-errors=*")
    endif ()
    # clang-tidy does not infer x86 from the path to MSVC's cl.exe. Its default
    # target is x64 even when the compilation database uses Hostx64/x86/cl.exe.
    if (MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 4)
        list(APPEND arguments "--extra-arg=--target=i686-pc-windows-msvc")
    endif ()
    if (debug_configuration AND LOCKD_CLANG_TIDY_EXECUTABLE AND supported_generator AND
            NOT LOCKD_CLANG_TIDY STREQUAL "OFF")
        lockd_collect_targets("${PROJECT_SOURCE_DIR}" targets)
        foreach(target IN LISTS targets)
            get_target_property(type ${target} TYPE)
            if (type MATCHES "^(EXECUTABLE|STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY)$")
                # A separate -p argument tells CMake to use the compilation database instead of
                # a fixed command line, whose MSVC slash options clang-tidy can misinterpret.
                set(command "${LOCKD_CLANG_TIDY_EXECUTABLE}" ${arguments}
                        -p "${PROJECT_BINARY_DIR}")

                # GCC can default to C++17 without a standard flag in the database, while
                # clang-tidy defaults to an older dialect. Explicit database flags win.
                if (MSVC)
                    set(cxx_command ${command} "--extra-arg-before=/std:c++17")
                else ()
                    set(cxx_command ${command} "--extra-arg-before=-std=gnu++17")
                endif ()

                # Multi-configuration generators select Debug at build time. Older CMake
                # versions support only the single-configuration branch above.
                if (CMAKE_CONFIGURATION_TYPES)
                    set(command "$<$<CONFIG:Debug>:${command}>")
                    set(cxx_command "$<$<CONFIG:Debug>:${cxx_command}>")
                endif ()
                set_target_properties(${target} PROPERTIES
                        C_CLANG_TIDY "${command}" CXX_CLANG_TIDY "${cxx_command}")
            endif ()
        endforeach ()
        message(STATUS "Clang-tidy Debug compilation checks: ${LOCKD_CLANG_TIDY_EXECUTABLE}")
    else ()
        message(STATUS "Clang-tidy compilation checks disabled for this configuration or tool unavailable; "
                "the clang_tidy target remains available for explicit scans")
    endif ()

    # Both targets scan the compilation database even after an unchanged build.
    # The full scan also requires every project component to be represented.
    set(scan_targets clang_tidy)
    if (NOT SKBUILD)
        list(APPEND scan_targets clang_tidy_full)
    endif ()
    foreach(scan_target IN LISTS scan_targets)
        set(full_scan OFF)
        if (scan_target STREQUAL "clang_tidy_full")
            set(full_scan ON)
        endif ()
        add_custom_target(${scan_target}
                COMMAND "${CMAKE_COMMAND}"
                        "-DLOCKD_TIDY_EXECUTABLE=${LOCKD_CLANG_TIDY_EXECUTABLE}"
                        "-DLOCKD_TIDY_BUILD_DIR=${PROJECT_BINARY_DIR}"
                        "-DLOCKD_TIDY_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
                        "-DLOCKD_TIDY_STRICT=${LOCKD_CLANG_TIDY_WARNINGS_AS_ERRORS}"
                        "-DLOCKD_TIDY_MSVC=${MSVC}"
                        "-DLOCKD_TIDY_POINTER_SIZE=${CMAKE_SIZEOF_VOID_P}"
                        "-DLOCKD_TIDY_FULL=${full_scan}"
                        "-DLOCKD_TIDY_JOBS=${LOCKD_CLANG_TIDY_JOBS}"
                        -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/lockd_run_clang_tidy.cmake"
                WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
                COMMENT "Checking configured C and C++ sources with clang-tidy"
                USES_TERMINAL VERBATIM)

        # Fresh analysis trees haven't built the daemon yet. Its event-log source
        # includes a header generated by the Windows message compiler.
        if (TARGET lockd_event_resources)
            add_dependencies(${scan_target} lockd_event_resources)
        endif ()
    endforeach ()

    if (BUILD_TESTING AND LOCKD_CLANG_TIDY_EXECUTABLE AND NOT SKBUILD)
        # Extracted release sources already live beneath output/obj. Compiler probes need
        # the outer host tree too, or their object/PDB paths exceed MSVC's path limit.
        include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/lockd_output_layout.cmake")
        string(SHA256 build_id "${PROJECT_BINARY_DIR}")
        # Isolate build profiles without spending 64 path characters on the identifier.
        # Nested compiler probes also need room for object and PDB names on Windows.
        string(SUBSTRING "${build_id}" 0 16 build_id)
        add_test(NAME lockd_clang_tidy_configuration
                COMMAND "${CMAKE_COMMAND}"
                        "-DLOCKD_TIDY_TEST_EXECUTABLE=${LOCKD_CLANG_TIDY_EXECUTABLE}"
                        "-DLOCKD_TIDY_TEST_COMPILER=${CMAKE_CXX_COMPILER}"
                        "-DLOCKD_TIDY_TEST_MSVC=${MSVC}"
                        "-DLOCKD_TIDY_TEST_DIR=${Lockd_HOST_BUILD_DIR}/verifications/clang_tidy/${build_id}"
                        -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/clang_tidy_test.cmake")
        set_tests_properties(lockd_clang_tidy_configuration PROPERTIES TIMEOUT 30)
        if (supported_generator)
            add_test(NAME lockd_clang_tidy_debug_build
                    COMMAND "${CMAKE_COMMAND}"
                            "-DLOCKD_TIDY_TEST_EXECUTABLE=${LOCKD_CLANG_TIDY_EXECUTABLE}"
                            "-DLOCKD_TIDY_TEST_COMPILER=${CMAKE_CXX_COMPILER}"
                            "-DLOCKD_TIDY_TEST_GENERATOR=${CMAKE_GENERATOR}"
                            "-DLOCKD_TIDY_TEST_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
                            "-DLOCKD_TIDY_TEST_DIR=${Lockd_HOST_BUILD_DIR}/verifications/clang_tidy/${build_id}"
                            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/clang_tidy_build_test.cmake")
            set_tests_properties(lockd_clang_tidy_debug_build PROPERTIES TIMEOUT 120)
        endif ()
    endif ()
endfunction()

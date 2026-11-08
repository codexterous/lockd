add_executable(lockd_test
        source/benchmark_test.cpp
        source/benchmark_mode_test.cpp
        source/detail/async_benchmark_detail.cpp
        source/detail/benchmark_detail.cpp
        source/lockd_test.cpp
        source/lifetime_test.cpp
        source/network_test.cpp
        source/recursive_test.cpp
        source/timed_test.cpp
        source/logging_test.cpp
        source/output_test.cpp
        source/daemon_fixture.cpp
        source/scripted_peer.cpp
        source/dll_test.cpp
        source/dll_c_header_test.c
        source/test_thread.cpp
        source/test_output.cpp
        source/program_options.cpp
        source/launcher.cpp
        source/version.cpp)
set_lockd_target_options(lockd_test utility/lockd_test)
if (WIN32)
    target_sources(lockd_test PRIVATE source/service_test.cpp)
endif ()
if (Lockd_FORK_EXISTS)
    target_sources(lockd_test PRIVATE source/posix_test.cpp)
endif ()
lockd_copy_runtime_dlls(lockd_test)
add_dependencies(lockd_test lockd)
target_link_libraries(lockd_test PRIVATE lockd_daemon_core liblockd_dll Boost::filesystem)

# Direct runs and CTest use the same port, separate from the daemon's default service port.
set(LOCKD_TEST_PORT 27198 CACHE STRING "Loopback port used by the lockd protocol tests")

# Only the launcher needs the daemon's location; resolve it for the active build configuration.
# lockd_test.cpp supplies Boost.Test's implementation for every test translation unit.
target_compile_definitions(lockd_test PRIVATE
        BOOST_TEST_INCLUDED=
        LOCKD_TEST_PORT=${LOCKD_TEST_PORT}
        LOCKD_TEST_OUTPUT_DIR="$<TARGET_FILE_DIR:lockd_test>"
        LOCKD_LOCKD_OUTPUT_DIR="$<TARGET_FILE_DIR:lockd>"
        LOCKD_INSTALLED_DAEMON_PATH="${CMAKE_INSTALL_FULL_BINDIR}/$<TARGET_FILE_NAME:lockd>"
        LOCKD_LOCKD_OUTPUT_NAME="$<TARGET_FILE_NAME:lockd>")
set(LOCKD_INSTALL_TESTDIR "${CMAKE_INSTALL_LIBEXECDIR}/lockd" CACHE STRING
        "Installation directory for native tests, relative to CMAKE_INSTALL_PREFIX")
install(TARGETS lockd_test RUNTIME DESTINATION ${LOCKD_INSTALL_TESTDIR}
        COMPONENT tests EXCLUDE_FROM_ALL)

################################################################################
# CTEST
################################################################################
# Only the protocol fixture needs a fixed port; the other groups use ephemeral loopback ports.
add_test(NAME lockd_protocol
        COMMAND lockd_test --run_test=protocol --report_level=short --
        --interface 127.0.0.1 --threads 2
        --lockd-path "$<TARGET_FILE:lockd>")
add_test(NAME lockd_lifetimes COMMAND lockd_test --run_test=lifetimes --report_level=short)
add_test(NAME lockd_dll COMMAND lockd_test --run_test=dll --report_level=short)
set_tests_properties(lockd_protocol lockd_lifetimes lockd_dll PROPERTIES
        TIMEOUT 90 WORKING_DIRECTORY "${PROJECT_BINARY_DIR}")
set_tests_properties(lockd_protocol PROPERTIES RESOURCE_LOCK lockd_protocol_port)
if (WIN32)
    # The test isolates its logging state itself, including when run outside CTest.
    add_test(NAME lockd_service_logging
            COMMAND lockd_test --run_test=service_logging --report_level=short)
    set_tests_properties(lockd_service_logging PROPERTIES
            TIMEOUT 30 WORKING_DIRECTORY "${PROJECT_BINARY_DIR}")
endif ()

if (BUILD_SHARED_LIBS)
    # Test runtime loading separately: linking this executable against the C wrapper would
    # let the OS load it at startup and hide failures in explicit loading or symbol lookup.
    add_executable(lockd_shared_test
            source/dll_export_test.cpp
            source/shared_library.cpp
            source/test_output.cpp
            source/daemon_fixture.cpp)
    set_lockd_target_options(lockd_shared_test utility/lockd_test)
    target_include_directories(lockd_shared_test PRIVATE "${PROJECT_SOURCE_DIR}/lib/liblockd_dll/include")
    target_link_libraries(lockd_shared_test PRIVATE lockd_daemon_core ${CMAKE_DL_LIBS})
    target_compile_definitions(lockd_shared_test PRIVATE
            BOOST_TEST_INCLUDED=
            LOCKD_C_LIBRARY_PATH="$<TARGET_FILE:liblockd_dll>")
    add_dependencies(lockd_shared_test liblockd_dll)
    lockd_copy_runtime_dlls(lockd_shared_test)
    add_dependencies(lockd_test lockd_shared_test)
    add_test(NAME lockd_shared_exports COMMAND lockd_shared_test --report_level=short)
    set_tests_properties(lockd_shared_exports PROPERTIES
            TIMEOUT 30 WORKING_DIRECTORY "${PROJECT_BINARY_DIR}")
endif ()

################################################################################
# LIBLOCKD_DLL
################################################################################
lockd_add_component(lib/liblockd_dll)

################################################################################
# DAEMON CORE
################################################################################
# The daemon and its integration tests use the same compiled ownership, queue, and network code.
# The selected platform implementation owns process state; executables supply their entry points.
add_library(lockd_daemon_core STATIC
        source/client.cpp
        source/lock_store.cpp
        source/logging.cpp
        source/mutex.cpp
        source/server.cpp
        source/version.cpp)
set_lockd_target_options(lockd_daemon_core .)
set_target_properties(lockd_daemon_core PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/internal")
target_link_libraries(lockd_daemon_core PUBLIC liblockd Boost::log Boost::program_options)
target_link_libraries(lockd_daemon_core PRIVATE Boost::headers)
target_compile_definitions(lockd_daemon_core PUBLIC
        LOCKD_PLATFORM_POSIX=$<BOOL:${UNIX}>
        LOCKD_HAS_FORK=$<BOOL:${Lockd_FORK_EXISTS}>)
if (UNIX)
    target_sources(lockd_daemon_core PRIVATE source/platform/posix.cpp)
elseif (WIN32)
    target_sources(lockd_daemon_core PRIVATE
            source/platform/win32/detail/listener_guard.cpp
            source/platform/win32/detail/unicode.cpp
            source/platform/win32/event_log.cpp
            source/platform/win32/service.cpp
            source/platform/win32/service_elevation.cpp
            source/platform/win32/win32.cpp)
    target_link_libraries(lockd_daemon_core PRIVATE advapi32 bcrypt ole32 shell32)
else ()
    message(FATAL_ERROR "Unsupported daemon platform: ${CMAKE_SYSTEM_NAME}")
endif ()

################################################################################
# LOCKD
################################################################################
add_executable(lockd source/lockd.cpp)
if (WIN32)
    # Link the message table directly into the executable, not a static archive whose resource
    # objects the linker can discard. No registration or Event Log writes occur during the build.
    enable_language(RC)
    get_filename_component(Lockd_RESOURCE_COMPILER_DIR "${CMAKE_RC_COMPILER}" DIRECTORY)
    find_program(Lockd_MESSAGE_COMPILER NAMES mc windmc
            HINTS "${Lockd_RESOURCE_COMPILER_DIR}" REQUIRED)
    set(Lockd_EVENT_RESOURCES "${PROJECT_BINARY_DIR}/event_log")
    file(MAKE_DIRECTORY "${Lockd_EVENT_RESOURCES}")
    add_custom_command(
            OUTPUT "${Lockd_EVENT_RESOURCES}/event_log_messages.rc"
                   "${Lockd_EVENT_RESOURCES}/event_log_messages.h"
            BYPRODUCTS "${Lockd_EVENT_RESOURCES}/event_log_english.bin"
            COMMAND "${Lockd_MESSAGE_COMPILER}" -h "${Lockd_EVENT_RESOURCES}"
                    -r "${Lockd_EVENT_RESOURCES}" "${PROJECT_SOURCE_DIR}/source/platform/win32/event_log_messages.mc"
            DEPENDS "${PROJECT_SOURCE_DIR}/source/platform/win32/event_log_messages.mc"
            WORKING_DIRECTORY "${Lockd_EVENT_RESOURCES}"
            VERBATIM)
    add_custom_target(lockd_event_resources DEPENDS
            "${Lockd_EVENT_RESOURCES}/event_log_messages.rc"
            "${Lockd_EVENT_RESOURCES}/event_log_messages.h")
    add_dependencies(lockd_daemon_core lockd_event_resources)
    target_include_directories(lockd_daemon_core PRIVATE "${Lockd_EVENT_RESOURCES}")
    target_sources(lockd PRIVATE "${Lockd_EVENT_RESOURCES}/event_log_messages.rc")
    target_include_directories(lockd PRIVATE "${Lockd_EVENT_RESOURCES}")
endif ()
set_lockd_target_options(lockd .)
lockd_copy_runtime_dlls(lockd)
target_link_libraries(lockd PRIVATE lockd_daemon_core)

################################################################################
# LOCKD_TEST
################################################################################
if (BUILD_TESTING)
    # Enable CTest without adding unused CDash dashboard targets and IDE run configurations.
    enable_testing()
    lockd_add_component(utility/lockd_test)
    include("${CMAKE_CURRENT_LIST_DIR}/lockd_build_tests.cmake")
endif ()

################################################################################
# NATIVE INSTALLATION AND CMAKE PACKAGE
################################################################################
# Keep the SDK available in either linkage mode. Component installs let runtime-only users
# omit headers, static/import libraries, and the downstream CMake package.
install(TARGETS lockd liblockd liblockd_dll EXPORT lockdTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT runtime
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT runtime NAMELINK_COMPONENT sdk
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT sdk)
install(DIRECTORY lib/liblockd/include/liblockd lib/liblockd_dll/include/liblockd_dll
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT sdk
        FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")
install(EXPORT lockdTargets NAMESPACE lockd::
        FILE lockd_targets.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lockd COMPONENT sdk)
configure_package_config_file("${CMAKE_CURRENT_LIST_DIR}/lockd_config.cmake.in"
        "${PROJECT_BINARY_DIR}/lockdConfig.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lockd)
write_basic_package_version_file("${PROJECT_BINARY_DIR}/lockdConfigVersion.cmake"
        VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES "${PROJECT_BINARY_DIR}/lockdConfig.cmake"
        "${PROJECT_BINARY_DIR}/lockdConfigVersion.cmake" "${CMAKE_CURRENT_LIST_DIR}/lockd_boost.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lockd COMPONENT sdk)

if (UNIX)
    # Installed executables and the C wrapper must find the shared client after relocation.
    if (APPLE)
        set(Lockd_LOADER_PATH "@loader_path")
    else ()
        set(Lockd_LOADER_PATH "$ORIGIN")
    endif ()
    set_target_properties(lockd PROPERTIES
            INSTALL_RPATH "${Lockd_LOADER_PATH}/../${CMAKE_INSTALL_LIBDIR}")
    set_target_properties(liblockd_dll PROPERTIES INSTALL_RPATH "${Lockd_LOADER_PATH}")
    if (TARGET lockd_test)
        get_filename_component(Lockd_FULL_TESTDIR "${LOCKD_INSTALL_TESTDIR}" ABSOLUTE
                BASE_DIR "${CMAKE_INSTALL_PREFIX}")
        file(RELATIVE_PATH Lockd_TEST_LIBRARY_PATH
                "${Lockd_FULL_TESTDIR}" "${CMAKE_INSTALL_FULL_LIBDIR}")
        set_target_properties(lockd_test PROPERTIES
                INSTALL_RPATH "${Lockd_LOADER_PATH}/${Lockd_TEST_LIBRARY_PATH}")
    endif ()
endif ()

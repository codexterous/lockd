set(CMAKE_COMPILE_WARNING_AS_ERROR ON)
configure_file("${CMAKE_CURRENT_LIST_DIR}/lockd_build_version.hpp.in"
        "${PROJECT_BINARY_DIR}/generated/lockd_build_version.hpp" @ONLY)
include("${CMAKE_CURRENT_LIST_DIR}/lockd_clang_tidy.cmake")
include(CheckSymbolExists)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include("${CMAKE_CURRENT_LIST_DIR}/lockd_boost.cmake")

option(BUILD_TESTING "Build the native tests" ON)
include("${CMAKE_CURRENT_LIST_DIR}/lockd_linkage.cmake")
lockd_detect_architecture(Lockd_ARCH)

# Wheels embed a static client with Python's shared runtime. Keep that archive away from
# native static presets, whose client can use a different CRT and dependency linkage.
if (SKBUILD)
    set(Lockd_DEFAULT_OUTPUT_ROOT "${PROJECT_BINARY_DIR}/output")
else ()
    set(Lockd_DEFAULT_OUTPUT_ROOT "${PROJECT_SOURCE_DIR}/output")
endif ()
set(LOCKD_OUTPUT_ROOT "${Lockd_DEFAULT_OUTPUT_ROOT}" CACHE PATH
        "Daemon output directory; overriding it also stages component outputs beneath this directory")
if (NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build configuration" FORCE)
endif ()
if (SKBUILD AND CMAKE_BUILD_TYPE STREQUAL "Debug" AND
        PROJECT_BINARY_DIR MATCHES "/lib/pylockd/output/obj/[^/]+/opt/static(/|$)")
    message(FATAL_ERROR "Debug wheel builds require a dbg object directory. "
            "Pass -Cbuild-dir=lib/pylockd/output/obj/{wheel_tag}/dbg/static.")
endif ()

################################################################################
# DEPENDENCIES
################################################################################
find_package(Threads REQUIRED)
if (SKBUILD)
    # Wheels only need the embedded client; daemon and native-test dependencies stay optional.
    lockd_find_boost()
else ()
    lockd_find_boost(log program_options)
    lockd_check_uuid()
    if (BUILD_TESTING)
        # Boost.Process v1 uses Boost.Filesystem internally, even with string executable paths.
        lockd_find_boost(filesystem)
    endif ()
endif ()
check_symbol_exists(fork "unistd.h" Lockd_FORK_EXISTS)

################################################################################
# TARGET OPTIONS AND OUTPUT DIRECTORIES
################################################################################
function(lockd_component_output_root component result)
    if (component STREQUAL ".")
        set(output_root "${LOCKD_OUTPUT_ROOT}")
    elseif (LOCKD_OUTPUT_ROOT STREQUAL "${PROJECT_SOURCE_DIR}/output")
        set(output_root "${PROJECT_SOURCE_DIR}/${component}/output")
    else ()
        # Alternate compiler builds can stage the complete component tree without overwriting
        # the normal outputs. Keep component ownership visible in that staging tree as well.
        set(output_root "${LOCKD_OUTPUT_ROOT}/${component}/output")
    endif ()
    set(${result} "${output_root}" PARENT_SCOPE)
endfunction()

function(lockd_add_component component)
    if (SKBUILD)
        # The wheel backend owns an isolated ABI tree, including its embedded client.
        get_filename_component(name "${component}" NAME)
        set(binary_dir "${PROJECT_BINARY_DIR}/${name}")
    else ()
        lockd_component_output_root("${component}" output_root)
        string(TOUPPER "${CMAKE_BUILD_TYPE}" configuration)
        if (configuration STREQUAL "DEBUG")
            set(profile dbg)
        else ()
            set(profile opt)
        endif ()
        if (BUILD_SHARED_LIBS)
            set(linkage shared)
        else ()
            set(linkage static)
        endif ()
        set(binary_dir "${output_root}/obj/${Lockd_ARCH}/${profile}/${linkage}")
    endif ()

    # CMake owns object naming below this directory. Declaring the target here keeps
    # its objects, dependency files and compiler symbols with the component that owns it.
    add_subdirectory("${PROJECT_SOURCE_DIR}/${component}" "${binary_dir}")
endfunction()

function(set_lockd_target_options target component)
    target_include_directories(${target} PRIVATE "${PROJECT_BINARY_DIR}/generated")

    # Exception filenames trim this prefix at compile time, including throws from headers.
    # Keep it private so installed SDK targets don't export a build-machine directory.
    target_compile_definitions(${target} PRIVATE "LOCKD_SOURCE_ROOT=\"${PROJECT_SOURCE_DIR}/\"")

    # Keep linkage modes in separate folders so deliverables retain the same names and cleaning
    # one build doesn't remove the other build's files.
    set(profile "$<IF:$<CONFIG:Debug>,dbg,opt>")
    if (BUILD_SHARED_LIBS)
        set(linkage shared)
    else ()
        set(linkage static)
    endif ()
    lockd_component_output_root("${component}" output_root)
    if (SKBUILD)
        # These libraries are wheel build inputs, not native component deliverables.
        # Keep them in the backend's ABI-specific tree, including compiler PDB files.
        set(artifact_dir "${PROJECT_BINARY_DIR}/artifacts/${component}")
    else ()
        set(artifact_dir "${output_root}/${Lockd_ARCH}/${profile}/${linkage}")
    endif ()
    set_target_properties(${target} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${artifact_dir}"
            LIBRARY_OUTPUT_DIRECTORY "${artifact_dir}"
            ARCHIVE_OUTPUT_DIRECTORY "${artifact_dir}"
            PDB_OUTPUT_DIRECTORY "${artifact_dir}")
    get_target_property(target_type ${target} TYPE)
    if (target_type STREQUAL "EXECUTABLE")
        if (LOCKD_STATIC_RUNTIME AND NOT MSVC)
            target_link_options(${target} PRIVATE -static)
        endif ()
    endif ()
    if (MSVC)
        # The source-root literal and __FILE__ must use the same encoding for Unicode checkouts.
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else ()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif ()
    if (CYGWIN)
        # Cygwin remains unsupported; retain its existing compatibility flags.
        target_compile_definitions(${target} PRIVATE
                _WIN32_WINNT=0x0601 NTDDI_VERSION=0x601
                __USE_W32_SOCKETS BOOST_ASIO_HAS_SECURE_RTL)
    endif ()
endfunction()

function(lockd_copy_runtime_dlls target)
    # Windows doesn't search sibling component directories for DLLs. Deploy the runtime
    # dependencies beside each executable/module so it also runs directly outside CTest.
    if (WIN32 AND BUILD_SHARED_LIBS)
        add_custom_command(TARGET ${target} POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                $<TARGET_RUNTIME_DLLS:${target}> $<TARGET_FILE_DIR:${target}>
                COMMAND_EXPAND_LISTS VERBATIM)
    endif ()
endfunction()

################################################################################
# LIBLOCKD
################################################################################
lockd_add_component(lib/liblockd)

# Declaring unused native targets would also require their daemon/test dependencies at configure.
if (NOT SKBUILD)
    include("${CMAKE_CURRENT_LIST_DIR}/lockd_native.cmake")

    # Package through pyproject.toml in its own ABI-specific tree, reusing this
    # profile's installed dependencies without running package installation again.
    set(LOCKD_PACKAGE_PYTHON "" CACHE FILEPATH
            "Python interpreter for pylockd_wheel; empty prefers .venv, then the active environment")
    add_custom_target(pylockd_wheel
            COMMAND "${CMAKE_COMMAND}" "-DLOCKD_PACKAGE_PYTHON=${LOCKD_PACKAGE_PYTHON}"
            "-DLOCKD_PACKAGE_BUILD_DIR=${PROJECT_BINARY_DIR}"
            -P "${CMAKE_CURRENT_LIST_DIR}/lockd_python_package.cmake"
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
            COMMENT "Building the pylockd wheel through pyproject.toml"
            USES_TERMINAL VERBATIM)

    add_custom_target(pylockd_wheel_install
            COMMAND "${CMAKE_COMMAND}" -DLOCKD_PACKAGE_INSTALL=ON
            "-DLOCKD_PACKAGE_BUILD_DIR=${PROJECT_BINARY_DIR}"
            -P "${CMAKE_CURRENT_LIST_DIR}/lockd_python_package.cmake"
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
            COMMENT "Building and installing the pylockd wheel into .venv"
            USES_TERMINAL VERBATIM)

    if (WIN32 AND NOT BUILD_SHARED_LIBS AND LOCKD_STATIC_RUNTIME
            AND CMAKE_BUILD_TYPE STREQUAL "Release" AND BUILD_TESTING)
        add_custom_target(windows_installer
                COMMAND "${CMAKE_COMMAND}" "-DLOCKD_PACKAGE_PYTHON=${LOCKD_PACKAGE_PYTHON}"
                "-DLOCKD_INSTALLER_ARCH=${Lockd_ARCH}"
                -P "${CMAKE_CURRENT_LIST_DIR}/lockd_windows_installer.cmake"
                DEPENDS lockd lockd_test
                WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
                COMMENT "Packaging the Windows portable download and installer"
                USES_TERMINAL VERBATIM)
        add_custom_target(windows_installer_test
                COMMAND "${CMAKE_COMMAND}" "-DLOCKD_PACKAGE_PYTHON=${LOCKD_PACKAGE_PYTHON}"
                "-DLOCKD_INSTALLER_ARCH=${Lockd_ARCH}" -DLOCKD_INSTALLER_LAUNCH=ON
                -P "${CMAKE_CURRENT_LIST_DIR}/lockd_windows_installer.cmake"
                DEPENDS windows_installer
                WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
                COMMENT "Opening the Windows installer for interactive testing"
                USES_TERMINAL VERBATIM)
    endif ()
endif ()

################################################################################
# PYLOCKD, PYTHON
################################################################################
set(Lockd_BUILD_PYTHON OFF)
if (NOT Lockd_PYTHON_MODE STREQUAL "OFF" AND NOT LOCKD_STATIC_RUNTIME)
    if (Lockd_PYTHON_MODE STREQUAL "ON")
        find_package(Python3 3.11...<3.16 REQUIRED COMPONENTS Interpreter Development.Module)
    else ()
        find_package(Python3 3.11...<3.16 QUIET COMPONENTS Interpreter Development.Module)
    endif ()
    if (Python3_FOUND)
        set(Lockd_BOOST_PYTHON_COMPONENT "python${Python3_VERSION_MAJOR}${Python3_VERSION_MINOR}")
        if (Lockd_PYTHON_MODE STREQUAL "ON")
            lockd_find_boost(${Lockd_BOOST_PYTHON_COMPONENT})
            set(Lockd_BUILD_PYTHON ON)
        else ()
            lockd_probe_boost(Lockd_BUILD_PYTHON ${Lockd_BOOST_PYTHON_COMPONENT})
            if (NOT Lockd_BUILD_PYTHON AND NOT SKBUILD)
                lockd_probe_cached_python(Lockd_BUILD_PYTHON)
            endif ()
        endif ()
    endif ()
endif ()

if (Lockd_BUILD_PYTHON)
    message(STATUS "pylockd: ${Python3_EXECUTABLE} (Boost.${Lockd_BOOST_PYTHON_COMPONENT})")
    if (SKBUILD AND BUILD_SHARED_LIBS)
        message(FATAL_ERROR "Python wheels embed the client library; use BUILD_SHARED_LIBS=OFF.")
    endif ()

    lockd_add_component(lib/pylockd)
elseif (Lockd_PYTHON_MODE STREQUAL "AUTO")
    if (LOCKD_STATIC_RUNTIME)
        message(STATUS "pylockd: not added to the fully static native build; use pylockd_wheel")
    elseif (NOT Python3_FOUND)
        message(STATUS "pylockd: matching Python development files not found; native targets only")
    else ()
        message(STATUS "pylockd: Boost.${Lockd_BOOST_PYTHON_COMPONENT} not found; native targets only")
    endif ()
endif ()

lockd_configure_clang_tidy()

# Generator expressions expose the actual object paths chosen by CMake, including
# its internal CMakeFiles directories. Keep this check usable for wheel builds too.
set(Lockd_LAYOUT_CONTENT "set(layout_source [==[${PROJECT_SOURCE_DIR}]==])\n"
        "set(layout_build [==[${PROJECT_BINARY_DIR}]==])\n"
        "set(layout_output [==[${LOCKD_OUTPUT_ROOT}]==])\n"
        "set(layout_wheel [==[${SKBUILD}]==])\n"
        "set(layout_arch ${Lockd_ARCH})\n"
        "set(layout_profile $<IF:$<CONFIG:Debug>,dbg,opt>)\n")
if (BUILD_SHARED_LIBS)
    list(APPEND Lockd_LAYOUT_CONTENT "set(layout_linkage shared)\n")
else ()
    list(APPEND Lockd_LAYOUT_CONTENT "set(layout_linkage static)\n")
endif ()
foreach(Lockd_LAYOUT_PAIR IN ITEMS "liblockd|lib/liblockd" "liblockd_dll|lib/liblockd_dll"
        "lockd_daemon_core|." "lockd|." "lockd_test|utility/lockd_test"
        "lockd_shared_test|utility/lockd_test" "pylockd|lib/pylockd")
    string(REPLACE "|" ";" Lockd_LAYOUT_PAIR "${Lockd_LAYOUT_PAIR}")
    list(GET Lockd_LAYOUT_PAIR 0 Lockd_LAYOUT_TARGET)
    list(GET Lockd_LAYOUT_PAIR 1 Lockd_LAYOUT_COMPONENT)
    if (TARGET ${Lockd_LAYOUT_TARGET})
        list(APPEND Lockd_LAYOUT_CONTENT
                "list(APPEND layout_targets ${Lockd_LAYOUT_TARGET})\n"
                "set(layout_component_${Lockd_LAYOUT_TARGET} ${Lockd_LAYOUT_COMPONENT})\n"
                "set(layout_objects_${Lockd_LAYOUT_TARGET} [==[$<TARGET_OBJECTS:${Lockd_LAYOUT_TARGET}>]==])\n"
                "set(layout_file_${Lockd_LAYOUT_TARGET} [==[$<TARGET_FILE:${Lockd_LAYOUT_TARGET}>]==])\n")
        get_target_property(Lockd_LAYOUT_TYPE ${Lockd_LAYOUT_TARGET} TYPE)
        if (MSVC AND Lockd_LAYOUT_TYPE MATCHES
                "^(EXECUTABLE|SHARED_LIBRARY|MODULE_LIBRARY)$")
            list(APPEND Lockd_LAYOUT_CONTENT
                    "set(layout_pdb_${Lockd_LAYOUT_TARGET} [==[$<TARGET_PDB_FILE:${Lockd_LAYOUT_TARGET}>]==])\n")
        endif ()
    endif ()
endforeach ()
list(JOIN Lockd_LAYOUT_CONTENT "" Lockd_LAYOUT_CONTENT)
file(GENERATE OUTPUT "${PROJECT_BINARY_DIR}/component_layout_$<CONFIG>.cmake"
        CONTENT "${Lockd_LAYOUT_CONTENT}")
if (BUILD_TESTING AND NOT SKBUILD)
    add_test(NAME lockd_component_layout
            COMMAND "${CMAKE_COMMAND}"
                    "-DLOCKD_LAYOUT_MANIFEST=${PROJECT_BINARY_DIR}/component_layout_$<CONFIG>.cmake"
                    -P "${CMAKE_CURRENT_LIST_DIR}/tests/component_layout_test.cmake")
endif ()

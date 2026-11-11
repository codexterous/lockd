cmake_minimum_required(VERSION 3.24)

# A wheel is a separate CMake configure because it embeds a static client and follows the
# selected Python ABI, while a native shared profile links the ordinary shared client library.
# Borrow dependency/compiler locations from that profile without changing its package manifest;
# scikit-build-core still owns the wheel build, installation layout, and packaging.
get_filename_component(Lockd_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if (WIN32)
    set(Lockd_VENV_PYTHON "${Lockd_SOURCE_DIR}/.venv/Scripts/python.exe")
else ()
    set(Lockd_VENV_PYTHON "${Lockd_SOURCE_DIR}/.venv/bin/python")
endif ()

# Both local wheel targets prefer the project's build environment. An explicit
# interpreter still selects another ABI for build-only wheels; installation always
# belongs to .venv, even when another environment is active in the IDE or shell.
if (LOCKD_PACKAGE_INSTALL OR
        (NOT LOCKD_PACKAGE_PYTHON AND EXISTS "${Lockd_VENV_PYTHON}"))
    set(Python3_EXECUTABLE "${Lockd_VENV_PYTHON}")
    if (NOT EXISTS "${Python3_EXECUTABLE}")
        message(FATAL_ERROR "Create the project's .venv before building and installing pylockd.")
    endif ()
elseif (LOCKD_PACKAGE_PYTHON)
    set(Python3_EXECUTABLE "${LOCKD_PACKAGE_PYTHON}")
endif ()
find_package(Python3 3.11...<3.16 REQUIRED COMPONENTS Interpreter)

execute_process(COMMAND "${Python3_EXECUTABLE}" -c
        "import platform, struct; print('arm64' if platform.machine().lower() in ('arm64', 'aarch64') else ('x86' if struct.calcsize('P') == 4 else 'x86_64'))"
        OUTPUT_VARIABLE Lockd_PYTHON_ARCH OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)

# A nested configure does not inherit the native build's dependency locations.
# Read them without running its toolchain, then make vcpkg discovery read-only:
# manifest reconciliation here could remove packages needed by the native build.
set(Lockd_PACKAGE_ARGS -Ccmake.define.LOCKD_DEPENDENCY_PROVIDER=SYSTEM
        -Ccmake.define.VCPKG_MANIFEST_INSTALL=OFF)
if (LOCKD_PACKAGE_BUILD_DIR)
    set(Lockd_PACKAGE_SETTINGS CMAKE_TOOLCHAIN_FILE CMAKE_C_COMPILER CMAKE_CXX_COMPILER
            CMAKE_MAKE_PROGRAM
            LOCKD_TARGET_ARCH LOCKD_VCPKG_ROOT VCPKG_TARGET_TRIPLET VCPKG_HOST_TRIPLET
            VCPKG_INSTALLED_DIR VCPKG_MANIFEST_MODE
            Boost_DIR Boost_ROOT BOOST_ROOT Boost_INCLUDE_DIR
            Boost_LIBRARY_DIR_RELEASE Boost_LIBRARY_DIR_DEBUG
            boost_python${Python3_VERSION_MAJOR}${Python3_VERSION_MINOR}_DIR
            Boost_PYTHON${Python3_VERSION_MAJOR}${Python3_VERSION_MINOR}_LIBRARY_RELEASE)
    load_cache("${LOCKD_PACKAGE_BUILD_DIR}" READ_WITH_PREFIX Lockd_PARENT_
            ${Lockd_PACKAGE_SETTINGS} LOCKD_STATIC_RUNTIME CMAKE_GENERATOR)
    if (WIN32 AND Lockd_PARENT_LOCKD_STATIC_RUNTIME)
        # Python needs the shared CRT even when the selected native profile uses /MT.
        # Reuse the matching shared profile, including its wheel cache, without
        # configuring it or allowing vcpkg to change either dependency installation.
        get_filename_component(Lockd_SHARED_BUILD_DIR
                "${LOCKD_PACKAGE_BUILD_DIR}/../shared" ABSOLUTE)
        if (EXISTS "${Lockd_SHARED_BUILD_DIR}/CMakeCache.txt")
            load_cache("${Lockd_SHARED_BUILD_DIR}" READ_WITH_PREFIX Lockd_SHARED_
                    ${Lockd_PACKAGE_SETTINGS} LOCKD_STATIC_RUNTIME CMAKE_HOME_DIRECTORY
                    CMAKE_GENERATOR)
            if (Lockd_SHARED_LOCKD_STATIC_RUNTIME OR
                    NOT Lockd_SHARED_LOCKD_TARGET_ARCH STREQUAL Lockd_PARENT_LOCKD_TARGET_ARCH OR
                    NOT Lockd_SHARED_CMAKE_HOME_DIRECTORY STREQUAL Lockd_SOURCE_DIR)
                message(FATAL_ERROR "The shared profile at ${Lockd_SHARED_BUILD_DIR} must use "
                        "this source tree, the same architecture, and the shared runtime.")
            endif ()
            foreach(setting IN LISTS Lockd_PACKAGE_SETTINGS)
                unset(Lockd_PARENT_${setting})
                if (DEFINED Lockd_SHARED_${setting})
                    set(Lockd_PARENT_${setting} "${Lockd_SHARED_${setting}}")
                endif ()
            endforeach ()
            set(Lockd_PARENT_LOCKD_STATIC_RUNTIME OFF)
            set(Lockd_PARENT_CMAKE_GENERATOR "${Lockd_SHARED_CMAKE_GENERATOR}")
            set(LOCKD_PACKAGE_BUILD_DIR "${Lockd_SHARED_BUILD_DIR}")
            message(STATUS "Python wheel dependencies: ${LOCKD_PACKAGE_BUILD_DIR}")
        endif ()
    endif ()

    if (WIN32 AND Lockd_PARENT_LOCKD_TARGET_ARCH AND
            NOT Lockd_PARENT_LOCKD_TARGET_ARCH STREQUAL Lockd_PYTHON_ARCH)
        message(FATAL_ERROR "The selected Python interpreter uses ${Lockd_PYTHON_ARCH}, but "
                "the native profile targets ${Lockd_PARENT_LOCKD_TARGET_ARCH}. "
                "Use a matching native profile for the wheel build.")
    endif ()
    set(Lockd_BOOST_PYTHON_COMPONENT
            "python${Python3_VERSION_MAJOR}${Python3_VERSION_MINOR}")
    set(Lockd_NATIVE_PYTHON_DIR "${Lockd_PARENT_boost_${Lockd_BOOST_PYTHON_COMPONENT}_DIR}")
    string(TOUPPER "${Lockd_BOOST_PYTHON_COMPONENT}" Lockd_BOOST_PYTHON_COMPONENT_UPPER)
    set(Lockd_NATIVE_PYTHON_LIBRARY
            "${Lockd_PARENT_Boost_${Lockd_BOOST_PYTHON_COMPONENT_UPPER}_LIBRARY_RELEASE}")
    if (WIN32 AND Lockd_PYTHON_ARCH STREQUAL "x86_64" AND
            NOT IS_DIRECTORY "${Lockd_NATIVE_PYTHON_DIR}" AND
            NOT EXISTS "${Lockd_NATIVE_PYTHON_LIBRARY}")
        include("${CMAKE_CURRENT_LIST_DIR}/lockd_output_layout.cmake")
        string(CONCAT Lockd_PACKAGE_BOOST_PREFIX
                "${Lockd_HOST_BUILD_DIR}/dependencies/release/boost_"
                "python${Python3_VERSION_MAJOR}${Python3_VERSION_MINOR}")
        string(CONCAT Lockd_PACKAGE_BOOST_PATTERN
                "${Lockd_PACKAGE_BOOST_PREFIX}/lib/*boost_" "${Lockd_BOOST_PYTHON_COMPONENT}")
        file(GLOB Lockd_PACKAGE_BOOST_LIBRARIES "${Lockd_PACKAGE_BOOST_PATTERN}*.lib")
        file(GLOB Lockd_PACKAGE_BOOST_DLLS "${Lockd_PACKAGE_BOOST_PATTERN}*.dll")
        if (NOT EXISTS "${Lockd_PACKAGE_BOOST_PREFIX}/include/boost-1_88/boost/version.hpp" OR
                NOT Lockd_PACKAGE_BOOST_LIBRARIES OR NOT Lockd_PACKAGE_BOOST_DLLS)
            find_program(Lockd_POWERSHELL powershell REQUIRED)
            execute_process(COMMAND "${Lockd_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
                    -File "${Lockd_SOURCE_DIR}/scripts/release/build_boost_python.ps1"
                    -Python "${Python3_EXECUTABLE}" COMMAND_ERROR_IS_FATAL ANY)
        endif ()

        # vcpkg's Python port has its own ABI. Use the complete interpreter-specific
        # SDK from the release recipe, without mixing in the native Boost headers or
        # reconciling the native manifest just to build a wheel.
        set(Lockd_PACKAGE_ARGS -Ccmake.define.LOCKD_DEPENDENCY_PROVIDER=SYSTEM
                "-Ccmake.define.Boost_ROOT=${Lockd_PACKAGE_BOOST_PREFIX}"
                -Ccmake.define.Boost_NO_BOOST_CMAKE=ON)
        set(Lockd_PACKAGE_SETTINGS CMAKE_C_COMPILER CMAKE_CXX_COMPILER
                CMAKE_MAKE_PROGRAM LOCKD_TARGET_ARCH)
        message(STATUS "Python wheel Boost SDK: ${Lockd_PACKAGE_BOOST_PREFIX}")
    elseif (WIN32 AND Lockd_PARENT_LOCKD_STATIC_RUNTIME)
        message(FATAL_ERROR "Python wheels need shared-runtime Boost.Python dependencies. "
                "Configure the matching shared profile first: ${Lockd_SHARED_BUILD_DIR}")
    endif ()
    foreach(setting IN LISTS Lockd_PACKAGE_SETTINGS)
        if (DEFINED Lockd_PARENT_${setting} AND
                NOT "${Lockd_PARENT_${setting}}" MATCHES "-NOTFOUND$")
            list(APPEND Lockd_PACKAGE_ARGS
                    "-Ccmake.define.${setting}=${Lockd_PARENT_${setting}}")
        endif ()
    endforeach ()

    # The generator and its build tool are a pair. Letting the frontend choose NMake
    # while forwarding Ninja as CMAKE_MAKE_PROGRAM fails before compiler discovery.
    set(ENV{CMAKE_GENERATOR} "${Lockd_PARENT_CMAKE_GENERATOR}")

    # The backend expands its ABI placeholder. Include forwarded compiler/dependency
    # settings in the cache identity: changing compilers in place makes CMake reset
    # its cache, discarding the backend's initial-cache variables during reconfigure.
    # A short identifier avoids repeating those paths in the Windows build directory.
    get_filename_component(Lockd_PARENT_BUILD_DIR "${LOCKD_PACKAGE_BUILD_DIR}" ABSOLUTE)
    set(Lockd_PARENT_BUILD_ID "${Lockd_PARENT_BUILD_DIR}")
    if (Lockd_PACKAGE_BOOST_PREFIX)
        string(APPEND Lockd_PARENT_BUILD_ID ";${Lockd_PACKAGE_BOOST_PREFIX}")
    endif ()
    string(APPEND Lockd_PARENT_BUILD_ID
            ";${Lockd_PARENT_CMAKE_GENERATOR};${Lockd_PACKAGE_ARGS}")
    string(SHA256 Lockd_PARENT_BUILD_ID "${Lockd_PARENT_BUILD_ID}")
    string(SUBSTRING "${Lockd_PARENT_BUILD_ID}" 0 12 Lockd_PARENT_BUILD_ID)
    set(Lockd_WHEEL_BUILD_DIR "lib/pylockd/output/obj/{wheel_tag}/opt/static")
    list(APPEND Lockd_PACKAGE_ARGS
            "-Cbuild-dir=${Lockd_WHEEL_BUILD_DIR}/native-${Lockd_PARENT_BUILD_ID}")
endif ()

# A package build must not put temporary environments, caches, or bytecode outside output.
set(Lockd_PACKAGE_TMP
        "${Lockd_SOURCE_DIR}/output/obj/${Lockd_PYTHON_ARCH}/opt/shared/python-packaging")
file(MAKE_DIRECTORY "${Lockd_PACKAGE_TMP}")
set(ENV{TMPDIR} "${Lockd_PACKAGE_TMP}")
set(ENV{TEMP} "${Lockd_PACKAGE_TMP}")
set(ENV{TMP} "${Lockd_PACKAGE_TMP}")
set(ENV{PYTHONDONTWRITEBYTECODE} 1)

# Keep the backend on the CMake driving this build action. Otherwise a compiler
# environment can put an older CMake first on PATH, with outdated FindBoost support.
# Preserve an explicit backend executable selected by the developer.
if ("$ENV{CMAKE_EXECUTABLE}" STREQUAL "")
    set(ENV{CMAKE_EXECUTABLE} "${CMAKE_COMMAND}")
endif ()

# Use the selected environment's installed backend without installing build dependencies.
# Stage installation builds separately so an older wheel cannot be selected by mistake.
set(Lockd_WHEEL_DESTINATION
        "${Lockd_SOURCE_DIR}/lib/pylockd/output/${Lockd_PYTHON_ARCH}/opt/static")
set(Lockd_WHEEL_OUTPUT "${Lockd_WHEEL_DESTINATION}")
if (LOCKD_PACKAGE_INSTALL)
    set(Lockd_WHEEL_OUTPUT "${Lockd_PACKAGE_TMP}/install-wheel")
    file(GLOB old_wheels "${Lockd_WHEEL_OUTPUT}/*.whl")
    if (old_wheels)
        file(REMOVE ${old_wheels})
    endif ()
endif ()
execute_process(COMMAND "${Python3_EXECUTABLE}" -m build --wheel --no-isolation
        -Ccmake.define.CMAKE_BUILD_TYPE=Release
        ${Lockd_PACKAGE_ARGS}
        --outdir "${Lockd_WHEEL_OUTPUT}"
        "${Lockd_SOURCE_DIR}"
        WORKING_DIRECTORY "${Lockd_SOURCE_DIR}"
        RESULT_VARIABLE Lockd_PACKAGE_RESULT)
if (NOT Lockd_PACKAGE_RESULT STREQUAL "0")
    message(FATAL_ERROR "pylockd wheel build failed. The selected Python needs build and "
            "scikit-build-core>=1.0,<2, plus matching Boost.Python development libraries. "
            "See PYTHON.md for interpreter and dependency settings.")
endif ()

if (LOCKD_PACKAGE_INSTALL)
    file(GLOB wheels "${Lockd_WHEEL_OUTPUT}/*.whl")
    list(LENGTH wheels wheel_count)
    if (NOT wheel_count EQUAL 1)
        message(FATAL_ERROR "Expected exactly one newly built pylockd wheel in ${Lockd_WHEEL_OUTPUT}.")
    endif ()
    file(COPY ${wheels} DESTINATION "${Lockd_WHEEL_DESTINATION}")

    # The version need not change between local edits; replace the installed binary every time.
    execute_process(COMMAND "${Python3_EXECUTABLE}" -m pip install
            --no-index --no-deps --force-reinstall ${wheels}
            RESULT_VARIABLE Lockd_INSTALL_RESULT)
    if (NOT Lockd_INSTALL_RESULT STREQUAL "0")
        message(FATAL_ERROR "Installing pylockd into .venv failed. Stop Python runs or debug "
                "sessions that may hold the extension DLL open, then retry.")
    endif ()
endif ()

# Use installed Boost packages, preferring Boost's own imported targets. Some distributions
# provide only headers and libraries; CMake's FindBoost module supports those installations.
# Package installation belongs to the selected toolchain, never a fallback performed here.
function(lockd_probe_boost result)
    find_package(Boost 1.82 QUIET CONFIG GLOBAL COMPONENTS ${ARGN})
    foreach(component IN LISTS ARGN)
        if (NOT TARGET Boost::${component})
            set(Boost_FOUND FALSE)
        endif ()
    endforeach ()
    if (NOT Boost_FOUND)
        set(Boost_NO_BOOST_CMAKE ON)
        find_package(Boost 1.82 QUIET MODULE GLOBAL COMPONENTS ${ARGN})
    endif ()
    set(${result} ${Boost_FOUND} PARENT_SCOPE)
endfunction()

function(lockd_detect_windows_api result)
    # Boost.Log's Windows API level is part of its ABI (nt6 versus nt62). Package
    # builds can use either, independently of the Boost version. Link, but do not
    # run, a probe so cross builds and existing dependency installations also work.
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_LIBRARIES Boost::log)
    set(CMAKE_TRY_COMPILE_TARGET_TYPE EXECUTABLE)
    foreach(version IN ITEMS 0x0601 0x0A00)
        set(CMAKE_REQUIRED_DEFINITIONS "-D_WIN32_WINNT=${version}")
        # Recheck on configure: the selected package may have changed in this tree.
        unset(Lockd_BOOST_LOG_ABI_MATCH CACHE)
        check_cxx_source_compiles("#include <boost/log/core.hpp>
            int main() { return boost::log::core::get() ? 0 : 1; }"
                Lockd_BOOST_LOG_ABI_MATCH)
        if (Lockd_BOOST_LOG_ABI_MATCH)
            set(${result} "${version}" PARENT_SCOPE)
            message(STATUS "Boost.Log Windows API target: ${version}")
            return()
        endif ()
    endforeach()
    message(FATAL_ERROR "Could not link Boost.Log with either supported Windows API target. "
            "Check the selected Boost libraries, compiler, and runtime linkage.")
endfunction()

function(lockd_find_boost)
    lockd_probe_boost(found ${ARGN})
    if (NOT found)
        set(Boost_NO_BOOST_CMAKE ON)
        find_package(Boost 1.82 REQUIRED MODULE GLOBAL COMPONENTS ${ARGN})
    endif ()
endfunction()

# The Windows wheel recipe builds Boost.Python for the selected interpreter,
# which can differ from vcpkg's Python ABI. Reuse its standard package only when
# its Boost version matches the native headers; never mix two Boost releases.
function(lockd_probe_cached_python result)
    set(${result} FALSE PARENT_SCOPE)
    if (NOT WIN32 OR NOT MSVC OR NOT Lockd_ARCH STREQUAL "x86_64")
        return()
    endif ()
    get_target_property(include_directories Boost::headers INTERFACE_INCLUDE_DIRECTORIES)
    find_file(version_header boost/version.hpp PATHS ${include_directories}
            NO_DEFAULT_PATH NO_CACHE)
    if (NOT version_header)
        return()
    endif ()
    file(STRINGS "${version_header}" native_version REGEX "^#define BOOST_VERSION ")
    string(REGEX REPLACE "^#define BOOST_VERSION " "" native_version "${native_version}")
    math(EXPR major "${native_version} / 100000")
    math(EXPR minor "${native_version} / 100 % 1000")
    math(EXPR patch "${native_version} % 100")
    set(version "${major}.${minor}.${patch}")
    include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/lockd_output_layout.cmake")
    set(suffix "${Python3_VERSION_MAJOR}${Python3_VERSION_MINOR}")
    set(prefix "${Lockd_HOST_BUILD_DIR}/dependencies/release/boost_python${suffix}")
    set(sdk_version_header "${prefix}/include/boost-${major}_${minor}/boost/version.hpp")
    if (NOT EXISTS "${sdk_version_header}")
        return()
    endif ()
    file(STRINGS "${sdk_version_header}" sdk_version REGEX "^#define BOOST_VERSION ")
    if (NOT sdk_version STREQUAL "#define BOOST_VERSION ${native_version}")
        return()
    endif ()

    # Use Boost's package to select its compiler/runtime and Debug/Release variants. Matching
    # version.hpp alone would not justify inventing an imported library from a filename.
    set(Boost_PYTHON_VERSION "${Python3_VERSION_MAJOR}.${Python3_VERSION_MINOR}")
    find_package(boost_python ${version} EXACT QUIET CONFIG GLOBAL
            PATHS "${prefix}/lib/cmake/boost_python-${version}" NO_DEFAULT_PATH)
    if (NOT TARGET Boost::python)
        return()
    endif ()
    if (CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(configuration DEBUG)
    else ()
        set(configuration RELEASE)
    endif ()
    get_target_property(library Boost::python IMPORTED_IMPLIB_${configuration})
    if (NOT library OR NOT EXISTS "${library}")
        return()
    endif ()

    # The rest of the project asks for python<ABI>. Both names must refer to this same imported
    # target, so native modules reuse the cached SDK without changing the native Boost headers.
    add_library(Boost::${Lockd_BOOST_PYTHON_COMPONENT} ALIAS Boost::python)
    set(${result} TRUE PARENT_SCOPE)
endfunction()

# UUID is header-only and older FindBoost versions treat COMPONENTS uuid as a compiled
# library. Probe its actual use instead, including the OS entropy dependency on Windows.
function(lockd_check_uuid)
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_LIBRARIES Boost::headers)
    if (WIN32)
        list(APPEND CMAKE_REQUIRED_LIBRARIES bcrypt)
    endif ()

    # Dependencies may have been installed or changed since the previous configure.
    unset(Lockd_BOOST_UUID_AVAILABLE CACHE)
    check_cxx_source_compiles("#include <boost/uuid/random_generator.hpp>
        #include <boost/uuid/uuid.hpp>
        int main() { return boost::uuids::random_generator()().is_nil(); }"
            Lockd_BOOST_UUID_AVAILABLE OUTPUT_VARIABLE Lockd_BOOST_UUID_OUTPUT)
    if (NOT Lockd_BOOST_UUID_AVAILABLE)
        if (VCPKG_MANIFEST_MODE AND NOT VCPKG_MANIFEST_INSTALL)
            string(CONCAT Lockd_UUID_DEPENDENCY_HINT
                    "Automatic vcpkg installation is disabled by VCPKG_MANIFEST_INSTALL=OFF. "
                    "Reconfigure with -DVCPKG_MANIFEST_INSTALL=ON "
                    "to install declared dependencies.")
        else ()
            string(CONCAT Lockd_UUID_DEPENDENCY_HINT
                    "Check the selected Boost.UUID headers and platform entropy libraries.")
        endif ()
        message(FATAL_ERROR "Could not compile and link Boost.UUID for the daemon.\n"
                "${Lockd_UUID_DEPENDENCY_HINT}\nCompiler output:\n${Lockd_BOOST_UUID_OUTPUT}")
    endif ()
endfunction()

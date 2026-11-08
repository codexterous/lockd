include("${CMAKE_CURRENT_LIST_DIR}/lockd_architecture.cmake")

# Each configure owns exactly one dbg/opt object tree. Reject generators that mix
# configurations before project() can start installing dependencies for that tree.
get_property(Lockd_MULTI_CONFIGURATION GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if (Lockd_MULTI_CONFIGURATION)
    message(FATAL_ERROR "Use a single-configuration generator such as Ninja or Unix Makefiles. "
            "The repository presets keep dbg and opt artifacts in separate directories.")
endif ()

# The dependency toolchain needs linkage choices before project() installs packages.
# Preserve native defaults while allowing presets and wheel builds to override both.
if (CMAKE_SYSTEM_NAME STREQUAL "Windows" OR
        (NOT CMAKE_SYSTEM_NAME AND CMAKE_HOST_WIN32))
    set(Lockd_DEFAULT_SHARED OFF)
else ()
    set(Lockd_DEFAULT_SHARED ON)
endif ()
option(BUILD_SHARED_LIBS "Build shared client libraries" ${Lockd_DEFAULT_SHARED})
if (BUILD_SHARED_LIBS)
    set(Lockd_DEFAULT_STATIC_RUNTIME OFF)
else ()
    set(Lockd_DEFAULT_STATIC_RUNTIME ON)
endif ()
option(LOCKD_STATIC_RUNTIME "Link dependencies and executable runtimes statically"
        ${Lockd_DEFAULT_STATIC_RUNTIME})

# Installed Boost packages can require FindBoost instead of BoostConfig.cmake. vcpkg
# captures the caller's policies when project() loads its find_package wrapper, so
# choosing this compatibility behavior inside lockd_find_boost() would be too late.
if (POLICY CMP0167)
    cmake_policy(SET CMP0167 OLD)
endif ()

# Runtime DLL filters already accept both separators. Normalize Windows paths before
# matching, and let vcpkg's install wrapper capture this policy before it is defined.
if (POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
endif ()

# AUTO keeps native development available without a matching Python SDK. When the
# SDK and Boost.Python are present, the real module belongs to this CMake project,
# so every IDE receives its compiler settings through the normal project model.
set(LOCKD_BUILD_PYTHON AUTO CACHE STRING "Build the Python extension: AUTO, ON, or OFF")
set_property(CACHE LOCKD_BUILD_PYTHON PROPERTY STRINGS AUTO ON OFF)
string(TOUPPER "${LOCKD_BUILD_PYTHON}" Lockd_PYTHON_MODE)
if (Lockd_PYTHON_MODE MATCHES "^(1|TRUE|YES)$")
    set(Lockd_PYTHON_MODE ON)
elseif (Lockd_PYTHON_MODE MATCHES "^(0|FALSE|NO)$")
    set(Lockd_PYTHON_MODE OFF)
elseif (NOT Lockd_PYTHON_MODE MATCHES "^(AUTO|ON|OFF)$")
    message(FATAL_ERROR "LOCKD_BUILD_PYTHON must be AUTO, ON, or OFF.")
endif ()

# AUTO only uses existing Python dependencies. Installing the optional vcpkg
# feature requires an explicit ON request; configuring native targets must never
# install Python just to make an optional target available to the editor.
if (Lockd_PYTHON_MODE STREQUAL "ON")
    list(APPEND VCPKG_MANIFEST_FEATURES python)
    list(REMOVE_DUPLICATES VCPKG_MANIFEST_FEATURES)
endif ()

# Windows uses the caller's vcpkg installation; Unix retains system-package
# discovery. An explicitly selected toolchain always takes precedence, including
# one supplied by the environment. vcpkg itself owns manifest/classic behavior.
set(LOCKD_DEPENDENCY_PROVIDER AUTO CACHE STRING "Dependency provider: AUTO, SYSTEM, or VCPKG")
set_property(CACHE LOCKD_DEPENDENCY_PROVIDER PROPERTY STRINGS AUTO SYSTEM VCPKG)
if (NOT LOCKD_DEPENDENCY_PROVIDER MATCHES "^(AUTO|SYSTEM|VCPKG)$")
    message(FATAL_ERROR "LOCKD_DEPENDENCY_PROVIDER must be AUTO, SYSTEM, or VCPKG.")
endif ()
if (NOT CMAKE_TOOLCHAIN_FILE AND "$ENV{CMAKE_TOOLCHAIN_FILE}" STREQUAL "" AND
        (LOCKD_DEPENDENCY_PROVIDER STREQUAL "VCPKG" OR
        (LOCKD_DEPENDENCY_PROVIDER STREQUAL "AUTO" AND CMAKE_HOST_WIN32)))
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/lockd_toolchain.cmake"
            CACHE FILEPATH "CMake toolchain")
endif ()

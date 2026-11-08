# Adapt vcpkg's target/runtime selection to lockd's native presets. The caller owns
# the vcpkg installation and its caches; never clone another package manager or
# replace the user's download, binary-cache, registry, or telemetry settings.
include("${CMAKE_CURRENT_LIST_DIR}/lockd_vcpkg_settings.cmake")
lockd_select_vcpkg_root(LOCKD_VCPKG_ROOT)
lockd_select_vcpkg_install_options(Lockd_VCPKG_INSTALL_OPTIONS)
if (NOT DEFINED VCPKG_INSTALL_OPTIONS AND NOT Lockd_VCPKG_INSTALL_OPTIONS STREQUAL "")
    set(VCPKG_INSTALL_OPTIONS "${Lockd_VCPKG_INSTALL_OPTIONS}")
endif ()
if (NOT EXISTS "${LOCKD_VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
    message(FATAL_ERROR "Select an existing vcpkg installation with VCPKG_ROOT or "
            "CMAKE_TOOLCHAIN_FILE. See BUILDING.md for setup and for consuming "
            "already-installed packages with VCPKG_MANIFEST_MODE=OFF.")
endif ()
option(LOCKD_VCPKG_COMPILER_ENVIRONMENT "Use the current developer environment for vcpkg" OFF)
set(LOCKD_VCPKG_PLATFORM_TOOLSET "" CACHE STRING
        "MSVC toolset for the developer environment (e.g. v143)")

# Match the compiler target and runtime rather than using the host's default triplet.
# Keep an explicitly supplied triplet available for other platforms and cross toolchains.
if (CMAKE_HOST_WIN32)
    if (LOCKD_TARGET_ARCH STREQUAL "x86_64")
        set(Lockd_VCPKG_ARCH x64)
    elseif (LOCKD_TARGET_ARCH)
        set(Lockd_VCPKG_ARCH "${LOCKD_TARGET_ARCH}")
    else ()
        set(Lockd_VCPKG_ARCH "$ENV{VSCMD_ARG_TGT_ARCH}")
    endif ()
    if (Lockd_VCPKG_ARCH AND NOT DEFINED VCPKG_TARGET_TRIPLET)
        if (LOCKD_STATIC_RUNTIME)
            set(VCPKG_TARGET_TRIPLET "${Lockd_VCPKG_ARCH}-windows-static" CACHE STRING
                    "vcpkg target")
        else ()
            set(VCPKG_TARGET_TRIPLET "${Lockd_VCPKG_ARCH}-windows" CACHE STRING "vcpkg target")
        endif ()
    endif ()
endif ()

# Portable compiler installations may work in CLion without being registered with
# Visual Studio Installer. In that case vcpkg must use the same developer environment
# instead of trying to discover and initialize another Visual Studio installation.
if (CMAKE_HOST_WIN32 AND LOCKD_VCPKG_COMPILER_ENVIRONMENT)
    if (NOT LOCKD_VCPKG_PLATFORM_TOOLSET)
        message(FATAL_ERROR "Set LOCKD_VCPKG_PLATFORM_TOOLSET for the developer environment "
                "(for example, v143 for Visual Studio 2022).")
    endif ()
    set(Lockd_VCPKG_TRIPLET_DIR "${CMAKE_BINARY_DIR}/vcpkg-triplets")
    foreach(Lockd_VCPKG_TRIPLET x86-windows x86-windows-static
            x64-windows x64-windows-static arm64-windows arm64-windows-static)
        set(Lockd_VCPKG_BASE_TRIPLET "triplets/${Lockd_VCPKG_TRIPLET}.cmake")
        if (NOT EXISTS "${LOCKD_VCPKG_ROOT}/${Lockd_VCPKG_BASE_TRIPLET}")
            set(Lockd_VCPKG_BASE_TRIPLET
                    "triplets/community/${Lockd_VCPKG_TRIPLET}.cmake")
        endif ()
        configure_file("${CMAKE_CURRENT_LIST_DIR}/lockd_vcpkg_environment.cmake.in"
                "${Lockd_VCPKG_TRIPLET_DIR}/${Lockd_VCPKG_TRIPLET}.cmake"
                @ONLY NEWLINE_STYLE CRLF)
    endforeach()
    list(APPEND VCPKG_OVERLAY_TRIPLETS "${Lockd_VCPKG_TRIPLET_DIR}")
    list(REMOVE_DUPLICATES VCPKG_OVERLAY_TRIPLETS)

    # This environment supplies one compiler target, including for vcpkg's build tools.
    if (Lockd_VCPKG_ARCH AND NOT DEFINED VCPKG_HOST_TRIPLET)
        set(VCPKG_HOST_TRIPLET "${Lockd_VCPKG_ARCH}-windows" CACHE STRING "vcpkg host")
    endif ()
endif ()

list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES LOCKD_VCPKG_ROOT
        LOCKD_TARGET_ARCH LOCKD_STATIC_RUNTIME LOCKD_BUILD_PYTHON
        LOCKD_DEPENDENCY_PROVIDER
        LOCKD_VCPKG_COMPILER_ENVIRONMENT LOCKD_VCPKG_PLATFORM_TOOLSET)
message(STATUS "vcpkg installation: ${LOCKD_VCPKG_ROOT}")
include("${LOCKD_VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")

# Choose the compiler's target before language detection. Ninja's architecture preset is IDE
# metadata on Windows, so command-line users must also select the matching compiler environment.
set(LOCKD_TARGET_ARCH "" CACHE STRING "Target architecture: x86, x86_64, or arm64 (empty: compiler default)")
set_property(CACHE LOCKD_TARGET_ARCH PROPERTY STRINGS "" x86 x86_64 arm64)
if (LOCKD_TARGET_ARCH)
    if (CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
        if (LOCKD_TARGET_ARCH STREQUAL "x86")
            set(CMAKE_OSX_ARCHITECTURES i386 CACHE STRING "macOS target architectures")
        else ()
            set(CMAKE_OSX_ARCHITECTURES "${LOCKD_TARGET_ARCH}" CACHE STRING "macOS target architectures")
        endif ()
    elseif (NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
        if (LOCKD_TARGET_ARCH STREQUAL "x86")
            string(APPEND CMAKE_C_FLAGS_INIT " -m32")
            string(APPEND CMAKE_CXX_FLAGS_INIT " -m32")
        elseif (LOCKD_TARGET_ARCH STREQUAL "x86_64")
            string(APPEND CMAKE_C_FLAGS_INIT " -m64")
            string(APPEND CMAKE_CXX_FLAGS_INIT " -m64")
        endif ()
    endif ()
endif ()

function(lockd_detect_architecture result)
    include(CheckCXXSourceCompiles)
    check_cxx_source_compiles("
        #if !defined(_M_ARM64) && !defined(__aarch64__) && !defined(__arm64__)
        #error Not an ARM64 target
        #endif
        int main() { return 0; }" Lockd_COMPILER_ARM64)
    check_cxx_source_compiles("
        #if !defined(_M_IX86) && !defined(_M_X64) && !defined(__i386__) && !defined(__x86_64__)
        #error Not an x86 target
        #endif
        int main() { return 0; }" Lockd_COMPILER_X86)
    if (APPLE AND CMAKE_OSX_ARCHITECTURES MATCHES ";")
        set(arch universal)
    elseif (Lockd_COMPILER_ARM64)
        set(arch arm64)
    elseif (Lockd_COMPILER_X86 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(arch x86_64)
    elseif (Lockd_COMPILER_X86 AND CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(arch x86)
    else ()
        message(FATAL_ERROR "Unsupported compiler target architecture; expected x86, x86_64, or arm64.")
    endif ()
    if (LOCKD_TARGET_ARCH AND NOT LOCKD_TARGET_ARCH STREQUAL arch)
        message(FATAL_ERROR "Requested ${LOCKD_TARGET_ARCH}, but the compiler targets ${arch}. "
                "Select the matching compiler environment or cross-compilation toolchain.")
    endif ()
    set(${result} "${arch}" PARENT_SCOPE)
endfunction()

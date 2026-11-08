# Library type and dependency linkage are separate so Python can embed the client while using
# its interpreter's shared runtime. Native static presets enable both choices together.
set(Boost_USE_STATIC_LIBS ${LOCKD_STATIC_RUNTIME})
if (LOCKD_STATIC_RUNTIME)
    if (BUILD_SHARED_LIBS)
        message(FATAL_ERROR "LOCKD_STATIC_RUNTIME=ON requires BUILD_SHARED_LIBS=OFF.")
    endif ()
    if (Lockd_PYTHON_MODE STREQUAL "ON")
        message(FATAL_ERROR "Fully static builds cannot enable the Python extension. "
                "For an extension with an embedded client, set LOCKD_STATIC_RUNTIME=OFF.")
    endif ()
    if (APPLE)
        message(FATAL_ERROR "macOS does not support fully static system executables. "
                "Set LOCKD_STATIC_RUNTIME=OFF to build static libraries using the system runtime.")
    endif ()
    if (MSVC)
        set(Boost_USE_STATIC_RUNTIME ON)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    elseif (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        include(CheckLinkerFlag)
        check_linker_flag(CXX "-static" Lockd_HAS_STATIC_LINKER)
        if (NOT Lockd_HAS_STATIC_LINKER)
            message(FATAL_ERROR "The compiler cannot link a static executable. "
                    "Install the target's static C/C++ runtime libraries or select a shared profile.")
        endif ()
    else ()
        message(FATAL_ERROR "Fully static runtime flags are not configured for ${CMAKE_CXX_COMPILER_ID}.")
    endif ()
endif ()

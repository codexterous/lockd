# Read saved Windows settings when an IDE has not inherited the current environment.
function(lockd_windows_environment name result)
    set(${result} "" PARENT_SCOPE)
    foreach(key IN ITEMS "HKCU/Environment"
            "HKLM/SYSTEM/CurrentControlSet/Control/Session Manager/Environment")
        cmake_host_system_information(RESULT value QUERY WINDOWS_REGISTRY "${key}"
                VALUE "${name}" VIEW HOST ERROR_VARIABLE registry_error)
        if (NOT registry_error AND NOT value STREQUAL "")
            set(${result} "${value}" PARENT_SCOPE)
            return()
        endif ()
    endforeach()
endfunction()

# Select the caller's package manager before loading any dependency installation code.
function(lockd_select_vcpkg_root result)
    if (LOCKD_VCPKG_ROOT)
        set(${result} "${LOCKD_VCPKG_ROOT}" PARENT_SCOPE)
        return()
    endif ()

    set(root "$ENV{VCPKG_ROOT}")
    set(visual_studio_default FALSE)
    if (CMAKE_HOST_WIN32 AND root AND NOT "$ENV{VSINSTALLDIR}" STREQUAL "")
        file(REAL_PATH "${root}" process_root)
        file(REAL_PATH "$ENV{VSINSTALLDIR}/VC/vcpkg" bundled_root)
        string(TOLOWER "${process_root}" process_root)
        string(TOLOWER "${bundled_root}" bundled_root)
        if (process_root STREQUAL bundled_root)
            set(visual_studio_default TRUE)
        endif ()
    endif ()

    # VsDevCmd replaces VCPKG_ROOT with its bundled installation. Preserve a saved
    # Windows environment override in that case, including when the IDE started
    # before the override was saved. Other process-level selections take precedence.
    if (CMAKE_HOST_WIN32 AND (NOT root OR visual_studio_default))
        lockd_windows_environment(VCPKG_ROOT configured_root)
        if (NOT configured_root STREQUAL "")
            set(root "${configured_root}")
        endif ()
    endif ()

    if (NOT root)
        find_program(Lockd_VCPKG_EXECUTABLE NAMES vcpkg NO_CACHE)
        if (Lockd_VCPKG_EXECUTABLE)
            get_filename_component(root "${Lockd_VCPKG_EXECUTABLE}" DIRECTORY)
        endif ()
    endif ()
    set(${result} "${root}" PARENT_SCOPE)
endfunction()

# Keep machine-specific install arguments in the user's environment, not in shared
# presets. A CMake value, including an explicitly empty one, overrides that default.
function(lockd_select_vcpkg_install_options result)
    if (DEFINED VCPKG_INSTALL_OPTIONS)
        set(options "${VCPKG_INSTALL_OPTIONS}")
    elseif (DEFINED ENV{VCPKG_INSTALL_OPTIONS})
        set(options "$ENV{VCPKG_INSTALL_OPTIONS}")
    elseif (CMAKE_HOST_WIN32)
        lockd_windows_environment(VCPKG_INSTALL_OPTIONS options)
    else ()
        set(options "")
    endif ()
    set(${result} "${options}" PARENT_SCOPE)
endfunction()

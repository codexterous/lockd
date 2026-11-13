cmake_minimum_required(VERSION 3.24)

get_filename_component(Lockd_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if (LOCKD_PACKAGE_PYTHON)
    set(Python3_EXECUTABLE "${LOCKD_PACKAGE_PYTHON}")
endif ()
find_package(Python3 3.11...<3.16 REQUIRED COMPONENTS Interpreter)
find_program(Lockd_POWERSHELL powershell REQUIRED)

if (LOCKD_INSTALLER_LAUNCH)
    set(Lockd_INSTALLER_OPTIONS -LaunchOnly)
else ()
    execute_process(COMMAND "${Python3_EXECUTABLE}"
            "${Lockd_SOURCE_DIR}/scripts/release/package.py" windows
            --architecture "${LOCKD_INSTALLER_ARCH}"
            COMMAND_ERROR_IS_FATAL ANY)
endif ()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=PSModulePath
        "${Lockd_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
        -File "${Lockd_SOURCE_DIR}/scripts/release/build_windows_installer.ps1"
        -Architecture "${LOCKD_INSTALLER_ARCH}" -Python "${Python3_EXECUTABLE}"
        ${Lockd_INSTALLER_OPTIONS}
        COMMAND_ERROR_IS_FATAL ANY)

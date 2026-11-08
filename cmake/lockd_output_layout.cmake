# Build tools run on the host even when their packages target another architecture.
# Share their caches across native profiles; moving between dbg/opt or static/shared
# must not force another download or rebuild of identical dependencies.
cmake_host_system_information(RESULT Lockd_HOST_PROCESSOR QUERY OS_PLATFORM)
string(TOLOWER "${Lockd_HOST_PROCESSOR}" Lockd_HOST_PROCESSOR)
if (Lockd_HOST_PROCESSOR MATCHES "^(arm64|aarch64)$")
    set(Lockd_HOST_ARCH arm64)
elseif (Lockd_HOST_PROCESSOR MATCHES "^(amd64|x86_64|x64)$")
    set(Lockd_HOST_ARCH x86_64)
elseif (Lockd_HOST_PROCESSOR MATCHES "^(x86|i[3-6]86)$")
    set(Lockd_HOST_ARCH x86)
else ()
    message(FATAL_ERROR "Unsupported build host architecture: ${Lockd_HOST_PROCESSOR}")
endif ()
# Extracted release sources share the outer workspace's project-owned tools in CI.
# vcpkg manages its installation and caches independently of this output layout.
if (DEFINED ENV{LOCKD_HOST_BUILD_DIR} AND NOT "$ENV{LOCKD_HOST_BUILD_DIR}" STREQUAL "")
    get_filename_component(Lockd_HOST_BUILD_DIR "$ENV{LOCKD_HOST_BUILD_DIR}" ABSOLUTE)
else ()
    get_filename_component(Lockd_HOST_BUILD_DIR
            "${CMAKE_CURRENT_LIST_DIR}/../output/obj/${Lockd_HOST_ARCH}/opt/shared" ABSOLUTE)
endif ()

add_library(liblockd
        source/mutex.cpp
        source/timed_mutex.cpp
        source/recursive_mutex.cpp
        source/version.cpp
        source/detail/client_contract.cpp
        source/detail/lockd_test_access.cpp)
add_library(lockd::client ALIAS liblockd)
set_lockd_target_options(liblockd lib/liblockd)
set_target_properties(liblockd PROPERTIES POSITION_INDEPENDENT_CODE ON EXPORT_NAME client)
if (UNIX)
    # CMake supplies the lib prefix for both shared libraries and static archives.
    set_target_properties(liblockd PROPERTIES OUTPUT_NAME lockd)
endif ()
if (BUILD_SHARED_LIBS)
    set_target_properties(liblockd PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION 1)
endif ()
target_compile_features(liblockd PUBLIC cxx_std_17)
if (BUILD_SHARED_LIBS)
    target_compile_definitions(liblockd PUBLIC LIBLOCKD_SHARED PRIVATE BUILDING_LIBLOCKD)
endif ()
target_include_directories(liblockd PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/lib/liblockd/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

# Boost.System is header-only since 1.69; its compiled stub was removed in 1.89.
target_link_libraries(liblockd PUBLIC Threads::Threads Boost::headers)
if (WIN32)
    # Asio's public headers and consumers must agree on the Windows API baseline.
    set(Lockd_WINDOWS_API 0x0601)
    if (NOT SKBUILD)
        lockd_detect_windows_api(Lockd_WINDOWS_API)
    endif ()
    target_compile_definitions(liblockd PUBLIC "_WIN32_WINNT=${Lockd_WINDOWS_API}")
    target_link_libraries(liblockd PUBLIC ws2_32)
endif ()

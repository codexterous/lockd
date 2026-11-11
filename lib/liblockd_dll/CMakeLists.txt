add_library(liblockd_dll
        source/liblockd_dll.cpp
        source/version.cpp)
add_library(lockd::c ALIAS liblockd_dll)
set_lockd_target_options(liblockd_dll lib/liblockd_dll)
lockd_copy_runtime_dlls(liblockd_dll)
set_target_properties(liblockd_dll PROPERTIES EXPORT_NAME c)
if (BUILD_SHARED_LIBS)
    set_target_properties(liblockd_dll PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION 1)
    target_compile_definitions(liblockd_dll PRIVATE BUILDING_DLL)
else ()
    target_compile_definitions(liblockd_dll PUBLIC LIBLOCKD_DLL_STATIC)
endif ()
target_include_directories(liblockd_dll PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/lib/liblockd_dll/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(liblockd_dll PRIVATE liblockd)

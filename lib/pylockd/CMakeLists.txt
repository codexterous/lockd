# Native profiles and wheels compile these same bindings. A native module links the configured
# liblockd target; the wheel backend builds that client statically in its own ABI-specific tree.
# Keep registration in the module entry point and private implementations beneath detail/.
Python3_add_library(pylockd MODULE WITH_SOABI
        source/pylockd.cpp source/detail/async_detail.cpp source/detail/sync_detail.cpp)
if (Lockd_PYTHON_MODE STREQUAL "AUTO")
    # Discovering the module for editing must not make a normal native build
    # build Python too. Explicit ON and wheel builds include it in the default build.
    set_target_properties(pylockd PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif ()
set_lockd_target_options(pylockd lib/pylockd)
lockd_copy_runtime_dlls(pylockd)
target_link_libraries(pylockd PRIVATE liblockd Boost::${Lockd_BOOST_PYTHON_COMPONENT})
if (MSVC)
    # Preserve the Python debug ABI when CMake selects a debug interpreter library.
    get_target_property(Lockd_PYTHON_DEBUG_LIBRARY Python3::Module IMPORTED_IMPLIB_DEBUG)
    if (NOT Lockd_PYTHON_DEBUG_LIBRARY)
        get_target_property(Lockd_PYTHON_DEBUG_LIBRARY Python3::Module IMPORTED_LOCATION_DEBUG)
    endif ()
    if (Lockd_PYTHON_DEBUG_LIBRARY MATCHES "_d[.]lib$")
        target_compile_definitions(pylockd PRIVATE "$<$<CONFIG:Debug>:BOOST_DEBUG_PYTHON>")
    endif ()
endif ()

# scikit-build-core packages only this component. Native SDK and daemon installation
# rules remain independent, and FindPython supplies the importing interpreter's ABI suffix.
install(TARGETS pylockd RUNTIME_DEPENDENCY_SET pylockd_runtime_dependencies
        LIBRARY DESTINATION . COMPONENT python
        RUNTIME DESTINATION . COMPONENT python)
if (SKBUILD AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(LOCKD_PYTHON_DEPENDENCY_LICENSE "" CACHE FILEPATH
            "License notice for dependencies bundled by auditwheel")
    if (LOCKD_PYTHON_DEPENDENCY_LICENSE)
        install(FILES "${LOCKD_PYTHON_DEPENDENCY_LICENSE}"
                DESTINATION pylockd_licenses COMPONENT python)
    endif ()
endif ()
if (WIN32)
    # Follow this module's DLL imports rather than copying every ABI's app-local DLLs.
    # Python and Windows system libraries belong to the importing machine.
    set(Lockd_PYTHON_RUNTIME_DIRS "$<TARGET_FILE_DIR:pylockd>")
    foreach (Lockd_PYTHON_RUNTIME_TARGET IN ITEMS
            Boost::python Boost::${Lockd_BOOST_PYTHON_COMPONENT})
        if (TARGET ${Lockd_PYTHON_RUNTIME_TARGET})
            # Boost config packages expose versioned Python names as interface aliases.
            get_target_property(Lockd_PYTHON_RUNTIME_TYPE
                    ${Lockd_PYTHON_RUNTIME_TARGET} TYPE)
            if (NOT Lockd_PYTHON_RUNTIME_TYPE STREQUAL "INTERFACE_LIBRARY")
                list(APPEND Lockd_PYTHON_RUNTIME_DIRS
                        "$<TARGET_FILE_DIR:${Lockd_PYTHON_RUNTIME_TARGET}>")
            endif ()
        endif ()
    endforeach ()
    install(RUNTIME_DEPENDENCY_SET pylockd_runtime_dependencies
            DIRECTORIES ${Lockd_PYTHON_RUNTIME_DIRS}
            PRE_EXCLUDE_REGEXES "^[Aa][Pp][Ii]-" "^[Ee][Xx][Tt]-"
            "^[Pp][Yy][Tt][Hh][Oo][Nn][0-9].*\\.dll$"
            POST_EXCLUDE_REGEXES ".*[/\\\\][Ss][Yy][Ss][Tt][Ee][Mm]32[/\\\\].*"
            RUNTIME DESTINATION . COMPONENT python)
endif ()

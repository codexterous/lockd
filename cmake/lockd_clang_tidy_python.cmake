# Borrow only binding commands from a compatible build. Native targets keep the
# selected profile's flags, even when the wheel embeds another build of liblockd.
function(lockd_python_analysis_sources build_directory source_directory architecture compiler result)
    set(${result} "" PARENT_SCOPE)
    if (NOT EXISTS "${build_directory}/CMakeCache.txt" OR
            NOT EXISTS "${build_directory}/compile_commands.json")
        return()
    endif ()
    load_cache("${build_directory}" READ_WITH_PREFIX python_analysis_
            LOCKD_STATIC_RUNTIME LOCKD_TARGET_ARCH CMAKE_CXX_COMPILER CMAKE_HOME_DIRECTORY)
    if (NOT DEFINED python_analysis_LOCKD_STATIC_RUNTIME OR
            python_analysis_LOCKD_STATIC_RUNTIME OR
            NOT python_analysis_LOCKD_TARGET_ARCH STREQUAL architecture OR
            NOT python_analysis_CMAKE_CXX_COMPILER STREQUAL compiler OR
            NOT python_analysis_CMAKE_HOME_DIRECTORY STREQUAL source_directory)
        return()
    endif ()

    file(READ "${build_directory}/compile_commands.json" commands)
    string(JSON count LENGTH "${commands}")
    set(sources)
    set(python_directory "${source_directory}/lib/pylockd")
    if (count GREATER 0)
        math(EXPR last "${count} - 1")
        foreach(index RANGE 0 ${last})
            string(JSON source GET "${commands}" ${index} file)
            cmake_path(IS_PREFIX python_directory "${source}" NORMALIZE in_python)
            if (in_python AND source MATCHES "\\.(c|C|cc|cp|cpp|CPP|cxx|c\\+\\+)$")
                list(APPEND sources "${source}")
            endif ()
        endforeach ()
    endif ()
    list(REMOVE_DUPLICATES sources)
    set(${result} "${sources}" PARENT_SCOPE)
endfunction()

function(lockd_resolve_checkout_version)
    get_filename_component(source_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)

    # Archives already contain their assigned version. Wheel metadata is read by
    # the Python backend before CMake runs, so its source version stays authoritative.
    if (NOT EXISTS "${source_dir}/.git" OR SKBUILD OR
            NOT LOCKD_VERSION_STRING MATCHES "[.]dev0$")
        return()
    endif ()

    option(LOCKD_REFRESH_VERSION "Fetch assigned build versions when configuring" ON)
    set(LOCKD_VERSION_REMOTE origin CACHE STRING "Git remote providing build version tags")
    find_package(Git QUIET)
    if (NOT Git_FOUND)
        return()
    endif ()

    if (LOCKD_REFRESH_VERSION)
        execute_process(COMMAND "${GIT_EXECUTABLE}" remote get-url "${LOCKD_VERSION_REMOTE}"
                WORKING_DIRECTORY "${source_dir}" RESULT_VARIABLE remote_result
                OUTPUT_QUIET ERROR_QUIET)
        if (remote_result STREQUAL "0")
            # Fetch only assignments, never branches or working-tree files. Bound
            # the wait and disable terminal prompts so offline IDE reloads can finish.
            execute_process(COMMAND "${CMAKE_COMMAND}" -E env GIT_TERMINAL_PROMPT=0
                    "${GIT_EXECUTABLE}" -c credential.interactive=false fetch
                    --quiet --no-tags --no-write-fetch-head "${LOCKD_VERSION_REMOTE}"
                    "refs/tags/build/v*:refs/tags/build/v*"
                    WORKING_DIRECTORY "${source_dir}" TIMEOUT 15
                    RESULT_VARIABLE fetch_result OUTPUT_QUIET ERROR_QUIET)
            if (NOT fetch_result STREQUAL "0")
                message(STATUS "Build version refresh unavailable; using local assignments")
            endif ()
        endif ()
    endif ()

    execute_process(COMMAND "${GIT_EXECUTABLE}" for-each-ref --merged=HEAD
            --sort=-version:refname "--format=%(refname:strip=3)" refs/tags/build/
            WORKING_DIRECTORY "${source_dir}" RESULT_VARIABLE tags_result
            OUTPUT_VARIABLE tags OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if (NOT tags_result STREQUAL "0" OR tags STREQUAL "")
        return()
    endif ()
    string(REPLACE "\n" ";" tags "${tags}")
    list(GET tags 0 tag)
    if (NOT tag MATCHES "^v(0|[1-9][0-9]*)[.](0|[1-9][0-9]*)[.](0|[1-9][0-9]?)$")
        message(FATAL_ERROR "Invalid build version tag: build/${tag}")
    endif ()
    string(SUBSTRING "${tag}" 1 -1 version)

    # An assignment supplies the number, not release status. Development checkouts
    # remain development builds even when HEAD is the clean assigned commit.
    string(APPEND version ".dev0")
    set(LOCKD_VERSION_STRING "${version}" PARENT_SCOPE)
endfunction()

# Source archives carry the assigned version; checkouts can resolve GitHub's tags.
if (NOT CMAKE_SCRIPT_MODE_FILE)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_LIST_DIR}/version.txt")
endif ()
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/version.txt" LOCKD_VERSION_STRING LIMIT_COUNT 1)
lockd_resolve_checkout_version()
if (NOT LOCKD_VERSION_STRING MATCHES "^(0|[1-9][0-9]*)[.](0|[1-9][0-9]*)[.](0|[1-9][0-9]?)([.]dev0)?$")
    message(FATAL_ERROR "Invalid version in cmake/version.txt: ${LOCKD_VERSION_STRING}")
endif ()
set(Lockd_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(Lockd_VERSION_MINOR "${CMAKE_MATCH_2}")
set(Lockd_VERSION_BUILD "${CMAKE_MATCH_3}")
if (Lockd_VERSION_BUILD GREATER 99)
    message(FATAL_ERROR "The build version must be between 0 and 99.")
endif ()
set(Lockd_VERSION "${Lockd_VERSION_MAJOR}.${Lockd_VERSION_MINOR}.${Lockd_VERSION_BUILD}")

# Native version displays use -dev; Python source metadata retains PEP 440 .dev0.
string(REGEX REPLACE "[.]dev0$" "-dev" LOCKD_NATIVE_VERSION_STRING "${LOCKD_VERSION_STRING}")

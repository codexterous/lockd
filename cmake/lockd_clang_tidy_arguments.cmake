# LLVM 20 supports the shared inspection policy. Keep version checks in one place
# for both compilation-time checks and explicit scans.
function(lockd_clang_tidy_arguments executable output)
    execute_process(COMMAND "${executable}" --version
            OUTPUT_VARIABLE version ERROR_VARIABLE errors RESULT_VARIABLE result)
    if (NOT result STREQUAL "0" OR NOT version MATCHES "version ([0-9]+)")
        message(FATAL_ERROR "Cannot determine clang-tidy version: ${version}${errors}")
    endif ()
    set(major "${CMAKE_MATCH_1}")
    set(arguments)
    if (major LESS 20)
        message(FATAL_ERROR "clang-tidy 20 or newer is required by the project inspection policy. "
                "Selected ${executable} reports LLVM ${major}; set LOCKD_CLANG_TIDY_EXECUTABLE "
                "to a supported tool. No analysis was performed.")
    endif ()
    set(${output} "${arguments}" PARENT_SCOPE)
    set(LOCKD_TIDY_VERSION_MAJOR "${major}" PARENT_SCOPE)
endfunction()

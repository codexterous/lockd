cmake_minimum_required(VERSION 3.24)

# Each process captures its own diagnostics so concurrent scans cannot interleave
# individual findings. Keep filtering identical for serial and parallel execution.
function(lockd_analyze_source result_variable)
    execute_process(COMMAND "${LOCKD_TIDY_EXECUTABLE}" ${source_arguments} "${source}"
            WORKING_DIRECTORY "${LOCKD_TIDY_SOURCE_DIR}" RESULT_VARIABLE result
            OUTPUT_VARIABLE output ERROR_VARIABLE errors)
    # --quiet leaves Clang's pre-filter warning count on stderr. Remove only whole
    # count lines; preserve diagnostics, notes, and the original exit status.
    while (errors MATCHES "(^|\n)[0-9]+ warnings? generated\.[\r]?\n")
        string(REGEX REPLACE "(^|\n)[0-9]+ warnings? generated\.[\r]?\n" "\\1" errors "${errors}")
    endwhile ()
    if (LOCKD_TIDY_INPUT)
        file(WRITE "${LOCKD_TIDY_INPUT}.log" "${output}${errors}")
    endif ()
    if (NOT output STREQUAL "")
        message("${output}")
    endif ()
    if (NOT errors STREQUAL "")
        message("${errors}")
    endif ()
    set(${result_variable} "${result}" PARENT_SCOPE)
endfunction()

if (LOCKD_TIDY_INPUT)
    include("${LOCKD_TIDY_INPUT}")
    lockd_analyze_source(result)
    if (NOT result STREQUAL "0")
        message(FATAL_ERROR "Clang-tidy failed for ${source}.")
    endif ()
endif ()

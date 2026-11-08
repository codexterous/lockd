cmake_minimum_required(VERSION 3.24)

# Use the real tool with a deliberate finding. This checks that the runner reports warnings,
# propagates strict failures and parse errors, and quotes paths without requiring a compiler run.
set(fixture "${LOCKD_TIDY_TEST_DIR}/fixture with spaces")
file(MAKE_DIRECTORY "${fixture}")
file(TO_CMAKE_PATH "${LOCKD_TIDY_TEST_COMPILER}" compiler)
set(source "${fixture}/warning.cpp")
file(WRITE "${source}" "int main()\n{\n    int* const value = 0;\n    return value != 0;\n}\n")
file(WRITE "${fixture}/.clang-tidy" "Checks: '-*,modernize-use-nullptr'\nWarningsAsErrors: ''\n")
if (LOCKD_TIDY_TEST_MSVC)
    set(standard "-std:c++17")
    # clang-cl ignores the bare GCC spelling; forward it explicitly to Clang.
    set(pedantic_errors "/clang:-pedantic-errors")
else ()
    set(standard "-std=c++17")
    set(pedantic_errors "-pedantic-errors")
endif ()
set(arguments "[\"${compiler}\",\"${standard}\",\"-c\",\"${source}\"]")
string(JSON entry SET "{}" directory "\"${fixture}\"")
string(JSON entry SET "${entry}" file "\"${source}\"")
string(JSON entry SET "${entry}" arguments "${arguments}")
# A resource entry must not reach clang-tidy, even with a deliberately invalid command.
set(resource "${fixture}/messages.rc")
file(WRITE "${resource}" "1 RCDATA { 0 }\n")
string(JSON resource_entry SET "{}" directory "\"${fixture}\"")
string(JSON resource_entry SET "${resource_entry}" file "\"${resource}\"")
string(JSON resource_entry SET "${resource_entry}" arguments
        "[\"rc.exe\",\"/fo\",\"messages.res\",\"${resource}\"]")
file(WRITE "${fixture}/compile_commands.json" "[${entry},${resource_entry}]\n")
get_filename_component(runner "${CMAKE_CURRENT_LIST_DIR}/../lockd_run_clang_tidy.cmake" ABSOLUTE)
set(command "${CMAKE_COMMAND}"
        "-DLOCKD_TIDY_EXECUTABLE=${LOCKD_TIDY_TEST_EXECUTABLE}"
        "-DLOCKD_TIDY_SOURCE_DIR=${fixture}"
        "-DLOCKD_TIDY_BUILD_DIR=${fixture}"
        "-DLOCKD_TIDY_MSVC=${LOCKD_TIDY_TEST_MSVC}")

# A partial compilation database must never pass as a full project scan.
execute_process(COMMAND ${command} -DLOCKD_TIDY_FULL=ON -P "${runner}"
        RESULT_VARIABLE coverage_result OUTPUT_VARIABLE coverage_output ERROR_VARIABLE coverage_errors)
if (coverage_result STREQUAL "0" OR NOT coverage_errors MATCHES "missing components")
    message(FATAL_ERROR "Full scan accepted incomplete coverage: ${coverage_output}${coverage_errors}")
endif ()

# GCC's default dialect may satisfy CMake without a standard flag. The analysis
# fallback must handle that database, preserve explicit newer standards, and skip C.
foreach(language_case IN ITEMS default explicit c)
    set(case_source "${source}")
    set(case_arguments "[\"${compiler}\",\"-c\",\"${case_source}\"]")
    set(case_text "static_assert(__cplusplus >= 201703L);\n")
    if (LOCKD_TIDY_TEST_MSVC)
        set(case_text "static_assert(_MSVC_LANG >= 201703L);\n")
    endif ()
    if (language_case STREQUAL "explicit")
        string(REPLACE "17" "20" case_standard "${standard}")
        set(case_arguments "[\"${compiler}\",\"${case_standard}\",\"-c\",\"${case_source}\"]")
        string(REPLACE "201703L" "202002L" case_text "${case_text}")
    elseif (language_case STREQUAL "c")
        set(case_source "${fixture}/language.c")
        set(case_arguments "[\"${compiler}\",\"-c\",\"${case_source}\"]")
        if (NOT LOCKD_TIDY_TEST_MSVC)
            set(case_arguments "[\"${compiler}\",\"-x\",\"c\",\"-c\",\"${case_source}\"]")
        endif ()
        set(case_text "#ifdef __cplusplus\n#error C source parsed as C++\n#endif\nint value(void) { return 0; }\n")
    endif ()
    file(WRITE "${case_source}" "${case_text}")
    string(JSON case_entry SET "${entry}" file "\"${case_source}\"")
    string(JSON case_entry SET "${case_entry}" arguments "${case_arguments}")
    file(WRITE "${fixture}/compile_commands.json" "[${case_entry}]\n")
    execute_process(COMMAND ${command} -DLOCKD_TIDY_STRICT=ON -P "${runner}"
            RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
    if (NOT result STREQUAL "0")
        message(FATAL_ERROR "Language case ${language_case} failed:\n${output}${errors}")
    endif ()
endforeach ()

# Full scans from static profiles must retain native flags and use the shared
# database only for Python. Different defines make a wrong database fail parsing.
set(native_build "${fixture}/profiles/static")
set(shared_build "${fixture}/profiles/shared")
file(MAKE_DIRECTORY "${native_build}" "${shared_build}")
foreach(profile IN ITEMS native shared)
    if (profile STREQUAL "native")
        set(static_runtime ON)
    else ()
        set(static_runtime OFF)
    endif ()
    file(WRITE "${${profile}_build}/CMakeCache.txt"
            "LOCKD_STATIC_RUNTIME:BOOL=${static_runtime}\n"
            "LOCKD_TARGET_ARCH:STRING=x86_64\n"
            "CMAKE_CXX_COMPILER:FILEPATH=${compiler}\n"
            "CMAKE_HOME_DIRECTORY:INTERNAL=${fixture}\n")
endforeach ()
set(native_entries "[]")
set(native_index 0)
foreach(component IN ITEMS source lib/liblockd lib/liblockd_dll utility/lockd_test lib/pylockd)
    file(MAKE_DIRECTORY "${fixture}/${component}")
    set(full_source "${fixture}/${component}/full_scan.cpp")
    if (component STREQUAL "lib/pylockd")
        set(expected_definition SHARED_PYTHON)
    else ()
        set(expected_definition NATIVE_STATIC)
    endif ()
    file(WRITE "${full_source}" "#ifndef ${expected_definition}\n#error Wrong database\n#endif\n")
    string(JSON full_entry SET "${entry}" file "\"${full_source}\"")
    string(JSON full_entry SET "${full_entry}" arguments
            "[\"${compiler}\",\"${standard}\",\"-D${expected_definition}\",\"-c\",\"${full_source}\"]")
    if (component STREQUAL "lib/pylockd")
        file(WRITE "${shared_build}/compile_commands.json" "[${full_entry}]\n")
    else ()
        string(JSON native_entries SET "${native_entries}" ${native_index} "${full_entry}")
        math(EXPR native_index "${native_index} + 1")
    endif ()
endforeach ()
file(WRITE "${native_build}/compile_commands.json" "${native_entries}\n")
execute_process(COMMAND ${command} "-DLOCKD_TIDY_BUILD_DIR=${native_build}"
        -DLOCKD_TIDY_FULL=ON -DLOCKD_TIDY_STRICT=ON -P "${runner}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if (NOT result STREQUAL "0" OR NOT output MATCHES "lib/pylockd: 1 translation unit")
    message(FATAL_ERROR "Static full scan did not use both databases correctly:\n${output}${errors}")
endif ()

# A native shared profile may lack Boost.Python too. Both profiles must borrow
# binding flags from the wheel and ignore its deliberately wrong native command.
set(wheel_build "${fixture}/lib/pylockd/output/obj/cp314-fixture/opt/static/native-wheel")
file(MAKE_DIRECTORY "${wheel_build}")
file(READ "${shared_build}/CMakeCache.txt" wheel_cache)
file(READ "${shared_build}/compile_commands.json" wheel_entries)
string(JSON wrong_native GET "${native_entries}" 0)
string(JSON wrong_native SET "${wrong_native}" arguments
        "[\"${compiler}\",\"${standard}\",\"-c\",\"${fixture}/source/full_scan.cpp\"]")
string(JSON wheel_entries SET "${wheel_entries}" 1 "${wrong_native}")
file(WRITE "${wheel_build}/CMakeCache.txt" "${wheel_cache}")
file(WRITE "${wheel_build}/compile_commands.json" "${wheel_entries}")
file(WRITE "${shared_build}/compile_commands.json" "${native_entries}")
foreach(profile IN ITEMS native shared)
    foreach(jobs IN ITEMS 1 2)
        execute_process(COMMAND ${command} "-DLOCKD_TIDY_BUILD_DIR=${${profile}_build}"
                -DLOCKD_TIDY_FULL=ON -DLOCKD_TIDY_STRICT=ON "-DLOCKD_TIDY_JOBS=${jobs}"
                -P "${runner}" RESULT_VARIABLE result
                OUTPUT_VARIABLE output ERROR_VARIABLE errors)
        if (NOT result STREQUAL "0" OR NOT output MATCHES "native-wheel/compile_commands.json" OR
                NOT output MATCHES "lib/pylockd: 1 translation unit")
            message(FATAL_ERROR "${profile} full scan with ${jobs} workers missed the wheel:\n"
                    "${output}${errors}")
        endif ()
    endforeach ()
endforeach ()

# Stale caches for another architecture, compiler, checkout, or CRT must never
# satisfy full coverage. Missing metadata must not inherit a prior candidate's values.
foreach(mismatch IN ITEMS architecture compiler checkout runtime incomplete)
    if (mismatch STREQUAL "incomplete")
        file(WRITE "${wheel_build}/CMakeCache.txt" "")
    else ()
        file(WRITE "${wheel_build}/CMakeCache.txt" "${wheel_cache}")
        if (mismatch STREQUAL "architecture")
            file(APPEND "${wheel_build}/CMakeCache.txt" "LOCKD_TARGET_ARCH:STRING=arm64\n")
        elseif (mismatch STREQUAL "compiler")
            file(APPEND "${wheel_build}/CMakeCache.txt"
                    "CMAKE_CXX_COMPILER:FILEPATH=${fixture}/another-compiler\n")
        elseif (mismatch STREQUAL "checkout")
            file(APPEND "${wheel_build}/CMakeCache.txt"
                    "CMAKE_HOME_DIRECTORY:INTERNAL=${fixture}/another-checkout\n")
        else ()
            file(APPEND "${wheel_build}/CMakeCache.txt" "LOCKD_STATIC_RUNTIME:BOOL=ON\n")
        endif ()
    endif ()
    execute_process(COMMAND ${command} "-DLOCKD_TIDY_BUILD_DIR=${native_build}"
            -DLOCKD_TIDY_FULL=ON -P "${runner}" RESULT_VARIABLE result
            OUTPUT_VARIABLE output ERROR_VARIABLE errors)
    if (result STREQUAL "0" OR NOT errors MATCHES "missing components")
        message(FATAL_ERROR "Full scan accepted wheel with ${mismatch} mismatch:\n${output}${errors}")
    endif ()
endforeach ()

# Without a wheel, an incompatible shared profile still leaves Python uncovered.
file(REMOVE "${wheel_build}/compile_commands.json")
file(WRITE "${shared_build}/compile_commands.json" "[${full_entry}]\n")
file(APPEND "${shared_build}/CMakeCache.txt" "LOCKD_TARGET_ARCH:STRING=arm64\n")
execute_process(COMMAND ${command} "-DLOCKD_TIDY_BUILD_DIR=${native_build}"
        -DLOCKD_TIDY_FULL=ON -P "${runner}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if (result STREQUAL "0" OR NOT errors MATCHES "missing components")
    message(FATAL_ERROR "Full scan accepted a shared profile for another architecture:\n${output}${errors}")
endif ()
file(WRITE "${fixture}/compile_commands.json" "[${entry},${resource_entry}]\n")
file(WRITE "${source}" "int main()\n{\n    int* const value = 0;\n    return value != 0;\n}\n")

if (LOCKD_TIDY_TEST_MSVC)
    # The compiler path alone does not make clang-tidy use MSVC's x86 ABI.
    file(WRITE "${source}" "static_assert(sizeof(void*) == 4);\n")
    execute_process(COMMAND ${command} -DLOCKD_TIDY_POINTER_SIZE=4
            -DLOCKD_TIDY_STRICT=ON -P "${runner}"
            RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
    if (NOT result STREQUAL "0")
        message(FATAL_ERROR "MSVC x86 analysis used the wrong pointer size:\n${output}${errors}")
    endif ()
    file(WRITE "${source}" "int main()\n{\n    int* const value = 0;\n    return value != 0;\n}\n")
endif ()

foreach(strict IN ITEMS OFF ON)
    execute_process(COMMAND ${command} "-DLOCKD_TIDY_STRICT=${strict}" -P "${runner}"
            RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
    if (NOT "${output}${errors}" MATCHES "modernize-use-nullptr")
        message(FATAL_ERROR "The real clang-tidy warning was not reported:\n${output}${errors}")
    endif ()
    if ((strict AND result STREQUAL "0") OR (NOT strict AND NOT result STREQUAL "0"))
        message(FATAL_ERROR "Wrong scan result with strict=${strict}: ${result}\n${output}${errors}")
    endif ()
endforeach ()

# Parser failures must fail even when lint warnings are advisory.
file(WRITE "${source}" "int main( {\n")
execute_process(COMMAND ${command} -DLOCKD_TIDY_STRICT=OFF -P "${runner}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if (result STREQUAL "0" OR NOT "${output}${errors}" MATCHES "clang-diagnostic-error")
    message(FATAL_ERROR "A parse error was mistaken for a successful scan:\n${output}${errors}")
endif ()

execute_process(COMMAND ${command} "-DLOCKD_TIDY_EXECUTABLE=${fixture}/missing-tool"
        -P "${runner}" RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if (result STREQUAL "0" OR NOT "${output}${errors}" MATCHES "No analysis was performed")
    message(FATAL_ERROR "A missing tool was mistaken for a successful scan:\n${output}${errors}")
endif ()

# Use the shared policy with ordinary (non-system) vendor headers. Project headers must remain
# visible, while dependency findings must neither appear nor fail a strict scan.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../../.clang-tidy" header_filter
        REGEX "^HeaderFilterRegex:")
if (NOT header_filter)
    message(FATAL_ERROR "The shared policy has no explicit project-header filter.")
endif ()
file(WRITE "${fixture}/.clang-tidy"
        "Checks: '-*,modernize-use-nullptr'\nWarningsAsErrors: ''\n${header_filter}\n")
set(project_headers include/client.hpp lib/liblockd/include/liblockd/mutex.hpp
        lib/pylockd/include/detail/async_detail.hpp utility/lockd_test/include/daemon_fixture.hpp)
set(vendor_headers third_party/include/vendor.hpp
        output/obj/x86_64/opt/shared/dependencies/include/boost/asio/impl/read.hpp)
set(source_text "")
set(index 0)
foreach(header IN LISTS project_headers vendor_headers)
    math(EXPR index "${index} + 1")
    get_filename_component(directory "${fixture}/${header}" DIRECTORY)
    file(MAKE_DIRECTORY "${directory}")
    file(WRITE "${fixture}/${header}" "inline int* pointer_${index}() { return 0; }\n")
    string(APPEND source_text "#include \"${header}\"\n")
endforeach ()
file(WRITE "${source}" "${source_text}")
execute_process(COMMAND ${command} -DLOCKD_TIDY_STRICT=ON -P "${runner}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if (result STREQUAL "0")
    message(FATAL_ERROR "Project-header warnings did not fail a strict scan.")
endif ()
foreach(header IN LISTS project_headers)
    get_filename_component(name "${header}" NAME)
    if (NOT "${output}${errors}" MATCHES "${name}:[0-9]+:[0-9]+: error:")
        message(FATAL_ERROR "Project header ${header} was excluded:\n${output}${errors}")
    endif ()
    file(WRITE "${fixture}/${header}" "")
endforeach ()
execute_process(COMMAND ${command} -DLOCKD_TIDY_STRICT=ON -P "${runner}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if (NOT result STREQUAL "0" OR "${output}${errors}" MATCHES "modernize-use-nullptr")
    message(FATAL_ERROR "Dependency-header warnings escaped the filter:\n${output}${errors}")
endif ()
if ("${output}${errors}" MATCHES "[0-9]+ warnings? generated|Suppressed [0-9]+ warnings|Use -header-filter|Use -system-headers")
    message(FATAL_ERROR "Filtered dependency warnings produced summary noise:\n${output}${errors}")
endif ()

# Compiler diagnostics must remain strict in both tests and production sources.
# Boost.Test's system-header macros need no blanket extension allowance; a direct
# __COUNTER__ use must still fail on Clang versions that diagnose it under pedantic errors.
include("${CMAKE_CURRENT_LIST_DIR}/../lockd_clang_tidy_arguments.cmake")
lockd_clang_tidy_arguments("${LOCKD_TIDY_TEST_EXECUTABLE}" compatibility_arguments)
set(counter_source "${fixture}/utility/lockd_test/source/counter.cpp")
file(MAKE_DIRECTORY "${fixture}/utility/lockd_test/source")
foreach(counter IN ITEMS "${counter_source}" "${source}")
    file(WRITE "${counter}" "int counter() { return __COUNTER__; }\n")
    string(JSON entry SET "${entry}" file "\"${counter}\"")
    string(JSON entry SET "${entry}" arguments
            "[\"${compiler}\",\"${standard}\",\"${pedantic_errors}\",\"-c\",\"${counter}\"]")
    file(WRITE "${fixture}/compile_commands.json" "[${entry}]\n")
    execute_process(COMMAND ${command} -DLOCKD_TIDY_STRICT=ON -P "${runner}"
            RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
    if (LOCKD_TIDY_VERSION_MAJOR GREATER_EQUAL 22)
        if (result STREQUAL "0" OR NOT "${output}${errors}" MATCHES "c2y-extensions")
            message(FATAL_ERROR "Pedantic compiler diagnostic was suppressed for ${counter}:\n${output}${errors}")
        endif ()
    elseif (NOT result STREQUAL "0")
        message(FATAL_ERROR "Compiler extension check failed: ${output}${errors}")
    endif ()
endforeach ()

# Intentional discards are annotated at the call site, not exempted in the policy.
# An adjacent unannotated call must still fail the strict scan.
file(WRITE "${fixture}/.clang-tidy"
        "Checks: '-*,bugprone-unused-return-value'\nWarningsAsErrors: ''\n"
        "CheckOptions:\n  bugprone-unused-return-value.CheckedFunctions: '::checked_result'\n")
string(JSON entry SET "${entry}" file "\"${source}\"")
string(JSON entry SET "${entry}" arguments
        "[\"${compiler}\",\"${standard}\",\"-c\",\"${source}\"]")
file(WRITE "${fixture}/compile_commands.json" "[${entry}]\n")
string(CONCAT intentional_discard "int checked_result();\nvoid cleanup() {\n"
        "// NOLINTNEXTLINE(bugprone-unused-return-value)\n"
        "static_cast<void>(checked_result());\n}\n")
foreach(unannotated IN ITEMS TRUE FALSE)
    file(WRITE "${source}" "${intentional_discard}")
    if (unannotated)
        file(APPEND "${source}" "void accidental_discard() { checked_result(); }\n")
    endif ()
    execute_process(COMMAND ${command} -DLOCKD_TIDY_STRICT=ON -P "${runner}"
            RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
    if (unannotated)
        if (result STREQUAL "0" OR NOT "${output}${errors}" MATCHES "bugprone-unused-return-value")
            message(FATAL_ERROR "Unannotated discarded result escaped analysis:\n${output}${errors}")
        endif ()
    elseif (NOT result STREQUAL "0")
        message(FATAL_ERROR "Intentional annotated discard failed analysis:\n${output}${errors}")
    endif ()
endforeach ()

# Parallel scans must report every failed translation unit and clear failures on
# a subsequent unchanged invocation after the sources have been corrected.
file(WRITE "${fixture}/.clang-tidy" "Checks: '-*,modernize-use-nullptr'\nWarningsAsErrors: ''\n")
set(parallel_entries "[]")
foreach(index RANGE 0 2)
    set(parallel_source "${fixture}/parallel_${index}.cpp")
    file(WRITE "${parallel_source}" "int* pointer_${index}() { return 0; }\n")
    string(JSON parallel_entry SET "${entry}" file "\"${parallel_source}\"")
    string(JSON parallel_entry SET "${parallel_entry}" arguments
            "[\"${compiler}\",\"${standard}\",\"-c\",\"${parallel_source}\"]")
    string(JSON parallel_entries SET "${parallel_entries}" ${index} "${parallel_entry}")
endforeach ()
file(WRITE "${fixture}/compile_commands.json" "${parallel_entries}\n")
execute_process(COMMAND ${command} -DLOCKD_TIDY_JOBS=2 -DLOCKD_TIDY_STRICT=OFF -P "${runner}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if (NOT result STREQUAL "0" OR NOT "${output}${errors}" MATCHES "modernize-use-nullptr")
    message(FATAL_ERROR "Parallel non-strict scan hid findings or failed:\n${output}${errors}")
endif ()
execute_process(COMMAND ${command} -DLOCKD_TIDY_JOBS=2 -DLOCKD_TIDY_STRICT=ON -P "${runner}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if (result STREQUAL "0")
    message(FATAL_ERROR "Parallel findings did not fail the scan:\n${output}${errors}")
endif ()
foreach(index RANGE 0 2)
    if (NOT "${output}${errors}" MATCHES "parallel_${index}\\.cpp:[0-9]+:[0-9]+: error:")
        message(FATAL_ERROR "Parallel scan lost diagnostics for source ${index}:\n${output}${errors}")
    endif ()
    file(WRITE "${fixture}/parallel_${index}.cpp" "int* pointer_${index}() { return nullptr; }\n")
endforeach ()
foreach(repeat RANGE 1 2)
    execute_process(COMMAND ${command} -DLOCKD_TIDY_JOBS=2 -DLOCKD_TIDY_STRICT=ON -P "${runner}"
            RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
    if (NOT result STREQUAL "0")
        message(FATAL_ERROR "Clean parallel scan ${repeat} failed:\n${output}${errors}")
    endif ()
    if ("${output}${errors}" MATCHES "Test command:|Test timeout|tests passed|Test project|modernize-use-nullptr")
        message(FATAL_ERROR "Clean scan emitted scheduling noise or stale findings:\n${output}${errors}")
    endif ()
endforeach ()

# LLVM 20 must accept the required destructor spelling. Ordinary virtual methods
# still require override; a negative control guards against disabling the whole check.
get_filename_component(policy "${CMAKE_CURRENT_LIST_DIR}/../../.clang-tidy" ABSOLUTE)
foreach(declaration IN ITEMS "void method() override;" "void method();")
    file(WRITE "${source}"
            "struct base { virtual ~base() = default; virtual void method(); };\n"
            "struct derived : base { virtual ~derived() override = default; ${declaration} };\n")
    execute_process(COMMAND "${LOCKD_TIDY_TEST_EXECUTABLE}" "${source}"
            "--config-file=${policy}" "--checks=-*,modernize-use-override"
            "--warnings-as-errors=*" --quiet -- -std=c++17
            RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
    if (declaration MATCHES "override")
        if (NOT result STREQUAL "0")
            message(FATAL_ERROR "Explicit destructor virtual override was rejected:\n${output}${errors}")
        endif ()
    elseif (result STREQUAL "0" OR NOT "${output}${errors}" MATCHES "modernize-use-override")
        message(FATAL_ERROR "Missing method override was accepted:\n${output}${errors}")
    endif ()
endforeach ()

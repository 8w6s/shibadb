if(NOT DEFINED NM OR NOT DEFINED LIBRARY OR NOT DEFINED EXPECTED)
    message(FATAL_ERROR "NM, LIBRARY and EXPECTED are required")
endif()
execute_process(
    COMMAND "${NM}" -D --defined-only "${LIBRARY}"
    RESULT_VARIABLE nm_status
    OUTPUT_VARIABLE nm_output
    ERROR_VARIABLE nm_error
)
if(NOT nm_status EQUAL 0)
    message(FATAL_ERROR "nm failed: ${nm_error}")
endif()
string(REPLACE "\n" ";" nm_lines "${nm_output}")
set(actual)
foreach(line IN LISTS nm_lines)
    if(line MATCHES "[ \t]([A-Za-z_][A-Za-z0-9_]*)$")
        list(APPEND actual "${CMAKE_MATCH_1}")
    endif()
endforeach()
list(SORT actual)
file(STRINGS "${EXPECTED}" expected)
list(SORT expected)
if(NOT actual STREQUAL expected)
    message(FATAL_ERROR
        "Shared ABI symbol mismatch\nExpected: ${expected}\nActual: ${actual}"
    )
endif()

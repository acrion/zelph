# Runs one of the checks located in tools/ and holds its exit code and message to
# match what the caller anticipated. Driven by add_test() in the
# CMakeLists.txt adjacent to it.

foreach (required PYTHON SCRIPT ARGS EXPECT_CODE)
    if (NOT DEFINED ${required})
        message(FATAL_ERROR "run_check.cmake needs -D${required}=")
    endif ()
endforeach ()

separate_arguments(argument_list UNIX_COMMAND "${ARGS}")

execute_process(
        COMMAND "${PYTHON}" "${SCRIPT}" ${argument_list}
        RESULT_VARIABLE exit_code
        OUTPUT_VARIABLE standard_output
        ERROR_VARIABLE standard_error)

message(STATUS "${standard_output}${standard_error}")

# The exact code, because a check leaves with 2 where it could not put the
# question at all. Accepting any non-zero code here would let a broken check
# look like a working one.
if (NOT exit_code EQUAL EXPECT_CODE)
    message(FATAL_ERROR
            "expected ${SCRIPT} to exit ${EXPECT_CODE}, but it exited ${exit_code}")
endif ()

if (DEFINED EXPECT_OUTPUT AND NOT "${standard_output}${standard_error}" MATCHES "${EXPECT_OUTPUT}")
    message(FATAL_ERROR
            "${SCRIPT} exited ${exit_code} as expected, but said nothing matching '${EXPECT_OUTPUT}'")
endif ()

# Verifies that validating a disabled/unavailable subject plan exits with
# code 3 (the "clearly skip unavailable executable subjects" contract).
# Usage: cmake -DOZBENCH=<path> -DPLAN=<path> -P check_skip.cmake
if(NOT DEFINED OZBENCH OR NOT DEFINED PLAN)
    message(FATAL_ERROR "OZBENCH and PLAN must be defined")
endif()

execute_process(
    COMMAND "${OZBENCH}" validate "${PLAN}"
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)

if(NOT rc EQUAL 3)
    message(FATAL_ERROR "expected exit code 3 (subject unavailable/disabled), got ${rc}\nstdout: ${out}\nstderr: ${err}")
endif()

string(FIND "${out}" "subject_available" has_field)
if(has_field EQUAL -1)
    message(FATAL_ERROR "validation record missing subject_available field\nstdout: ${out}")
endif()

message(STATUS "check_skip: OK (exit 3, subject_available present)")
# Verifies that a timed-out subject run is recorded with "timed_out":true and
# that the runner returns promptly (the CLI itself exits 0 because the run was
# recorded, not lost).
# Usage: cmake -DOZBENCH=<path> -DPLAN=<path> -DFIXTURE=<path> -P check_timeout.cmake
if(NOT DEFINED OZBENCH OR NOT DEFINED PLAN OR NOT DEFINED FIXTURE)
    message(FATAL_ERROR "OZBENCH, PLAN and FIXTURE must be defined")
endif()

execute_process(
    COMMAND "${OZBENCH}" run "${PLAN}" --set fixture_exe=${FIXTURE}
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)

if(NOT rc EQUAL 0)
    message(FATAL_ERROR "expected exit 0 (run recorded), got ${rc}\nstdout: ${out}\nstderr: ${err}")
endif()

string(FIND "${out}" "\"timed_out\":true" has_timeout)
if(has_timeout EQUAL -1)
    message(FATAL_ERROR "expected \"timed_out\":true in output\nstdout: ${out}")
endif()

string(FIND "${out}" "\"launched\":true" has_launched)
if(has_launched EQUAL -1)
    message(FATAL_ERROR "expected \"launched\":true in output\nstdout: ${out}")
endif()

message(STATUS "check_timeout: OK (timed_out:true, launched:true)")
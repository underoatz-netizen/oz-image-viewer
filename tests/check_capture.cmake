# Verifies the `ozbench capture` CLI end-to-end against the control-window
# fixture: the ExternalFirstImageVisible endpoint must be recorded as matched.
# When Desktop Duplication is unavailable (no graphical desktop in this
# session) the test skips cleanly instead of failing.
# Usage: cmake -DOZBENCH=<path> -DCONTROL=<path> -DPLAN=<path> -DOUT=<path>
#        -P check_capture.cmake
if(NOT DEFINED OZBENCH OR NOT DEFINED CONTROL OR NOT DEFINED PLAN OR NOT DEFINED OUT)
    message(FATAL_ERROR "OZBENCH, CONTROL, PLAN and OUT must be defined")
endif()

execute_process(
    COMMAND "${OZBENCH}" capture "${PLAN}" --out "${OUT}" --set control_exe=${CONTROL}
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)

if(NOT EXISTS "${OUT}")
    message(FATAL_ERROR "capture produced no output file\nrc=${rc}\nstdout: ${out}\nstderr: ${err}")
endif()

file(READ "${OUT}" content)

if(content MATCHES "\"status\":\"matched\"")
    string(FIND "${content}" "ExternalFirstImageVisible" has_endpoint)
    if(has_endpoint EQUAL -1)
        message(FATAL_ERROR "matched capture missing ExternalFirstImageVisible endpoint\n${content}")
    endif()
    string(FIND "${content}" "subject_path\":null" has_redaction)
    if(has_redaction EQUAL -1)
        message(FATAL_ERROR "capture records must redact subject_path\n${content}")
    endif()
    message(STATUS "check_capture: OK (ExternalFirstImageVisible matched, redacted)")
elseif(content MATCHES "Desktop Duplication unavailable|access denied|no graphical desktop")
    message(STATUS "check_capture: SKIP (no graphical desktop in this session)")
else()
    message(FATAL_ERROR "capture did not match\nrc=${rc}\nstdout: ${out}\nstderr: ${err}\ncontent: ${content}")
endif()
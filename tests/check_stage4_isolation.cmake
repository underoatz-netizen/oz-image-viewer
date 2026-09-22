# check_stage4_isolation.cmake - asserts Stage 4 benchmark isolation contract:
# 1. ozviewer --benchmark <image> must emit exactly 10 canonical markers through FirstPresent.
# 2. No folder enumeration, no dialog, no open button, and clean exit code 0.
#
# Usage:
#   cmake -DOZVIEWER=<exe> -DFIXTURE=<image> -DOUT_DIR=<dir> -P check_stage4_isolation.cmake

if(NOT EXISTS "${OZVIEWER}")
    message(FATAL_ERROR "OZVIEWER executable missing: ${OZVIEWER}")
endif()

if(NOT EXISTS "${FIXTURE}")
    message(FATAL_ERROR "FIXTURE missing: ${FIXTURE}")
endif()

set(JSONL "${OUT_DIR}/stage4_isolation_test.jsonl")

execute_process(
    COMMAND "${OZVIEWER}" --benchmark "${FIXTURE}"
    OUTPUT_FILE "${JSONL}"
    ERROR_VARIABLE err_text
    RESULT_VARIABLE run_rc
    TIMEOUT 30
)

if(NOT run_rc EQUAL 0)
    message(FATAL_ERROR "ozviewer --benchmark exited with code ${run_rc}: ${err_text}")
endif()

file(READ "${JSONL}" content)
string(REPLACE "\n" ";" lines "${content}")

set(markers "")
foreach(line ${lines})
    string(STRIP "${line}" line)
    if(line STREQUAL "")
        continue()
    endif()
    string(REGEX MATCH "\"marker\":\"([A-Za-z]+)\"" m "${line}")
    if(CMAKE_MATCH_1)
        list(APPEND markers "${CMAKE_MATCH_1}")
        if(CMAKE_MATCH_1 MATCHES "FolderEnumeration")
            message(FATAL_ERROR "Forbidden folder enumeration marker emitted: ${line}")
        endif()
    endif()
endforeach()

set(expected "ProcessStart;RendererReady;FileOpened;CodecDetected;HeaderParsed;DecodeStart;DecodeComplete;GpuUploadStart;GpuUploadComplete;FirstPresent")
if(NOT markers STREQUAL expected)
    message(FATAL_ERROR "Stage 4 isolation marker mismatch!\nExpected: ${expected}\nActual:   ${markers}")
endif()

message(STATUS "check_stage4_isolation: OK (Benchmark isolation verified: exact 10 markers, 0 folder enumeration, clean auto-exit)")

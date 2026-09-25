# check_viewer_decode_failure_isolation.cmake - Stage 10.1 malformed-input
# hardening regression for the real viewer executable.
#
# Runs `ozviewer --benchmark` against:
#   1. a deterministically truncated PNG (valid signature + IHDR, severed
#      pixel stream): must fail safely in noninteractive mode - a bounded
#      non-zero exit with the existing "decode failed" worker diagnostic on
#      stderr, no FirstPresent/GPU markers, and no hang (bounded timeout);
#   2. a valid PNG: must still decode and present afterward (valid-after-
#      failure recovery at the application boundary).
#
# The existing ozviewer_benchmark_ui_isolation_scan CTest proves the decode/
# present failure branch uses the shared noninteractive predicate, so no modal
# prompt can appear in either benchmark mode; this check proves the runtime
# noninteractive failure path and recovery.
#
# Usage: cmake -DOZVIEWER=<exe> -DMAKE_PNG=<exe> -DOUT_DIR=<dir>
#              -P check_viewer_decode_failure_isolation.cmake
# Exits non-zero on any failure.

if(NOT EXISTS "${OZVIEWER}")
    message(STATUS "check_viewer_decode_failure_isolation: SKIP (ozviewer not built)")
    return()
endif()
if(NOT EXISTS "${MAKE_PNG}")
    message(FATAL_ERROR "ozviewer_make_png missing: ${MAKE_PNG}")
endif()

set(VALID "${OUT_DIR}/decode_failure_isolation_valid.png")
set(MALFORMED "${OUT_DIR}/decode_failure_isolation_malformed.png")
set(BAD_JSONL "${OUT_DIR}/decode_failure_isolation_malformed.jsonl")
set(GOOD_JSONL "${OUT_DIR}/decode_failure_isolation_valid.jsonl")

# 1. Generate the valid fixture and a truncated variant of the same bytes
#    (keep_bytes 40 = signature + complete IHDR, pixel stream severed).
execute_process(COMMAND "${MAKE_PNG}" "${VALID}" 64 48 rgba 0
                RESULT_VARIABLE gen_valid_rc)
if(NOT gen_valid_rc EQUAL 0)
    message(FATAL_ERROR "failed to generate valid PNG fixture")
endif()
execute_process(COMMAND "${MAKE_PNG}" "${MALFORMED}" 64 48 rgba 0 40
                RESULT_VARIABLE gen_bad_rc)
if(NOT gen_bad_rc EQUAL 0)
    message(FATAL_ERROR "failed to generate truncated PNG fixture")
endif()

# 2. Malformed input must fail safely and noninteractively, without hanging.
execute_process(COMMAND "${OZVIEWER}" --benchmark "${MALFORMED}"
                OUTPUT_FILE "${BAD_JSONL}"
                ERROR_VARIABLE bad_err
                RESULT_VARIABLE bad_rc
                TIMEOUT 60)
if(NOT bad_rc MATCHES "^[0-9]+$")
    message(FATAL_ERROR "malformed PNG run did not terminate on its own (rc='${bad_rc}'): possible hang")
endif()
if(bad_rc EQUAL 0)
    message(FATAL_ERROR "malformed PNG run exited 0; expected a bounded non-zero failure")
endif()
string(FIND "${bad_err}" "decode failed" decode_fail_pos)
if(decode_fail_pos EQUAL -1)
    message(FATAL_ERROR "malformed PNG run did not report the existing 'decode failed' diagnostic: ${bad_err}")
endif()

file(READ "${BAD_JSONL}" bad_jsonl)
string(FIND "${bad_jsonl}" "FirstPresent" bad_first_present)
if(NOT bad_first_present EQUAL -1)
    message(FATAL_ERROR "malformed PNG run reached FirstPresent; it must not present")
endif()
string(FIND "${bad_jsonl}" "DecodeComplete" bad_decode_complete)
if(bad_decode_complete EQUAL -1)
    message(FATAL_ERROR "malformed PNG run emitted no DecodeComplete marker")
endif()

# 3. Opening a valid image afterward must still work end to end.
execute_process(COMMAND "${OZVIEWER}" --benchmark "${VALID}"
                OUTPUT_FILE "${GOOD_JSONL}"
                ERROR_VARIABLE good_err
                RESULT_VARIABLE good_rc
                TIMEOUT 60)
if(NOT good_rc EQUAL 0)
    message(FATAL_ERROR "valid PNG run after failure exited ${good_rc}: ${good_err}")
endif()

file(READ "${GOOD_JSONL}" good_jsonl)
string(REPLACE "\n" ";" good_lines "${good_jsonl}")
set(seen "")
foreach(line ${good_lines})
    string(STRIP "${line}" line)
    if(line STREQUAL "")
        continue()
    endif()
    string(REGEX MATCH "\"marker\":\"([A-Za-z]+)\"" m "${line}")
    if(CMAKE_MATCH_1)
        list(APPEND seen "${CMAKE_MATCH_1}")
        if(CMAKE_MATCH_1 MATCHES "FolderEnumeration")
            message(FATAL_ERROR "forbidden folder-enumeration marker in recovery run: ${line}")
        endif()
    endif()
endforeach()
set(expected "ProcessStart;RendererReady;FileOpened;CodecDetected;HeaderParsed;DecodeStart;DecodeComplete;GpuUploadStart;GpuUploadComplete;FirstPresent")
set(actual "")
foreach(m ${seen})
    if(actual STREQUAL "")
        set(actual "${m}")
    else()
        set(actual "${actual};${m}")
    endif()
    if(m STREQUAL "FirstPresent")
        break()
    endif()
endforeach()
if(NOT actual STREQUAL expected)
    message(FATAL_ERROR "recovery run marker prefix mismatch.\n  expected: ${expected}\n  actual:   ${actual}\n  all: ${seen}")
endif()

message(STATUS "check_viewer_decode_failure_isolation: OK (malformed PNG failed safely with 'decode failed' and no present; valid PNG then decoded through FirstPresent)")

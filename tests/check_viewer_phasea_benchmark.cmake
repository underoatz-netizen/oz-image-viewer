# check_viewer_phasea_benchmark.cmake - Phase A integration benchmark test.
#
# Runs `ozviewer --benchmark <fixture>` for one Phase A fixture (BMP, static
# GIF, classic TIFF page 0, ICO deterministic entry) and asserts:
#   - a valid marker prefix through FirstPresent (ProcessStart..FirstPresent)
#   - no folder-enumeration markers
#   - automatic exit (the process terminates on its own after FirstPresent)
#
# Usage:
#   cmake -DOZVIEWER=<exe> -DFIXTURE=<file> -DOUT_DIR=<dir> -DOUT_NAME=<name>
#         -P check_viewer_phasea_benchmark.cmake
# Exits non-zero on any failure. Skips cleanly when ozviewer is not built.
if(NOT EXISTS "${OZVIEWER}")
    message(STATUS "check_viewer_phasea_benchmark: SKIP (ozviewer not built)")
    return()
endif()
if(NOT EXISTS "${FIXTURE}")
    message(STATUS "check_viewer_phasea_benchmark: SKIP (fixture missing)")
    return()
endif()
if(NOT OUT_NAME)
    set(OUT_NAME "viewer_benchmark_phasea.jsonl")
endif()

set(JSONL "${OUT_DIR}/${OUT_NAME}")

execute_process(COMMAND "${OZVIEWER}" --benchmark "${FIXTURE}"
                OUTPUT_FILE "${JSONL}"
                ERROR_VARIABLE err_text
                RESULT_VARIABLE run_rc
                TIMEOUT 60)
if(NOT run_rc EQUAL 0)
    message(FATAL_ERROR "ozviewer --benchmark <${FIXTURE}> exited with ${run_rc}: ${err_text}")
endif()

file(READ "${JSONL}" jsonl)
string(REPLACE "\n" ";" lines "${jsonl}")

set(seen_markers "")
set(found_first_present 0)
set(found_folder 0)
foreach(line ${lines})
    string(STRIP "${line}" line)
    if(line STREQUAL "")
        continue()
    endif()
    string(REGEX MATCH "\"marker\":\"([A-Za-z]+)\"" m "${line}")
    if(CMAKE_MATCH_1)
        list(APPEND seen_markers "${CMAKE_MATCH_1}")
        if(CMAKE_MATCH_1 STREQUAL "FirstPresent")
            set(found_first_present 1)
        endif()
        if(CMAKE_MATCH_1 MATCHES "FolderEnumeration")
            set(found_folder 1)
        endif()
    endif()
endforeach()

if(NOT found_first_present)
    message(FATAL_ERROR "FirstPresent marker missing for ${FIXTURE}; got: ${seen_markers}")
endif()
if(found_folder)
    message(FATAL_ERROR "folder-enumeration marker emitted for ${FIXTURE}: ${seen_markers}")
endif()

set(expected "ProcessStart;RendererReady;FileOpened;CodecDetected;HeaderParsed;DecodeStart;DecodeComplete;GpuUploadStart;GpuUploadComplete;FirstPresent")
set(actual "")
foreach(m ${seen_markers})
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
    message(FATAL_ERROR "Phase A marker prefix mismatch for ${FIXTURE}.\n  expected: ${expected}\n  actual:   ${actual}\n  all: ${seen_markers}")
endif()

message(STATUS "check_viewer_phasea_benchmark: OK (${FIXTURE})")

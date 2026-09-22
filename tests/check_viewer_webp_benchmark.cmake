# check_viewer_webp_benchmark.cmake - Stage 3.2 WebP integration benchmark test.
#
# Uses the checked-in decoder-independent lossless WebP fixture
# (tests/data/webp/bench_64x48.webp, pinned libwebp 1.6.0 bytes, no test
# encoder), runs `ozviewer --benchmark <webp>`, and asserts:
#   - a valid marker prefix through FirstPresent (ProcessStart..FirstPresent)
#   - no folder-enumeration markers
#   - automatic exit (the process terminates on its own after FirstPresent)
#
# Usage:
#   cmake -DOZVIEWER=<exe> -DWEBP_FIXTURE=<bench_64x48.webp> -DOUT_DIR=<dir>
#         -P check_viewer_webp_benchmark.cmake
# Exits non-zero on any failure. Skips cleanly when ozviewer is not built
# (e.g. the no-vcpkg / feature-disabled configuration) or WebP is disabled.
if(NOT EXISTS "${OZVIEWER}")
    message(STATUS "check_viewer_webp_benchmark: SKIP (ozviewer not built)")
    return()
endif()
if(NOT EXISTS "${WEBP_FIXTURE}")
    message(STATUS "check_viewer_webp_benchmark: SKIP (WebP fixture missing)")
    return()
endif()

set(JSONL "${OUT_DIR}/viewer_benchmark_webp.jsonl")

# 1. Run the viewer in benchmark mode on the checked-in fixture; it must
#    exit on its own.
execute_process(COMMAND "${OZVIEWER}" --benchmark "${WEBP_FIXTURE}"
                OUTPUT_FILE "${JSONL}"
                ERROR_VARIABLE err_text
                RESULT_VARIABLE run_rc
                TIMEOUT 60)
if(NOT run_rc EQUAL 0)
    message(FATAL_ERROR "ozviewer --benchmark <webp> exited with ${run_rc}: ${err_text}")
endif()

# 2. Parse the marker JSONL.
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
    # Extract the marker name.
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
    message(FATAL_ERROR "FirstPresent marker missing for WebP; got: ${seen_markers}")
endif()
if(found_folder)
    message(FATAL_ERROR "folder-enumeration marker emitted for WebP: ${seen_markers}")
endif()

# 3. Assert the canonical prefix order ProcessStart..FirstPresent.
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
    message(FATAL_ERROR "WebP marker prefix mismatch.\n  expected: ${expected}\n  actual:   ${actual}\n  all: ${seen_markers}")
endif()

message(STATUS "check_viewer_webp_benchmark: OK (valid prefix through FirstPresent, no folder markers, auto-exit)")

# check_viewer_png_benchmark.cmake - Stage 3.1 PNG integration benchmark test.
#
# Generates a small PNG fixture with the decoder-independent mini writer,
# runs `ozviewer --benchmark <png>`, and asserts:
#   - a valid marker prefix through FirstPresent (ProcessStart..FirstPresent)
#   - no folder-enumeration markers
#   - automatic exit (the process terminates on its own after FirstPresent)
#
# Usage:
#   cmake -DOZVIEWER=<exe> -DMAKE_PNG=<exe> -DOUT_DIR=<dir>
#         -P check_viewer_png_benchmark.cmake
# Exits non-zero on any failure.

if(NOT EXISTS "${OZVIEWER}")
    message(STATUS "check_viewer_png_benchmark: SKIP (ozviewer not built)")
    return()
endif()

set(PNG "${OUT_DIR}/viewer_benchmark_fixture.png")
set(JSONL "${OUT_DIR}/viewer_benchmark_png.jsonl")

# 1. Generate the PNG fixture (opaque RGBA gradient, decoder-independent).
execute_process(COMMAND "${MAKE_PNG}" "${PNG}" 64 48 rgba 0
                RESULT_VARIABLE gen_rc)
if(NOT gen_rc EQUAL 0)
    message(FATAL_ERROR "failed to generate PNG fixture")
endif()

# 2. Run the viewer in benchmark mode; it must exit on its own.
execute_process(COMMAND "${OZVIEWER}" --benchmark "${PNG}"
                OUTPUT_FILE "${JSONL}"
                ERROR_VARIABLE err_text
                RESULT_VARIABLE run_rc
                TIMEOUT 60)
if(NOT run_rc EQUAL 0)
    message(FATAL_ERROR "ozviewer --benchmark <png> exited with ${run_rc}: ${err_text}")
endif()

# 3. Parse the marker JSONL.
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
    message(FATAL_ERROR "FirstPresent marker missing for PNG; got: ${seen_markers}")
endif()
if(found_folder)
    message(FATAL_ERROR "folder-enumeration marker emitted for PNG: ${seen_markers}")
endif()

# 4. Assert the canonical prefix order ProcessStart..FirstPresent.
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
    message(FATAL_ERROR "PNG marker prefix mismatch.\n  expected: ${expected}\n  actual:   ${actual}\n  all: ${seen_markers}")
endif()

message(STATUS "check_viewer_png_benchmark: OK (valid prefix through FirstPresent, no folder markers, auto-exit)")

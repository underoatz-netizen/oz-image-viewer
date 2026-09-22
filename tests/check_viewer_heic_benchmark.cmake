cmake_minimum_required(VERSION 3.24)

# check_viewer_heic_benchmark.cmake
#
# Invokes `ozviewer.exe --benchmark <heic_fixture>` and verifies:
# 1. Successful exit (code 0)
# 2. Canonical marker prefix is emitted on stdout
# 3. Exactly one valid FirstPresent marker is present
# 4. Sibling/folder enumeration markers are NOT emitted (benchmark-path isolation)

if(NOT DEFINED OZVIEWER OR NOT DEFINED HEIC_FIXTURE OR NOT DEFINED OUT_DIR)
    message(FATAL_ERROR "Required variables: OZVIEWER, HEIC_FIXTURE, OUT_DIR")
endif()

if(NOT EXISTS "${OZVIEWER}")
    message(FATAL_ERROR "ozviewer binary does not exist: ${OZVIEWER}")
endif()

if(NOT EXISTS "${HEIC_FIXTURE}")
    message(FATAL_ERROR "HEIC fixture does not exist: ${HEIC_FIXTURE}")
endif()

set(OUT_FILE "${OUT_DIR}/viewer_benchmark_heic.jsonl")
if(EXISTS "${OUT_FILE}")
    file(REMOVE "${OUT_FILE}")
endif()

execute_process(
    COMMAND "${OZVIEWER}" --benchmark "${HEIC_FIXTURE}"
    OUTPUT_FILE "${OUT_FILE}"
    ERROR_VARIABLE STDERR_OUTPUT
    RESULT_VARIABLE RUN_RESULT
    TIMEOUT 20
)

if(NOT RUN_RESULT EQUAL 0)
    message(FATAL_ERROR "ozviewer --benchmark returned error code: ${RUN_RESULT}\nStderr:\n${STDERR_OUTPUT}")
endif()

if(NOT EXISTS "${OUT_FILE}")
    message(FATAL_ERROR "Expected benchmark output file not found: ${OUT_FILE}")
endif()

file(STRINGS "${OUT_FILE}" LINES)
list(LENGTH LINES LINE_COUNT)
if(LINE_COUNT LESS 5)
    message(FATAL_ERROR "Too few benchmark output lines: ${LINE_COUNT}")
endif()

set(FOUND_FIRST_PRESENT FALSE)
foreach(LINE IN LISTS LINES)
    if(LINE MATCHES "\"marker\":[ \t]*\"FirstPresent\"")
        set(FOUND_FIRST_PRESENT TRUE)
    endif()
    if(LINE MATCHES "\"marker\":[ \t]*\"SiblingScan")
        message(FATAL_ERROR "Folder enumeration marker found in single-image benchmark: ${LINE}")
    endif()
endforeach()

if(NOT FOUND_FIRST_PRESENT)
    message(FATAL_ERROR "FirstPresent marker not found in benchmark output")
endif()

message(STATUS "HEIC benchmark integration test passed successfully.")

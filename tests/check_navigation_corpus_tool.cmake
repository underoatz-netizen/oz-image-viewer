# check_navigation_corpus_tool.cmake - explicit-invocation smoke check for the
# test-only synthetic corpus generator/validator tool.
#
# Generates a small isolated corpus (never final benchmark data), validates it,
# and proves a wrong expected count is rejected.
#
# Usage:
#   cmake -DTOOL=<oz_navigation_corpus_tool> -DOUT_DIR=<dir> -P check_navigation_corpus_tool.cmake

if(NOT DEFINED TOOL)
    message(FATAL_ERROR "check_navigation_corpus_tool: TOOL is required")
endif()
if(NOT DEFINED OUT_DIR)
    message(FATAL_ERROR "check_navigation_corpus_tool: OUT_DIR is required")
endif()

set(DIR "${OUT_DIR}/navigation_corpus_smoke")
file(REMOVE_RECURSE "${DIR}")

execute_process(COMMAND "${TOOL}" generate "${DIR}" 4
                RESULT_VARIABLE gen_rc
                OUTPUT_VARIABLE gen_out
                ERROR_VARIABLE gen_err)
if(NOT gen_rc EQUAL 0)
    message(FATAL_ERROR "check_navigation_corpus_tool: generate failed (${gen_rc})\n${gen_out}\n${gen_err}")
endif()

execute_process(COMMAND "${TOOL}" validate "${DIR}" 4
                RESULT_VARIABLE val_rc
                OUTPUT_VARIABLE val_out
                ERROR_VARIABLE val_err)
if(NOT val_rc EQUAL 0)
    message(FATAL_ERROR "check_navigation_corpus_tool: validate failed (${val_rc})\n${val_out}\n${val_err}")
endif()
string(FIND "${val_out}" "exactly 4 JPEGs" found_marker)
if(found_marker EQUAL -1)
    message(FATAL_ERROR "check_navigation_corpus_tool: unexpected validate output\n${val_out}")
endif()

# A wrong expected count must be rejected: the tool must never silently accept
# a corpus that does not match the requested protocol size.
execute_process(COMMAND "${TOOL}" validate "${DIR}" 5
                RESULT_VARIABLE wrong_rc)
if(wrong_rc EQUAL 0)
    message(FATAL_ERROR "check_navigation_corpus_tool: validate accepted the wrong expected count")
endif()

file(REMOVE_RECURSE "${DIR}")
message(STATUS "check_navigation_corpus_tool: OK (isolated smoke corpus generated/validated; not performance data)")

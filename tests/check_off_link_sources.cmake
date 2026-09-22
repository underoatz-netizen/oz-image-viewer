# check_off_link_sources.cmake - source-scan CTest for the Editing-OFF link contract.
#
# Regression guard for the separately tracked Editing-OFF ozviewer.exe link
# failure. Three Editing-ON-only symbol families must never be referenced from
# unconditionally built viewer code:
#   1. read_file_stable_identity (owned by the always-built source_identity.cpp;
#      edit_controller.cpp must not be its sole owner);
#   2. the edit-pipeline materialization engine (materialize_edits /
#      get_system_edit_admission_budget) from the always-built copy path;
#   3. the save-pipeline extension helpers (save_format_for_extension /
#      preferred_extension) from the always-built save dialogs.
#
# Usage: cmake -DVIEWER_DIR=<src/viewer> -P check_off_link_sources.cmake

if(NOT DEFINED VIEWER_DIR)
    message(FATAL_ERROR "VIEWER_DIR not set")
endif()

set(VIEWER_CMAKE "${VIEWER_DIR}/CMakeLists.txt")
set(COPY_CPP "${VIEWER_DIR}/copy_controller.cpp")
set(EDIT_CPP "${VIEWER_DIR}/edit_controller.cpp")
set(EDIT_HPP "${VIEWER_DIR}/edit_controller.hpp")
set(IDENTITY_CPP "${VIEWER_DIR}/source_identity.cpp")
set(IDENTITY_HPP "${VIEWER_DIR}/source_identity.hpp")
set(DIALOGS_CPP "${VIEWER_DIR}/modern_dialogs.cpp")
set(MAIN_CPP "${VIEWER_DIR}/main.cpp")

foreach(f IN ITEMS "${VIEWER_CMAKE}" "${COPY_CPP}" "${EDIT_CPP}" "${EDIT_HPP}"
                  "${IDENTITY_CPP}" "${IDENTITY_HPP}" "${DIALOGS_CPP}" "${MAIN_CPP}")
    if(NOT EXISTS "${f}")
        message(FATAL_ERROR "required file not found: ${f}")
    endif()
endforeach()

file(READ "${VIEWER_CMAKE}" cmake_content)
file(READ "${COPY_CPP}" copy_content)
file(READ "${EDIT_HPP}" edit_hpp_content)
file(READ "${EDIT_CPP}" edit_cpp_content)
file(READ "${IDENTITY_CPP}" identity_cpp_content)
file(READ "${IDENTITY_HPP}" identity_hpp_content)
file(READ "${DIALOGS_CPP}" dialogs_content)
file(READ "${MAIN_CPP}" main_content)

# 1. The copy worker depends on the stable-identity helper.
string(FIND "${copy_content}" "read_file_stable_identity" copy_dep)
if(copy_dep EQUAL -1)
    message(FATAL_ERROR "copy_controller.cpp no longer calls read_file_stable_identity (scan assumption changed)")
endif()

# 1a. The shared implementation must provide the helper.
foreach(sym IN ITEMS "read_file_stable_identity" "verify_source_file_metadata" "scoped_stable_read_hook")
    string(FIND "${identity_cpp_content}" "${sym}" pos_cpp)
    if(pos_cpp EQUAL -1)
        message(FATAL_ERROR "source_identity.cpp does not define ${sym}")
    endif()
    string(FIND "${identity_hpp_content}" "${sym}" pos_hpp)
    if(pos_hpp EQUAL -1)
        message(FATAL_ERROR "source_identity.hpp does not declare ${sym}")
    endif()
endforeach()

# 1b. edit_controller.cpp must not be the sole owner of the helper.
string(FIND "${edit_cpp_content}" "bool read_file_stable_identity" edit_defines_helper)
if(NOT edit_defines_helper EQUAL -1)
    message(FATAL_ERROR "edit_controller.cpp still defines read_file_stable_identity; it must live only in source_identity.cpp")
endif()

# 2. The edit-pipeline engine must be gated out of the Editing-OFF copy path.
string(FIND "${copy_content}" "#if OZVIEWER_ENABLE_EDITING" copy_gate)
if(copy_gate EQUAL -1)
    message(FATAL_ERROR "copy_controller.cpp has no OZVIEWER_ENABLE_EDITING gate for the materialization branch")
endif()
string(FIND "${copy_content}" "materialize_edits" copy_mat)
if(copy_mat LESS copy_gate)
    message(FATAL_ERROR "copy_controller.cpp references materialize_edits before/without the editing gate")
endif()
string(FIND "${copy_content}" "edit commands not supported when editing is disabled" copy_off_fallback)
if(copy_off_fallback EQUAL -1)
    message(FATAL_ERROR "copy_controller.cpp lost its Editing-OFF fail-closed branch")
endif()

# 2a. The Editing-OFF copy submission must not consume the pipeline budget helper.
string(FIND "${main_content}" "req.memory_budget = 0;" main_off_budget)
if(main_off_budget EQUAL -1)
    message(FATAL_ERROR "main.cpp copy path lost its Editing-OFF zero-budget branch")
endif()

# 3. The save dialogs (built unconditionally) must decline without linking the
#    save-pipeline helpers when editing is off.
string(FIND "${dialogs_content}" "Save is an Editing-ON feature" dialogs_stub)
if(dialogs_stub EQUAL -1)
    message(FATAL_ERROR "modern_dialogs.cpp lost its Editing-OFF save-dialog stubs")
endif()

# 4. The viewer build must compile the shared helper unconditionally, keep the
#    copy worker unconditional, and keep the edit worker behind the editing gate.
string(FIND "${cmake_content}" "if(OZVIEWER_ENABLE_EDITING)" gate_pos)
if(gate_pos EQUAL -1)
    message(FATAL_ERROR "src/viewer/CMakeLists.txt editing gate not found")
endif()
string(SUBSTRING "${cmake_content}" 0 ${gate_pos} cmake_before_gate)
string(FIND "${cmake_before_gate}" "source_identity.cpp" identity_unconditional)
if(identity_unconditional EQUAL -1)
    message(FATAL_ERROR "source_identity.cpp is not in the unconditional viewer sources; Editing-OFF would not link it")
endif()
string(FIND "${cmake_before_gate}" "copy_controller.cpp" copy_unconditional)
if(copy_unconditional EQUAL -1)
    message(FATAL_ERROR "copy_controller.cpp is not in the unconditional viewer sources (scan assumption changed)")
endif()
string(FIND "${cmake_before_gate}" "edit_controller.cpp" edit_unconditional)
if(NOT edit_unconditional EQUAL -1)
    message(FATAL_ERROR "edit_controller.cpp must stay behind the OZVIEWER_ENABLE_EDITING gate")
endif()

# 5. The Editing-ON public include path must stay intact: edit_controller.hpp
#    still exposes the helpers (via the shared header).
string(FIND "${edit_hpp_content}" "source_identity.hpp" edit_reexports)
if(edit_reexports EQUAL -1)
    message(FATAL_ERROR "edit_controller.hpp no longer includes source_identity.hpp; Editing-ON include path changed")
endif()

message(STATUS "check_off_link_sources: OK (OFF link contract holds; edit/save engines stay gated)")

# check_print_isolation.cmake - source-scan CTest for the Stage 9.8 print
# isolation contract: an interactive-only single-flight print controller that
# is separate from Save/the renderer, shows no dialog and performs no GDI
# printing, worker, or source acquisition work in benchmark or
# navigation-benchmark modes, and never reuses the Save pipeline or renderer
# readback.
#
# Usage: cmake -DMAIN_CPP=<src/viewer/main.cpp> -DVIEWER_DIR=<src/viewer>
#        -P check_print_isolation.cmake

if(NOT EXISTS "${MAIN_CPP}")
    message(FATAL_ERROR "main.cpp not found: ${MAIN_CPP}")
endif()
if(NOT DEFINED VIEWER_DIR)
    message(FATAL_ERROR "VIEWER_DIR not set")
endif()

set(PRINT_JOB_HPP "${VIEWER_DIR}/print_job.hpp")
set(PRINT_CTRL_HPP "${VIEWER_DIR}/print_controller.hpp")
set(PRINT_CTRL_CPP "${VIEWER_DIR}/print_controller.cpp")
set(PRINT_DLG_HPP "${VIEWER_DIR}/print_dialog.hpp")
set(PRINT_DLG_CPP "${VIEWER_DIR}/print_dialog.cpp")
set(CMD_STATE_HPP "${VIEWER_DIR}/ui_command_state.hpp")
set(CMD_STATE_CPP "${VIEWER_DIR}/ui_command_state.cpp")
set(VIEWER_CMAKE "${VIEWER_DIR}/CMakeLists.txt")
foreach(f IN ITEMS "${PRINT_JOB_HPP}" "${PRINT_CTRL_HPP}" "${PRINT_CTRL_CPP}"
                   "${PRINT_DLG_HPP}" "${PRINT_DLG_CPP}"
                   "${CMD_STATE_HPP}" "${CMD_STATE_CPP}" "${VIEWER_CMAKE}")
    if(NOT EXISTS "${f}")
        message(FATAL_ERROR "required Stage 9.8 file not found: ${f}")
    endif()
endforeach()

file(READ "${MAIN_CPP}" content)
file(READ "${PRINT_CTRL_CPP}" print_content)
file(READ "${PRINT_DLG_CPP}" print_dlg_content)
file(READ "${CMD_STATE_HPP}" cmd_hpp_content)
file(READ "${CMD_STATE_CPP}" cmd_cpp_content)
file(READ "${VIEWER_CMAKE}" cmake_content)

# 1. The print entry point exists and refuses both non-interactive modes first.
string(FIND "${content}" "static void start_print(app_state& app) {" start_pos)
if(start_pos EQUAL -1)
    message(FATAL_ERROR "start_print definition not found in main.cpp")
endif()
string(SUBSTRING "${content}" ${start_pos} 300 start_window)
string(FIND "${start_window}" "app.benchmark || app.nav_benchmark" start_guard_pos)
if(start_guard_pos EQUAL -1)
    message(FATAL_ERROR "start_print must guard against benchmark and nav_benchmark")
endif()

# Delimit the whole start_print definition for ownership assertions.
string(SUBSTRING "${content}" ${start_pos} -1 start_tail)
string(FIND "${start_tail}" "\n}" start_end_rel)
if(start_end_rel EQUAL -1)
    message(FATAL_ERROR "could not delimit the start_print body")
endif()
string(SUBSTRING "${content}" ${start_pos} ${start_end_rel} start_body)

# 2. Explicit Print dialog helper: show_print_dialog with owner HWND, and
#    print_dialog.cpp owning the single explicit PrintDlgExW site.
string(FIND "${start_body}" "show_print_dialog(app.hwnd)" dlg_call_pos)
if(dlg_call_pos EQUAL -1)
    message(FATAL_ERROR "start_print must call show_print_dialog(app.hwnd)")
endif()
string(FIND "${content}" "PrintDlgExW(" main_dlg_pos)
if(NOT main_dlg_pos EQUAL -1)
    message(FATAL_ERROR "PrintDlgExW must not appear in main.cpp; print_dialog.cpp owns the single dialog site")
endif()
string(FIND "${print_dlg_content}" "PrintDlgExW(" dlg_pos)
if(dlg_pos EQUAL -1)
    message(FATAL_ERROR "PrintDlgExW not found in print_dialog.cpp")
endif()
string(FIND "${print_dlg_content}" "PrintDlgExW(" dlg_second_pos REVERSE)
if(NOT dlg_second_pos EQUAL dlg_pos)
    message(FATAL_ERROR "PrintDlgExW must appear exactly once in print_dialog.cpp (single explicit-Print site)")
endif()
foreach(marker IN ITEMS "PD_RETURNDC" "pdx.dwResultAction == PD_RESULT_PRINT")
    string(FIND "${print_dlg_content}" "${marker}" marker_pos)
    if(marker_pos EQUAL -1)
        message(FATAL_ERROR "print_dialog.cpp lost its ${marker} dialog contract")
    endif()
endforeach()

# 3. Dialog/DC ownership is always released before COM teardown; dialog cancel
#    and apply-only are quiet.
foreach(marker IN ITEMS "fn_global_free(pdx.hDevMode)" "fn_global_free(pdx.hDevNames)"
                        "fn_delete_dc(pdx.hDC)")
    string(FIND "${print_dlg_content}" "${marker}" marker_pos)
    if(marker_pos EQUAL -1)
        message(FATAL_ERROR "print_dialog.cpp lost its ${marker} ownership/cleanup contract")
    endif()
endforeach()

# 3a. The dialog helper must scope and balance apartment-threaded COM around
#     the one dialog call: S_OK and S_FALSE both require one CoUninitialize,
#     while init failure (including RPC_E_CHANGED_MODE) must not uninitialize.
foreach(marker IN ITEMS "fn_co_init(nullptr, COINIT_APARTMENTTHREADED)"
                        "hr_init == S_OK || hr_init == S_FALSE"
                        "fn_co_uninit();")
    string(FIND "${print_dlg_content}" "${marker}" marker_pos)
    if(marker_pos EQUAL -1)
        message(FATAL_ERROR "print_dialog.cpp lost its COM STA lifecycle contract: ${marker}")
    endif()
endforeach()

# 4. No GDI printing, spooling, or printer discovery in main.cpp. The printer
#    DC arrives only from the dialog; spooling lives in print_controller.cpp.
#    (StartPage/EndPage/EndDoc match with a paren so the PRINTDLGEX nStartPage
#    field assignment is not a false positive.)
foreach(sym IN ITEMS "StartDocW" "StartPage(" "StretchDIBits" "EndPage(" "EndDoc("
                     "AbortDoc" "CreateDCW" "CreateICW" "OpenPrinterW")
    string(FIND "${content}" "${sym}" sym_pos)
    if(NOT sym_pos EQUAL -1)
        message(FATAL_ERROR "Direct ${sym} in main.cpp; GDI spool work must stay in print_controller.cpp and discovery must come only from PrintDlgExW")
    endif()
endforeach()

# 5. The spool worker owns the real GDI calls plus AbortDoc, never the Save
#    pipeline and never renderer readback.
foreach(sym IN ITEMS "StartDocW" "StretchDIBits" "EndDoc" "AbortDoc" "DeleteDC")
    string(FIND "${print_content}" "${sym}" sym_pos)
    if(sym_pos EQUAL -1)
        message(FATAL_ERROR "print_controller.cpp does not own ${sym} (worker assumption changed)")
    endif()
endforeach()
foreach(sym IN ITEMS "execute_save" "redraw_retained_plan" "mark_saved" "push_command")
    string(FIND "${print_content}" "${sym}" sym_pos)
    if(NOT sym_pos EQUAL -1)
        message(FATAL_ERROR "${sym} found in print_controller.cpp; the print worker must not reuse Save or mutate renderer/history/checkpoint state")
    endif()
endforeach()

# 6. The edit-pipeline engine stays gated out of the Editing-OFF print path.
string(FIND "${print_content}" "#if OZVIEWER_ENABLE_EDITING" print_gate)
if(print_gate EQUAL -1)
    message(FATAL_ERROR "print_controller.cpp has no OZVIEWER_ENABLE_EDITING gate for the materialization branch")
endif()
string(FIND "${print_content}" "materialize_edits" print_mat)
if(print_mat LESS print_gate)
    message(FATAL_ERROR "print_controller.cpp references materialize_edits before/without the editing gate")
endif()
string(FIND "${print_content}" "edit commands not supported when editing is disabled" print_off_fallback)
if(print_off_fallback EQUAL -1)
    message(FATAL_ERROR "print_controller.cpp lost its Editing-OFF fail-closed branch")
endif()
string(FIND "${content}" "req.memory_budget = 0;" main_off_budget)
if(main_off_budget EQUAL -1)
    message(FATAL_ERROR "main.cpp print path lost its Editing-OFF zero-budget branch")
endif()

# 7. Single-flight lazy controller: exactly one construction site, inside
#    start_print (never at startup), wired to the print completion message.
set(marker "std::make_unique<oz::viewer::print_controller>")
string(FIND "${content}" "${marker}" ctrl_pos)
if(ctrl_pos EQUAL -1)
    message(FATAL_ERROR "print_controller instantiation not found in main.cpp")
endif()
string(FIND "${content}" "${marker}" ctrl_second_pos REVERSE)
if(NOT ctrl_second_pos EQUAL ctrl_pos)
    message(FATAL_ERROR "print_controller must be instantiated exactly once in main.cpp")
endif()
if(NOT start_pos LESS ctrl_pos)
    message(FATAL_ERROR "print_controller must be created lazily inside start_print, never at startup (no startup printer discovery)")
endif()
string(FIND "${content}" "app.print_ctrl->start(app.hwnd, kMsgPrintComplete)" ctrl_start_pos)
if(ctrl_start_pos EQUAL -1)
    message(FATAL_ERROR "print controller is not wired to kMsgPrintComplete")
endif()

# 8. Completion handling: request-identity gate, matching document only, and
#    no source/history/dirty/save-checkpoint mutation on any path.
string(FIND "${content}" "static void handle_print_completion(app_state& app," handle_pos)
if(handle_pos EQUAL -1)
    message(FATAL_ERROR "handle_print_completion definition not found in main.cpp")
endif()
string(SUBSTRING "${content}" ${handle_pos} -1 handle_tail)
string(FIND "${handle_tail}" "\n}" handle_end_rel)
if(handle_end_rel EQUAL -1)
    message(FATAL_ERROR "could not delimit the handle_print_completion body")
endif()
string(SUBSTRING "${content}" ${handle_pos} ${handle_end_rel} handle_body)
string(FIND "${handle_body}" "print_completion_is_for_pending" identity_pos)
if(identity_pos EQUAL -1)
    message(FATAL_ERROR "handle_print_completion must gate on print_completion_is_for_pending")
endif()
foreach(sym IN ITEMS "mark_saved" "push_command" "execute_save" "update_source_file_metadata"
                     "history().undo" "history().redo")
    string(FIND "${handle_body}" "${sym}" sym_pos)
    if(NOT sym_pos EQUAL -1)
        message(FATAL_ERROR "${sym} in handle_print_completion; print completions must never mutate source/history/dirty/save state")
    endif()
endforeach()

# 9. WM_COMMAND File Print dispatch is guarded by noninteractive_mode and the
#    command state.
string(FIND "${content}" "if (id == kIdFilePrint) {" cmd_pos)
if(cmd_pos EQUAL -1)
    message(FATAL_ERROR "kIdFilePrint handler not found in main.cpp")
endif()
string(SUBSTRING "${content}" ${cmd_pos} 600 cmd_window)
string(FIND "${cmd_window}" "noninteractive_mode(*app)" cmd_guard_pos)
if(cmd_guard_pos EQUAL -1)
    message(FATAL_ERROR "kIdFilePrint handler must be guarded by noninteractive_mode(*app)")
endif()
string(FIND "${cmd_window}" "start_print(*app)" cmd_start_pos)
if(cmd_start_pos EQUAL -1)
    message(FATAL_ERROR "kIdFilePrint handler must route to start_print(*app)")
endif()

# 10. Ctrl+P shortcut is guarded by noninteractive_mode, routes through the
#     command state, and defers to the modal Resize dialog in the prefilter.
string(FIND "${content}" "wparam == 'P'" key_pos)
if(key_pos EQUAL -1)
    message(FATAL_ERROR "WM_KEYDOWN Ctrl+P handler not found in main.cpp")
endif()
string(SUBSTRING "${content}" ${key_pos} 700 key_window)
string(FIND "${key_window}" "noninteractive_mode(*app)" key_guard_pos)
if(key_guard_pos EQUAL -1)
    message(FATAL_ERROR "WM_KEYDOWN Ctrl+P dispatch must be guarded by noninteractive_mode(*app)")
endif()
string(FIND "${content}" "msg.wParam == 'P'" prefilter_pos)
if(prefilter_pos EQUAL -1)
    message(FATAL_ERROR "message-loop prefilter does not handle the 'P' key")
endif()
string(SUBSTRING "${content}" ${prefilter_pos} 1200 prefilter_window)
string(FIND "${prefilter_window}" "IsChild(resize_hwnd, focused)" prefilter_dialog_pos)
if(prefilter_dialog_pos EQUAL -1)
    message(FATAL_ERROR "message-loop prefilter must defer Ctrl+P to the modal Resize dialog")
endif()

# 11. The print completion message is ignored in both benchmark modes.
string(FIND "${content}" "case kMsgPrintComplete:" complete_pos)
if(complete_pos EQUAL -1)
    message(FATAL_ERROR "kMsgPrintComplete handler not found in main.cpp")
endif()
string(SUBSTRING "${content}" ${complete_pos} 300 complete_window)
string(FIND "${complete_window}" "app->benchmark || app->nav_benchmark" complete_guard_pos)
if(complete_guard_pos EQUAL -1)
    message(FATAL_ERROR "kMsgPrintComplete handler must guard against benchmark and nav_benchmark")
endif()

# 12. The File menu Print item is created only in the interactive menu block.
string(FIND "${content}" "kIdFilePrint, L\"&Print...\\tCtrl+P\"" menu_pos)
if(menu_pos EQUAL -1)
    message(FATAL_ERROR "File menu Print item not found in main.cpp")
endif()
math(EXPR menu_window_start "${menu_pos} - 2000")
if(menu_window_start LESS 0)
    set(menu_window_start 0)
endif()
string(SUBSTRING "${content}" ${menu_window_start} 2000 menu_window)
string(FIND "${menu_window}" "!benchmark && !nav_benchmark" menu_guard_pos)
if(menu_guard_pos EQUAL -1)
    message(FATAL_ERROR "File menu Print item is not inside the interactive (!benchmark && !nav_benchmark) menu block")
endif()

# 13. Command-state seam: file_print id, print_busy context, and derivation.
string(FIND "${cmd_hpp_content}" "file_print = 110" id_pos)
if(id_pos EQUAL -1)
    message(FATAL_ERROR "ui_command_id::file_print = 110 not found")
endif()
string(FIND "${cmd_hpp_content}" "print_busy" busy_pos)
if(busy_pos EQUAL -1)
    message(FATAL_ERROR "ui_command_context lost its print_busy field")
endif()
string(FIND "${cmd_cpp_content}" "ui_command_id::file_print" derive_pos)
if(derive_pos EQUAL -1)
    message(FATAL_ERROR "derive_command_state has no file_print case")
endif()

# 14. The viewer build keeps the print worker unconditional (Editing-OFF
#     prints too) while the edit worker stays behind the editing gate.
string(FIND "${cmake_content}" "if(OZVIEWER_ENABLE_EDITING)" gate_pos)
if(gate_pos EQUAL -1)
    message(FATAL_ERROR "src/viewer/CMakeLists.txt editing gate not found")
endif()
string(SUBSTRING "${cmake_content}" 0 ${gate_pos} cmake_before_gate)
string(FIND "${cmake_before_gate}" "print_controller.cpp" print_unconditional)
if(print_unconditional EQUAL -1)
    message(FATAL_ERROR "print_controller.cpp is not in the unconditional viewer sources; Editing-OFF would not link it")
endif()
string(FIND "${cmake_before_gate}" "print_dialog.cpp" dialog_unconditional)
if(dialog_unconditional EQUAL -1)
    message(FATAL_ERROR "print_dialog.cpp is not in the unconditional viewer sources; Editing-OFF would not link it")
endif()

# 15. Actual command-context integration guard: current_ui_command_context
#     must wire ctx.print_busy directly to is_print_active(app), which
#     subsumes both print_dialog_in_progress and pending/in-flight print.
string(FIND "${content}" "static oz::viewer::ui_command_context current_ui_command_context(const app_state& app)" ctx_fn_pos)
if(ctx_fn_pos EQUAL -1)
    message(FATAL_ERROR "current_ui_command_context definition not found in main.cpp")
endif()
string(SUBSTRING "${content}" ${ctx_fn_pos} -1 ctx_tail)
string(FIND "${ctx_tail}" "\n}" ctx_end_rel)
if(ctx_end_rel EQUAL -1)
    message(FATAL_ERROR "could not delimit current_ui_command_context body")
endif()
string(SUBSTRING "${content}" ${ctx_fn_pos} ${ctx_end_rel} ctx_body)
string(FIND "${ctx_body}" "ctx.print_busy = is_print_active(app);" ctx_print_busy_pos)
if(ctx_print_busy_pos EQUAL -1)
    message(FATAL_ERROR "current_ui_command_context must assign ctx.print_busy = is_print_active(app);")
endif()

# Also verify is_print_active includes print_dialog_in_progress.
string(FIND "${content}" "static bool is_print_active(const app_state& app) {" is_print_fn_pos)
if(is_print_fn_pos EQUAL -1)
    message(FATAL_ERROR "is_print_active definition not found in main.cpp")
endif()
string(SUBSTRING "${content}" ${is_print_fn_pos} -1 is_print_tail)
string(FIND "${is_print_tail}" "\n}" is_print_end_rel)
if(is_print_end_rel EQUAL -1)
    message(FATAL_ERROR "could not delimit is_print_active body")
endif()
string(SUBSTRING "${content}" ${is_print_fn_pos} ${is_print_end_rel} is_print_body)
string(FIND "${is_print_body}" "app.print_dialog_in_progress" flag_pos)
if(flag_pos EQUAL -1)
    message(FATAL_ERROR "is_print_active must include app.print_dialog_in_progress")
endif()

# 16. start_print must install the RAII reentrancy guard before presenting the
#     dialog and only release it when the pending request is safely installed.
foreach(marker IN ITEMS "basic_print_attempt_guard dlg_guard(app"
                        "dlg_guard.disengage_and_preserve();")
    string(FIND "${start_body}" "${marker}" guard_pos)
    if(guard_pos EQUAL -1)
        message(FATAL_ERROR "start_print lost its print reentrancy guard contract: ${marker}")
    endif()
endforeach()

message(STATUS "check_print_isolation: OK (explicit-Print single-flight worker, dialog/DC ownership, benchmark isolation, no Save/renderer reuse)")

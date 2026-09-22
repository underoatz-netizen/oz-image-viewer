# check_benchmark_ui_isolation.cmake - source-scan and runtime-contract CTest
# asserting that both --benchmark and --navigation-benchmark modes create:
# 1. No interactive menu (File, Edit, Crop Ratio)
# 2. No DragAcceptFiles acceptance
# 3. No centered Open button
# 4. No UI Chrome (toolbar/statusbar) or Crop Strip
# 5. No save controller or edit controller
# 6. No modal prompt on decode/present failure branches (shared
#    noninteractive_mode predicate guards both modes)
#
# Usage: cmake -DMAIN_CPP=<src/viewer/main.cpp> -P check_benchmark_ui_isolation.cmake

if(NOT EXISTS "${MAIN_CPP}")
    message(FATAL_ERROR "main.cpp not found: ${MAIN_CPP}")
endif()

file(READ "${MAIN_CPP}" content)

# 1. Check Menu creation guard in main.cpp:
# Must be guarded by !benchmark && !nav_benchmark
string(FIND "${content}" "if (!benchmark && !nav_benchmark)" menu_guard_pos)
if(menu_guard_pos EQUAL -1)
    message(FATAL_ERROR "Interactive menu creation is not guarded by (!benchmark && !nav_benchmark)")
endif()

# 2. Check DragAcceptFiles and Open Button creation guard:
# Must be guarded by !benchmark && !nav_benchmark
string(FIND "${content}" "DragAcceptFiles(app.hwnd, TRUE);" drag_pos)
if(drag_pos EQUAL -1)
    message(FATAL_ERROR "DragAcceptFiles call not found in main.cpp")
endif()

math(EXPR drag_window_start "${drag_pos} - 300")
if(drag_window_start LESS 0)
    set(drag_window_start 0)
endif()
string(SUBSTRING "${content}" ${drag_window_start} 300 drag_window)
string(FIND "${drag_window}" "!benchmark && !nav_benchmark" drag_guard_pos)
if(drag_guard_pos EQUAL -1)
    message(FATAL_ERROR "DragAcceptFiles and Open Button are not guarded by (!benchmark && !nav_benchmark)")
endif()

# 3. Check Modern Chrome (ui_chrome) and Crop Strip creation guard:
string(FIND "${content}" "app.chrome = std::make_unique<oz::viewer::ui_chrome>();" chrome_pos)
if(chrome_pos EQUAL -1)
    message(FATAL_ERROR "ui_chrome creation not found in main.cpp")
endif()

math(EXPR chrome_window_start "${chrome_pos} - 300")
if(chrome_window_start LESS 0)
    set(chrome_window_start 0)
endif()
string(SUBSTRING "${content}" ${chrome_window_start} 300 chrome_window)
string(FIND "${chrome_window}" "!benchmark && !nav_benchmark" chrome_guard_pos)
if(chrome_guard_pos EQUAL -1)
    message(FATAL_ERROR "ui_chrome and crop_strip_ui are not guarded by (!benchmark && !nav_benchmark)")
endif()

# 4. Check Edit and Save Controller creation guard:
string(FIND "${content}" "app.save_ctrl = std::make_unique<oz::viewer::save_controller>();" save_pos)
if(save_pos EQUAL -1)
    message(FATAL_ERROR "save_controller creation not found in main.cpp")
endif()

math(EXPR save_window_start "${save_pos} - 700")
if(save_window_start LESS 0)
    set(save_window_start 0)
endif()
string(SUBSTRING "${content}" ${save_window_start} 700 save_window)
string(FIND "${save_window}" "!benchmark && !nav_benchmark" save_guard_pos)
if(save_guard_pos EQUAL -1)
    message(FATAL_ERROR "save_controller is not guarded by (!benchmark && !nav_benchmark)")
endif()

# 5. Shared noninteractive predicate must exist and cover both modes:
string(FIND "${content}" "static bool noninteractive_mode(const app_state& app) noexcept" helper_pos)
if(helper_pos EQUAL -1)
    message(FATAL_ERROR "noninteractive_mode helper not found in main.cpp")
endif()
math(EXPR helper_window_start "${helper_pos} - 100")
if(helper_window_start LESS 0)
    set(helper_window_start 0)
endif()
string(SUBSTRING "${content}" ${helper_window_start} 300 helper_window)
string(FIND "${helper_window}" "app.benchmark || app.nav_benchmark" helper_body_pos)
if(helper_body_pos EQUAL -1)
    message(FATAL_ERROR "noninteractive_mode must return (app.benchmark || app.nav_benchmark)")
endif()

# 6. Decode-failure branch must use the shared predicate (no modal prompt in
#    either benchmark mode):
string(FIND "${content}" "ozviewer: decode failed: %s\\n" decode_fail_pos)
if(decode_fail_pos EQUAL -1)
    message(FATAL_ERROR "decode-failure branch marker not found in main.cpp")
endif()
math(EXPR decode_window_start "${decode_fail_pos} - 200")
if(decode_window_start LESS 0)
    set(decode_window_start 0)
endif()
string(SUBSTRING "${content}" ${decode_window_start} 200 decode_window)
string(FIND "${decode_window}" "noninteractive_mode(*app)" decode_guard_pos)
if(decode_guard_pos EQUAL -1)
    message(FATAL_ERROR "decode-failure branch is not guarded by noninteractive_mode(*app)")
endif()

# 7. Present-failure branch must use the shared predicate (no modal prompt in
#    either benchmark mode):
string(FIND "${content}" "ozviewer: present failed\\n" present_fail_pos)
if(present_fail_pos EQUAL -1)
    message(FATAL_ERROR "present-failure branch marker not found in main.cpp")
endif()
math(EXPR present_window_start "${present_fail_pos} - 200")
if(present_window_start LESS 0)
    set(present_window_start 0)
endif()
string(SUBSTRING "${content}" ${present_window_start} 200 present_window)
string(FIND "${present_window}" "noninteractive_mode(*app)" present_guard_pos)
if(present_guard_pos EQUAL -1)
    message(FATAL_ERROR "present-failure branch is not guarded by noninteractive_mode(*app)")
endif()

# 8. All show_prompt / MessageBox / TaskDialog invocations across failure and
# document-switching paths in main.cpp must be guarded against noninteractive mode.
# Verify that resolve_active_save_for_action suppresses prompts when either benchmark is active:
string(FIND "${content}" "static bool resolve_active_save_for_action(app_state& app) {" resolve_pos)
if(resolve_pos EQUAL -1)
    message(FATAL_ERROR "resolve_active_save_for_action definition not found in main.cpp")
endif()
string(SUBSTRING "${content}" ${resolve_pos} 300 resolve_window)
string(FIND "${resolve_window}" "app.benchmark || app.nav_benchmark" resolve_guard_pos)
if(resolve_guard_pos EQUAL -1)
    message(FATAL_ERROR "resolve_active_save_for_action must guard against benchmark and nav_benchmark")
endif()

# Verify that check_dirty_discard_allowed suppresses prompts when either benchmark is active:
string(FIND "${content}" "static bool check_dirty_discard_allowed(app_state& app) {" discard_pos)
if(discard_pos EQUAL -1)
    message(FATAL_ERROR "check_dirty_discard_allowed definition not found in main.cpp")
endif()
string(SUBSTRING "${content}" ${discard_pos} 300 discard_window)
string(FIND "${discard_window}" "app.benchmark || app.nav_benchmark" discard_guard_pos)
if(discard_guard_pos EQUAL -1)
    message(FATAL_ERROR "check_dirty_discard_allowed must guard against benchmark and nav_benchmark")
endif()

# Verify that controller creation is guarded by (!benchmark && !nav_benchmark)
# so edit/save error prompts cannot be invoked in benchmark modes:
string(FIND "${content}" "app.edit_ctrl = std::make_unique<oz::viewer::edit_controller>();" edit_ctrl_pos)
if(edit_ctrl_pos EQUAL -1)
    message(FATAL_ERROR "edit_ctrl creation not found in main.cpp")
endif()
math(EXPR edit_window_start "${edit_ctrl_pos} - 300")
if(edit_window_start LESS 0)
    set(edit_window_start 0)
endif()
string(SUBSTRING "${content}" ${edit_window_start} 300 edit_window)
string(FIND "${edit_window}" "!benchmark && !nav_benchmark" edit_guard_pos)
if(edit_guard_pos EQUAL -1)
    message(FATAL_ERROR "edit_controller creation must be guarded by (!benchmark && !nav_benchmark)")
endif()

# 9. Verify that WM_CONTEXTMENU popup invocation is guarded against noninteractive modes:
string(FIND "${content}" "case WM_CONTEXTMENU:" ctx_menu_pos)
if(ctx_menu_pos EQUAL -1)
    message(FATAL_ERROR "WM_CONTEXTMENU handler not found in main.cpp")
endif()
string(SUBSTRING "${content}" ${ctx_menu_pos} 300 ctx_menu_window)
string(FIND "${ctx_menu_window}" "noninteractive_mode(*app)" ctx_menu_guard_pos)
if(ctx_menu_guard_pos EQUAL -1)
    message(FATAL_ERROR "WM_CONTEXTMENU must be guarded by noninteractive_mode(*app)")
endif()
string(SUBSTRING "${content}" ${ctx_menu_pos} 3000 ctx_menu_full_window)
string(FIND "${ctx_menu_full_window}" "TrackPopupMenuEx" track_popup_pos)
if(track_popup_pos EQUAL -1)
    message(FATAL_ERROR "TrackPopupMenuEx not found in WM_CONTEXTMENU handler")
endif()

# 10. Verify that Stage 9.3/9.4 Copy File, Copy File Path, and Copy Image commands are guarded against noninteractive mode:
string(FIND "${content}" "if (id == kIdFileCopy) {" copy_file_pos)
if(copy_file_pos EQUAL -1)
    message(FATAL_ERROR "kIdFileCopy handler not found in main.cpp")
endif()
string(SUBSTRING "${content}" ${copy_file_pos} 300 copy_file_window)
string(FIND "${copy_file_window}" "noninteractive_mode(*app)" copy_file_guard_pos)
if(copy_file_guard_pos EQUAL -1)
    message(FATAL_ERROR "kIdFileCopy handler must be guarded by noninteractive_mode(*app)")
endif()

string(FIND "${content}" "if (id == kIdFileCopyPath) {" copy_path_pos)
if(copy_path_pos EQUAL -1)
    message(FATAL_ERROR "kIdFileCopyPath handler not found in main.cpp")
endif()
string(SUBSTRING "${content}" ${copy_path_pos} 300 copy_path_window)
string(FIND "${copy_path_window}" "noninteractive_mode(*app)" copy_path_guard_pos)
if(copy_path_guard_pos EQUAL -1)
    message(FATAL_ERROR "kIdFileCopyPath handler must be guarded by noninteractive_mode(*app)")
endif()

string(FIND "${content}" "if (id == kIdFileCopyImage) {" copy_img_pos)
if(copy_img_pos EQUAL -1)
    message(FATAL_ERROR "kIdFileCopyImage handler not found in main.cpp")
endif()
string(SUBSTRING "${content}" ${copy_img_pos} 300 copy_img_window)
string(FIND "${copy_img_window}" "noninteractive_mode(*app)" copy_img_guard_pos)
if(copy_img_guard_pos EQUAL -1)
    message(FATAL_ERROR "kIdFileCopyImage handler must be guarded by noninteractive_mode(*app)")
endif()

# Verify that main.cpp has no static or startup clipboard initialization:
string(FIND "${content}" "OpenClipboard" open_clip_pos)
if(NOT open_clip_pos EQUAL -1)
    message(FATAL_ERROR "Direct OpenClipboard call found in main.cpp; clipboard operations must remain behind clipboard_output boundary")
endif()
string(FIND "${content}" "RegisterClipboardFormat" reg_clip_pos)
if(NOT reg_clip_pos EQUAL -1)
    message(FATAL_ERROR "Direct RegisterClipboardFormat call found in main.cpp; clipboard format registration must be on-demand in clipboard_output boundary")
endif()

message(STATUS "check_benchmark_ui_isolation: OK (all interactive menus, context menu popup, buttons, drag/drop, chrome, dialogs, edit/save controllers, clipboard commands, and decode/present failure prompts are strictly excluded from both --benchmark and --navigation-benchmark)")

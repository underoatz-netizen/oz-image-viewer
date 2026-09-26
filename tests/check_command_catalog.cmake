# check_command_catalog.cmake - source-scan CTest for the icon-first command
# catalog boundary.
#
# 1. The distinct Paste Image command ID (111) exists in the catalog and in
#    main.cpp, and the WM_COMMAND path routes catalog commands through the thin
#    dispatch adapter (dispatch_catalog_command).
# 2. Paste is routed to the existing start_paste_from_clipboard behavior and is
#    NEVER a synthesized Ctrl+V key event (no WM_KEYDOWN 'V' injection, no
#    keybd_event/SendInput).
# 3. Zoom In/Out and First/Previous/Next/Last route through the adapter to the
#    existing view/navigation paths (zoom_at_point / execute_navigation_request).
# 4. The adapter consults the authoritative command state via
#    plan_command_dispatch and consumes disabled commands as no-ops.
#
# Usage: cmake -DMAIN_CPP=<src/viewer/main.cpp> -DVIEWER_DIR=<src/viewer>
#        -P check_command_catalog.cmake

if(NOT EXISTS "${MAIN_CPP}")
    message(FATAL_ERROR "main.cpp not found: ${MAIN_CPP}")
endif()
if(NOT DEFINED VIEWER_DIR)
    message(FATAL_ERROR "VIEWER_DIR not set")
endif()

set(STATE_HPP "${VIEWER_DIR}/ui_command_state.hpp")
set(STATE_CPP "${VIEWER_DIR}/ui_command_state.cpp")
set(DISPATCH_HPP "${VIEWER_DIR}/command_dispatch.hpp")
set(DISPATCH_CPP "${VIEWER_DIR}/command_dispatch.cpp")
foreach(f IN ITEMS "${STATE_HPP}" "${STATE_CPP}" "${DISPATCH_HPP}" "${DISPATCH_CPP}")
    if(NOT EXISTS "${f}")
        message(FATAL_ERROR "required command-catalog file not found: ${f}")
    endif()
endforeach()

file(READ "${MAIN_CPP}" content)
file(READ "${STATE_HPP}" state_hpp)
file(READ "${STATE_CPP}" state_cpp)
file(READ "${DISPATCH_HPP}" dispatch_hpp)
file(READ "${DISPATCH_CPP}" dispatch_cpp)

# 1. Distinct, stable Paste Image command ID in the catalog and in main.cpp.
string(FIND "${state_hpp}" "file_paste_image = 111" paste_id_pos)
if(paste_id_pos EQUAL -1)
    message(FATAL_ERROR "ui_command_state.hpp must declare file_paste_image = 111")
endif()
string(FIND "${content}" "constexpr int kIdFilePasteImage = 111;" paste_const_pos)
if(paste_const_pos EQUAL -1)
    message(FATAL_ERROR "main.cpp must define kIdFilePasteImage = 111")
endif()

# 2. The WM_COMMAND path routes catalog commands through the adapter.
string(FIND "${content}" "if (dispatch_catalog_command(*app, id)) {" dispatch_call_pos)
if(dispatch_call_pos EQUAL -1)
    message(FATAL_ERROR "WM_COMMAND must route catalog commands through dispatch_catalog_command(*app, id)")
endif()
string(FIND "${content}" "static bool dispatch_catalog_command(app_state& app, int id) {" dispatch_def_pos)
if(dispatch_def_pos EQUAL -1)
    message(FATAL_ERROR "dispatch_catalog_command definition not found in main.cpp")
endif()

# 3. The adapter consults the authoritative command state and consumes disabled
#    commands as no-ops.
string(FIND "${content}" "plan_command_dispatch(" plan_call_pos)
if(plan_call_pos EQUAL -1)
    message(FATAL_ERROR "dispatch_catalog_command must consult plan_command_dispatch")
endif()
string(FIND "${content}" "if (!plan.enabled) {" disabled_guard_pos)
if(disabled_guard_pos EQUAL -1)
    message(FATAL_ERROR "dispatch_catalog_command must consume disabled commands as no-ops")
endif()

# 4. Paste routes to the existing paste behavior, never a synthesized Ctrl+V.
string(FIND "${content}" "start_paste_from_clipboard(app);" paste_route_pos)
if(paste_route_pos EQUAL -1)
    message(FATAL_ERROR "dispatch_catalog_command must route paste to start_paste_from_clipboard(app)")
endif()
# No synthetic input injection anywhere in main.cpp.
foreach(sym IN ITEMS "keybd_event" "SendInput")
    string(FIND "${content}" "${sym}" synth_pos)
    if(NOT synth_pos EQUAL -1)
        message(FATAL_ERROR "main.cpp must never synthesize input (found ${sym})")
    endif()
endforeach()
# The paste dispatch must not inject a WM_KEYDOWN 'V' event: the adapter's
# paste branch calls start_paste_from_clipboard directly. (The message loop's
# pre-existing WM_KEYDOWN forwarding for other keys is unrelated and allowed.)
string(SUBSTRING "${content}" ${dispatch_def_pos} 4000 dispatch_body)
string(FIND "${dispatch_body}" "WM_KEYDOWN" paste_keydown_pos)
if(NOT paste_keydown_pos EQUAL -1)
    message(FATAL_ERROR "dispatch_catalog_command must not synthesize a WM_KEYDOWN event for paste")
endif()
string(FIND "${dispatch_body}" "start_paste_from_clipboard(app);" paste_direct_pos)
if(paste_direct_pos EQUAL -1)
    message(FATAL_ERROR "dispatch_catalog_command must call start_paste_from_clipboard(app) directly")
endif()

# 5. Zoom and navigation route to the existing paths.
string(FIND "${content}" "oz::viewer_core::zoom_at_point(app.view" zoom_route_pos)
if(zoom_route_pos EQUAL -1)
    message(FATAL_ERROR "dispatch_catalog_command must route zoom through the existing zoom_at_point path")
endif()
string(FIND "${content}" "execute_navigation_request(app, vk_key, \"interactive\");" nav_route_pos)
if(nav_route_pos EQUAL -1)
    message(FATAL_ERROR "dispatch_catalog_command must route navigation through the existing execute_navigation_request path")
endif()

# 6. The pure mapping seam owns exactly the catalog intents and mirrors the
#    authoritative command state.
foreach(kind IN ITEMS "zoom_in" "zoom_out" "nav_first" "nav_prev" "nav_next" "nav_last" "paste_image")
    string(FIND "${dispatch_hpp}" "${kind}" kind_pos)
    if(kind_pos EQUAL -1)
        message(FATAL_ERROR "command_dispatch.hpp is missing dispatch kind ${kind}")
    endif()
endforeach()
string(FIND "${dispatch_cpp}" "plan.enabled = derive_command_state(id, ctx).enabled;" parity_pos)
if(parity_pos EQUAL -1)
    message(FATAL_ERROR "plan_command_dispatch must mirror derive_command_state for enabled parity")
endif()

# 7. The paste command state is explicit (not the default disabled branch) and
#    uses the authoritative admission helper.
string(FIND "${state_cpp}" "case ui_command_id::file_paste_image:" paste_case_pos)
if(paste_case_pos EQUAL -1)
    message(FATAL_ERROR "derive_command_state must have an explicit file_paste_image case")
endif()
string(FIND "${state_cpp}" "state.enabled = can_paste_image(ctx);" paste_policy_pos)
if(paste_policy_pos EQUAL -1)
    message(FATAL_ERROR "file_paste_image state must use the authoritative can_paste_image policy")
endif()
string(FIND "${state_hpp}" "inline bool can_paste_image(const ui_command_context& ctx) noexcept" paste_helper_pos)
if(paste_helper_pos EQUAL -1)
    message(FATAL_ERROR "ui_command_state.hpp must declare the can_paste_image admission helper")
endif()

message(STATUS "check_command_catalog: OK (distinct Paste Image ID, authoritative admission policy, thin dispatch adapter routes zoom/navigation/paste to existing paths, disabled commands are no-ops, and Ctrl+V is never synthesized)")

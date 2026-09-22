// oz/viewer/shortcut_routing.cpp - testable pure keyboard shortcut routing (Stage 9.4).
#include "shortcut_routing.hpp"

namespace oz::viewer {

namespace {

bool is_text_input_control(HWND hwnd) noexcept {
    if (!hwnd || !::IsWindow(hwnd)) {
        return false;
    }
    wchar_t cls_name[64]{};
    if (::GetClassNameW(hwnd, cls_name, 64) == 0) {
        return false;
    }
    // Case-insensitive match against "Edit" and "RichEdit" variants
    if (_wcsicmp(cls_name, L"Edit") == 0) {
        return true;
    }
    if (_wcsnicmp(cls_name, L"RichEdit", 8) == 0) {
        return true;
    }
    return false;
}

}  // namespace

shortcut_action route_shortcut_key(
    WPARAM wparam,
    bool ctrl_down,
    bool shift_down,
    bool alt_down,
    HWND focused_hwnd,
    HWND resize_dlg_hwnd,
    bool crop_active,
    const ui_item_state& copy_image_state) noexcept {
    // Only handle 'C' key
    if (wparam != 'C' && wparam != 'c') {
        return shortcut_action::none;
    }

    // 1. Native Resize Dialog owns all keys when active
    if (resize_dlg_hwnd != nullptr && (focused_hwnd == resize_dlg_hwnd || ::IsChild(resize_dlg_hwnd, focused_hwnd))) {
        return shortcut_action::dialog_default;
    }

    // 2. Crop mode consumes Ctrl+C and plain C (no copy)
    if (crop_active) {
        if (!alt_down) {
            return shortcut_action::crop_consume;
        }
        return shortcut_action::none;
    }

    // 3. Focused Edit/RichEdit control gets native text copy on Ctrl+C (Shift/Alt up)
    if (ctrl_down && !shift_down && !alt_down) {
        if (is_text_input_control(focused_hwnd)) {
            return shortcut_action::native_text_copy;
        }

        // 4. Exact Ctrl down with Shift/Alt up routes to Copy Image command path
        if (copy_image_state.enabled) {
            return shortcut_action::file_copy_image;
        } else {
            // Disabled command consumes with no clipboard work/warning
            return shortcut_action::command_disabled;
        }
    }

    return shortcut_action::none;
}

}  // namespace oz::viewer

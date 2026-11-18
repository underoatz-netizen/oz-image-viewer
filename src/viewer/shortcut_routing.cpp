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
    const bool is_copy_key = (wparam == 'C' || wparam == 'c');
    const bool is_paste_key = (wparam == 'V' || wparam == 'v');
    // Only handle 'C' (Copy Image) and 'V' (Paste Image) keys
    if (!is_copy_key && !is_paste_key) {
        return shortcut_action::none;
    }

    // 1. Native Resize Dialog owns all keys when active
    if (resize_dlg_hwnd != nullptr && (focused_hwnd == resize_dlg_hwnd || ::IsChild(resize_dlg_hwnd, focused_hwnd))) {
        return shortcut_action::dialog_default;
    }

    // 2. Crop mode consumes Ctrl+C / Ctrl+V and plain C (no copy/paste)
    if (crop_active) {
        if (!alt_down) {
            return shortcut_action::crop_consume;
        }
        return shortcut_action::none;
    }

    // 3. Exact Ctrl down with Shift/Alt up routes text controls natively,
    //    otherwise to the Copy Image (C) or Paste Image (V) command path.
    //    Ctrl+Shift+V (Flip Vertical) and other modified variants fall
    //    through as none so existing owners keep them.
    if (ctrl_down && !shift_down && !alt_down) {
        if (is_text_input_control(focused_hwnd)) {
            return is_paste_key ? shortcut_action::native_text_paste
                                : shortcut_action::native_text_copy;
        }

        if (is_paste_key) {
            // Paste availability depends on live clipboard contents, which
            // this pure function never inspects; the caller attempts the
            // bounded import and fails quietly when nothing pastable exists.
            return shortcut_action::file_paste_image;
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

f10_overflow_request route_f10_overflow_menu(
    UINT msg,
    WPARAM vk,
    bool ctrl_down,
    bool shift_down,
    bool alt_down,
    bool key_repeat,
    bool interactive_mode,
    bool is_fullscreen,
    bool chrome_created,
    bool resize_dialog_open) noexcept {
    if ((msg != WM_KEYDOWN && msg != WM_SYSKEYDOWN) || vk != VK_F10) {
        return {};
    }
    // Shift+F10 is the standard Context Menu key; Ctrl/Alt variants keep their
    // platform meaning. Only bare F10 belongs to More Commands.
    if (ctrl_down || shift_down || alt_down) {
        return {};
    }
    // Automated benchmark modes, fullscreen, missing command bar, and a modal
    // Resize dialog retain standard Win32 behavior.
    if (!interactive_mode || is_fullscreen || !chrome_created || resize_dialog_open) {
        return {};
    }
    // Consume auto-repeat without re-opening the menu (the popup runs its own
    // loop, so this is a safety net for synthetic or queued repeats).
    return f10_overflow_request{true, !key_repeat};
}

bool should_open_overflow_menu_on_syscommand(
    WPARAM wparam,
    LPARAM lparam,
    bool interactive_mode,
    bool is_fullscreen,
    bool chrome_created,
    bool resize_dialog_open) noexcept {
    if ((wparam & 0xFFF0) != SC_KEYMENU || lparam != 0) {
        return false;
    }
    return interactive_mode && !is_fullscreen && chrome_created && !resize_dialog_open;
}

}  // namespace oz::viewer

// oz/viewer/shortcut_routing.hpp - testable pure keyboard shortcut routing (Stage 9.4).
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "ui_command_state.hpp"

namespace oz::viewer {

enum class shortcut_action {
    none,
    dialog_default,     // Native dialog (e.g. Resize dialog) owns key
    crop_consume,       // In-progress crop consumes key (e.g. C or Ctrl+C)
    native_text_copy,   // Focused Edit/RichEdit control receives standard text copy
    native_text_paste,  // Focused Edit/RichEdit control receives standard text paste
    file_copy_image,    // Routes to Copy Image (ID 108) command path
    file_paste_image,   // Routes to Paste Image (Ctrl+V clipboard import) path
    command_disabled    // Target command is disabled; consume key with no clipboard work or warning
};

// Pure shortcut routing evaluation for unit testing and main dispatch.
[[nodiscard]] shortcut_action route_shortcut_key(
    WPARAM wparam,
    bool ctrl_down,
    bool shift_down,
    bool alt_down,
    HWND focused_hwnd,
    HWND resize_dlg_hwnd,
    bool crop_active,
    const ui_item_state& copy_image_state) noexcept;

// Stage 7 accessibility: plain F10 ("More Commands") routing decision.
// A focused Win32 window can receive plain F10 as WM_KEYDOWN, not only as
// WM_SYSKEYDOWN; both message kinds route through this pure predicate so the
// prefilter and the window procedure cannot drift apart.
struct f10_overflow_request {
    // The message belongs to More Commands and must not reach DefWindowProc
    // (the SC_KEYMENU fallback) or the navigation key handling.
    bool consume = false;
    // Open the overflow menu now. False for key auto-repeat: the repeated
    // message is still swallowed instead of falling through to Windows.
    bool open = false;
};

// Message is either WM_KEYDOWN or WM_SYSKEYDOWN; vk must be VK_F10.
// Modified F10 keeps its standard meaning (Shift+F10 is the Context Menu key).
// Benchmark/navigation-benchmark, fullscreen, missing chrome, and an open
// modal Resize dialog all keep standard Win32 behavior.
[[nodiscard]] f10_overflow_request route_f10_overflow_menu(
    UINT msg,
    WPARAM vk,
    bool ctrl_down,
    bool shift_down,
    bool alt_down,
    bool key_repeat,
    bool interactive_mode,
    bool is_fullscreen,
    bool chrome_created,
    bool resize_dialog_open) noexcept;

// Stage 7 accessibility: bare Alt (WM_SYSCOMMAND SC_KEYMENU with lparam == 0)
// opens the More Commands overflow menu from both welcome and loaded canvas,
// gated identically on interactive mode, non-fullscreen, created chrome, and
// no open modal Resize dialog.
[[nodiscard]] bool should_open_overflow_menu_on_syscommand(
    WPARAM wparam,
    LPARAM lparam,
    bool interactive_mode,
    bool is_fullscreen,
    bool chrome_created,
    bool resize_dialog_open) noexcept;

}  // namespace oz::viewer

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
    file_copy_image,    // Routes to Copy Image (ID 108) command path
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

}  // namespace oz::viewer

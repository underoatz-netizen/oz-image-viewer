// oz/viewer/command_menu.hpp - Pure definition of the command row's overflow menu.
//
// Replaces the retired Win32 File / Edit menu bar with a single-owner table
// driven by the right-pinned "More commands" (ellipsis-vertical) button and
// F10 / Alt. Keeping it as plain data makes menu completeness and unique
// mnemonics unit-testable without creating a window.
#pragma once

#include "ui_command_state.hpp"

#include <cstddef>

namespace oz::viewer::menu {

inline constexpr int kIdOverflowMenu = 390;

inline constexpr int kMenuSeparator = 0;
inline constexpr int kMenuCopyPopup = 0xFFFE;
inline constexpr int kMenuEditPopup = 0xFFFD;
inline constexpr int kMenuCropRatioPopup = 0xFFFC;

struct item {
    // Command integer id (matching ui_command_id), kMenuSeparator, or a popup marker.
    int command = 0;
    // Displayed menu string; a single '&' marks the mnemonic character.
    const wchar_t* label = nullptr;
    // True when this item is only active in Editing-ON builds.
    bool editing_only = false;
};

inline constexpr item kOverflowItems[] = {
    {static_cast<int>(ui_command_id::file_open), L"&Open...\tCtrl+O", false},
    {static_cast<int>(ui_command_id::file_paste_image), L"Pas&te Image\tCtrl+V", false},
    {static_cast<int>(ui_command_id::file_save), L"&Save\tCtrl+S", true},
    {static_cast<int>(ui_command_id::file_save_as), L"Save &As...\tCtrl+Shift+S", true},
    {kMenuSeparator, nullptr, false},
    {static_cast<int>(ui_command_id::file_open_location), L"Open File &Location", false},
    {kMenuCopyPopup, L"&Copy", false},
    {static_cast<int>(ui_command_id::file_print), L"&Print...\tCtrl+P", false},
    {static_cast<int>(ui_command_id::file_delete), L"&Delete\tDel", false},
    {kMenuSeparator, nullptr, true},
    {kMenuEditPopup, L"&Edit", true},
    {kMenuSeparator, nullptr, false},
    {static_cast<int>(ui_command_id::view_fit), L"Fit to &Window\tF", false},
    {static_cast<int>(ui_command_id::view_actual_size), L"Actual Si&ze\t1", false},
    {static_cast<int>(ui_command_id::view_zoom_in), L"Zoo&m In\t+", false},
    {static_cast<int>(ui_command_id::view_zoom_out), L"Zoom O&ut\t-", false},
    {static_cast<int>(ui_command_id::view_fullscreen), L"Fullsc&reen\tF11", false},
    {kMenuSeparator, nullptr, false},
    {static_cast<int>(ui_command_id::nav_first), L"First &Image\tHome", false},
    {static_cast<int>(ui_command_id::nav_prev), L"Pre&vious Image\tLeft", false},
    {static_cast<int>(ui_command_id::nav_next), L"&Next Image\tRight", false},
    {static_cast<int>(ui_command_id::nav_last), L"Last Ima&ge\tEnd", false},
};

inline constexpr item kCopySubmenuItems[] = {
    {static_cast<int>(ui_command_id::file_copy_image), L"Copy &Image\tCtrl+C", false},
    {static_cast<int>(ui_command_id::file_copy), L"Copy &File", false},
    {static_cast<int>(ui_command_id::file_copy_path), L"Copy File &Path", false},
};

inline constexpr item kEditSubmenuItems[] = {
    {static_cast<int>(ui_command_id::edit_undo), L"&Undo\tCtrl+Z", true},
    {static_cast<int>(ui_command_id::edit_redo), L"&Redo\tCtrl+Y", true},
    {kMenuSeparator, nullptr, true},
    {static_cast<int>(ui_command_id::edit_rotate_left), L"Rotate &Left\tCtrl+L", true},
    {static_cast<int>(ui_command_id::edit_rotate_right), L"Rotate R&ight\tCtrl+R", true},
    {kMenuSeparator, nullptr, true},
    {static_cast<int>(ui_command_id::edit_flip_h), L"Flip &Horizontal\tCtrl+Shift+H", true},
    {static_cast<int>(ui_command_id::edit_flip_v), L"Flip &Vertical\tCtrl+Shift+V", true},
    {kMenuSeparator, nullptr, true},
    {static_cast<int>(ui_command_id::edit_crop), L"&Crop...\tC", true},
    {static_cast<int>(ui_command_id::edit_resize), L"Resi&ze...\tCtrl+Alt+R", true},
    {kMenuSeparator, nullptr, true},
    {static_cast<int>(ui_command_id::edit_crop_apply), L"&Apply Crop\tEnter", true},
    {static_cast<int>(ui_command_id::edit_crop_cancel), L"Ca&ncel Crop\tEsc", true},
    {static_cast<int>(ui_command_id::edit_crop_reset), L"Re&set Crop\tR", true},
    {kMenuCropRatioPopup, L"Crop Ra&tio", true},
};

inline constexpr item kCropRatioSubmenuItems[] = {
    {static_cast<int>(ui_command_id::crop_ratio_free), L"&Free", true},
    {static_cast<int>(ui_command_id::crop_ratio_original), L"&Original", true},
    {static_cast<int>(ui_command_id::crop_ratio_1x1), L"&1:1", true},
    {static_cast<int>(ui_command_id::crop_ratio_3x2), L"&3:2", true},
    {static_cast<int>(ui_command_id::crop_ratio_4x3), L"&4:3", true},
    {static_cast<int>(ui_command_id::crop_ratio_5x4), L"&5:4", true},
    {static_cast<int>(ui_command_id::crop_ratio_16x9), L"1&6:9", true},
    {static_cast<int>(ui_command_id::crop_ratio_9x16), L"&9:16", true},
    {static_cast<int>(ui_command_id::crop_ratio_a_series), L"&A-series", true},
};

inline constexpr std::size_t kOverflowItemCount = sizeof(kOverflowItems) / sizeof(kOverflowItems[0]);
inline constexpr std::size_t kCopySubmenuCount = sizeof(kCopySubmenuItems) / sizeof(kCopySubmenuItems[0]);
inline constexpr std::size_t kEditSubmenuCount = sizeof(kEditSubmenuItems) / sizeof(kEditSubmenuItems[0]);
inline constexpr std::size_t kCropRatioSubmenuCount = sizeof(kCropRatioSubmenuItems) / sizeof(kCropRatioSubmenuItems[0]);

// Every active command from the retired File / Edit menu bar and the 27-command
// standard surface must stay reachable through the overflow menu.
inline constexpr ui_command_id kRequiredCommands[] = {
    ui_command_id::file_open,
    ui_command_id::file_paste_image,
    ui_command_id::file_save,
    ui_command_id::file_save_as,
    ui_command_id::file_open_location,
    ui_command_id::file_copy_image,
    ui_command_id::file_copy,
    ui_command_id::file_copy_path,
    ui_command_id::file_print,
    ui_command_id::file_delete,
    ui_command_id::edit_undo,
    ui_command_id::edit_redo,
    ui_command_id::edit_rotate_left,
    ui_command_id::edit_rotate_right,
    ui_command_id::edit_flip_h,
    ui_command_id::edit_flip_v,
    ui_command_id::edit_crop,
    ui_command_id::edit_resize,
    ui_command_id::edit_crop_apply,
    ui_command_id::edit_crop_cancel,
    ui_command_id::edit_crop_reset,
    ui_command_id::crop_ratio_free,
    ui_command_id::crop_ratio_original,
    ui_command_id::crop_ratio_1x1,
    ui_command_id::crop_ratio_3x2,
    ui_command_id::crop_ratio_4x3,
    ui_command_id::crop_ratio_5x4,
    ui_command_id::crop_ratio_16x9,
    ui_command_id::crop_ratio_9x16,
    ui_command_id::crop_ratio_a_series,
    ui_command_id::view_fit,
    ui_command_id::view_actual_size,
    ui_command_id::view_zoom_in,
    ui_command_id::view_zoom_out,
    ui_command_id::view_fullscreen,
    ui_command_id::nav_first,
    ui_command_id::nav_prev,
    ui_command_id::nav_next,
    ui_command_id::nav_last,
};

inline constexpr std::size_t kRequiredCommandCount = sizeof(kRequiredCommands) / sizeof(kRequiredCommands[0]);

inline constexpr std::size_t npos = static_cast<std::size_t>(-1);

inline constexpr std::size_t mnemonic_index(const wchar_t* label) noexcept {
    if (label == nullptr) {
        return npos;
    }
    for (std::size_t i = 0; label[i] != L'\0'; ++i) {
        if (label[i] != L'&') {
            continue;
        }
        if (label[i + 1] == L'&') {
            ++i;
            continue;
        }
        return i + 1;
    }
    return npos;
}

inline constexpr wchar_t mnemonic_char(const wchar_t* label) noexcept {
    const std::size_t idx = mnemonic_index(label);
    if (idx == npos) {
        return L'\0';
    }
    const wchar_t ch = label[idx];
    return (ch >= L'a' && ch <= L'z') ? static_cast<wchar_t>(ch - L'a' + L'A') : ch;
}

}  // namespace oz::viewer::menu

// oz/viewer/ui_command_state.hpp - presentation-only UI command-state seam (Stage 7 foundation).
#pragma once

#include "oz/viewer_core/crop_interaction.hpp"
#include "oz/viewer_core/edit_state.hpp"

#include <cstdint>
#include <optional>

namespace oz::viewer {

// Standard Command IDs used in viewer UI (menus, future toolbars/shortcuts)
enum class ui_command_id : int {
    file_open = 101,
    btn_open = 102,
    file_save = 103,
    file_save_as = 104,
    file_open_location = 105,
    file_copy = 106,
    file_copy_path = 107,
    file_copy_image = 108,

    edit_rotate_left = 201,
    edit_rotate_right = 202,
    edit_flip_h = 203,
    edit_flip_v = 204,
    edit_undo = 205,
    edit_redo = 206,
    edit_crop = 207,
    edit_crop_apply = 208,
    edit_crop_cancel = 209,
    edit_crop_reset = 210,
    edit_resize = 211,

    crop_ratio_free = 221,
    crop_ratio_original = 222,
    crop_ratio_1x1 = 223,
    crop_ratio_3x2 = 224,
    crop_ratio_4x3 = 225,
    crop_ratio_5x4 = 226,
    crop_ratio_16x9 = 227,
    crop_ratio_9x16 = 228,
    crop_ratio_a_series = 229,

    // Navigation and View commands (interactive presentation state)
    view_actual_size = 301,
    view_fit = 302,
    view_zoom_in = 303,
    view_zoom_out = 304,
    nav_prev = 305,
    nav_next = 306,
    nav_first = 307,
    nav_last = 308,
    view_fullscreen = 309
};

// Pure presentation state of a single command item
struct ui_item_state {
    bool enabled = false;
    bool checked = false;
    bool visible = true;
};

// Snapshot of domain/viewer authoritative states fed into the derivation function.
// Pure value type with no Win32 GUI handles or side effects.
struct ui_command_context {
    bool benchmark_mode = false;
    bool have_image = false;
    bool has_document = false;
    bool has_source = false;
    bool is_dirty = false;
    bool can_undo = false;
    bool can_redo = false;

    // Crop interaction state
    bool crop_active = false;
    std::optional<oz::viewer_core::crop_ratio_preset> crop_preset = std::nullopt;

    // Modality / Async task busy state
    bool resize_busy = false;
    bool save_busy = false;
    bool copy_busy = false;

    // View / Navigation state
    bool can_navigate_prev = false;
    bool can_navigate_next = false;
    bool has_navigation_items = false;
    bool is_fullscreen = false;
    bool is_fit_mode = true;

    // File location / reveal capability (presentation invariant: valid file-backed committed document)
    bool has_file_path = false;
};

// Pure derivation function: returns presentation state for a given command given context.
[[nodiscard]] ui_item_state derive_command_state(ui_command_id id, const ui_command_context& ctx) noexcept;

// Helper to determine if general editing commands (rotate, flip, crop, resize) can be invoked.
[[nodiscard]] inline bool can_edit(const ui_command_context& ctx) noexcept {
    return ctx.have_image && ctx.has_document && !ctx.crop_active && !ctx.resize_busy && !ctx.save_busy && !ctx.copy_busy;
}

}  // namespace oz::viewer

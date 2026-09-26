// oz/viewer/crop_icon_bar.hpp - pure layout and command catalog for the active crop strip.
//
// This seam defines:
// 1. The 12 active crop command descriptors (9 ratios + 3 actions).
// 2. Pure deterministic multi-row / horizontal scroll layout computation ensuring
//    all controls meet the 40 DIP minimum cell hit target and remain reachable
//    (never clipped, hidden, or collapsed into an overflow menu) at narrow widths.
#pragma once

#include "ui_command_state.hpp"
#include "oz/viewer_core/crop_interaction.hpp"

#include <cstdint>
#include <vector>

namespace oz::viewer {

inline constexpr float kCropIconCellDip = 40.0f;
inline constexpr float kCropIconSeparatorDip = 8.0f;
inline constexpr float kCropIconBarPadDip = 4.0f;
inline constexpr float kCropIconScrollbarDip = 16.0f;
inline constexpr float kCropIconMinDimSummaryWidthDip = 80.0f;
inline constexpr std::int32_t kCropIconMaxRows = 2;

enum class crop_control_kind : std::uint8_t {
    ratio_button,
    action_button,
    dim_summary,
    separator
};

struct crop_command_item {
    ui_command_id id;
    std::int32_t command_int_id;
    crop_control_kind kind;
    oz::viewer_core::crop_ratio_preset preset = oz::viewer_core::crop_ratio_preset::free;
    bool group_start = false;
};

// Returns the 12 crop command items in exact order:
// 9 ratios (Free, Original, 1:1, 3:2, 4:3, 5:4, 16:9, 9:16, A-series)
// followed by 3 actions (Reset, Apply, Cancel).
[[nodiscard]] const std::vector<crop_command_item>& get_crop_command_items() noexcept;

struct crop_layout_cell {
    crop_control_kind kind = crop_control_kind::ratio_button;
    ui_command_id id = ui_command_id::crop_ratio_free;
    std::int32_t command_int_id = 0;
    oz::viewer_core::crop_ratio_preset preset = oz::viewer_core::crop_ratio_preset::free;
    std::int32_t row = 0;
    float x_dip = 0.0f;
    float width_dip = 0.0f;
    bool group_start = false;
};

struct crop_icon_bar_layout {
    bool valid = false;
    std::int32_t rows = 0;
    bool needs_scroll = false;
    float available_width_dip = 0.0f;
    float content_width_dip = 0.0f;
    float scroll_range_dip = 0.0f;
    float bar_height_dip = 0.0f;
    // Dimension summary geometry in DIP within the strip (always placed on row 0 or reserved)
    float dim_x_dip = 0.0f;
    float dim_y_dip = 0.0f;
    float dim_width_dip = 0.0f;
    float dim_height_dip = 0.0f;
    std::vector<crop_layout_cell> cells;
};

[[nodiscard]] crop_icon_bar_layout compute_crop_icon_bar_layout(float available_width_dip) noexcept;

}  // namespace oz::viewer

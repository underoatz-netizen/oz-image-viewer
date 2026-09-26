// oz/viewer/crop_icon_bar.cpp - pure layout implementation for active crop controls.
#include "crop_icon_bar.hpp"

#include <algorithm>
#include <cmath>

namespace oz::viewer {

const std::vector<crop_command_item>& get_crop_command_items() noexcept {
    // 9 ratios + 3 actions (Reset, Apply, Cancel).
    // Exact command IDs:
    // Free: 221
    // Original: 222
    // 1:1: 223
    // 3:2: 224
    // 4:3: 225
    // 5:4: 226
    // 16:9: 227
    // 9:16: 228
    // A-series: 229
    // Reset: 210
    // Apply: 208
    // Cancel: 209
    static const std::vector<crop_command_item> s_items = {
        // Ratios group (starts with Free)
        {ui_command_id::crop_ratio_free, 221, crop_control_kind::ratio_button, oz::viewer_core::crop_ratio_preset::free, true},
        {ui_command_id::crop_ratio_original, 222, crop_control_kind::ratio_button, oz::viewer_core::crop_ratio_preset::original, false},
        {ui_command_id::crop_ratio_1x1, 223, crop_control_kind::ratio_button, oz::viewer_core::crop_ratio_preset::square_1x1, false},
        {ui_command_id::crop_ratio_3x2, 224, crop_control_kind::ratio_button, oz::viewer_core::crop_ratio_preset::ratio_3x2, false},
        {ui_command_id::crop_ratio_4x3, 225, crop_control_kind::ratio_button, oz::viewer_core::crop_ratio_preset::ratio_4x3, false},
        {ui_command_id::crop_ratio_5x4, 226, crop_control_kind::ratio_button, oz::viewer_core::crop_ratio_preset::ratio_5x4, false},
        {ui_command_id::crop_ratio_16x9, 227, crop_control_kind::ratio_button, oz::viewer_core::crop_ratio_preset::ratio_16x9, false},
        {ui_command_id::crop_ratio_9x16, 228, crop_control_kind::ratio_button, oz::viewer_core::crop_ratio_preset::ratio_9x16, false},
        {ui_command_id::crop_ratio_a_series, 229, crop_control_kind::ratio_button, oz::viewer_core::crop_ratio_preset::a_series, false},
        // Action group: Reset, Apply, Cancel
        {ui_command_id::edit_crop_reset, 210, crop_control_kind::action_button, oz::viewer_core::crop_ratio_preset::free, true},
        {ui_command_id::edit_crop_apply, 208, crop_control_kind::action_button, oz::viewer_core::crop_ratio_preset::free, false},
        {ui_command_id::edit_crop_cancel, 209, crop_control_kind::action_button, oz::viewer_core::crop_ratio_preset::free, false}
    };
    return s_items;
}

crop_icon_bar_layout compute_crop_icon_bar_layout(float available_width_dip) noexcept {
    crop_icon_bar_layout out{};
    if (!(available_width_dip > 0.0f)) {
        return out;
    }

    try {
        out.available_width_dip = available_width_dip;
        out.valid = true;

        const auto& commands = get_crop_command_items();

        // Items sequence:
        // Group 1: 9 ratio buttons
        // Separator (between ratios and dimension summary)
        // Group 2: Dimension summary item (acts as readable non-command block)
        // Separator (between dimension summary and action buttons)
        // Group 3: 3 action buttons (Reset, Apply, Cancel)
        //
        // Total single-row width:
        // 9 * 40 (ratios) = 360
        // + 8 (sep)
        // + 96 (dim summary)
        // + 8 (sep)
        // + 3 * 40 (actions) = 120
        // = 592 DIP.
        const float dim_w = 96.0f;

        struct layout_item {
            crop_control_kind kind;
            const crop_command_item* cmd;
            float width;
            bool group_start;
        };

        std::vector<layout_item> items;
        items.reserve(commands.size() + 3);

        // Add 9 ratio items
        for (std::size_t i = 0; i < 9; ++i) {
            items.push_back({crop_control_kind::ratio_button, &commands[i], kCropIconCellDip, commands[i].group_start});
        }
        // Separator
        items.push_back({crop_control_kind::separator, nullptr, kCropIconSeparatorDip, false});
        // Dimension summary
        items.push_back({crop_control_kind::dim_summary, nullptr, dim_w, false});
        // Separator
        items.push_back({crop_control_kind::separator, nullptr, kCropIconSeparatorDip, false});
        // 3 action buttons
        for (std::size_t i = 9; i < commands.size(); ++i) {
            items.push_back({crop_control_kind::action_button, &commands[i], kCropIconCellDip, commands[i].group_start});
        }

        float total_width = 0.0f;
        for (const auto& it : items) {
            total_width += it.width;
        }

        float content_width = total_width;
        bool needs_scroll = false;
        if (total_width > available_width_dip) {
            const float two_row_min = std::ceil(total_width * 0.5f) + kCropIconCellDip;
            if (available_width_dip >= two_row_min) {
                content_width = available_width_dip;
            } else {
                content_width = two_row_min;
                needs_scroll = true;
            }
        }

        out.content_width_dip = content_width;
        out.needs_scroll = needs_scroll;
        out.scroll_range_dip = needs_scroll ? (content_width - available_width_dip) : 0.0f;

        out.cells.reserve(items.size());
        std::int32_t row = 0;
        float x = 0.0f;
        for (const auto& it : items) {
            if (x > 0.0f && (x + it.width) > (content_width + 0.001f)) {
                row = (std::min)(row + 1, kCropIconMaxRows - 1);
                x = 0.0f;
            }
            if (it.kind == crop_control_kind::dim_summary) {
                out.dim_x_dip = x;
                out.dim_y_dip = static_cast<float>(row) * kCropIconCellDip + kCropIconBarPadDip;
                out.dim_width_dip = it.width;
                out.dim_height_dip = kCropIconCellDip;
            }
            crop_layout_cell cell{};
            cell.kind = it.kind;
            cell.row = row;
            cell.x_dip = x;
            cell.width_dip = it.width;
            cell.group_start = it.group_start;
            if (it.cmd) {
                cell.id = it.cmd->id;
                cell.command_int_id = it.cmd->command_int_id;
                cell.preset = it.cmd->preset;
            }
            out.cells.push_back(cell);
            x += it.width;
        }

        std::int32_t max_row = 0;
        for (const auto& cell : out.cells) {
            max_row = (std::max)(max_row, cell.row);
        }
        out.rows = (std::max)(1, (std::min)(max_row + 1, kCropIconMaxRows));
        out.bar_height_dip =
            static_cast<float>(out.rows) * kCropIconCellDip +
            2.0f * kCropIconBarPadDip +
            (out.needs_scroll ? kCropIconScrollbarDip : 0.0f);

        return out;
    } catch (...) {
        crop_icon_bar_layout failed{};
        return failed;
    }
}

}  // namespace oz::viewer

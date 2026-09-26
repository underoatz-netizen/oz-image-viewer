// oz/viewer/std_icon_bar.cpp - pure standard direct-icon surface layout seam.
#include "std_icon_bar.hpp"

#include <algorithm>
#include <cmath>

namespace oz::viewer {

const std::vector<std_icon_command>& get_std_icon_commands() noexcept {
    // Fixed order; the first command of each group sets `group_start`. The
    // integer command id equals the ui_command_id value, so the chrome can use
    // static_cast<int>(id) as the WM_COMMAND identifier.
    static const std::vector<std_icon_command> s_commands = {
        // File / document
        {ui_command_id::file_open, true},
        {ui_command_id::file_paste_image, false},
        {ui_command_id::file_save, false},
        {ui_command_id::file_save_as, false},
        {ui_command_id::file_open_location, false},
        {ui_command_id::file_copy_image, false},
        {ui_command_id::file_copy, false},
        {ui_command_id::file_copy_path, false},
        {ui_command_id::file_print, false},
        {ui_command_id::file_delete, false},

        // History / transform
        {ui_command_id::edit_undo, true},
        {ui_command_id::edit_redo, false},
        {ui_command_id::edit_rotate_left, false},
        {ui_command_id::edit_rotate_right, false},
        {ui_command_id::edit_flip_h, false},
        {ui_command_id::edit_flip_v, false},
        {ui_command_id::edit_crop, false},
        {ui_command_id::edit_resize, false},

        // View / navigation
        {ui_command_id::view_fit, true},
        {ui_command_id::view_actual_size, false},
        {ui_command_id::view_zoom_in, false},
        {ui_command_id::view_zoom_out, false},
        {ui_command_id::nav_first, false},
        {ui_command_id::nav_prev, false},
        {ui_command_id::nav_next, false},
        {ui_command_id::nav_last, false},
        {ui_command_id::view_fullscreen, false},
    };
    return s_commands;
}

std_icon_bar_layout compute_std_icon_bar_layout(float available_width_dip) noexcept {
    std_icon_bar_layout out{};
    if (!(available_width_dip > 0.0f)) {
        return out;
    }

    try {
        out.available_width_dip = available_width_dip;
        out.valid = true;

        const auto& commands = get_std_icon_commands();

        // Build the ordered item list (separator before each later group) and
        // accumulate the single-row content width.
        struct item_ref {
            const std_icon_command* command;
            bool separator;
        };
        std::vector<item_ref> items;
        items.reserve(commands.size() * 2);
        float total_width = 0.0f;
        bool first = true;
        for (const auto& command : commands) {
            if (command.group_start && !first) {
                items.push_back({&command, true});
                total_width += kStdIconSeparatorDip;
            }
            items.push_back({&command, false});
            total_width += kStdIconCellDip;
            first = false;
        }

        // Single row when everything fits, otherwise wrap to two rows. When the
        // two-row minimum still exceeds the band, widen the content and mark it
        // scrollable so no command is ever clipped or dropped.
        float content_width = total_width;
        bool needs_scroll = false;
        if (total_width > available_width_dip) {
            const float two_row_min =
                std::ceil(total_width * 0.5f) + kStdIconCellDip;
            if (available_width_dip >= two_row_min) {
                content_width = available_width_dip;
            } else {
                content_width = two_row_min;
                needs_scroll = true;
            }
        }

        out.content_width_dip = content_width;
        out.needs_scroll = needs_scroll;
        out.scroll_range_dip =
            needs_scroll ? (content_width - available_width_dip) : 0.0f;

        // Greedy pack; content_width >= half the total plus one cell guarantees
        // at most kStdIconMaxRows rows.
        out.cells.reserve(items.size());
        std::int32_t row = 0;
        float x = 0.0f;
        for (const auto& item : items) {
            const float width = item.separator ? kStdIconSeparatorDip : kStdIconCellDip;
            if (x > 0.0f && (x + width) > (content_width + 0.001f)) {
                row = (std::min)(row + 1, kStdIconMaxRows - 1);
                x = 0.0f;
            }
            std_icon_cell cell{};
            cell.separator = item.separator;
            cell.group_start = !item.separator && item.command->group_start;
            if (!item.separator) {
                cell.id = item.command->id;
                cell.command_int_id = static_cast<std::int32_t>(item.command->id);
            }
            cell.row = row;
            cell.x_dip = x;
            cell.width_dip = width;
            out.cells.push_back(cell);
            x += width;
        }

        std::int32_t max_row = 0;
        for (const auto& cell : out.cells) {
            max_row = (std::max)(max_row, cell.row);
        }
        out.rows = (std::max)(1, (std::min)(max_row + 1, kStdIconMaxRows));
        out.bar_height_dip =
            static_cast<float>(out.rows) * kStdIconCellDip +
            2.0f * kStdIconBarPadDip +
            (out.needs_scroll ? kStdIconScrollbarDip : 0.0f);

        return out;
    } catch (...) {
        // Allocation failure: report an invalid layout so the caller keeps the
        // previous safe geometry rather than rendering a partial surface.
        std_icon_bar_layout failed{};
        return failed;
    }
}

}  // namespace oz::viewer

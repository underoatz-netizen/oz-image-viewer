// oz/viewer/std_icon_bar.cpp - Pure single-row command bar layout policy implementation.
#include "std_icon_bar.hpp"

#include <algorithm>

namespace oz::viewer {

namespace {

struct group_state {
    cmd_bar_group kind = cmd_bar_group::open;
    int width = 0;
    bool visible = true;
    // Higher drops first; 0 never drops; -1 marks the flexible name slot.
    int drop_priority = 0;
    bool separator_before = false;
};

constexpr int separator_width(int sep_gap_px) noexcept {
    return sep_gap_px * 2 + 1;
}

}  // namespace

const std::vector<std_icon_command>& get_std_icon_commands() noexcept {
    static const std::vector<std_icon_command> s_commands = {
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

        {ui_command_id::edit_undo, true},
        {ui_command_id::edit_redo, false},
        {ui_command_id::edit_rotate_left, false},
        {ui_command_id::edit_rotate_right, false},
        {ui_command_id::edit_flip_h, false},
        {ui_command_id::edit_flip_v, false},
        {ui_command_id::edit_crop, false},
        {ui_command_id::edit_resize, false},

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

cmd_bar_layout compute_command_bar_layout(const cmd_bar_input& in) noexcept {
    cmd_bar_layout out{};
    if (in.client_width_px <= 0) {
        return out;
    }

    try {
        const int dpi = (in.dpi > 0) ? in.dpi : 96;
        out.valid = true;
        out.height_px = scale_dip_round(kCommandBarHeightDip, dpi);

        const int icon_w = scale_dip_round(kCommandBarButtonDip, dpi);
        const int gap = scale_dip_round(kCommandBarGroupGapDip, dpi);
        const int pad_l = scale_dip_round(kCommandBarPadLeftDip, dpi);
        const int pad_r = scale_dip_round(kCommandBarPadRightDip, dpi);
        const int sep_gap = scale_dip_round(kCommandBarSeparatorGapDip, dpi);
        const int zoom_label_w = scale_dip_round(kCommandBarZoomLabelDip, dpi);
        const int name_min = scale_dip_round(kCommandBarNameMinDip, dpi);
        const int nav_digits = (std::max)(1, in.nav_digits);
        const int nav_counter_w =
            in.has_nav_counter
                ? (scale_dip_round(kCommandBarNavBaseDip, dpi) +
                   scale_dip_round(kCommandBarNavDigitDip, dpi) * (nav_digits * 2))
                : 0;

        // Left-to-right groups:
        //   Open | DocumentName | NavCounter | <spacer> |
        //   Prev Next | ZoomOut Zoom% ZoomIn  Fit 1:1 | RotL RotR  Crop | Delete Print | Overflow
        std::vector<group_state> groups = {
            {cmd_bar_group::open, icon_w, true, 0, false},
            {cmd_bar_group::name, 0, true, -1, false},
            {cmd_bar_group::nav_counter, nav_counter_w, in.has_nav_counter, 1, false},
            {cmd_bar_group::spacer, 0, true, 0, false},
            {cmd_bar_group::nav, icon_w * 2, true, 2, false},
            {cmd_bar_group::zoom, icon_w * 2 + zoom_label_w, true, 3, true},
            {cmd_bar_group::view_mode, icon_w * 2, true, 4, false},
            {cmd_bar_group::rotate, icon_w * 2, in.editing_enabled, 5, true},
            {cmd_bar_group::crop, icon_w, in.editing_enabled, 6, false},
            {cmd_bar_group::actions, icon_w * 2, true, 7, true},
            {cmd_bar_group::overflow, icon_w, true, 0, true},
        };

        const auto total_width = [&groups, pad_l, pad_r, gap, sep_gap](int name_w, int spacer_w) {
            int total = pad_l + pad_r;
            bool prev_visible = false;
            for (const group_state& g : groups) {
                if (!g.visible) {
                    continue;
                }
                int w = g.width;
                if (g.kind == cmd_bar_group::name) {
                    w = name_w;
                } else if (g.kind == cmd_bar_group::spacer) {
                    w = spacer_w;
                }
                total += w;
                if (prev_visible) {
                    total += g.separator_before ? separator_width(sep_gap) : gap;
                }
                prev_visible = true;
            }
            return total;
        };

        // 1. Drop secondary right-hand groups (highest drop_priority first) until
        //    the row fits with the document name at its ellipsized minimum.
        for (;;) {
            if (total_width(name_min, 0) <= in.client_width_px) {
                break;
            }
            int drop_idx = -1;
            int max_prio = 0;
            for (std::size_t i = 0; i < groups.size(); ++i) {
                const group_state& g = groups[i];
                if (!g.visible || g.drop_priority <= 0) {
                    continue;
                }
                if (g.drop_priority > max_prio) {
                    max_prio = g.drop_priority;
                    drop_idx = static_cast<int>(i);
                }
            }
            if (drop_idx < 0) {
                break;
            }
            groups[static_cast<std::size_t>(drop_idx)].visible = false;
            out.dropped_groups.push_back(groups[static_cast<std::size_t>(drop_idx)].kind);
        }

        // 2. Document name keeps natural width up to available room; hides if
        //    even name_min cannot fit without clipping.
        int name_width = (std::max)(0, in.name_text_width_px);
        const int available_for_name = in.client_width_px - total_width(0, 0);
        if (name_width > available_for_name) {
            name_width = (std::max)(0, available_for_name);
        }
        if (name_width < name_min && total_width(name_min, 0) > in.client_width_px) {
            name_width = 0;
            for (group_state& g : groups) {
                if (g.kind == cmd_bar_group::name) {
                    g.visible = false;
                }
            }
            out.dropped_groups.push_back(cmd_bar_group::name);
        }
        const int spacer_width = (std::max)(0, in.client_width_px - total_width(name_width, 0));

        // 3. Emit exact slot coordinates and hairline separator X coordinates.
        int x = pad_l;
        bool prev_visible = false;
        for (const group_state& g : groups) {
            if (!g.visible) {
                continue;
            }
            if (prev_visible) {
                if (g.separator_before) {
                    out.separators.push_back(x + sep_gap);
                    x += separator_width(sep_gap);
                } else {
                    x += gap;
                }
            }
            const int w = (g.kind == cmd_bar_group::name)
                              ? name_width
                              : (g.kind == cmd_bar_group::spacer ? spacer_width : g.width);
            switch (g.kind) {
                case cmd_bar_group::open:
                    out.slots.push_back({cmd_bar_item::open, x, w, true});
                    break;
                case cmd_bar_group::name:
                    out.slots.push_back({cmd_bar_item::document_name, x, w, w > 0});
                    break;
                case cmd_bar_group::nav_counter:
                    out.slots.push_back({cmd_bar_item::nav_counter, x, w, w > 0});
                    break;
                case cmd_bar_group::spacer:
                    break;
                case cmd_bar_group::nav:
                    out.slots.push_back({cmd_bar_item::nav_prev, x, icon_w, true});
                    out.slots.push_back({cmd_bar_item::nav_next, x + icon_w, icon_w, true});
                    break;
                case cmd_bar_group::zoom:
                    out.slots.push_back({cmd_bar_item::zoom_out, x, icon_w, true});
                    out.slots.push_back({cmd_bar_item::zoom_label, x + icon_w, zoom_label_w, true});
                    out.slots.push_back({cmd_bar_item::zoom_in, x + icon_w + zoom_label_w, icon_w, true});
                    break;
                case cmd_bar_group::view_mode:
                    out.slots.push_back({cmd_bar_item::view_fit, x, icon_w, true});
                    out.slots.push_back({cmd_bar_item::view_actual, x + icon_w, icon_w, true});
                    break;
                case cmd_bar_group::rotate:
                    out.slots.push_back({cmd_bar_item::rotate_left, x, icon_w, true});
                    out.slots.push_back({cmd_bar_item::rotate_right, x + icon_w, icon_w, true});
                    break;
                case cmd_bar_group::crop:
                    out.slots.push_back({cmd_bar_item::edit_crop, x, icon_w, true});
                    break;
                case cmd_bar_group::actions:
                    out.slots.push_back({cmd_bar_item::file_delete, x, icon_w, true});
                    out.slots.push_back({cmd_bar_item::file_print, x + icon_w, icon_w, true});
                    break;
                case cmd_bar_group::overflow:
                    // Even at ultra-narrow client widths (< ~85 DIP), clamp inside client area.
                    out.slots.push_back({cmd_bar_item::overflow,
                                         (std::min)(x, (std::max)(0, in.client_width_px - icon_w)),
                                         (std::min)(icon_w, in.client_width_px),
                                         true});
                    break;
            }
            x += w;
            prev_visible = true;
        }
        return out;
    } catch (...) {
        return cmd_bar_layout{};
    }
}

}  // namespace oz::viewer

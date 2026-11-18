// oz/viewer/std_icon_bar.hpp - Pure single-row command bar layout policy and command catalog.
//
// Models OZ PDF Viewer's ToolbarLayout.h:
// - Fixed 40 DIP single-row height (never wraps, never spawns a horizontal scrollbar).
// - 32 DIP icon buttons with hairline group separators.
// - Document name keeps its natural width up to available space, shrinks to an
//   ellipsized minimum, and hides only when even the minimum cannot fit.
// - Secondary right-hand clusters drop in deterministic priority order while
//   Open (left) and More commands / Overflow (right) never drop.
#pragma once

#include "ui_command_state.hpp"

#include <cstdint>
#include <vector>

namespace oz::viewer {

inline constexpr int kCommandBarHeightDip = 40;
inline constexpr int kCommandBarButtonDip = 32;
inline constexpr int kCommandBarGlyphDip = 18;
inline constexpr int kCommandBarLabelHeightDip = 20;
inline constexpr int kCommandBarPadLeftDip = 8;
inline constexpr int kCommandBarPadRightDip = 8;
inline constexpr int kCommandBarGroupGapDip = 6;
inline constexpr int kCommandBarSeparatorGapDip = 6;
inline constexpr int kCommandBarNameMinDip = 40;
inline constexpr int kCommandBarZoomLabelDip = 46;
inline constexpr int kCommandBarNavBaseDip = 18;
inline constexpr int kCommandBarNavDigitDip = 8;

// Controls hosted on the single-row command bar.
enum class cmd_bar_item : std::uint8_t {
    open,
    document_name,
    nav_counter,
    nav_prev,
    nav_next,
    zoom_out,
    zoom_label,
    zoom_in,
    view_fit,
    view_actual,
    rotate_left,
    rotate_right,
    edit_crop,
    file_delete,
    file_print,
    overflow
};

// Visual groups in left-to-right order. A group is dropped as an atomic unit.
enum class cmd_bar_group : std::uint8_t {
    open,
    name,
    nav_counter,
    spacer,
    nav,
    zoom,
    view_mode,
    rotate,
    crop,
    actions,
    overflow
};

[[nodiscard]] inline int scale_dip_round(int dip, int dpi) noexcept {
    const int effective = (dpi > 0) ? dpi : 96;
    return (dip * effective + 48) / 96;
}

struct cmd_bar_input {
    int client_width_px = 0;
    int dpi = 96;
    int nav_digits = 1;
    int name_text_width_px = 0;
    bool has_nav_counter = false;
    bool editing_enabled = true;
};

struct cmd_bar_slot {
    cmd_bar_item kind = cmd_bar_item::open;
    int x = 0;
    int width = 0;
    bool visible = false;
};

struct cmd_bar_layout {
    bool valid = false;
    int height_px = 0;
    std::vector<cmd_bar_slot> slots;
    // Device-pixel X coordinates of hairline vertical separators.
    std::vector<int> separators;
    // Drop order record (groups hidden because client width was too narrow).
    std::vector<cmd_bar_group> dropped_groups;

    [[nodiscard]] const cmd_bar_slot* find(cmd_bar_item kind) const noexcept {
        for (const cmd_bar_slot& s : slots) {
            if (s.kind == kind) {
                return &s;
            }
        }
        return nullptr;
    }

    [[nodiscard]] bool is_visible(cmd_bar_item kind) const noexcept {
        const cmd_bar_slot* s = find(kind);
        return s != nullptr && s->visible;
    }
};

// Computes the single-row command bar layout for |in.client_width_px|.
[[nodiscard]] cmd_bar_layout compute_command_bar_layout(const cmd_bar_input& in) noexcept;

// Ordered catalog of every active non-crop command (27) kept for descriptor
// parity and overflow-menu reachability tests.
struct std_icon_command {
    ui_command_id id;
    bool group_start = false;
};

[[nodiscard]] const std::vector<std_icon_command>& get_std_icon_commands() noexcept;

}  // namespace oz::viewer

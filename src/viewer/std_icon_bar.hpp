// oz/viewer/std_icon_bar.hpp - pure layout seam for the standard (non-crop)
// direct-icon command surface (icon-first boundary).
//
// This seam owns the deterministic, DPI-independent layout of the normal
// interactive icon bar: the ordered catalog of every active non-crop command,
// its group boundaries, and a bounded two-row horizontally scrollable packing
// that guarantees a minimum 40x40 DIP cell for every command. It is pure: no
// Win32 handles, no allocation outside std::vector, no side effects. The viewer
// chrome consumes the result to size and position a native toolbar plus a
// native horizontal scrollbar, so all commands stay reachable at any width
// (no overflow menu, no hidden command).
#pragma once

#include "ui_command_state.hpp"

#include <cstdint>
#include <vector>

namespace oz::viewer {

// Minimum interactive cell edge in DIP. Every command button is at least this
// square, matching the 40x40 DIP accessibility target.
inline constexpr float kStdIconCellDip = 40.0f;
// Width reserved for a group separator in DIP.
inline constexpr float kStdIconSeparatorDip = 8.0f;
// Vertical band padding in DIP (added once top and once bottom).
inline constexpr float kStdIconBarPadDip = 4.0f;
// Height of the horizontal scrollbar in DIP (only reserved when scrolling).
inline constexpr float kStdIconScrollbarDip = 16.0f;
// Hard upper bound on rows; the surface never grows past two rows.
inline constexpr std::int32_t kStdIconMaxRows = 2;

// One command in the standard non-crop surface. `group_start` marks the first
// command of a logical group; a separator is placed immediately before it.
struct std_icon_command {
    ui_command_id id;
    bool group_start = false;
};

// Ordered catalog of every active non-crop command (27), grouped as
// file/document, history/transform, and view/navigation.
[[nodiscard]] const std::vector<std_icon_command>& get_std_icon_commands() noexcept;

// One laid-out slot. `separator` slots carry no command and are inserted only
// between groups; command slots always reference a real stable command id.
struct std_icon_cell {
    ui_command_id id = ui_command_id::file_open;
    std::int32_t command_int_id = 0;
    std::int32_t row = 0;
    float x_dip = 0.0f;
    float width_dip = 0.0f;
    bool group_start = false;
    bool separator = false;
};

// Deterministic layout for a band of `available_width_dip` DIP.
struct std_icon_bar_layout {
    bool valid = false;
    std::int32_t rows = 0;                 // 1 or 2
    bool needs_scroll = false;             // content wider than the band
    float available_width_dip = 0.0f;
    float content_width_dip = 0.0f;        // width the toolbar control needs
    float scroll_range_dip = 0.0f;         // content - available when scrolling
    float bar_height_dip = 0.0f;           // rows + padding (+ scrollbar)
    std::vector<std_icon_cell> cells;      // separators and commands, in order
};

// Computes the bounded (<=2 row) layout for a band of `available_width_dip`.
// When the two-row minimum content width exceeds the band, the layout reports
// `needs_scroll` and a positive `scroll_range_dip`; every command still appears
// exactly once. Returns an invalid layout for a non-positive width.
[[nodiscard]] std_icon_bar_layout compute_std_icon_bar_layout(
    float available_width_dip) noexcept;

}  // namespace oz::viewer

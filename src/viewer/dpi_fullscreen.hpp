// oz/viewer/dpi_fullscreen.hpp - dependency-free seam for the WM_DPICHANGED
// destination-rectangle decision (Stage 10.2 fullscreen mixed-DPI fix).
//
// Windows sends WM_DPICHANGED with an OS-suggested RECT. In windowed mode the
// window must honor that suggestion (it preserves the window's DIP size across
// a DPI change). In borderless fullscreen, honoring it would shrink the window
// below the destination monitor's bounds while `is_fullscreen` remains true, so
// the destination monitor's full physical rcMonitor must be re-asserted
// instead. The suggested RECT is still the correct input for identifying the
// destination monitor - including monitors with negative screen coordinates
// (left of / above the primary).
//
// This header models the second half of that policy: the rectangle to apply
// once the caller has attempted the destination-monitor lookup. Keeping it pure
// makes the policy deterministic and testable without mixed-DPI hardware; the
// Win32 MonitorFromRect/GetMonitorInfo calls stay in main.cpp.
#pragma once

#include "ui_layout.hpp"

namespace oz::viewer {

// Destination monitor's full physical bounds (MONITORINFO::rcMonitor) after a
// successful lookup. `found == false` (or an empty rect) models a failed
// MonitorFromRect / GetMonitorInfo, which must fall back to the suggested RECT.
struct dpi_monitor_bounds {
    bool found = false;
    ui_rect_physical monitor{};

    [[nodiscard]] constexpr bool usable() const noexcept {
        return found && !monitor.is_empty();
    }
};

// Placement decision for WM_DPICHANGED.
struct dpi_target_rect {
    ui_rect_physical rect{};
    // True when `rect` is the destination monitor's rcMonitor (fullscreen
    // coverage); false when the OS-suggested RECT was applied.
    bool used_monitor_rect = false;
};

// Decide the window rectangle to apply for WM_DPICHANGED:
//  - windowed: the OS-suggested RECT (unchanged behavior);
//  - fullscreen with a resolved destination monitor: that monitor's rcMonitor
//    (true fullscreen, including a different DPI or negative origin);
//  - fullscreen with a failed monitor lookup: the suggested RECT (safe
//    fallback: never move the window to an unknown position).
[[nodiscard]] constexpr dpi_target_rect resolve_dpi_changed_target(
    const ui_rect_physical& suggested,
    const dpi_monitor_bounds& destination,
    bool is_fullscreen) noexcept {
    if (is_fullscreen && destination.usable()) {
        return dpi_target_rect{destination.monitor, true};
    }
    return dpi_target_rect{suggested, false};
}

}  // namespace oz::viewer

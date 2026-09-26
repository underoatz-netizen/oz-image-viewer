// oz/viewer/command_dispatch.hpp - pure command-catalog dispatch mapping seam.
//
// The icon-first redesign routes every command through a single catalog ID.
// This seam maps a catalog ID to the existing handler intent (zoom / navigation
// / paste) and reports whether the authoritative command state permits it. It
// is deliberately pure: no Win32 handles, no clipboard access, no side effects.
// The viewer's thin adapter consults this plan and then invokes the existing
// view/navigation/paste paths, so all dirty-discard/crop/file/busy safeguards
// stay owned by those paths.
#pragma once

#include "ui_command_state.hpp"

namespace oz::viewer {

// Existing handler intent a catalog command routes to. `none` means the ID is
// not part of this adapter's boundary (the caller keeps its own handling).
enum class command_dispatch_kind {
    none,
    zoom_in,
    zoom_out,
    nav_first,
    nav_prev,
    nav_next,
    nav_last,
    paste_image
};

// Pure dispatch plan: the intent plus the authoritative enabled state derived
// from the same ui_command_context the UI uses for enablement.
struct command_dispatch_plan {
    command_dispatch_kind kind = command_dispatch_kind::none;
    bool enabled = false;
};

// Maps a catalog command ID to its dispatch intent and enabled state. Returns
// {none, false} for IDs outside this adapter's boundary. For a recognized ID,
// `enabled` is exactly derive_command_state(id, ctx).enabled, so the adapter
// can never disagree with the menu/toolbar enablement.
[[nodiscard]] command_dispatch_plan plan_command_dispatch(ui_command_id id,
                                                          const ui_command_context& ctx) noexcept;

}  // namespace oz::viewer

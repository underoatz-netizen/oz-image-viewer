// oz/viewer/command_dispatch.cpp - pure command-catalog dispatch mapping seam.
#include "command_dispatch.hpp"

namespace oz::viewer {

command_dispatch_plan plan_command_dispatch(ui_command_id id,
                                            const ui_command_context& ctx) noexcept {
    command_dispatch_plan plan{};

    switch (id) {
        case ui_command_id::view_zoom_in:
            plan.kind = command_dispatch_kind::zoom_in;
            break;
        case ui_command_id::view_zoom_out:
            plan.kind = command_dispatch_kind::zoom_out;
            break;
        case ui_command_id::nav_first:
            plan.kind = command_dispatch_kind::nav_first;
            break;
        case ui_command_id::nav_prev:
            plan.kind = command_dispatch_kind::nav_prev;
            break;
        case ui_command_id::nav_next:
            plan.kind = command_dispatch_kind::nav_next;
            break;
        case ui_command_id::nav_last:
            plan.kind = command_dispatch_kind::nav_last;
            break;
        case ui_command_id::file_paste_image:
            plan.kind = command_dispatch_kind::paste_image;
            break;
        default:
            // Not part of this adapter's boundary: the caller keeps its own
            // handling (e.g. Fit, Actual Size, Fullscreen, file/edit commands).
            return plan;
    }

    // Single source of truth: the adapter's enabled state is exactly the
    // authoritative command state, so a disabled command is always a no-op.
    plan.enabled = derive_command_state(id, ctx).enabled;
    return plan;
}

}  // namespace oz::viewer

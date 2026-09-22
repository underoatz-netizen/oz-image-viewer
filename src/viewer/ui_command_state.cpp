// oz/viewer/ui_command_state.cpp - presentation-only UI command-state seam implementation.
#include "ui_command_state.hpp"

namespace oz::viewer {

ui_item_state derive_command_state(ui_command_id id, const ui_command_context& ctx) noexcept {
    ui_item_state state{};
    state.visible = true;

    // Benchmark mode disables interactive UI commands
    if (ctx.benchmark_mode) {
        state.enabled = false;
        state.checked = false;
        state.visible = false;
        return state;
    }

    const bool doc_ready = ctx.have_image && ctx.has_document;
    const bool not_modal = !ctx.crop_active && !ctx.resize_busy && !ctx.save_busy && !ctx.copy_busy;
    const bool edit_allowed = doc_ready && not_modal;

    switch (id) {
        case ui_command_id::file_open:
            // File open is allowed whenever not actively busy with save or modal resize
            state.enabled = !ctx.save_busy && !ctx.resize_busy;
            break;

        case ui_command_id::btn_open:
            // The centered Open button is only visible and enabled when no image is loaded
            state.visible = !ctx.have_image;
            state.enabled = !ctx.have_image && !ctx.save_busy && !ctx.resize_busy;
            break;

        case ui_command_id::file_save:
            // Save requires document, source, dirty state, and no active crop/resize/save
            state.enabled = doc_ready && ctx.has_source && ctx.is_dirty && not_modal;
            break;

        case ui_command_id::file_save_as:
            // Save As requires document, source, and no active crop/resize/save
            state.enabled = doc_ready && ctx.has_source && not_modal;
            break;

        case ui_command_id::file_open_location:
        case ui_command_id::file_copy:
        case ui_command_id::file_copy_path:
            // File operations require a successfully presented image, committed document, valid file path, and not modal
            state.enabled = ctx.have_image && ctx.has_document && ctx.has_file_path && not_modal;
            break;

        case ui_command_id::file_copy_image:
            // Copy Image requires committed document/image, adequate source or file path to acquire, and not modal
            state.enabled = ctx.have_image && ctx.has_document && (ctx.has_source || ctx.has_file_path) && not_modal;
            break;

        case ui_command_id::edit_rotate_left:
        case ui_command_id::edit_rotate_right:
        case ui_command_id::edit_flip_h:
        case ui_command_id::edit_flip_v:
        case ui_command_id::edit_crop:
        case ui_command_id::edit_resize:
            state.enabled = edit_allowed;
            break;

        case ui_command_id::edit_undo:
            state.enabled = edit_allowed && ctx.can_undo;
            break;

        case ui_command_id::edit_redo:
            state.enabled = edit_allowed && ctx.can_redo;
            break;

        case ui_command_id::edit_crop_apply:
        case ui_command_id::edit_crop_cancel:
        case ui_command_id::edit_crop_reset:
            state.enabled = ctx.crop_active;
            break;

        case ui_command_id::crop_ratio_free:
            state.enabled = ctx.crop_active;
            state.checked = ctx.crop_active && (ctx.crop_preset == oz::viewer_core::crop_ratio_preset::free);
            break;

        case ui_command_id::crop_ratio_original:
            state.enabled = ctx.crop_active;
            state.checked = ctx.crop_active && (ctx.crop_preset == oz::viewer_core::crop_ratio_preset::original);
            break;

        case ui_command_id::crop_ratio_1x1:
            state.enabled = ctx.crop_active;
            state.checked = ctx.crop_active && (ctx.crop_preset == oz::viewer_core::crop_ratio_preset::square_1x1);
            break;

        case ui_command_id::crop_ratio_3x2:
            state.enabled = ctx.crop_active;
            state.checked = ctx.crop_active && (ctx.crop_preset == oz::viewer_core::crop_ratio_preset::ratio_3x2);
            break;

        case ui_command_id::crop_ratio_4x3:
            state.enabled = ctx.crop_active;
            state.checked = ctx.crop_active && (ctx.crop_preset == oz::viewer_core::crop_ratio_preset::ratio_4x3);
            break;

        case ui_command_id::crop_ratio_5x4:
            state.enabled = ctx.crop_active;
            state.checked = ctx.crop_active && (ctx.crop_preset == oz::viewer_core::crop_ratio_preset::ratio_5x4);
            break;

        case ui_command_id::crop_ratio_16x9:
            state.enabled = ctx.crop_active;
            state.checked = ctx.crop_active && (ctx.crop_preset == oz::viewer_core::crop_ratio_preset::ratio_16x9);
            break;

        case ui_command_id::crop_ratio_9x16:
            state.enabled = ctx.crop_active;
            state.checked = ctx.crop_active && (ctx.crop_preset == oz::viewer_core::crop_ratio_preset::ratio_9x16);
            break;

        case ui_command_id::crop_ratio_a_series:
            state.enabled = ctx.crop_active;
            state.checked = ctx.crop_active && (ctx.crop_preset == oz::viewer_core::crop_ratio_preset::a_series);
            break;

        case ui_command_id::view_actual_size:
        case ui_command_id::view_fit:
        case ui_command_id::view_zoom_in:
        case ui_command_id::view_zoom_out:
            state.enabled = ctx.have_image;
            if (id == ui_command_id::view_fit) {
                state.checked = ctx.have_image && ctx.is_fit_mode;
            } else if (id == ui_command_id::view_actual_size) {
                state.checked = ctx.have_image && !ctx.is_fit_mode;
            }
            break;

        case ui_command_id::nav_prev:
            state.enabled = ctx.have_image && ctx.can_navigate_prev && not_modal;
            break;

        case ui_command_id::nav_next:
            state.enabled = ctx.have_image && ctx.can_navigate_next && not_modal;
            break;

        case ui_command_id::nav_first:
        case ui_command_id::nav_last:
            state.enabled = ctx.have_image && ctx.has_navigation_items && not_modal;
            break;

        case ui_command_id::view_fullscreen:
            state.enabled = true;
            state.checked = ctx.is_fullscreen;
            break;

        default:
            state.enabled = false;
            state.checked = false;
            state.visible = false;
            break;
    }

    return state;
}

}  // namespace oz::viewer

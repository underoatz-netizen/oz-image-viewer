// oz/viewer/resize_dialog.hpp - Native modal Win32 Resize dialog (Stage 5 Slice 5).
//
// The dialog is a two-phase, presentation-only control surface:
// - Live input reports an optional *draft* target through preview_cb. A draft
//   never touches edit history and never owns image pixels.
// - Apply invokes apply_cb(dialog, target) exactly once and then waits. The
//   caller asynchronously validates/materializes the proposed command sequence
//   and reports the outcome with notify_resize_dialog_result(); no history
//   command is committed until that matching validation succeeds.
#pragma once

#include "oz/viewer_core/edit_state.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <functional>
#include <optional>
#include <string>

namespace oz::viewer {

struct resize_dialog_result {
    bool accepted = false;
    oz::viewer_core::edit_dimensions target_dimensions{};
};

// Invoked on every valid input change with the draft target, or with nullopt
// when the inputs are back at the opening canvas dimensions (Reset, or a
// no-op target) which clears the presentation draft.
using resize_preview_fn =
    std::function<void(const std::optional<oz::viewer_core::edit_dimensions>&)>;

// Invoked once when the user presses Apply for a valid, non-no-op target. The
// implementation must return promptly after capturing/submitting exactly one
// validation request; the dialog remains modal and responsive until
// notify_resize_dialog_result() is called for that dialog.
using resize_apply_fn =
    std::function<void(HWND dialog, const oz::viewer_core::edit_dimensions& target)>;

// Shows the native modal Resize dialog for the current logical canvas.
// Returns after the dialog closes; `accepted` is true only when Apply closed
// the dialog (including the no-op case where no history command was added).
resize_dialog_result show_resize_dialog(
    HWND parent,
    const oz::viewer_core::edit_dimensions& current_dims,
    resize_preview_fn preview_cb,
    resize_apply_fn apply_cb);

// Delivers the outcome of a pending Apply to the modal dialog. Must be called
// on the UI thread that owns the dialog.
// - ok = true: the validated command was committed; the dialog closes accepted.
// - ok = false: history is unchanged; the dialog re-enables its inputs and
//   shows the bounded error text while remaining usable.
// Safe to call for a destroyed or non-pending dialog (no-op).
void notify_resize_dialog_result(HWND dialog, bool ok, const std::wstring& error);

}  // namespace oz::viewer

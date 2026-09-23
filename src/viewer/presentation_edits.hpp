// oz/viewer/presentation_edits.hpp - Stage 9.5 M1 presentation-planning policy seam.
//
// The edit-aware presentation plan (document edit history + draft resize) may
// only be built from the *current* document. Transactional document swaps
// (file open commit, navigation commit, clipboard paste) install the incoming
// bitmap while app.image_doc still describes the document being replaced, and
// mark that window with app_state::suppress_edit_presentation. Suppression
// therefore wins over edit state: this predicate returns false before any
// history/draft state is consulted, and the planner falls back to the plain
// non-edit fit plan for the incoming bitmap.
#pragma once

#include "oz/viewer_core/image_document.hpp"

#include <optional>

namespace oz::viewer {

// True only when the active document's edit state may drive the presentation
// plan. `suppress_edit_presentation` is checked first so a transactional swap
// can never observe the replaced document's history or draft resize.
[[nodiscard]] inline bool should_use_edit_presentation(
    bool suppress_edit_presentation,
    const oz::viewer_core::image_document* document,
    const std::optional<oz::viewer_core::edit_dimensions>& draft_resize) noexcept {
    if (suppress_edit_presentation) {
        return false;
    }
    return document != nullptr &&
           (document->history().total_commands() > 0 || draft_resize.has_value());
}

}  // namespace oz::viewer

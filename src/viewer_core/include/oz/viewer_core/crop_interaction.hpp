// oz/viewer_core/crop_interaction.hpp - pure crop-overlay interaction model.
#pragma once

#include "oz/viewer_core/edit_state.hpp"

#include <cstdint>

namespace oz::viewer_core {

// The visual handle selected by an in-progress crop drag. Coordinates given to
// this model are logical image-pixel coordinates, never HWND/D2D coordinates.
enum class crop_drag_target : std::uint8_t {
    none,
    create,
    move,
    top_left,
    top,
    top_right,
    right,
    bottom_right,
    bottom,
    bottom_left,
    left
};

// UI-thread-owned, allocation-free state for one pending crop. It is a draft
// only: no edit_history command is changed until the caller explicitly applies
// `selection` as a crop_command.
struct crop_interaction_state {
    bool active = false;
    crop_ratio_preset preset = crop_ratio_preset::free;
    bool portrait_ratio = false;
    edit_dimensions canvas{};
    edit_rect selection{};
    crop_drag_target drag = crop_drag_target::none;
    edit_rect drag_initial{};
    double drag_start_x = 0.0;
    double drag_start_y = 0.0;
};

[[nodiscard]] crop_interaction_state begin_crop_interaction(
    edit_dimensions canvas,
    crop_ratio_preset preset = crop_ratio_preset::free) noexcept;

void reset_crop_interaction(crop_interaction_state& state) noexcept;

// Changes the constraint and resets to the largest centered valid selection.
void set_crop_interaction_ratio(crop_interaction_state& state,
                                crop_ratio_preset preset) noexcept;

// Tests the eight handles, then the interior (move), otherwise returns create.
// `handle_radius` is expressed in logical image pixels and is caller-scaled
// from the desired on-screen handle radius.
[[nodiscard]] crop_drag_target crop_hit_test(const crop_interaction_state& state,
                                              double x, double y,
                                              double handle_radius) noexcept;

[[nodiscard]] bool begin_crop_drag(crop_interaction_state& state,
                                   double x, double y,
                                   double handle_radius) noexcept;

// Updates the draft selection. Returns true only when the selection changed.
[[nodiscard]] bool update_crop_drag(crop_interaction_state& state,
                                    double x, double y) noexcept;

void end_crop_drag(crop_interaction_state& state) noexcept;

}  // namespace oz::viewer_core

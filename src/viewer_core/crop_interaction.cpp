// oz/viewer_core/crop_interaction.cpp - pure crop-overlay interaction model.
#include "oz/viewer_core/crop_interaction.hpp"

#include <algorithm>
#include <cmath>

namespace oz::viewer_core {
namespace {

double clamp_coord(double value, std::int32_t upper) noexcept {
    return std::clamp(value, 0.0, static_cast<double>(upper));
}

bool prefers_portrait(crop_ratio_preset preset,
                      const edit_dimensions& canvas) noexcept {
    switch (preset) {
        case crop_ratio_preset::ratio_9x16:
            return true;
        case crop_ratio_preset::a_series:
            return canvas.height > canvas.width;
        default:
            return false;
    }
}

std::optional<double> active_aspect(const crop_interaction_state& state) noexcept {
    return get_crop_aspect_ratio(state.preset, state.canvas, state.portrait_ratio);
}

edit_rect full_canvas(const edit_dimensions& canvas) noexcept {
    return canvas.is_valid() ? edit_rect{0, 0, canvas.width, canvas.height}
                             : edit_rect{};
}

edit_rect rect_from_bounds(double left, double top, double right, double bottom,
                           const edit_dimensions& canvas) noexcept {
    if (!canvas.is_valid()) {
        return {};
    }
    left = clamp_coord(left, canvas.width);
    top = clamp_coord(top, canvas.height);
    right = clamp_coord(right, canvas.width);
    bottom = clamp_coord(bottom, canvas.height);
    if (right < left) std::swap(left, right);
    if (bottom < top) std::swap(top, bottom);

    std::int32_t x = std::clamp(static_cast<std::int32_t>(std::floor(left)),
                                0, canvas.width - 1);
    std::int32_t y = std::clamp(static_cast<std::int32_t>(std::floor(top)),
                                0, canvas.height - 1);
    std::int32_t r = std::clamp(static_cast<std::int32_t>(std::ceil(right)),
                                x + 1, canvas.width);
    std::int32_t b = std::clamp(static_cast<std::int32_t>(std::ceil(bottom)),
                                y + 1, canvas.height);
    return {x, y, r - x, b - y};
}

edit_rect translate_rect(const edit_rect& rect, double dx, double dy,
                         const edit_dimensions& canvas) noexcept {
    if (!rect.is_valid() || !canvas.is_valid()) {
        return {};
    }
    const std::int32_t max_x = canvas.width - rect.width;
    const std::int32_t max_y = canvas.height - rect.height;
    const std::int32_t x = std::clamp(
        static_cast<std::int32_t>(std::lround(static_cast<double>(rect.x) + dx)),
        0, max_x);
    const std::int32_t y = std::clamp(
        static_cast<std::int32_t>(std::lround(static_cast<double>(rect.y) + dy)),
        0, max_y);
    return {x, y, rect.width, rect.height};
}

edit_rect free_resize(const crop_interaction_state& state, double x, double y) noexcept {
    const edit_rect& initial = state.drag_initial;
    double left = initial.x;
    double right = initial.right();
    double top = initial.y;
    double bottom = initial.bottom();
    x = clamp_coord(x, state.canvas.width);
    y = clamp_coord(y, state.canvas.height);

    switch (state.drag) {
        case crop_drag_target::top_left: left = x; top = y; break;
        case crop_drag_target::top: top = y; break;
        case crop_drag_target::top_right: right = x; top = y; break;
        case crop_drag_target::right: right = x; break;
        case crop_drag_target::bottom_right: right = x; bottom = y; break;
        case crop_drag_target::bottom: bottom = y; break;
        case crop_drag_target::bottom_left: left = x; bottom = y; break;
        case crop_drag_target::left: left = x; break;
        default: break;
    }

    if (right - left < 1.0) {
        if (state.drag == crop_drag_target::left ||
            state.drag == crop_drag_target::top_left ||
            state.drag == crop_drag_target::bottom_left) {
            left = right - 1.0;
        } else {
            right = left + 1.0;
        }
    }
    if (bottom - top < 1.0) {
        if (state.drag == crop_drag_target::top ||
            state.drag == crop_drag_target::top_left ||
            state.drag == crop_drag_target::top_right) {
            top = bottom - 1.0;
        } else {
            bottom = top + 1.0;
        }
    }
    return rect_from_bounds(left, top, right, bottom, state.canvas);
}

edit_rect aspect_from_corner(const edit_dimensions& canvas, double anchor_x,
                             double anchor_y, int sx, int sy, double x,
                             double y, double aspect) noexcept {
    if (!canvas.is_valid() || aspect <= 0.0 || sx == 0 || sy == 0) {
        return {};
    }
    const double max_w_x = sx > 0 ? static_cast<double>(canvas.width) - anchor_x : anchor_x;
    const double max_h_y = sy > 0 ? static_cast<double>(canvas.height) - anchor_y : anchor_y;
    const double max_w = std::max(0.0, std::min(max_w_x, max_h_y * aspect));
    if (max_w < 1.0) {
        return {};
    }

    const double want_w = std::max(std::abs(x - anchor_x), std::abs(y - anchor_y) * aspect);
    const double width = std::clamp(want_w, 1.0, max_w);
    const double height = width / aspect;
    const double left = sx > 0 ? anchor_x : anchor_x - width;
    const double right = sx > 0 ? anchor_x + width : anchor_x;
    const double top = sy > 0 ? anchor_y : anchor_y - height;
    const double bottom = sy > 0 ? anchor_y + height : anchor_y;
    return rect_from_bounds(left, top, right, bottom, canvas);
}

edit_rect aspect_resize(const crop_interaction_state& state, double x, double y,
                        double aspect) noexcept {
    const edit_rect& initial = state.drag_initial;
    const double left = initial.x;
    const double right = initial.right();
    const double top = initial.y;
    const double bottom = initial.bottom();
    const double cx = (left + right) * 0.5;
    const double cy = (top + bottom) * 0.5;
    x = clamp_coord(x, state.canvas.width);
    y = clamp_coord(y, state.canvas.height);

    switch (state.drag) {
        case crop_drag_target::top_left:
            return aspect_from_corner(state.canvas, right, bottom, -1, -1, x, y, aspect);
        case crop_drag_target::top_right:
            return aspect_from_corner(state.canvas, left, bottom, 1, -1, x, y, aspect);
        case crop_drag_target::bottom_right:
            return aspect_from_corner(state.canvas, left, top, 1, 1, x, y, aspect);
        case crop_drag_target::bottom_left:
            return aspect_from_corner(state.canvas, right, top, -1, 1, x, y, aspect);
        case crop_drag_target::top:
        case crop_drag_target::bottom: {
            const int sy = state.drag == crop_drag_target::top ? -1 : 1;
            const double anchor_y = state.drag == crop_drag_target::top ? bottom : top;
            const double max_h_y = sy > 0 ? static_cast<double>(state.canvas.height) - anchor_y : anchor_y;
            const double max_w_centered = 2.0 * std::min(cx, static_cast<double>(state.canvas.width) - cx);
            const double max_h = std::max(0.0, std::min(max_h_y, max_w_centered / aspect));
            if (max_h < 1.0) {
                return initial;
            }
            const double height = std::clamp(std::abs(y - anchor_y), 1.0, max_h);
            const double width = height * aspect;
            const double new_top = sy > 0 ? anchor_y : anchor_y - height;
            const double new_bottom = sy > 0 ? anchor_y + height : anchor_y;
            return rect_from_bounds(cx - width * 0.5, new_top,
                                    cx + width * 0.5, new_bottom, state.canvas);
        }
        case crop_drag_target::left:
        case crop_drag_target::right: {
            const int sx = state.drag == crop_drag_target::left ? -1 : 1;
            const double anchor_x = state.drag == crop_drag_target::left ? right : left;
            const double max_w_x = sx > 0 ? static_cast<double>(state.canvas.width) - anchor_x : anchor_x;
            const double max_h_centered = 2.0 * std::min(cy, static_cast<double>(state.canvas.height) - cy);
            const double max_w = std::max(0.0, std::min(max_w_x, max_h_centered * aspect));
            if (max_w < 1.0) {
                return initial;
            }
            const double width = std::clamp(std::abs(x - anchor_x), 1.0, max_w);
            const double height = width / aspect;
            const double new_left = sx > 0 ? anchor_x : anchor_x - width;
            const double new_right = sx > 0 ? anchor_x + width : anchor_x;
            return rect_from_bounds(new_left, cy - height * 0.5,
                                    new_right, cy + height * 0.5, state.canvas);
        }
        default:
            return initial;
    }
}

}  // namespace

crop_interaction_state begin_crop_interaction(edit_dimensions canvas,
                                              crop_ratio_preset preset) noexcept {
    crop_interaction_state state;
    state.active = canvas.is_valid();
    state.canvas = canvas;
    state.preset = preset;
    state.portrait_ratio = prefers_portrait(preset, canvas);
    reset_crop_interaction(state);
    return state;
}

void reset_crop_interaction(crop_interaction_state& state) noexcept {
    state.drag = crop_drag_target::none;
    state.drag_initial = {};
    if (!state.active || !state.canvas.is_valid()) {
        state.selection = {};
        return;
    }
    if (const auto aspect = active_aspect(state); aspect.has_value()) {
        state.selection = compute_centered_aspect_crop(state.canvas, *aspect);
    } else {
        state.selection = full_canvas(state.canvas);
    }
}

void set_crop_interaction_ratio(crop_interaction_state& state,
                                crop_ratio_preset preset) noexcept {
    state.preset = preset;
    state.portrait_ratio = prefers_portrait(preset, state.canvas);
    reset_crop_interaction(state);
}

crop_drag_target crop_hit_test(const crop_interaction_state& state, double x,
                               double y, double handle_radius) noexcept {
    if (!state.active || !state.selection.is_valid()) {
        return crop_drag_target::none;
    }
    const edit_rect& r = state.selection;
    const double left = r.x;
    const double right = r.right();
    const double top = r.y;
    const double bottom = r.bottom();
    const double cx = (left + right) * 0.5;
    const double cy = (top + bottom) * 0.5;
    const double radius = std::max(0.5, handle_radius);
    const auto near = [radius](double a, double b) noexcept { return std::abs(a - b) <= radius; };

    if (near(x, left) && near(y, top)) return crop_drag_target::top_left;
    if (near(x, right) && near(y, top)) return crop_drag_target::top_right;
    if (near(x, right) && near(y, bottom)) return crop_drag_target::bottom_right;
    if (near(x, left) && near(y, bottom)) return crop_drag_target::bottom_left;
    if (near(x, cx) && near(y, top)) return crop_drag_target::top;
    if (near(x, right) && near(y, cy)) return crop_drag_target::right;
    if (near(x, cx) && near(y, bottom)) return crop_drag_target::bottom;
    if (near(x, left) && near(y, cy)) return crop_drag_target::left;
    if (x >= left && x <= right && y >= top && y <= bottom) {
        return crop_drag_target::move;
    }
    return crop_drag_target::create;
}

bool begin_crop_drag(crop_interaction_state& state, double x, double y,
                     double handle_radius) noexcept {
    if (!state.active || !state.canvas.is_valid()) {
        return false;
    }
    state.drag = crop_hit_test(state, x, y, handle_radius);
    if (state.drag == crop_drag_target::none) {
        return false;
    }
    state.drag_initial = state.selection;
    state.drag_start_x = clamp_coord(x, state.canvas.width);
    state.drag_start_y = clamp_coord(y, state.canvas.height);
    return true;
}

bool update_crop_drag(crop_interaction_state& state, double x, double y) noexcept {
    if (!state.active || !state.canvas.is_valid() ||
        state.drag == crop_drag_target::none) {
        return false;
    }

    const edit_rect before = state.selection;
    x = clamp_coord(x, state.canvas.width);
    y = clamp_coord(y, state.canvas.height);

    if (state.drag == crop_drag_target::move) {
        state.selection = translate_rect(state.drag_initial,
                                         x - state.drag_start_x,
                                         y - state.drag_start_y,
                                         state.canvas);
    } else if (state.drag == crop_drag_target::create) {
        if (const auto aspect = active_aspect(state); aspect.has_value()) {
            const int sx = x >= state.drag_start_x ? 1 : -1;
            const int sy = y >= state.drag_start_y ? 1 : -1;
            state.selection = aspect_from_corner(state.canvas, state.drag_start_x,
                                                  state.drag_start_y, sx, sy, x, y,
                                                  *aspect);
        } else {
            state.selection = rect_from_bounds(state.drag_start_x, state.drag_start_y,
                                               x, y, state.canvas);
        }
    } else if (const auto aspect = active_aspect(state); aspect.has_value()) {
        state.selection = aspect_resize(state, x, y, *aspect);
    } else {
        state.selection = free_resize(state, x, y);
    }

    if (!state.selection.is_valid()) {
        state.selection = before;
    }
    return state.selection != before;
}

void end_crop_drag(crop_interaction_state& state) noexcept {
    state.drag = crop_drag_target::none;
    state.drag_initial = {};
}

}  // namespace oz::viewer_core

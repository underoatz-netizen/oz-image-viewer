// oz/viewer_core/edit_state.cpp - pure non-destructive edit model and history foundation (Stage 5 Slice 1).
#include "oz/viewer_core/edit_state.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace oz::viewer_core {

namespace {

constexpr double kSqrt2 = 1.41421356237309504880;

bool rect_is_within(const edit_rect& rect, const edit_dimensions& dims) noexcept {
    return dims.is_valid() && rect.is_valid() &&
           rect.right() <= dims.width && rect.bottom() <= dims.height;
}

}  // namespace

std::optional<edit_rect> map_half_open_rect(
    const d4_transform& transform,
    const edit_rect& rect,
    const edit_dimensions& input_dims) noexcept {
    if (!rect_is_within(rect, input_dims)) {
        return std::nullopt;
    }

    // D4 point mappings operate on pixel centers and therefore use dimension -
    // 1 for a reflected axis. Rectangle bounds are continuous half-open edges,
    // so their corresponding affine translation uses the full dimension.
    const auto map_bound = [&transform, &input_dims](std::int32_t x,
                                                     std::int32_t y) noexcept {
        const std::int64_t tx =
            (transform.m00 < 0 ? static_cast<std::int64_t>(input_dims.width) : 0) +
            (transform.m01 < 0 ? static_cast<std::int64_t>(input_dims.height) : 0);
        const std::int64_t ty =
            (transform.m10 < 0 ? static_cast<std::int64_t>(input_dims.width) : 0) +
            (transform.m11 < 0 ? static_cast<std::int64_t>(input_dims.height) : 0);
        return std::pair<std::int64_t, std::int64_t>{
            static_cast<std::int64_t>(transform.m00) * x +
                static_cast<std::int64_t>(transform.m01) * y + tx,
            static_cast<std::int64_t>(transform.m10) * x +
                static_cast<std::int64_t>(transform.m11) * y + ty};
    };

    const auto p0 = map_bound(rect.x, rect.y);
    const auto p1 = map_bound(rect.right(), rect.y);
    const auto p2 = map_bound(rect.x, rect.bottom());
    const auto p3 = map_bound(rect.right(), rect.bottom());

    const std::int64_t left = (std::min)({p0.first, p1.first, p2.first, p3.first});
    const std::int64_t right = (std::max)({p0.first, p1.first, p2.first, p3.first});
    const std::int64_t top = (std::min)({p0.second, p1.second, p2.second, p3.second});
    const std::int64_t bottom = (std::max)({p0.second, p1.second, p2.second, p3.second});

    const edit_dimensions output_dims = transform.transform_dimensions(input_dims);
    if (left < 0 || top < 0 || right <= left || bottom <= top ||
        right > output_dims.width || bottom > output_dims.height ||
        right - left > kMaxEditDimension || bottom - top > kMaxEditDimension) {
        return std::nullopt;
    }

    return edit_rect{static_cast<std::int32_t>(left), static_cast<std::int32_t>(top),
                     static_cast<std::int32_t>(right - left),
                     static_cast<std::int32_t>(bottom - top)};
}

std::optional<double> get_crop_aspect_ratio(
    crop_ratio_preset preset,
    const edit_dimensions& canvas_dims,
    bool portrait) noexcept {
    switch (preset) {
        case crop_ratio_preset::free:
            return std::nullopt;

        case crop_ratio_preset::original: {
            if (canvas_dims.width <= 0 || canvas_dims.height <= 0) {
                return 1.0;
            }
            return static_cast<double>(canvas_dims.width) / static_cast<double>(canvas_dims.height);
        }

        case crop_ratio_preset::square_1x1:
            return 1.0;

        case crop_ratio_preset::ratio_3x2:
            return portrait ? (2.0 / 3.0) : (3.0 / 2.0);

        case crop_ratio_preset::ratio_4x3:
            return portrait ? (3.0 / 4.0) : (4.0 / 3.0);

        case crop_ratio_preset::ratio_5x4:
            return portrait ? (4.0 / 5.0) : (5.0 / 4.0);

        case crop_ratio_preset::ratio_16x9:
            return portrait ? (9.0 / 16.0) : (16.0 / 9.0);

        case crop_ratio_preset::ratio_9x16:
            return portrait ? (16.0 / 9.0) : (9.0 / 16.0);

        case crop_ratio_preset::a_series: {
            // Adaptive A-series ratio:
            // Standard ISO 216 paper ratio is 1 : sqrt(2).
            // If portrait requested (or canvas is taller than wide and portrait wasn't forced false), 1 / sqrt(2).
            return portrait ? (1.0 / kSqrt2) : kSqrt2;
        }
    }
    return std::nullopt;
}

edit_rect clamp_crop_rect(
    const edit_rect& requested,
    const edit_dimensions& canvas_dims) noexcept {
    if (canvas_dims.width <= 0 || canvas_dims.height <= 0) {
        return {0, 0, 0, 0};
    }

    // Clamp coordinates to canvas
    std::int32_t x = std::clamp(requested.x, 0, canvas_dims.width - 1);
    std::int32_t y = std::clamp(requested.y, 0, canvas_dims.height - 1);

    // Max available width and height starting from clamped (x, y)
    std::int32_t max_w = canvas_dims.width - x;
    std::int32_t max_h = canvas_dims.height - y;

    // Width and height must be at least 1 and fit within canvas
    std::int32_t w = std::clamp(requested.width, 1, max_w);
    std::int32_t h = std::clamp(requested.height, 1, max_h);

    return {x, y, w, h};
}

edit_rect compute_centered_aspect_crop(
    const edit_dimensions& canvas_dims,
    double target_aspect_ratio) noexcept {
    if (canvas_dims.width <= 0 || canvas_dims.height <= 0 || target_aspect_ratio <= 0.0) {
        return {0, 0, 0, 0};
    }

    const double canvas_aspect = static_cast<double>(canvas_dims.width) / static_cast<double>(canvas_dims.height);
    std::int32_t crop_w = 0;
    std::int32_t crop_h = 0;

    if (canvas_aspect > target_aspect_ratio) {
        // Canvas is wider than target aspect: fit height, compute width
        crop_h = canvas_dims.height;
        crop_w = std::clamp(static_cast<std::int32_t>(std::round(crop_h * target_aspect_ratio)), 1, canvas_dims.width);
    } else {
        // Canvas is taller than target aspect: fit width, compute height
        crop_w = canvas_dims.width;
        crop_h = std::clamp(static_cast<std::int32_t>(std::round(crop_w / target_aspect_ratio)), 1, canvas_dims.height);
    }

    std::int32_t x = (canvas_dims.width - crop_w) / 2;
    std::int32_t y = (canvas_dims.height - crop_h) / 2;

    return {x, y, crop_w, crop_h};
}

std::optional<edit_dimensions> calculate_resize_dimensions(
    const resize_calculation_params& params) noexcept {
    if (!params.current_dims.is_valid()) {
        return std::nullopt;
    }

    const double orig_w = static_cast<double>(params.current_dims.width);
    const double orig_h = static_cast<double>(params.current_dims.height);

    if (params.unit == resize_unit::percent) {
        // Percent mode is finite and strictly positive: NaN/Infinity would slip
        // through the range comparisons and produce an out-of-contract cast.
        if (!params.requested_percent.has_value() || !std::isfinite(*params.requested_percent) ||
            *params.requested_percent <= 0.0) {
            return std::nullopt;
        }
        const double factor = *params.requested_percent / 100.0;
        const double target_w = std::round(orig_w * factor);
        const double target_h = std::round(orig_h * factor);

        if (target_w < 1.0 || target_h < 1.0 ||
            target_w > static_cast<double>(kMaxEditDimension) ||
            target_h > static_cast<double>(kMaxEditDimension)) {
            return std::nullopt;
        }

        return edit_dimensions{static_cast<std::int32_t>(target_w), static_cast<std::int32_t>(target_h)};
    }

    // Unit: pixels
    if (!params.requested_width.has_value() && !params.requested_height.has_value()) {
        return std::nullopt;
    }

    if (params.requested_width.has_value() && *params.requested_width <= 0) {
        return std::nullopt;
    }
    if (params.requested_height.has_value() && *params.requested_height <= 0) {
        return std::nullopt;
    }

    if (params.maintain_aspect_ratio) {
        if (params.requested_width.has_value() && !params.requested_height.has_value()) {
            const double w = static_cast<double>(*params.requested_width);
            const double h = std::round(w * (orig_h / orig_w));
            if (h < 1.0 || h > static_cast<double>(kMaxEditDimension) || w > static_cast<double>(kMaxEditDimension)) {
                return std::nullopt;
            }
            return edit_dimensions{static_cast<std::int32_t>(w), static_cast<std::int32_t>(h)};
        }

        if (!params.requested_width.has_value() && params.requested_height.has_value()) {
            const double h = static_cast<double>(*params.requested_height);
            const double w = std::round(h * (orig_w / orig_h));
            if (w < 1.0 || w > static_cast<double>(kMaxEditDimension) || h > static_cast<double>(kMaxEditDimension)) {
                return std::nullopt;
            }
            return edit_dimensions{static_cast<std::int32_t>(w), static_cast<std::int32_t>(h)};
        }

        // Both provided: compute aspect-constrained dimension preserving ratio based on requested_width
        // or validate exact match
        const double w = static_cast<double>(*params.requested_width);
        const double h = std::round(w * (orig_h / orig_w));
        if (w < 1.0 || w > static_cast<double>(kMaxEditDimension) ||
            h < 1.0 || h > static_cast<double>(kMaxEditDimension)) {
            return std::nullopt;
        }
        return edit_dimensions{static_cast<std::int32_t>(w), static_cast<std::int32_t>(h)};
    }

    // Independent width & height
    if (!params.requested_width.has_value() || !params.requested_height.has_value()) {
        // In unconstrained mode, both width and height must be provided
        return std::nullopt;
    }

    if (*params.requested_width > kMaxEditDimension || *params.requested_height > kMaxEditDimension) {
        return std::nullopt;
    }

    return edit_dimensions{*params.requested_width, *params.requested_height};
}

std::optional<edit_dimensions> evaluate_command_dimensions(
    const edit_dimensions& in_dims,
    const edit_command& cmd) noexcept {
    if (!in_dims.is_valid()) {
        return std::nullopt;
    }

    return std::visit(
        [&](const auto& c) -> std::optional<edit_dimensions> {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, rotate_command>) {
                // Rotations 90 left/right swap dimensions
                return edit_dimensions{in_dims.height, in_dims.width};
            } else if constexpr (std::is_same_v<T, flip_command>) {
                // Flips preserve dimensions
                return in_dims;
            } else if constexpr (std::is_same_v<T, crop_command>) {
                // Crop rectangle must be non-empty and strictly inside in_dims
                if (c.rect.x < 0 || c.rect.y < 0 || c.rect.width <= 0 || c.rect.height <= 0) {
                    return std::nullopt;
                }
                if (c.rect.x + c.rect.width > in_dims.width ||
                    c.rect.y + c.rect.height > in_dims.height) {
                    return std::nullopt;
                }
                return edit_dimensions{c.rect.width, c.rect.height};
            } else if constexpr (std::is_same_v<T, resize_command>) {
                if (!c.target.is_valid()) {
                    return std::nullopt;
                }
                return c.target;
            }
        },
        cmd);
}

bool is_command_valid(
    const edit_dimensions& in_dims,
    const edit_command& cmd) noexcept {
    return evaluate_command_dimensions(in_dims, cmd).has_value();
}

affine_proxy_plan compute_affine_proxy_plan(
    const edit_dimensions& source_dimensions,
    std::span<const edit_command> commands,
    const std::optional<edit_dimensions>& draft_resize) noexcept {
    affine_proxy_plan plan{};
    if (!source_dimensions.is_valid()) {
        return plan;
    }

    // Start with retained source rectangle [0, 0, w, h] in source coordinates
    edit_rectf src_rect{
        0.0f, 0.0f,
        static_cast<float>(source_dimensions.width),
        static_cast<float>(source_dimensions.height)
    };
    edit_dimensions current_canvas = source_dimensions;
    d4_transform current_d4 = d4_transform::make_identity();

    for (const auto& cmd : commands) {
        if (const auto* r = std::get_if<rotate_command>(&cmd)) {
            if (r->direction == rotate_direction::right_90) {
                current_d4 = current_d4.compose(d4_transform::make_rot90_cw());
            } else {
                current_d4 = current_d4.compose(d4_transform::make_rot90_ccw());
            }
            current_canvas = edit_dimensions{current_canvas.height, current_canvas.width};
        } else if (const auto* f = std::get_if<flip_command>(&cmd)) {
            if (f->direction == flip_direction::horizontal) {
                current_d4 = current_d4.compose(d4_transform::make_flip_h());
            } else {
                current_d4 = current_d4.compose(d4_transform::make_flip_v());
            }
        } else if (const auto* c = std::get_if<crop_command>(&cmd)) {
            if (!is_command_valid(current_canvas, *c)) {
                return plan;
            }
            // Map crop rectangle back through inverse d4 to unrotated canvas
            const auto unrotated_crop = map_half_open_rect(current_d4.inverse(), c->rect, current_canvas);
            if (!unrotated_crop.has_value()) {
                return plan;
            }
            const edit_dimensions unrot_canvas = current_d4.inverse().transform_dimensions(current_canvas);
            // Scale unrotated crop into source coordinates
            const double scale_x = static_cast<double>(src_rect.width) / static_cast<double>(unrot_canvas.width);
            const double scale_y = static_cast<double>(src_rect.height) / static_cast<double>(unrot_canvas.height);

            src_rect.x += static_cast<float>(unrotated_crop->x * scale_x);
            src_rect.y += static_cast<float>(unrotated_crop->y * scale_y);
            src_rect.width = static_cast<float>(unrotated_crop->width * scale_x);
            src_rect.height = static_cast<float>(unrotated_crop->height * scale_y);

            current_canvas = edit_dimensions{c->rect.width, c->rect.height};
        } else if (const auto* res = std::get_if<resize_command>(&cmd)) {
            if (!is_command_valid(current_canvas, *res)) {
                return plan;
            }
            current_canvas = res->target;
        }
    }

    if (draft_resize.has_value()) {
        if (!draft_resize->is_valid()) {
            return plan;
        }
        current_canvas = *draft_resize;
    }

    if (!src_rect.is_valid() || !current_canvas.is_valid()) {
        return plan;
    }

    plan.valid = true;
    plan.logical_dimensions = current_canvas;
    plan.transform = current_d4;
    plan.source_rectf = src_rect;
    return plan;
}

std::optional<edit_dimensions> edit_history::current_dimensions() const noexcept {
    if (!source_dims_.is_valid()) {
        return std::nullopt;
    }

    edit_dimensions curr = source_dims_;
    for (std::size_t i = 0; i < cursor_; ++i) {
        auto next = evaluate_command_dimensions(curr, commands_[i]);
        if (!next.has_value()) {
            return std::nullopt;
        }
        curr = *next;
    }
    return curr;
}

bool edit_history::push_command(edit_command cmd) {
    // Check lifetime command limit: maximum 256 total lifetime commands
    if (total_pushed_commands_ >= kMaxTotalCommands) {
        return false;
    }

    // Evaluate current dimensions before pushing
    auto curr_dims = current_dimensions();
    if (!curr_dims.has_value()) {
        return false;
    }

    // Verify command validity against current canvas dimensions
    if (!is_command_valid(*curr_dims, cmd)) {
        return false;
    }

    // Truncate redo stack if cursor is not at end
    if (cursor_ < commands_.size()) {
        commands_.erase(commands_.begin() + static_cast<std::ptrdiff_t>(cursor_), commands_.end());
    }

    commands_.push_back(std::move(cmd));
    cursor_ = commands_.size();

    // Recompute undo floor: can only undo at most the last 64 transitions
    if (cursor_ > kMaxUndoTransitions) {
        undo_floor_ = (std::max)(undo_floor_, cursor_ - kMaxUndoTransitions);
    }

    ++revision_;
    ++total_pushed_commands_;

    return true;
}

bool edit_history::undo() noexcept {
    if (!can_undo()) {
        return false;
    }
    --cursor_;
    ++revision_;
    return true;
}

bool edit_history::redo() noexcept {
    if (!can_redo()) {
        return false;
    }
    ++cursor_;
    ++revision_;
    return true;
}

}  // namespace oz::viewer_core

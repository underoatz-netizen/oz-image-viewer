// oz/viewer_core/view_state.hpp - pure, testable view state and geometry model (Stage 4).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace oz::viewer_core {

enum class view_mode {
    fit,
    custom
};

// Pure testable ViewState model: UI thread only.
// Scale is relative to oriented source physical pixels.
// Coordinates are integer or double physical pixels; conversion to Direct2D DIP is done once at the boundary.
struct view_state {
    view_mode mode = view_mode::fit;
    double scale = 1.0;
    double pan_x = 0.0;
    double pan_y = 0.0;

    static constexpr double kMinScale = 1.0 / 64.0;
    static constexpr double kMaxScale = 32.0;
    static constexpr double kStepFactor = 1.2;
};

// Physical-pixel viewport rectangle computed from view state, source dimensions, and viewport dimensions.
struct view_geometry {
    double scale = 1.0;
    double dst_x = 0.0;
    double dst_y = 0.0;
    double dst_w = 0.0;
    double dst_h = 0.0;
    double pan_x = 0.0;
    double pan_y = 0.0;
};

// Direct2D destination rectangle in DIP coordinates.
struct view_rectf {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

// Clamp scale to [kMinScale, kMaxScale].
inline double clamp_scale(double scale) noexcept {
    return std::clamp(scale, view_state::kMinScale, view_state::kMaxScale);
}

// Compute fit scale for an oriented source (src_w, src_h) inside a viewport (view_w, view_h).
// Contract: min(view_w / src_w, view_h / src_h, 1.0) with clamp.
inline double compute_fit_scale(std::int32_t src_w, std::int32_t src_h,
                                std::int32_t view_w, std::int32_t view_h) noexcept {
    if (src_w <= 0 || src_h <= 0 || view_w <= 0 || view_h <= 0) {
        return 1.0;
    }
    const double sw = static_cast<double>(view_w) / static_cast<double>(src_w);
    const double sh = static_cast<double>(view_h) / static_cast<double>(src_h);
    const double s = std::min({sw, sh, 1.0});
    return clamp_scale(s);
}

// Clamp pan for a given extent and viewport dimension.
// If extent <= viewport: pan is 0 (centered).
// Otherwise pan is clamped to [-(extent - viewport)/2, +(extent - viewport)/2].
inline double clamp_pan_axis(double pan, double extent, double viewport) noexcept {
    if (extent <= viewport) {
        return 0.0;
    }
    const double max_pan = (extent - viewport) * 0.5;
    return std::clamp(pan, -max_pan, max_pan);
}

// Clamp pan_x and pan_y according to rendered extent and viewport size.
inline void clamp_pan(view_state& vs, std::int32_t src_w, std::int32_t src_h,
                      std::int32_t view_w, std::int32_t view_h) noexcept {
    if (vs.mode == view_mode::fit) {
        vs.pan_x = 0.0;
        vs.pan_y = 0.0;
        return;
    }
    if (src_w <= 0 || src_h <= 0 || view_w <= 0 || view_h <= 0) {
        vs.pan_x = 0.0;
        vs.pan_y = 0.0;
        return;
    }
    const double extent_w = static_cast<double>(src_w) * vs.scale;
    const double extent_h = static_cast<double>(src_h) * vs.scale;
    vs.pan_x = clamp_pan_axis(vs.pan_x, extent_w, static_cast<double>(view_w));
    vs.pan_y = clamp_pan_axis(vs.pan_y, extent_h, static_cast<double>(view_h));
}

// Compute physical destination rectangle and pan for rendering.
inline view_geometry compute_view_geometry(const view_state& vs,
                                           std::int32_t src_w, std::int32_t src_h,
                                           std::int32_t view_w, std::int32_t view_h) noexcept {
    view_geometry geom;
    if (src_w <= 0 || src_h <= 0 || view_w <= 0 || view_h <= 0) {
        return geom;
    }

    double active_scale = vs.scale;
    double active_pan_x = vs.pan_x;
    double active_pan_y = vs.pan_y;

    if (vs.mode == view_mode::fit) {
        active_scale = compute_fit_scale(src_w, src_h, view_w, view_h);
        active_pan_x = 0.0;
        active_pan_y = 0.0;
    } else {
        active_scale = clamp_scale(active_scale);
        const double extent_w = static_cast<double>(src_w) * active_scale;
        const double extent_h = static_cast<double>(src_h) * active_scale;
        active_pan_x = clamp_pan_axis(active_pan_x, extent_w, static_cast<double>(view_w));
        active_pan_y = clamp_pan_axis(active_pan_y, extent_h, static_cast<double>(view_h));
    }

    const double dst_w = static_cast<double>(src_w) * active_scale;
    const double dst_h = static_cast<double>(src_h) * active_scale;
    // Centered at viewport center + pan
    const double dst_x = (static_cast<double>(view_w) - dst_w) * 0.5 + active_pan_x;
    const double dst_y = (static_cast<double>(view_h) - dst_h) * 0.5 + active_pan_y;

    geom.scale = active_scale;
    geom.dst_x = dst_x;
    geom.dst_y = dst_y;
    geom.dst_w = dst_w;
    geom.dst_h = dst_h;
    geom.pan_x = active_pan_x;
    geom.pan_y = active_pan_y;
    return geom;
}

// Convert physical view geometry to Direct2D DIP rectangle.
// Conversion factor is 96 / dpi.
inline view_rectf view_geometry_to_dip(const view_geometry& geom, float dpi) noexcept {
    view_rectf out;
    if (geom.dst_w <= 0.0 || geom.dst_h <= 0.0) {
        return out;
    }
    const float s = (dpi > 0.0f) ? (96.0f / dpi) : 1.0f;
    out.x = static_cast<float>(geom.dst_x) * s;
    out.y = static_cast<float>(geom.dst_y) * s;
    out.width = static_cast<float>(geom.dst_w) * s;
    out.height = static_cast<float>(geom.dst_h) * s;
    return out;
}

// Set mode to Fit.
inline void set_fit(view_state& vs, std::int32_t src_w, std::int32_t src_h,
                    std::int32_t view_w, std::int32_t view_h) noexcept {
    vs.mode = view_mode::fit;
    vs.scale = compute_fit_scale(src_w, src_h, view_w, view_h);
    vs.pan_x = 0.0;
    vs.pan_y = 0.0;
}

// Set mode to 100% (scale = 1.0).
// If anchor_cursor is provided, anchor at that viewport coordinate.
// If anchor_cursor is null, preserve center or anchor at viewport center.
inline void set_one_to_one(view_state& vs, std::int32_t src_w, std::int32_t src_h,
                           std::int32_t view_w, std::int32_t view_h,
                           const double* cursor_x = nullptr,
                           const double* cursor_y = nullptr) noexcept {
    if (src_w <= 0 || src_h <= 0 || view_w <= 0 || view_h <= 0) {
        vs.mode = view_mode::custom;
        vs.scale = 1.0;
        vs.pan_x = 0.0;
        vs.pan_y = 0.0;
        return;
    }

    const auto cur_geom = compute_view_geometry(vs, src_w, src_h, view_w, view_h);
    double cx = static_cast<double>(view_w) * 0.5;
    double cy = static_cast<double>(view_h) * 0.5;
    if (cursor_x && cursor_y) {
        cx = *cursor_x;
        cy = *cursor_y;
    }

    // Source coordinate under the anchor point before zoom:
    // cx = cur_geom.dst_x + src_pt_x * cur_geom.scale
    // => src_pt_x = (cx - cur_geom.dst_x) / cur_geom.scale
    const double src_pt_x = (cur_geom.scale > 0.0) ? (cx - cur_geom.dst_x) / cur_geom.scale : (src_w * 0.5);
    const double src_pt_y = (cur_geom.scale > 0.0) ? (cy - cur_geom.dst_y) / cur_geom.scale : (src_h * 0.5);

    vs.mode = view_mode::custom;
    vs.scale = 1.0;

    // After setting scale = 1.0, we want:
    // cx = new_dst_x + src_pt_x * 1.0
    // new_dst_x = (view_w - src_w * 1.0) / 2 + new_pan_x
    // => cx = (view_w - src_w) / 2 + new_pan_x + src_pt_x
    // => new_pan_x = cx - (view_w - src_w) / 2 - src_pt_x
    const double base_center_x = (static_cast<double>(view_w) - static_cast<double>(src_w)) * 0.5;
    const double base_center_y = (static_cast<double>(view_h) - static_cast<double>(src_h)) * 0.5;
    vs.pan_x = cx - base_center_x - src_pt_x;
    vs.pan_y = cy - base_center_y - src_pt_y;

    clamp_pan(vs, src_w, src_h, view_w, view_h);
}

// Double-click toggle: Fit -> 100% (anchored at click); custom -> Fit.
inline void toggle_fit_or_one(view_state& vs, std::int32_t src_w, std::int32_t src_h,
                              std::int32_t view_w, std::int32_t view_h,
                              double cursor_x, double cursor_y) noexcept {
    if (vs.mode == view_mode::fit) {
        set_one_to_one(vs, src_w, src_h, view_w, view_h, &cursor_x, &cursor_y);
    } else {
        set_fit(vs, src_w, src_h, view_w, view_h);
    }
}

// Cursor-centered zoom by factor with support for high-res wheel deltas:
// scale_ratio = pow(kStepFactor, delta_steps).
// Preserves source point under (cursor_x, cursor_y).
inline void zoom_at_point(view_state& vs, std::int32_t src_w, std::int32_t src_h,
                          std::int32_t view_w, std::int32_t view_h,
                          double cursor_x, double cursor_y,
                          double delta_steps) noexcept {
    if (src_w <= 0 || src_h <= 0 || view_w <= 0 || view_h <= 0 || delta_steps == 0.0) {
        return;
    }

    const auto cur_geom = compute_view_geometry(vs, src_w, src_h, view_w, view_h);
    const double old_scale = cur_geom.scale;
    const double new_scale = clamp_scale(old_scale * std::pow(view_state::kStepFactor, delta_steps));
    if (std::abs(new_scale - old_scale) < 1e-9) {
        return;
    }

    // Source point under cursor:
    const double src_pt_x = (cursor_x - cur_geom.dst_x) / old_scale;
    const double src_pt_y = (cursor_y - cur_geom.dst_y) / old_scale;

    vs.mode = view_mode::custom;
    vs.scale = new_scale;

    // cursor_x = (view_w - src_w * new_scale)/2 + pan_x + src_pt_x * new_scale
    // pan_x = cursor_x - (view_w - src_w * new_scale)/2 - src_pt_x * new_scale
    const double base_center_x = (static_cast<double>(view_w) - static_cast<double>(src_w) * new_scale) * 0.5;
    const double base_center_y = (static_cast<double>(view_h) - static_cast<double>(src_h) * new_scale) * 0.5;
    vs.pan_x = cursor_x - base_center_x - src_pt_x * new_scale;
    vs.pan_y = cursor_y - base_center_y - src_pt_y * new_scale;

    clamp_pan(vs, src_w, src_h, view_w, view_h);
}

// On viewport resize:
// If in fit mode, recompute fit scale and keep pan zero.
// If in custom mode, preserve center source point, then clamp pan.
inline void on_viewport_resized(view_state& vs, std::int32_t src_w, std::int32_t src_h,
                                std::int32_t old_w, std::int32_t old_h,
                                std::int32_t new_w, std::int32_t new_h) noexcept {
    if (src_w <= 0 || src_h <= 0 || new_w <= 0 || new_h <= 0) {
        vs.pan_x = 0.0;
        vs.pan_y = 0.0;
        return;
    }
    if (vs.mode == view_mode::fit) {
        vs.scale = compute_fit_scale(src_w, src_h, new_w, new_h);
        vs.pan_x = 0.0;
        vs.pan_y = 0.0;
        return;
    }

    // In custom mode: preserve center source point before resize.
    if (old_w > 0 && old_h > 0) {
        const double old_center_x = static_cast<double>(old_w) * 0.5;
        const double old_center_y = static_cast<double>(old_h) * 0.5;
        const auto old_geom = compute_view_geometry(vs, src_w, src_h, old_w, old_h);
        const double center_src_x = (old_geom.scale > 0.0) ? (old_center_x - old_geom.dst_x) / old_geom.scale : (src_w * 0.5);
        const double center_src_y = (old_geom.scale > 0.0) ? (old_center_y - old_geom.dst_y) / old_geom.scale : (src_h * 0.5);

        const double new_center_x = static_cast<double>(new_w) * 0.5;
        const double new_center_y = static_cast<double>(new_h) * 0.5;
        const double base_center_x = (static_cast<double>(new_w) - static_cast<double>(src_w) * vs.scale) * 0.5;
        const double base_center_y = (static_cast<double>(new_h) - static_cast<double>(src_h) * vs.scale) * 0.5;

        vs.pan_x = new_center_x - base_center_x - center_src_x * vs.scale;
        vs.pan_y = new_center_y - base_center_y - center_src_y * vs.scale;
    }
    clamp_pan(vs, src_w, src_h, new_w, new_h);
}

// Pan during mouse drag: adds delta physical pixels and clamps.
inline void apply_pan_delta(view_state& vs, std::int32_t src_w, std::int32_t src_h,
                            std::int32_t view_w, std::int32_t view_h,
                            double delta_x, double delta_y) noexcept {
    if (vs.mode == view_mode::fit) {
        // Switching to custom with current fit scale when pan begins
        vs.mode = view_mode::custom;
        vs.scale = compute_fit_scale(src_w, src_h, view_w, view_h);
        vs.pan_x = 0.0;
        vs.pan_y = 0.0;
    }
    vs.pan_x += delta_x;
    vs.pan_y += delta_y;
    clamp_pan(vs, src_w, src_h, view_w, view_h);
}

}  // namespace oz::viewer_core

#include "oz/viewer_core/fit.hpp"

#include <algorithm>

namespace oz::viewer_core {

fit_rect fit_centered(std::int32_t img_w, std::int32_t img_h,
                      std::int32_t view_w, std::int32_t view_h) noexcept {
    fit_rect out;
    if (img_w <= 0 || img_h <= 0 || view_w <= 0 || view_h <= 0) {
        return out;
    }
    const double scale = std::min({static_cast<double>(view_w) / img_w,
                                   static_cast<double>(view_h) / img_h, 1.0});
    out.width = static_cast<std::int32_t>(img_w * scale);
    out.height = static_cast<std::int32_t>(img_h * scale);
    if (out.width < 1) out.width = 1;
    if (out.height < 1) out.height = 1;
    out.x = (view_w - out.width) / 2;
    out.y = (view_h - out.height) / 2;
    return out;
}

fit_rectf fit_centered_dip(std::int32_t img_w, std::int32_t img_h,
                           std::int32_t view_w, std::int32_t view_h, float dpi) noexcept {
    const fit_rect physical = fit_centered(img_w, img_h, view_w, view_h);
    fit_rectf out;
    if (physical.width <= 0 || physical.height <= 0) {
        return out;  // empty DIP rectangle for degenerate inputs
    }
    // Direct2D maps DIP -> physical by dpi/96, so physical -> DIP is 96/dpi.
    const float s = (dpi > 0.0f) ? (96.0f / dpi) : 1.0f;
    out.x = static_cast<float>(physical.x) * s;
    out.y = static_cast<float>(physical.y) * s;
    out.width = static_cast<float>(physical.width) * s;
    out.height = static_cast<float>(physical.height) * s;
    return out;
}

int choose_scaling_denominator(std::int32_t img_w, std::int32_t img_h,
                               std::int32_t view_w, std::int32_t view_h) noexcept {
    if (img_w <= 0 || img_h <= 0 || view_w <= 0 || view_h <= 0) {
        return 1;
    }
    // Largest denominator (smallest decode) whose scaled dimensions still
    // cover the viewport. TurboJPEG scales to ceil(dim / den).
    int best = 1;
    for (const int den : {1, 2, 4, 8}) {
        const std::int64_t sw = (static_cast<std::int64_t>(img_w) + den - 1) / den;
        const std::int64_t sh = (static_cast<std::int64_t>(img_h) + den - 1) / den;
        if (sw >= view_w && sh >= view_h) {
            best = den;
        }
    }
    return best;
}

}  // namespace oz::viewer_core
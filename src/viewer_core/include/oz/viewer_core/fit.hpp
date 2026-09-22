// oz/viewer_core/fit.hpp - fit-to-window geometry (Stage 1).
#pragma once

#include <cstdint>

namespace oz::viewer_core {

// A destination rectangle in viewport coordinates.
struct fit_rect {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
};

// Compute the fit-to-window rectangle for an image of (img_w, img_h) inside a
// viewport of (view_w, view_h). Never upscales; the result is centred.
// Degenerate inputs (zero/negative dimensions) yield an empty rectangle.
fit_rect fit_centered(std::int32_t img_w, std::int32_t img_h,
                      std::int32_t view_w, std::int32_t view_h) noexcept;

// A destination rectangle in Direct2D device-independent-pixel (DIP) coordinates.
// Kept separate from fit_rect (integer physical pixels) because DrawBitmap takes
// a float DIP rectangle whose mapping to physical pixels depends on the DPI of
// the render target.
struct fit_rectf {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

// Fit-to-window destination rectangle expressed in the DIP coordinate space of a
// Direct2D render target whose DPI is `dpi`. The image is (img_w, img_h) physical
// pixels and the swap-chain backbuffer is (view_w, view_h) physical pixels. The
// no-upscale physical fit is computed by fit_centered, then converted to DIPs by
// the Direct2D factor 96/dpi so that drawing the result on a target at DPI `dpi`
// lands on exactly the same physical pixels. At dpi == 96 this equals
// fit_centered; dpi <= 0 is treated as 96. Degenerate inputs yield an empty
// rectangle. This is the single DPI/coordinate contract the renderer relies on.
fit_rectf fit_centered_dip(std::int32_t img_w, std::int32_t img_h,
                           std::int32_t view_w, std::int32_t view_h, float dpi) noexcept;

// Choose the TurboJPEG scaling denominator from {1, 2, 4, 8} that decodes
// closest to the viewport without undershooting it (the decoded dimensions
// must be >= the viewport so the fit never upscales a too-small decode).
// Returns 1 when the image is smaller than the viewport (full resolution).
int choose_scaling_denominator(std::int32_t img_w, std::int32_t img_h,
                               std::int32_t view_w, std::int32_t view_h) noexcept;

}  // namespace oz::viewer_core
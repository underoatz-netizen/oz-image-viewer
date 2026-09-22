// oz/viewer/ui_layout.hpp - DPI-aware UI layout and canvas-rectangle seam (Stage 7 foundation).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace oz::viewer {

// Device-independent pixel (DIP) or logical point
struct ui_point_logical {
    float x = 0.0f;
    float y = 0.0f;
};

// Logical rectangle in DIPs
struct ui_rect_logical {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    [[nodiscard]] constexpr bool is_empty() const noexcept {
        return width <= 0.0f || height <= 0.0f;
    }

    [[nodiscard]] constexpr float right() const noexcept {
        return x + width;
    }

    [[nodiscard]] constexpr float bottom() const noexcept {
        return y + height;
    }

    [[nodiscard]] constexpr bool contains(float px, float py) const noexcept {
        return px >= x && px < right() && py >= y && py < bottom();
    }
};

// Physical pixel rectangle
struct ui_rect_physical {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;

    [[nodiscard]] constexpr bool is_empty() const noexcept {
        return width <= 0 || height <= 0;
    }

    [[nodiscard]] constexpr std::int32_t right() const noexcept {
        return x + width;
    }

    [[nodiscard]] constexpr std::int32_t bottom() const noexcept {
        return y + height;
    }

    [[nodiscard]] constexpr bool contains(std::int32_t px, std::int32_t py) const noexcept {
        return px >= x && px < right() && py >= y && py < bottom();
    }
};

// DPI scale conversions
[[nodiscard]] inline float dip_to_physical_scale(float dpi) noexcept {
    return (dpi > 0.0f) ? (dpi / 96.0f) : 1.0f;
}

[[nodiscard]] inline float physical_to_dip_scale(float dpi) noexcept {
    return (dpi > 0.0f) ? (96.0f / dpi) : 1.0f;
}

[[nodiscard]] inline std::int32_t dip_to_physical_round(float dip, float dpi) noexcept {
    return static_cast<std::int32_t>(std::round(dip * dip_to_physical_scale(dpi)));
}

[[nodiscard]] inline float physical_to_dip(std::int32_t physical, float dpi) noexcept {
    return static_cast<float>(physical) * physical_to_dip_scale(dpi);
}

// Convert logical rect to physical rect using round
[[nodiscard]] inline ui_rect_physical logical_to_physical_rect(const ui_rect_logical& logical, float dpi) noexcept {
    if (logical.is_empty()) {
        return {};
    }
    const float scale = dip_to_physical_scale(dpi);
    const std::int32_t px = static_cast<std::int32_t>(std::round(logical.x * scale));
    const std::int32_t py = static_cast<std::int32_t>(std::round(logical.y * scale));
    const std::int32_t pr = static_cast<std::int32_t>(std::round(logical.right() * scale));
    const std::int32_t pb = static_cast<std::int32_t>(std::round(logical.bottom() * scale));
    return ui_rect_physical{px, py, (std::max)(0, pr - px), (std::max)(0, pb - py)};
}

// Convert physical rect to logical rect
[[nodiscard]] inline ui_rect_logical physical_to_logical_rect(const ui_rect_physical& physical, float dpi) noexcept {
    if (physical.is_empty()) {
        return {};
    }
    const float s = physical_to_dip_scale(dpi);
    return ui_rect_logical{
        static_cast<float>(physical.x) * s,
        static_cast<float>(physical.y) * s,
        static_cast<float>(physical.width) * s,
        static_cast<float>(physical.height) * s
    };
}

// Layout configuration and heights (expressed in logical DIP units)
struct ui_layout_config {
    // Toolbar: 0 when hidden/absent. Default for Stage 7 foundation is 0.0f.
    float toolbar_height_dip = 0.0f;
    bool show_toolbar = false;

    // Contextual crop strip: 0 when hidden/absent (e.g. outside crop mode). Default 0.0f.
    float crop_strip_height_dip = 0.0f;
    bool show_crop_strip = false;

    // Status bar: 0 when hidden/absent. Default for Stage 7 foundation is 0.0f.
    float status_height_dip = 0.0f;
    bool show_status = false;

    // Centered Open button standard dimensions in DIP
    float open_btn_width_dip = 160.0f;
    float open_btn_height_dip = 44.0f;
};

// Computed UI layout partitions in both logical and physical units
struct ui_layout_result {
    // Client area
    ui_rect_logical client_logical{};
    ui_rect_physical client_physical{};

    // Image canvas area (the dedicated area for rendering the image and receiving view/crop interactions)
    ui_rect_logical canvas_logical{};
    ui_rect_physical canvas_physical{};

    // Chrome partitions
    ui_rect_logical toolbar_logical{};
    ui_rect_physical toolbar_physical{};

    ui_rect_logical crop_strip_logical{};
    ui_rect_physical crop_strip_physical{};

    ui_rect_logical status_logical{};
    ui_rect_physical status_physical{};

    // Centered Open button (centered within the canvas area)
    ui_rect_logical open_btn_logical{};
    ui_rect_physical open_btn_physical{};
};

// Pure layout derivation: computes rectangles given client size, DPI, and layout config.
[[nodiscard]] ui_layout_result compute_ui_layout(std::int32_t client_width,
                                                 std::int32_t client_height,
                                                 float dpi,
                                                 const ui_layout_config& config) noexcept;

}  // namespace oz::viewer

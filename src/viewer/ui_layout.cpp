// oz/viewer/ui_layout.cpp - DPI-aware UI layout and canvas-rectangle seam implementation.
#include "ui_layout.hpp"

#include <algorithm>
#include <cmath>

namespace oz::viewer {

ui_layout_result compute_ui_layout(std::int32_t client_width,
                                   std::int32_t client_height,
                                   float dpi,
                                   const ui_layout_config& config) noexcept {
    ui_layout_result result{};
    if (client_width <= 0 || client_height <= 0) {
        return result;
    }

    result.client_physical = ui_rect_physical{0, 0, client_width, client_height};
    result.client_logical = physical_to_logical_rect(result.client_physical, dpi);

    const float client_w_dip = result.client_logical.width;
    const float client_h_dip = result.client_logical.height;

    // 1. Toolbar layout (pinned to top)
    float tb_h = 0.0f;
    if (config.show_toolbar && config.toolbar_height_dip > 0.0f) {
        tb_h = (std::min)(config.toolbar_height_dip, client_h_dip);
        result.toolbar_logical = ui_rect_logical{0.0f, 0.0f, client_w_dip, tb_h};
        result.toolbar_physical = logical_to_physical_rect(result.toolbar_logical, dpi);
    }

    // 2. Crop strip layout (placed directly below toolbar when active)
    float cs_h = 0.0f;
    if (config.show_crop_strip && config.crop_strip_height_dip > 0.0f) {
        const float remaining_for_cs = (std::max)(0.0f, client_h_dip - tb_h);
        cs_h = (std::min)(config.crop_strip_height_dip, remaining_for_cs);
        result.crop_strip_logical = ui_rect_logical{0.0f, tb_h, client_w_dip, cs_h};
        result.crop_strip_physical = ui_rect_physical{
            0,
            result.toolbar_physical.bottom(),
            client_width,
            dip_to_physical_round(cs_h, dpi)
        };
    }

    // 3. Status bar layout (pinned to bottom)
    float sb_h = 0.0f;
    if (config.show_status && config.status_height_dip > 0.0f) {
        const float remaining_h = (std::max)(0.0f, client_h_dip - tb_h - cs_h);
        sb_h = (std::min)(config.status_height_dip, remaining_h);
        result.status_logical = ui_rect_logical{0.0f, client_h_dip - sb_h, client_w_dip, sb_h};
        result.status_physical = logical_to_physical_rect(result.status_logical, dpi);
    }

    // 4. Canvas layout (occupies remaining central space between top chrome and status bar)
    const float canvas_y = tb_h + cs_h;
    const float canvas_h = (std::max)(0.0f, client_h_dip - tb_h - cs_h - sb_h);
    const float canvas_w = client_w_dip;

    result.canvas_logical = ui_rect_logical{0.0f, canvas_y, canvas_w, canvas_h};

    // Calculate canvas physical rectangle by exact snapping to top chrome and status physical boundaries
    // to prevent 1-pixel rounding seams or gaps.
    const std::int32_t canvas_phys_top = (result.crop_strip_physical.height > 0)
                                             ? result.crop_strip_physical.bottom()
                                             : result.toolbar_physical.bottom();
    const std::int32_t canvas_phys_bottom = (result.status_physical.height > 0)
                                                ? result.status_physical.y
                                                : client_height;
    const std::int32_t canvas_phys_h = (std::max)(0, canvas_phys_bottom - canvas_phys_top);

    result.canvas_physical = ui_rect_physical{
        0,
        canvas_phys_top,
        client_width,
        canvas_phys_h
    };

    // 5. Centered Open button within canvas
    if (config.open_btn_width_dip > 0.0f && config.open_btn_height_dip > 0.0f) {
        const float btn_w = (std::min)(config.open_btn_width_dip, canvas_w);
        const float btn_h = (std::min)(config.open_btn_height_dip, canvas_h);
        const float btn_x = result.canvas_logical.x + (canvas_w - btn_w) * 0.5f;
        const float btn_y = result.canvas_logical.y + (canvas_h - btn_h) * 0.5f;

        result.open_btn_logical = ui_rect_logical{btn_x, btn_y, btn_w, btn_h};

        // For physical button, clamp to canvas physical extents so it never escapes
        // a small positive canvas at any DPI scale, and center within canvas physical bounds.
        const std::int32_t desired_phys_w = dip_to_physical_round(config.open_btn_width_dip, dpi);
        const std::int32_t desired_phys_h = dip_to_physical_round(config.open_btn_height_dip, dpi);
        const std::int32_t btn_phys_w = (std::max)(0, (std::min)(desired_phys_w, result.canvas_physical.width));
        const std::int32_t btn_phys_h = (std::max)(0, (std::min)(desired_phys_h, result.canvas_physical.height));
        const std::int32_t btn_phys_x = result.canvas_physical.x + (result.canvas_physical.width - btn_phys_w) / 2;
        const std::int32_t btn_phys_y = result.canvas_physical.y + (result.canvas_physical.height - btn_phys_h) / 2;

        result.open_btn_physical = ui_rect_physical{
            btn_phys_x,
            btn_phys_y,
            btn_phys_w,
            btn_phys_h
        };
    }

    return result;
}

}  // namespace oz::viewer

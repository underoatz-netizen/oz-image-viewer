// oz/viewer/crop_strip.hpp - native contextual crop strip (Stage 7 modern chrome).
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>

#include "oz/viewer_core/crop_interaction.hpp"
#include "crop_icon_bar.hpp"
#include "svg_icon_cache.hpp"
#include "ui_layout.hpp"
#include "ui_theme.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace oz::viewer {

// Control IDs for the contextual crop strip
constexpr int kIdCropStripComboRatio = 4001; // Compatibility ID retained
constexpr int kIdCropStripStaticDim  = 4002;
constexpr int kIdCropStripBtnReset   = 4003; // kIdEditCropReset (210) used on toolbar; this retained for compatibility
constexpr int kIdCropStripBtnApply   = 4004;
constexpr int kIdCropStripBtnCancel  = 4005;
constexpr int kIdCropStripToolbar    = 4010;
constexpr int kIdCropStripScrollBar  = 4011;

// Native contextual crop strip manager:
// Compact bar (<=2 rows, 40 DIP cells) visible solely while crop mode is active.
class crop_strip {
public:
    crop_strip() = default;
    ~crop_strip() {
        destroy();
    }

    crop_strip(const crop_strip&) = delete;
    crop_strip& operator=(const crop_strip&) = delete;

    bool create(HWND parent, HINSTANCE instance, float dpi);
    void destroy() noexcept;

    [[nodiscard]] bool is_created() const noexcept {
        return hwnd_strip_ != nullptr;
    }

    [[nodiscard]] HWND hwnd() const noexcept {
        return hwnd_strip_;
    }

    [[nodiscard]] HWND toolbar_hwnd() const noexcept {
        return hwnd_toolbar_;
    }

    [[nodiscard]] HWND scrollbar_hwnd() const noexcept {
        return hwnd_scroll_;
    }

    [[nodiscard]] HWND first_tab_stop() const noexcept {
        if (hwnd_toolbar_) return hwnd_toolbar_;
        return hwnd_strip_;
    }

    [[nodiscard]] bool contains_hwnd(HWND h) const noexcept {
        if (!h || !hwnd_strip_) return false;
        return (h == hwnd_strip_ || IsChild(hwnd_strip_, h));
    }

    void set_dpi(float dpi) noexcept;
    void set_theme(theme_mode mode) noexcept;
    void update_icons(ID2D1DeviceContext* d2d_context) noexcept;

    // Show or hide and update physical placement
    void update_layout(const ui_layout_result& layout, bool crop_active) noexcept;

    // Update ratio buttons exclusive checked state and selection dimension read-only summary
    void sync_state(const oz::viewer_core::crop_interaction_state* crop_state) noexcept;

    // Fast lightweight update of only the dimension summary text (for high-frequency drag moves)
    void sync_dimension_summary(const oz::viewer_core::crop_interaction_state* crop_state) noexcept;

    // Handle WM_NOTIFY for TTN_GETDISPINFOW tooltips and toolbar notifications
    LRESULT handle_notify(LPARAM lparam) noexcept;

    // Handle WM_HSCROLL for horizontal scrollbar
    void handle_hscroll(WPARAM wparam, LPARAM lparam) noexcept;

    // True when horizontal scrolling is active in the crop strip
    [[nodiscard]] bool scrolling_active() const noexcept {
        return hwnd_scroll_ != nullptr && scroll_range_px_ > 0;
    }

    // Handle background brush for child controls
    HBRUSH handle_ctl_color(HDC hdc, HWND control) noexcept;

private:
    void rebuild_fonts_and_brushes() noexcept;
    void setup_toolbar_buttons() noexcept;
    void setup_tooltips() noexcept;
    void apply_command_button_size() noexcept;
    void reposition_toolbar() noexcept;
    void scroll_to_offset(int pos_px) noexcept;
    void ensure_command_visible(int command_int_id) noexcept;

    HWND parent_ = nullptr;
    HWND hwnd_strip_ = nullptr;
    HWND hwnd_toolbar_ = nullptr;
    HWND hwnd_scroll_ = nullptr;
    HWND hwnd_txt_dim_ = nullptr;
    HWND hwnd_tooltip_ = nullptr;

    float dpi_ = 96.0f;
    theme_mode theme_mode_ = theme_mode::light;
    theme_palette palette_{};

    // Scroll geometry (physical pixels)
    int toolbar_x_ = 0;
    int toolbar_y_ = 0;
    int view_width_px_ = 0;
    int content_width_px_ = 0;
    int scroll_pos_px_ = 0;
    int scroll_range_px_ = 0;
    int toolbar_height_px_ = 0;

    HFONT font_ui_ = nullptr;
    HBRUSH brush_strip_bg_ = nullptr;

    svg_icon_cache icon_cache_;
};

}  // namespace oz::viewer

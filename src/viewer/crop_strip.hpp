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
#include "ui_layout.hpp"
#include "ui_theme.hpp"

#include <cstdint>
#include <string>

namespace oz::viewer {

// Control IDs for the contextual crop strip
constexpr int kIdCropStripComboRatio = 4001;
constexpr int kIdCropStripStaticDim  = 4002;
constexpr int kIdCropStripBtnReset   = 4003;
constexpr int kIdCropStripBtnApply   = 4004;
constexpr int kIdCropStripBtnCancel  = 4005;

// Native contextual crop strip manager:
// Compact bar (32 DIP) visible solely while crop mode is active.
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

    [[nodiscard]] HWND first_tab_stop() const noexcept {
        return hwnd_combo_ratio_;
    }

    [[nodiscard]] bool contains_hwnd(HWND h) const noexcept {
        if (!h || !hwnd_strip_) return false;
        return (h == hwnd_strip_ || IsChild(hwnd_strip_, h));
    }

    void set_dpi(float dpi) noexcept;
    void set_theme(theme_mode mode) noexcept;

    // Show or hide and update physical placement
    void update_layout(const ui_layout_result& layout, bool crop_active) noexcept;

    // Update ratio combo selection and selection dimension read-only summary
    void sync_state(const oz::viewer_core::crop_interaction_state* crop_state) noexcept;

    // Fast lightweight update of only the dimension summary text (for high-frequency drag moves)
    void sync_dimension_summary(const oz::viewer_core::crop_interaction_state* crop_state) noexcept;

    // Handle background brush for child controls
    HBRUSH handle_ctl_color(HDC hdc, HWND control) noexcept;

private:
    void rebuild_fonts_and_brushes() noexcept;

    HWND parent_ = nullptr;
    HWND hwnd_strip_ = nullptr;
    HWND hwnd_lbl_ratio_ = nullptr;
    HWND hwnd_combo_ratio_ = nullptr;
    HWND hwnd_txt_dim_ = nullptr;
    HWND hwnd_btn_reset_ = nullptr;
    HWND hwnd_btn_apply_ = nullptr;
    HWND hwnd_btn_cancel_ = nullptr;

    float dpi_ = 96.0f;
    theme_mode theme_mode_ = theme_mode::light;
    theme_palette palette_{};

    HFONT font_ui_ = nullptr;
    HBRUSH brush_strip_bg_ = nullptr;
};

}  // namespace oz::viewer

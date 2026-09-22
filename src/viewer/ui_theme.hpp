// oz/viewer/ui_theme.hpp - system-aware theme and DWM caption styling seam (Stage 7 modern chrome).
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>

namespace oz::viewer {

enum class theme_mode : std::uint8_t {
    light,
    dark,
    high_contrast
};

struct theme_palette {
    COLORREF bg_chrome = RGB(243, 243, 243);
    COLORREF text_normal = RGB(32, 32, 32);
    COLORREF text_disabled = RGB(160, 160, 160);
    COLORREF btn_hover = RGB(225, 225, 225);
    COLORREF btn_pressed = RGB(208, 208, 208);
    COLORREF btn_checked = RGB(200, 215, 235);
    COLORREF border = RGB(218, 218, 218);
    COLORREF status_bg = RGB(238, 238, 238);
    COLORREF status_text = RGB(90, 90, 90);
};

// Query the active system theme mode (light, dark, or high-contrast)
[[nodiscard]] theme_mode query_system_theme_mode() noexcept;

// Get the visual palette corresponding to the theme mode
[[nodiscard]] theme_palette get_theme_palette(theme_mode mode) noexcept;

// Apply system DWM window caption treatment (e.g. dark titlebar) with runtime fallback
void apply_dwm_caption_theme(HWND hwnd, theme_mode mode) noexcept;

}  // namespace oz::viewer

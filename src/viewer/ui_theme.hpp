// oz/viewer/ui_theme.hpp - system-aware theme and DWM caption styling seam.
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
    COLORREF btn_hover = RGB(224, 224, 228);
    COLORREF btn_pressed = RGB(206, 206, 212);
    COLORREF btn_checked = RGB(200, 218, 240);
    COLORREF border = RGB(218, 218, 222);
    COLORREF status_bg = RGB(238, 238, 240);
    COLORREF status_text = RGB(96, 96, 100);
    COLORREF glyph_normal = RGB(32, 32, 32);
    COLORREF text_muted = RGB(105, 105, 110);
    COLORREF separator = RGB(212, 212, 216);
    COLORREF canvas_bg = RGB(28, 28, 31);
};

[[nodiscard]] theme_mode query_system_theme_mode() noexcept;
[[nodiscard]] theme_palette get_theme_palette(theme_mode mode) noexcept;
void apply_dwm_caption_theme(HWND hwnd, theme_mode mode) noexcept;

}  // namespace oz::viewer

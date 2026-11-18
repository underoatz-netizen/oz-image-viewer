// oz/viewer/ui_theme.cpp - system-aware theme and DWM caption styling implementation.
#include "ui_theme.hpp"

#include <dwmapi.h>

namespace oz::viewer {

theme_mode query_system_theme_mode() noexcept {
    HIGHCONTRASTW hc{sizeof(HIGHCONTRASTW)};
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(HIGHCONTRASTW), &hc, 0)) {
        if ((hc.dwFlags & HCF_HIGHCONTRASTON) != 0) {
            return theme_mode::high_contrast;
        }
    }

    HKEY hKey = nullptr;
    const LSTATUS status = RegOpenKeyExW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        0,
        KEY_READ,
        &hKey);

    if (status == ERROR_SUCCESS && hKey != nullptr) {
        DWORD value = 1;
        DWORD size = sizeof(value);
        DWORD type = REG_DWORD;
        const LSTATUS query_status = RegQueryValueExW(
            hKey,
            L"AppsUseLightTheme",
            nullptr,
            &type,
            reinterpret_cast<LPBYTE>(&value),
            &size);
        RegCloseKey(hKey);

        if (query_status == ERROR_SUCCESS && type == REG_DWORD) {
            if (value == 0) {
                return theme_mode::dark;
            }
        }
    }

    return theme_mode::light;
}

theme_palette get_theme_palette(theme_mode mode) noexcept {
    theme_palette pal;
    switch (mode) {
        case theme_mode::high_contrast:
            pal.bg_chrome = GetSysColor(COLOR_WINDOW);
            pal.text_normal = GetSysColor(COLOR_WINDOWTEXT);
            pal.glyph_normal = GetSysColor(COLOR_WINDOWTEXT);
            pal.text_muted = GetSysColor(COLOR_WINDOWTEXT);
            pal.text_disabled = GetSysColor(COLOR_GRAYTEXT);
            pal.btn_hover = GetSysColor(COLOR_HIGHLIGHT);
            pal.btn_pressed = GetSysColor(COLOR_HOTLIGHT);
            pal.btn_checked = GetSysColor(COLOR_HIGHLIGHT);
            pal.border = GetSysColor(COLOR_WINDOWFRAME);
            pal.separator = GetSysColor(COLOR_WINDOWFRAME);
            pal.status_bg = GetSysColor(COLOR_BTNFACE);
            pal.status_text = GetSysColor(COLOR_BTNTEXT);
            pal.canvas_bg = GetSysColor(COLOR_WINDOW);
            break;

        case theme_mode::dark:
            pal.bg_chrome = RGB(51, 51, 51);
            pal.text_normal = RGB(232, 232, 232);
            pal.glyph_normal = RGB(240, 240, 240);
            pal.text_muted = RGB(178, 178, 181);
            pal.text_disabled = RGB(118, 118, 118);
            pal.btn_hover = RGB(74, 74, 78);
            pal.btn_pressed = RGB(92, 92, 96);
            pal.btn_checked = RGB(70, 92, 122);
            pal.border = RGB(62, 62, 66);
            pal.separator = RGB(76, 76, 80);
            pal.status_bg = RGB(36, 36, 39);
            pal.status_text = RGB(178, 178, 181);
            pal.canvas_bg = RGB(28, 28, 31);
            break;

        case theme_mode::light:
        default:
            pal.bg_chrome = RGB(243, 243, 243);
            pal.text_normal = RGB(32, 32, 32);
            pal.glyph_normal = RGB(32, 32, 32);
            pal.text_muted = RGB(105, 105, 110);
            pal.text_disabled = RGB(160, 160, 160);
            pal.btn_hover = RGB(224, 224, 228);
            pal.btn_pressed = RGB(206, 206, 212);
            pal.btn_checked = RGB(200, 218, 240);
            pal.border = RGB(218, 218, 222);
            pal.separator = RGB(212, 212, 216);
            pal.status_bg = RGB(238, 238, 240);
            pal.status_text = RGB(96, 96, 100);
            pal.canvas_bg = RGB(28, 28, 31);
            break;
    }
    return pal;
}

void apply_dwm_caption_theme(HWND hwnd, theme_mode mode) noexcept {
    if (!hwnd || !IsWindow(hwnd)) {
        return;
    }

    constexpr DWORD kDwmwaUseImmersiveDarkMode = 20;
    constexpr DWORD kDwmwaUseImmersiveDarkModeBefore20H1 = 19;

    BOOL use_dark = (mode == theme_mode::dark) ? TRUE : FALSE;

    HMODULE hDwm = GetModuleHandleW(L"dwmapi.dll");
    if (!hDwm) {
        hDwm = LoadLibraryW(L"dwmapi.dll");
    }
    if (!hDwm) {
        return;
    }

    using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    auto pfnDwmSetWindowAttribute = reinterpret_cast<DwmSetWindowAttributeFn>(
        GetProcAddress(hDwm, "DwmSetWindowAttribute"));

    if (pfnDwmSetWindowAttribute) {
        HRESULT hr = pfnDwmSetWindowAttribute(
            hwnd, kDwmwaUseImmersiveDarkMode, &use_dark, sizeof(use_dark));
        if (FAILED(hr)) {
            pfnDwmSetWindowAttribute(
                hwnd, kDwmwaUseImmersiveDarkModeBefore20H1, &use_dark, sizeof(use_dark));
        }
    }
}

}  // namespace oz::viewer

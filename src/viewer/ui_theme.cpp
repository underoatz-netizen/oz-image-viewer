// oz/viewer/ui_theme.cpp - system-aware theme and DWM caption styling implementation.
#include "ui_theme.hpp"

#include <dwmapi.h>

namespace oz::viewer {

theme_mode query_system_theme_mode() noexcept {
    // 1. Check for High Contrast mode first (overrides decorative light/dark colors)
    HIGHCONTRASTW hc{sizeof(HIGHCONTRASTW)};
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(HIGHCONTRASTW), &hc, 0)) {
        if ((hc.dwFlags & HCF_HIGHCONTRASTON) != 0) {
            return theme_mode::high_contrast;
        }
    }

    // 2. Query Windows personalization registry for App dark mode:
    // HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize -> AppsUseLightTheme
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
            pal.text_disabled = GetSysColor(COLOR_GRAYTEXT);
            pal.btn_hover = GetSysColor(COLOR_HIGHLIGHT);
            pal.btn_pressed = GetSysColor(COLOR_HOTLIGHT);
            pal.btn_checked = GetSysColor(COLOR_HIGHLIGHT);
            pal.border = GetSysColor(COLOR_WINDOWFRAME);
            pal.status_bg = GetSysColor(COLOR_BTNFACE);
            pal.status_text = GetSysColor(COLOR_BTNTEXT);
            break;

        case theme_mode::dark:
            pal.bg_chrome = RGB(32, 32, 32);
            pal.text_normal = RGB(235, 235, 235);
            pal.text_disabled = RGB(100, 100, 100);
            pal.btn_hover = RGB(50, 50, 50);
            pal.btn_pressed = RGB(65, 65, 65);
            pal.btn_checked = RGB(45, 65, 95);
            pal.border = RGB(55, 55, 55);
            pal.status_bg = RGB(24, 24, 24);
            pal.status_text = RGB(175, 175, 175);
            break;

        case theme_mode::light:
        default:
            pal.bg_chrome = RGB(243, 243, 243);
            pal.text_normal = RGB(32, 32, 32);
            pal.text_disabled = RGB(160, 160, 160);
            pal.btn_hover = RGB(225, 225, 225);
            pal.btn_pressed = RGB(208, 208, 208);
            pal.btn_checked = RGB(200, 215, 235);
            pal.border = RGB(218, 218, 218);
            pal.status_bg = RGB(238, 238, 238);
            pal.status_text = RGB(90, 90, 90);
            break;
    }
    return pal;
}

void apply_dwm_caption_theme(HWND hwnd, theme_mode mode) noexcept {
    if (!hwnd || !IsWindow(hwnd)) {
        return;
    }

    // Windows 11 / Windows 10 build 18985+ uses DWMWA_USE_IMMERSIVE_DARK_MODE (20)
    // Windows 10 build 17763 to 18985 used undocumented attribute 19
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

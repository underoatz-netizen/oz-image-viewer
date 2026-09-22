// oz/viewer/ui_chrome.hpp - modular native Win32 toolbar and status bar chrome (Stage 7 modern chrome).
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>

#include "svg_icon_cache.hpp"
#include "ui_command_state.hpp"
#include "ui_layout.hpp"
#include "ui_theme.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace oz::viewer {

// Status presentation info (event-driven snapshot)
struct status_bar_info {
    std::wstring file_name{};
    std::wstring dimensions{};
    std::wstring zoom{};
    std::wstring navigation{};
    std::wstring state_label{};
};

// Toolbar button item definition
struct toolbar_button_desc {
    ui_command_id cmd_id;
    int command_int_id;
    icon_id icon;
    const wchar_t* tooltip;
    const wchar_t* shortcut;
    const wchar_t* accessible_name;
    bool is_check_button = false;
};

// Returns standard descriptions for the 11 toolbar buttons
[[nodiscard]] const std::vector<toolbar_button_desc>& get_toolbar_button_descriptors() noexcept;

// Modern interactive-only UI Chrome manager:
// Owns native toolbar, status bar, and tooltip child HWNDs, theme brushes/fonts, and lazy SVG icon cache.
class ui_chrome {
public:
    ui_chrome() = default;
    ~ui_chrome() {
        destroy();
    }

    ui_chrome(const ui_chrome&) = delete;
    ui_chrome& operator=(const ui_chrome&) = delete;

    // Initialize chrome controls (called in interactive mode only)
    bool create(HWND parent, HINSTANCE instance, float dpi);

    // Destroy child HWNDs and GDI resources
    void destroy() noexcept;

    [[nodiscard]] bool is_created() const noexcept {
        return parent_ != nullptr && hwnd_toolbar_ != nullptr;
    }

    [[nodiscard]] HWND toolbar_hwnd() const noexcept {
        return hwnd_toolbar_;
    }

    [[nodiscard]] HWND statusbar_hwnd() const noexcept {
        return hwnd_status_;
    }

    [[nodiscard]] bool contains_hwnd(HWND h) const noexcept {
        if (!h) return false;
        return (h == hwnd_toolbar_ || IsChild(hwnd_toolbar_, h) ||
                h == hwnd_status_  || IsChild(hwnd_status_, h));
    }

    // Update DPI (re-creates fonts and updates control metric layout)
    void set_dpi(float dpi) noexcept;

    // Update theme and high-contrast styling
    void set_theme(theme_mode mode) noexcept;

    // Update visibility and physical positions according to computed layout
    void update_layout(const ui_layout_result& layout, bool is_fullscreen) noexcept;

    // Update toolbar button states (enabled, checked) from context snapshot
    void update_command_states(const ui_command_context& ctx) noexcept;

    // Synchronize toolbar button images with the Direct2D SVG icon cache if supported, or text fallback
    void update_icons(ID2D1DeviceContext* d2d_context) noexcept;

    // Update status bar text fields without per-frame polling
    void update_status(const status_bar_info& info) noexcept;

    // Handle WM_NOTIFY for TTN_GETDISPINFO and custom draw if needed
    LRESULT handle_notify(LPARAM lparam) noexcept;

    // Handle WM_CTLCOLORSTATIC / WM_CTLCOLORBTN for theme backgrounds
    HBRUSH handle_ctl_color(HDC hdc, HWND control) noexcept;

    [[nodiscard]] theme_mode current_theme() const noexcept {
        return theme_mode_;
    }

    [[nodiscard]] svg_icon_cache& icon_cache() noexcept {
        return icon_cache_;
    }

private:
    void rebuild_fonts() noexcept;
    void setup_toolbar_buttons() noexcept;
    void setup_tooltips() noexcept;

    HWND parent_ = nullptr;
    HWND hwnd_toolbar_ = nullptr;
    HWND hwnd_status_ = nullptr;
    HWND hwnd_tooltip_ = nullptr;

    float dpi_ = 96.0f;
    theme_mode theme_mode_ = theme_mode::light;
    theme_palette palette_{};

    HFONT font_ui_ = nullptr;
    HBRUSH brush_chrome_bg_ = nullptr;
    HBRUSH brush_status_bg_ = nullptr;

    HIMAGELIST himl_ = nullptr;
    bool using_image_list_ = false;

    svg_icon_cache icon_cache_;
    std::wstring cached_status_text_;
};

}  // namespace oz::viewer

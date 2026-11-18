// oz/viewer/ui_chrome.hpp - Single-row Lucide command bar, overflow menu, and clean status bar chrome.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "command_menu.hpp"
#include "std_icon_bar.hpp"
#include "svg_icon_cache.hpp"
#include "ui_command_state.hpp"
#include "ui_layout.hpp"
#include "ui_theme.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace oz::viewer {

struct status_bar_info {
    std::wstring file_name{};
    std::wstring full_path{};
    std::wstring dimensions{};
    std::wstring zoom{};
    std::wstring navigation{};
    std::wstring state_label{};
    int nav_digits = 1;
    bool has_nav_counter = false;
};

struct toolbar_button_desc {
    ui_command_id cmd_id;
    int command_int_id;
    icon_id icon;
    const wchar_t* tooltip;
    const wchar_t* shortcut;
    const wchar_t* accessible_name;
    bool is_check_button = false;
};

// Returns standard descriptions for every active non-crop command button (27),
// in std_icon_bar catalog order.
[[nodiscard]] const std::vector<toolbar_button_desc>& get_toolbar_button_descriptors() noexcept;

// Finds the descriptor for a command integer id, or nullptr when absent.
[[nodiscard]] const toolbar_button_desc* find_toolbar_button_descriptor(int command_int_id) noexcept;

class ui_chrome {
public:
    ui_chrome() = default;
    ~ui_chrome() {
        destroy();
    }

    ui_chrome(const ui_chrome&) = delete;
    ui_chrome& operator=(const ui_chrome&) = delete;

    bool create(HWND parent, HINSTANCE instance, float dpi);
    void destroy() noexcept;

    [[nodiscard]] bool is_created() const noexcept {
        return parent_ != nullptr && hwnd_band_ != nullptr;
    }

    [[nodiscard]] HWND toolbar_hwnd() const noexcept {
        return hwnd_band_;
    }

    [[nodiscard]] HWND statusbar_hwnd() const noexcept {
        return hwnd_status_;
    }

    [[nodiscard]] HWND bar_scrollbar_hwnd() const noexcept {
        return nullptr;
    }

    [[nodiscard]] HWND first_tab_stop() const noexcept;

    [[nodiscard]] bool contains_hwnd(HWND h) const noexcept {
        if (!h) return false;
        return (h == hwnd_band_ || IsChild(hwnd_band_, h) ||
                h == hwnd_status_ || IsChild(hwnd_status_, h));
    }

    void set_dpi(float dpi) noexcept;
    void set_theme(theme_mode mode) noexcept;
    void update_layout(const ui_layout_result& layout, bool is_fullscreen) noexcept;
    void update_command_states(const ui_command_context& ctx) noexcept;
    void update_icons(ID2D1DeviceContext* d2d_context) noexcept;
    void update_status(const status_bar_info& info) noexcept;

    // Shows the overflow ("More commands") popup menu anchored to the right-pinned
    // overflow button (or top-right of the band if invoked via F10 / Alt).
    void show_overflow_menu() noexcept;

    // Handles Tab / Left / Right keyboard traversal inside the command bar.
    bool handle_key_navigation(UINT vk, bool shift) noexcept;

    LRESULT handle_notify(LPARAM lparam) noexcept;
    void handle_hscroll(WPARAM wparam, LPARAM lparam) noexcept;

    [[nodiscard]] bool bar_scrolling_active() const noexcept {
        return false;
    }

    HBRUSH handle_ctl_color(HDC hdc, HWND control) noexcept;

    [[nodiscard]] theme_mode current_theme() const noexcept {
        return theme_mode_;
    }

    [[nodiscard]] svg_icon_cache& icon_cache() noexcept {
        return icon_cache_;
    }

    [[nodiscard]] const cmd_bar_layout& current_bar_layout() const noexcept {
        return bar_layout_;
    }

private:
    static LRESULT CALLBACK band_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK status_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK button_subclass_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

    void register_window_classes(HINSTANCE instance) noexcept;
    void rebuild_fonts_and_brushes() noexcept;
    void create_band_controls(HINSTANCE instance) noexcept;
    void create_overflow_menu() noexcept;
    void destroy_overflow_menu() noexcept;
    void add_tooltip(HWND control, const wchar_t* text) noexcept;
    void update_tooltip_text(HWND control, const std::wstring& text) noexcept;
    void layout_band_controls(int band_width_px) noexcept;
    void draw_band(HDC dc, const RECT& client) noexcept;
    void draw_status(HDC dc, const RECT& client) noexcept;
    void draw_button(const DRAWITEMSTRUCT& item) noexcept;
    void restore_focus_after_click(HWND button) noexcept;
    [[nodiscard]] std::vector<HWND> visible_enabled_buttons() const noexcept;
    [[nodiscard]] HWND control_for_slot(cmd_bar_item item) const noexcept;
    [[nodiscard]] bool is_button_checked(int command_int_id) const noexcept;

    HWND parent_ = nullptr;
    HWND hwnd_band_ = nullptr;
    HWND hwnd_status_ = nullptr;
    HWND hwnd_tooltip_ = nullptr;

    // Command bar child controls.
    HWND btn_open_ = nullptr;
    HWND lbl_name_ = nullptr;
    HWND lbl_nav_ = nullptr;
    HWND btn_prev_ = nullptr;
    HWND btn_next_ = nullptr;
    HWND btn_zoom_out_ = nullptr;
    HWND lbl_zoom_ = nullptr;
    HWND btn_zoom_in_ = nullptr;
    HWND btn_fit_ = nullptr;
    HWND btn_actual_ = nullptr;
    HWND btn_rotate_l_ = nullptr;
    HWND btn_rotate_r_ = nullptr;
    HWND btn_crop_ = nullptr;
    HWND btn_delete_ = nullptr;
    HWND btn_print_ = nullptr;
    HWND btn_overflow_ = nullptr;

    HWND hover_btn_ = nullptr;

    HMENU overflow_menu_ = nullptr;
    HMENU copy_submenu_ = nullptr;
    HMENU edit_submenu_ = nullptr;
    HMENU crop_ratio_submenu_ = nullptr;

    float dpi_ = 96.0f;
    theme_mode theme_mode_ = theme_mode::light;
    theme_palette palette_{};
    ui_command_context last_ctx_{};
    status_bar_info status_info_{};
    cmd_bar_layout bar_layout_{};
    int last_band_width_px_ = 0;

    HFONT font_ui_ = nullptr;
    HFONT font_status_ = nullptr;
    HBRUSH brush_chrome_bg_ = nullptr;
    HBRUSH brush_status_bg_ = nullptr;

    svg_icon_cache icon_cache_;
};

}  // namespace oz::viewer

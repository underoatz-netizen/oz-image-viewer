// oz/viewer/crop_strip.cpp - native contextual crop strip implementation.
#include "crop_strip.hpp"

namespace oz::viewer {

namespace {

constexpr const wchar_t* kRatioNames[] = {
    L"Free",
    L"Original",
    L"1:1 Square",
    L"3:2",
    L"4:3",
    L"5:4",
    L"16:9",
    L"9:16",
    L"A-series"
};

constexpr oz::viewer_core::crop_ratio_preset kRatioPresets[] = {
    oz::viewer_core::crop_ratio_preset::free,
    oz::viewer_core::crop_ratio_preset::original,
    oz::viewer_core::crop_ratio_preset::square_1x1,
    oz::viewer_core::crop_ratio_preset::ratio_3x2,
    oz::viewer_core::crop_ratio_preset::ratio_4x3,
    oz::viewer_core::crop_ratio_preset::ratio_5x4,
    oz::viewer_core::crop_ratio_preset::ratio_16x9,
    oz::viewer_core::crop_ratio_preset::ratio_9x16,
    oz::viewer_core::crop_ratio_preset::a_series
};

}  // namespace

bool crop_strip::create(HWND parent, HINSTANCE instance, float dpi) {
    destroy();
    parent_ = parent;
    dpi_ = (dpi > 0.0f) ? dpi : 96.0f;
    theme_mode_ = query_system_theme_mode();
    palette_ = get_theme_palette(theme_mode_);

    rebuild_fonts_and_brushes();

    // Contextual crop strip container window
    hwnd_strip_ = CreateWindowExW(
        0,
        L"STATIC",
        L"",
        WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | SS_NOTIFY,
        0, 0, 0, 0,
        parent_,
        nullptr,
        instance,
        nullptr);

    if (!hwnd_strip_) {
        return false;
    }

    // Ratio label
    hwnd_lbl_ratio_ = CreateWindowExW(
        0, L"STATIC", L"Aspect Ratio:",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
        0, 0, 0, 0,
        hwnd_strip_, nullptr, instance, nullptr);

    // Ratio drop-down combo box
    hwnd_combo_ratio_ = CreateWindowExW(
        0, WC_COMBOBOXW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 0, 0,
        hwnd_strip_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdCropStripComboRatio)),
        instance, nullptr);

    for (const auto* name : kRatioNames) {
        SendMessageW(hwnd_combo_ratio_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
    }
    SendMessageW(hwnd_combo_ratio_, CB_SETCURSEL, 0, 0);

    // Read-only crop dimension summary
    hwnd_txt_dim_ = CreateWindowExW(
        0, L"STATIC", L"-",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
        0, 0, 0, 0,
        hwnd_strip_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdCropStripStaticDim)),
        instance, nullptr);

    // Reset button
    hwnd_btn_reset_ = CreateWindowExW(
        0, WC_BUTTONW, L"Reset (R)",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0,
        hwnd_strip_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdCropStripBtnReset)),
        instance, nullptr);

    // Apply button
    hwnd_btn_apply_ = CreateWindowExW(
        0, WC_BUTTONW, L"Apply (Enter)",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        0, 0, 0, 0,
        hwnd_strip_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdCropStripBtnApply)),
        instance, nullptr);

    // Cancel button
    hwnd_btn_cancel_ = CreateWindowExW(
        0, WC_BUTTONW, L"Cancel (Esc)",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0,
        hwnd_strip_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdCropStripBtnCancel)),
        instance, nullptr);

    if (font_ui_) {
        for (HWND ctrl : {hwnd_lbl_ratio_, hwnd_combo_ratio_, hwnd_txt_dim_,
                          hwnd_btn_reset_, hwnd_btn_apply_, hwnd_btn_cancel_}) {
            if (ctrl) {
                SendMessageW(ctrl, WM_SETFONT, reinterpret_cast<WPARAM>(font_ui_), TRUE);
            }
        }
    }

    return true;
}

void crop_strip::destroy() noexcept {
    if (hwnd_strip_) {
        DestroyWindow(hwnd_strip_);
        hwnd_strip_ = nullptr;
    }
    hwnd_lbl_ratio_ = nullptr;
    hwnd_combo_ratio_ = nullptr;
    hwnd_txt_dim_ = nullptr;
    hwnd_btn_reset_ = nullptr;
    hwnd_btn_apply_ = nullptr;
    hwnd_btn_cancel_ = nullptr;

    if (font_ui_) {
        DeleteObject(font_ui_);
        font_ui_ = nullptr;
    }
    if (brush_strip_bg_) {
        DeleteObject(brush_strip_bg_);
        brush_strip_bg_ = nullptr;
    }
    parent_ = nullptr;
}

void crop_strip::rebuild_fonts_and_brushes() noexcept {
    if (font_ui_) {
        DeleteObject(font_ui_);
        font_ui_ = nullptr;
    }
    if (brush_strip_bg_) {
        DeleteObject(brush_strip_bg_);
        brush_strip_bg_ = nullptr;
    }

    brush_strip_bg_ = CreateSolidBrush(palette_.bg_chrome);

    const int font_height = dip_to_physical_round(11.0f, dpi_);
    font_ui_ = CreateFontW(
        -font_height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");
}

void crop_strip::set_dpi(float dpi) noexcept {
    if (dpi <= 0.0f) return;
    dpi_ = dpi;
    rebuild_fonts_and_brushes();

    if (font_ui_) {
        for (HWND ctrl : {hwnd_lbl_ratio_, hwnd_combo_ratio_, hwnd_txt_dim_,
                          hwnd_btn_reset_, hwnd_btn_apply_, hwnd_btn_cancel_}) {
            if (ctrl) {
                SendMessageW(ctrl, WM_SETFONT, reinterpret_cast<WPARAM>(font_ui_), TRUE);
            }
        }
    }
}

void crop_strip::set_theme(theme_mode mode) noexcept {
    theme_mode_ = mode;
    palette_ = get_theme_palette(theme_mode_);
    rebuild_fonts_and_brushes();

    if (hwnd_strip_) {
        InvalidateRect(hwnd_strip_, nullptr, TRUE);
    }
}

void crop_strip::update_layout(const ui_layout_result& layout, bool crop_active) noexcept {
    if (!hwnd_strip_) return;

    if (!crop_active || layout.crop_strip_physical.height <= 0) {
        ShowWindow(hwnd_strip_, SW_HIDE);
        return;
    }

    const auto& cs = layout.crop_strip_physical;
    SetWindowPos(hwnd_strip_, nullptr, cs.x, cs.y, cs.width, cs.height, SWP_NOZORDER | SWP_SHOWWINDOW);

    // Arrange child controls inside the crop strip with deterministic responsive layout.
    // Heights and paddings in physical pixels.
    const int pad_x = dip_to_physical_round(8.0f, dpi_);
    const int pad_y = dip_to_physical_round(4.0f, dpi_);
    const int gap = dip_to_physical_round(4.0f, dpi_);
    const int ctrl_h = (std::max)(1, cs.height - pad_y * 2);

    // 1. Right-aligned action buttons (Reset, Apply, Cancel) - MUST NEVER overlap or be clipped.
    // Base width 85 DIP; compact width 65 DIP if total space is constrained.
    int btn_w = dip_to_physical_round(85.0f, dpi_);
    const int min_btns_space = 3 * btn_w + 2 * gap + 2 * pad_x;
    if (cs.width < min_btns_space + dip_to_physical_round(240.0f, dpi_)) {
        btn_w = (std::max)(dip_to_physical_round(55.0f, dpi_), (cs.width - 2 * pad_x - 4 * gap) / 6);
    }

    int right_x = cs.width - pad_x;
    if (hwnd_btn_cancel_) {
        right_x -= btn_w;
        SetWindowPos(hwnd_btn_cancel_, nullptr, right_x, pad_y, btn_w, ctrl_h, SWP_NOZORDER | SWP_SHOWWINDOW);
        right_x -= gap;
    }

    if (hwnd_btn_apply_) {
        right_x -= btn_w;
        SetWindowPos(hwnd_btn_apply_, nullptr, right_x, pad_y, btn_w, ctrl_h, SWP_NOZORDER | SWP_SHOWWINDOW);
        right_x -= gap;
    }

    if (hwnd_btn_reset_) {
        right_x -= btn_w;
        SetWindowPos(hwnd_btn_reset_, nullptr, right_x, pad_y, btn_w, ctrl_h, SWP_NOZORDER | SWP_SHOWWINDOW);
    }

    const int action_buttons_left = right_x;

    // 2. Left-aligned controls: Ratio Label, Ratio Combo, and Dimension Summary.
    // Determine available space between pad_x and action_buttons_left - gap.
    const int available_left_space = (std::max)(0, action_buttons_left - gap - pad_x);

    const int full_lbl_w = dip_to_physical_round(80.0f, dpi_);
    const int combo_w = (std::min)(dip_to_physical_round(110.0f, dpi_), available_left_space);
    const int full_dim_w = dip_to_physical_round(110.0f, dpi_);

    int cur_x = pad_x;

    // Decide if label fits: if available_left_space < lbl_w + combo_w + dim_w, hide or collapse label
    const bool show_lbl = (available_left_space >= (full_lbl_w + combo_w + dip_to_physical_round(60.0f, dpi_)));
    if (hwnd_lbl_ratio_) {
        if (show_lbl) {
            SetWindowPos(hwnd_lbl_ratio_, nullptr, cur_x, pad_y, full_lbl_w, ctrl_h, SWP_NOZORDER | SWP_SHOWWINDOW);
            cur_x += full_lbl_w + gap;
        } else {
            ShowWindow(hwnd_lbl_ratio_, SW_HIDE);
        }
    }

    // Ratio combo: always visible
    if (hwnd_combo_ratio_) {
        const int actual_combo_w = (std::min)(combo_w, (std::max)(0, action_buttons_left - gap - cur_x));
        SetWindowPos(hwnd_combo_ratio_, nullptr, cur_x, pad_y, actual_combo_w, dip_to_physical_round(150.0f, dpi_), SWP_NOZORDER | SWP_SHOWWINDOW);
        cur_x += actual_combo_w + gap;
    }

    // Dimension summary: takes remaining space up to full_dim_w, never overlapping Reset button.
    // If available width is too small (< 40 DIP), hide text safely without clip/overlap.
    const int max_dim_w = (std::max)(0, action_buttons_left - gap - cur_x);
    const int actual_dim_w = (std::min)(full_dim_w, max_dim_w);
    const int min_usable_dim_w = dip_to_physical_round(40.0f, dpi_);

    if (hwnd_txt_dim_) {
        if (actual_dim_w >= min_usable_dim_w) {
            SetWindowPos(hwnd_txt_dim_, nullptr, cur_x, pad_y, actual_dim_w, ctrl_h, SWP_NOZORDER | SWP_SHOWWINDOW);
        } else {
            ShowWindow(hwnd_txt_dim_, SW_HIDE);
        }
    }
}

void crop_strip::sync_state(const oz::viewer_core::crop_interaction_state* crop_state) noexcept {
    if (!hwnd_strip_ || !crop_state || !crop_state->active) {
        return;
    }

    // 1. Sync ratio combo box
    int sel_idx = 0;
    for (std::size_t i = 0; i < sizeof(kRatioPresets) / sizeof(kRatioPresets[0]); ++i) {
        if (kRatioPresets[i] == crop_state->preset) {
            sel_idx = static_cast<int>(i);
            break;
        }
    }
    if (hwnd_combo_ratio_) {
        SendMessageW(hwnd_combo_ratio_, CB_SETCURSEL, static_cast<WPARAM>(sel_idx), 0);
    }

    // 2. Read-only current crop dimensions summary
    sync_dimension_summary(crop_state);
}

void crop_strip::sync_dimension_summary(const oz::viewer_core::crop_interaction_state* crop_state) noexcept {
    if (!hwnd_txt_dim_ || !crop_state || !crop_state->active) {
        return;
    }

    wchar_t buf[64];
    if (crop_state->selection.is_valid()) {
        swprintf_s(buf, L"%d × %d", crop_state->selection.width, crop_state->selection.height);
    } else {
        swprintf_s(buf, L"-");
    }
    SetWindowTextW(hwnd_txt_dim_, buf);
}

HBRUSH crop_strip::handle_ctl_color(HDC hdc, HWND control) noexcept {
    if (control == hwnd_strip_ || control == hwnd_lbl_ratio_ || control == hwnd_txt_dim_) {
        SetBkColor(hdc, palette_.bg_chrome);
        SetTextColor(hdc, palette_.text_normal);
        return brush_strip_bg_;
    }
    return nullptr;
}

}  // namespace oz::viewer

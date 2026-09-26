// oz/viewer/crop_strip.cpp - native contextual crop strip implementation.
#include "crop_strip.hpp"

#include "viewer_strings.hpp"

#include <shellapi.h>
#include <algorithm>
#include <vector>

namespace oz::viewer {

namespace {

constexpr std::size_t kCropButtonCount = 12;

struct crop_text_store {
    std::wstring tooltip[kCropButtonCount]{};
    std::wstring shortcut[kCropButtonCount]{};
    std::wstring accessible[kCropButtonCount]{};
};

const crop_text_store& get_crop_text_store() noexcept {
    static const crop_text_store s_store = [] {
        crop_text_store store{};
        try {
            // 9 ratios: Free, Original, 1:1, 3:2, 4:3, 5:4, 16:9, 9:16, A-series
            store.tooltip[0] = viewer_string(IDS_CROP_TIP_FREE);
            store.tooltip[1] = viewer_string(IDS_CROP_TIP_ORIGINAL);
            store.tooltip[2] = viewer_string(IDS_CROP_TIP_1X1);
            store.tooltip[3] = viewer_string(IDS_CROP_TIP_3X2);
            store.tooltip[4] = viewer_string(IDS_CROP_TIP_4X3);
            store.tooltip[5] = viewer_string(IDS_CROP_TIP_5X4);
            store.tooltip[6] = viewer_string(IDS_CROP_TIP_16X9);
            store.tooltip[7] = viewer_string(IDS_CROP_TIP_9X16);
            store.tooltip[8] = viewer_string(IDS_CROP_TIP_ASERIES);
            // 3 actions: Reset, Apply, Cancel
            store.tooltip[9] = viewer_string(IDS_CROP_TIP_RESET);
            store.tooltip[10] = viewer_string(IDS_CROP_TIP_APPLY);
            store.tooltip[11] = viewer_string(IDS_CROP_TIP_CANCEL);

            // Shortcuts
            store.shortcut[0] = L"";
            store.shortcut[1] = L"";
            store.shortcut[2] = L"";
            store.shortcut[3] = L"";
            store.shortcut[4] = L"";
            store.shortcut[5] = L"";
            store.shortcut[6] = L"";
            store.shortcut[7] = L"";
            store.shortcut[8] = L"";
            store.shortcut[9] = L"R";
            store.shortcut[10] = L"Enter";
            store.shortcut[11] = L"Esc";

            // Accessible names
            store.accessible[0] = viewer_string(IDS_CROP_ACC_FREE);
            store.accessible[1] = viewer_string(IDS_CROP_ACC_ORIGINAL);
            store.accessible[2] = viewer_string(IDS_CROP_ACC_1X1);
            store.accessible[3] = viewer_string(IDS_CROP_ACC_3X2);
            store.accessible[4] = viewer_string(IDS_CROP_ACC_4X3);
            store.accessible[5] = viewer_string(IDS_CROP_ACC_5X4);
            store.accessible[6] = viewer_string(IDS_CROP_ACC_16X9);
            store.accessible[7] = viewer_string(IDS_CROP_ACC_9X16);
            store.accessible[8] = viewer_string(IDS_CROP_ACC_ASERIES);
            store.accessible[9] = viewer_string(IDS_CROP_ACC_RESET);
            store.accessible[10] = viewer_string(IDS_CROP_ACC_APPLY);
            store.accessible[11] = viewer_string(IDS_CROP_ACC_CANCEL);
        } catch (...) {
        }
        return store;
    }();
    return s_store;
}

struct crop_button_desc {
    ui_command_id cmd_id;
    int command_int_id;
    icon_id icon;
    const wchar_t* tooltip;
    const wchar_t* shortcut;
    const wchar_t* accessible_name;
    bool is_check_button = false;
};

const std::vector<crop_button_desc>& get_crop_button_descriptors() noexcept {
    const crop_text_store& text = get_crop_text_store();
    static const std::vector<crop_button_desc> s_buttons = {
        {ui_command_id::crop_ratio_free, 221, icon_id::crop_free, text.tooltip[0].c_str(), text.shortcut[0].c_str(), text.accessible[0].c_str(), true},
        {ui_command_id::crop_ratio_original, 222, icon_id::crop_original, text.tooltip[1].c_str(), text.shortcut[1].c_str(), text.accessible[1].c_str(), true},
        {ui_command_id::crop_ratio_1x1, 223, icon_id::crop_1x1, text.tooltip[2].c_str(), text.shortcut[2].c_str(), text.accessible[2].c_str(), true},
        {ui_command_id::crop_ratio_3x2, 224, icon_id::crop_3x2, text.tooltip[3].c_str(), text.shortcut[3].c_str(), text.accessible[3].c_str(), true},
        {ui_command_id::crop_ratio_4x3, 225, icon_id::crop_4x3, text.tooltip[4].c_str(), text.shortcut[4].c_str(), text.accessible[4].c_str(), true},
        {ui_command_id::crop_ratio_5x4, 226, icon_id::crop_5x4, text.tooltip[5].c_str(), text.shortcut[5].c_str(), text.accessible[5].c_str(), true},
        {ui_command_id::crop_ratio_16x9, 227, icon_id::crop_16x9, text.tooltip[6].c_str(), text.shortcut[6].c_str(), text.accessible[6].c_str(), true},
        {ui_command_id::crop_ratio_9x16, 228, icon_id::crop_9x16, text.tooltip[7].c_str(), text.shortcut[7].c_str(), text.accessible[7].c_str(), true},
        {ui_command_id::crop_ratio_a_series, 229, icon_id::crop_a_series, text.tooltip[8].c_str(), text.shortcut[8].c_str(), text.accessible[8].c_str(), true},
        {ui_command_id::edit_crop_reset, 210, icon_id::crop_reset, text.tooltip[9].c_str(), text.shortcut[9].c_str(), text.accessible[9].c_str(), false},
        {ui_command_id::edit_crop_apply, 208, icon_id::crop_apply, text.tooltip[10].c_str(), text.shortcut[10].c_str(), text.accessible[10].c_str(), false},
        {ui_command_id::edit_crop_cancel, 209, icon_id::crop_cancel, text.tooltip[11].c_str(), text.shortcut[11].c_str(), text.accessible[11].c_str(), false}
    };
    return s_buttons;
}

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

    // Direct icon toolbar inside the crop strip container
    hwnd_toolbar_ = CreateWindowExW(
        0,
        TOOLBARCLASSNAMEW,
        L"CropToolbar",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP | TBSTYLE_FLAT | TBSTYLE_WRAPABLE | TBSTYLE_TOOLTIPS | CCS_NORESIZE | CCS_NODIVIDER | CCS_NOPARENTALIGN,
        0, 0, 0, 0,
        hwnd_strip_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdCropStripToolbar)),
        instance,
        nullptr);

    if (!hwnd_toolbar_) {
        destroy();
        return false;
    }

    SendMessageW(hwnd_toolbar_, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);

    // Read-only crop dimension summary label
    const std::wstring dim_initial = viewer_string(IDS_CROP_DIM_EMPTY);
    hwnd_txt_dim_ = CreateWindowExW(
        0, L"STATIC", dim_initial.c_str(),
        WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
        0, 0, 0, 0,
        hwnd_strip_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdCropStripStaticDim)),
        instance, nullptr);

    // Native horizontal scrollbar (revealed only when multi-row content exceeds available width)
    hwnd_scroll_ = CreateWindowExW(
        0,
        WC_SCROLLBARW,
        L"",
        WS_CHILD | SBS_HORZ | WS_TABSTOP,
        0, 0, 0, 0,
        hwnd_strip_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdCropStripScrollBar)),
        instance,
        nullptr);

    setup_toolbar_buttons();
    setup_tooltips();

    if (font_ui_) {
        SendMessageW(hwnd_toolbar_, WM_SETFONT, reinterpret_cast<WPARAM>(font_ui_), TRUE);
        if (hwnd_txt_dim_) {
            SendMessageW(hwnd_txt_dim_, WM_SETFONT, reinterpret_cast<WPARAM>(font_ui_), TRUE);
        }
    }

    return true;
}

void crop_strip::destroy() noexcept {
    if (hwnd_strip_) {
        DestroyWindow(hwnd_strip_);
        hwnd_strip_ = nullptr;
    }
    hwnd_toolbar_ = nullptr;
    hwnd_scroll_ = nullptr;
    hwnd_txt_dim_ = nullptr;
    hwnd_tooltip_ = nullptr;

    view_width_px_ = 0;
    toolbar_x_ = 0;
    toolbar_y_ = 0;
    content_width_px_ = 0;
    scroll_pos_px_ = 0;
    scroll_range_px_ = 0;
    toolbar_height_px_ = 0;

    if (font_ui_) {
        DeleteObject(font_ui_);
        font_ui_ = nullptr;
    }
    if (brush_strip_bg_) {
        DeleteObject(brush_strip_bg_);
        brush_strip_bg_ = nullptr;
    }
    icon_cache_.clear();
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

void crop_strip::apply_command_button_size() noexcept {
    if (!hwnd_toolbar_) return;
    const int cell_px = dip_to_physical_round(kCropIconCellDip, dpi_);
    SendMessageW(hwnd_toolbar_, TB_SETBUTTONSIZE, 0, MAKELPARAM(cell_px, cell_px));
}

void crop_strip::setup_toolbar_buttons() noexcept {
    if (!hwnd_toolbar_) return;

    const auto& descs = get_crop_button_descriptors();
    std::vector<TBBUTTON> buttons;
    buttons.reserve(descs.size() + 2);

    for (std::size_t i = 0; i < descs.size(); ++i) {
        if (i == 9) {
            // Group separator before Reset/Apply/Cancel
            TBBUTTON sep{};
            sep.iBitmap = 0;
            sep.idCommand = 0;
            sep.fsState = TBSTATE_ENABLED;
            sep.fsStyle = BTNS_SEP;
            buttons.push_back(sep);
        }
        const auto& desc = descs[i];
        TBBUTTON tb{};
        tb.iBitmap = I_IMAGENONE;
        tb.idCommand = desc.command_int_id;
        tb.fsState = TBSTATE_ENABLED;
        tb.fsStyle = BTNS_BUTTON | (desc.is_check_button ? BTNS_CHECK : 0);
        tb.dwData = static_cast<DWORD_PTR>(desc.cmd_id);
        tb.iString = reinterpret_cast<INT_PTR>(desc.tooltip);
        buttons.push_back(tb);
    }

    SendMessageW(hwnd_toolbar_, TB_ADDBUTTONSW, static_cast<WPARAM>(buttons.size()), reinterpret_cast<LPARAM>(buttons.data()));
    SendMessageW(hwnd_toolbar_, TB_SETEXTENDEDSTYLE, 0, TBSTYLE_EX_DRAWDDARROWS);
    apply_command_button_size();
}

void crop_strip::setup_tooltips() noexcept {
    if (!hwnd_toolbar_) return;
    hwnd_tooltip_ = reinterpret_cast<HWND>(SendMessageW(hwnd_toolbar_, TB_GETTOOLTIPS, 0, 0));
    if (hwnd_tooltip_) {
        SetWindowPos(hwnd_tooltip_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void crop_strip::set_dpi(float dpi) noexcept {
    if (dpi <= 0.0f) return;
    dpi_ = dpi;
    rebuild_fonts_and_brushes();

    if (font_ui_) {
        if (hwnd_toolbar_) {
            SendMessageW(hwnd_toolbar_, WM_SETFONT, reinterpret_cast<WPARAM>(font_ui_), TRUE);
            apply_command_button_size();
        }
        if (hwnd_txt_dim_) {
            SendMessageW(hwnd_txt_dim_, WM_SETFONT, reinterpret_cast<WPARAM>(font_ui_), TRUE);
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

void crop_strip::update_icons(ID2D1DeviceContext* d2d_context) noexcept {
    if (!hwnd_toolbar_) return;

    const auto& descs = get_crop_button_descriptors();

    auto apply_fallback_labels = [this, &descs]() {
        for (const auto& desc : descs) {
            std::wstring_view label = get_icon_fallback_label(desc.icon);
            std::wstring label_str(label);
            TBBUTTONINFOW tbinfo{sizeof(tbinfo)};
            tbinfo.dwMask = TBIF_TEXT;
            tbinfo.pszText = label_str.data();
            SendMessageW(hwnd_toolbar_, TB_SETBUTTONINFOW, desc.command_int_id, reinterpret_cast<LPARAM>(&tbinfo));
        }
        apply_command_button_size();
    };

    if (!icon_cache_.is_svg_supported(d2d_context)) {
        apply_fallback_labels();
        return;
    }

    const int icon_size_px = dip_to_physical_round(20.0f, dpi_);
    HIMAGELIST new_himl = ImageList_Create(icon_size_px, icon_size_px, ILC_COLOR32 | ILC_MASK, static_cast<int>(descs.size()), 0);
    if (!new_himl) {
        apply_fallback_labels();
        return;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = icon_size_px;
    bmi.bmiHeader.biHeight = -icon_size_px;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    COLORREF icon_color = palette_.text_normal;
    bool all_succeeded = true;

    for (const auto& desc : descs) {
        void* bits = nullptr;
        HBITMAP hbmp = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!hbmp || !bits) {
            all_succeeded = false;
            break;
        }

        const bool rendered = icon_cache_.rasterize_icon_pixels(d2d_context, desc.icon, icon_size_px, icon_color, bits);
        if (rendered) {
            ImageList_Add(new_himl, hbmp, nullptr);
        } else {
            all_succeeded = false;
        }
        DeleteObject(hbmp);

        if (!all_succeeded) break;
    }

    if (!all_succeeded) {
        ImageList_Destroy(new_himl);
        apply_fallback_labels();
        return;
    }

    HIMAGELIST old_himl = reinterpret_cast<HIMAGELIST>(SendMessageW(hwnd_toolbar_, TB_SETIMAGELIST, 0, reinterpret_cast<LPARAM>(new_himl)));
    if (old_himl) {
        ImageList_Destroy(old_himl);
    }

    for (const auto& desc : descs) {
        std::wstring acc_name(desc.accessible_name ? desc.accessible_name : L"");
        TBBUTTONINFOW tbinfo{sizeof(tbinfo)};
        tbinfo.dwMask = TBIF_TEXT;
        tbinfo.pszText = acc_name.data();
        SendMessageW(hwnd_toolbar_, TB_SETBUTTONINFOW, desc.command_int_id, reinterpret_cast<LPARAM>(&tbinfo));
    }
    apply_command_button_size();
}

void crop_strip::reposition_toolbar() noexcept {
    if (!hwnd_toolbar_ || view_width_px_ <= 0) {
        return;
    }

    SetWindowPos(hwnd_toolbar_, nullptr,
                 toolbar_x_ - scroll_pos_px_, toolbar_y_,
                 view_width_px_, toolbar_height_px_,
                 SWP_NOZORDER | SWP_SHOWWINDOW);

    if (hwnd_scroll_ && scroll_range_px_ > 0) {
        SCROLLINFO si{sizeof(si)};
        si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        si.nMin = 0;
        si.nMax = content_width_px_ - 1;
        si.nPage = static_cast<UINT>(view_width_px_);
        si.nPos = scroll_pos_px_;
        SetScrollInfo(hwnd_scroll_, SB_CTL, &si, TRUE);
    }
}

void crop_strip::scroll_to_offset(int pos_px) noexcept {
    const int clamped = (std::max)(0, (std::min)(pos_px, scroll_range_px_));
    if (clamped == scroll_pos_px_) {
        return;
    }
    scroll_pos_px_ = clamped;
    reposition_toolbar();
}

void crop_strip::ensure_command_visible(int command_int_id) noexcept {
    if (!hwnd_toolbar_ || scroll_range_px_ <= 0 || view_width_px_ <= 0) {
        return;
    }
    const int index = static_cast<int>(
        SendMessageW(hwnd_toolbar_, TB_COMMANDTOINDEX,
                     static_cast<WPARAM>(command_int_id), 0));
    if (index < 0) {
        return;
    }
    RECT item_rc{};
    if (!SendMessageW(hwnd_toolbar_, TB_GETITEMRECT,
                      static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&item_rc))) {
        return;
    }
    int pos = scroll_pos_px_;
    if (item_rc.left < pos) {
        pos = item_rc.left;
    } else if (item_rc.right > pos + view_width_px_) {
        pos = item_rc.right - view_width_px_;
    }
    scroll_to_offset(pos);
}

void crop_strip::update_layout(const ui_layout_result& layout, bool crop_active) noexcept {
    if (!hwnd_strip_) return;

    if (!crop_active || layout.crop_strip_physical.height <= 0) {
        ShowWindow(hwnd_strip_, SW_HIDE);
        return;
    }

    const auto& cs = layout.crop_strip_physical;
    SetWindowPos(hwnd_strip_, nullptr, cs.x, cs.y, cs.width, cs.height, SWP_NOZORDER | SWP_SHOWWINDOW);

    const float available_dip = physical_to_dip(cs.width, dpi_);
    const auto crop_bar = compute_crop_icon_bar_layout(available_dip);

    const int scrollbar_px = crop_bar.needs_scroll ? dip_to_physical_round(kCropIconScrollbarDip, dpi_) : 0;
    const int toolbar_h = (std::max)(0, cs.height - scrollbar_px);
    const int content_px = dip_to_physical_round(crop_bar.content_width_dip, dpi_);

    // Position dimension readout: reserve space in the native container so it
    // does not overlay Reset/Apply/Cancel or ratio buttons.
    // When the strip is wide enough to host the reserved right-hand summary band (toolbar width <= strip width - dim_band),
    // place hwnd_txt_dim_ on the right edge and constrain hwnd_toolbar_ to the remaining width.
    // At constrained widths (e.g. 480 DIP or narrower) where a dedicated right band would crowd controls into excessive rows/scroll,
    // stack the dimension summary band on row 1 or place it cleanly without overlapping the toolbar or scrollbar.
    const int dim_w = dip_to_physical_round(crop_bar.dim_width_dip, dpi_);
    const int dim_h = dip_to_physical_round(kCropIconCellDip, dpi_);
    const int pad_x = dip_to_physical_round(8.0f, dpi_);
    const int pad_y = dip_to_physical_round(kCropIconBarPadDip, dpi_);
    const int dim_band_w = dim_w + pad_x * 2;

    int tb_x = 0;
    int tb_y = 0;
    int tb_avail_w = cs.width;
    int tb_avail_h = toolbar_h;
    int dim_target_x = 0;
    int dim_target_y = 0;

    // Check if we can reserve a dedicated right-hand dimension band:
    // Requires total width >= content_px + dim_band_w (or available DIP allows single row + right band)
    const bool can_reserve_right_band = (cs.width >= content_px + dim_band_w) ||
        (crop_bar.rows == 1 && cs.width >= dip_to_physical_round(592.0f + 32.0f, dpi_));

    if (can_reserve_right_band) {
        // Dedicated right band reserved: toolbar cannot extend into this band
        tb_avail_w = (std::max)(1, cs.width - dim_band_w);
        tb_avail_h = toolbar_h;
        dim_target_x = cs.width - dim_w - pad_x;
        dim_target_y = pad_y + (std::max)(0, (toolbar_h - dim_h) / 2);
    } else {
        // Constrained width or multi-row layout:
        // When multi-row, place dimension summary on row 0 or row 1 in a dedicated band,
        // or position toolbar and dim label so their rectangles NEVER overlap.
        // Specifically: if bar has 2 rows and height allows stacked placement, toolbar gets top/bottom row
        // OR toolbar is placed above/below dim summary, OR toolbar available width is cs.width - dim_band_w
        // with scrollbar handling the rest.
        // Guaranteeing NON-OVERLAP: tb_avail_w = cs.width - dim_band_w ensures the toolbar viewport
        // and hwnd_txt_dim_ NEVER intersect horizontally!
        if (cs.width >= dim_band_w + dip_to_physical_round(kCropIconCellDip * 3, dpi_)) {
            // Reserve right-hand band for dimension summary; toolbar view is bounded to [0, cs.width - dim_band_w)
            tb_avail_w = cs.width - dim_band_w;
            dim_target_x = cs.width - dim_w - pad_x;
            dim_target_y = pad_y + (std::max)(0, (toolbar_h - dim_h) / 2);
        } else {
            // Extremely narrow: stack toolbar on top, dim summary below, or reserve top band
            const int stacked_h = dim_h + pad_y * 2;
            if (cs.height >= stacked_h + dip_to_physical_round(kCropIconCellDip, dpi_)) {
                tb_y = 0;
                tb_avail_w = cs.width;
                tb_avail_h = (std::max)(1, cs.height - scrollbar_px - stacked_h);
                dim_target_x = (std::max)(0, (cs.width - dim_w) / 2);
                dim_target_y = tb_avail_h + pad_y;
            } else {
                tb_avail_w = (std::max)(1, cs.width - dim_w - pad_x);
                dim_target_x = cs.width - dim_w;
                dim_target_y = pad_y;
            }
        }
    }

    view_width_px_ = tb_avail_w;
    toolbar_x_ = tb_x;
    toolbar_y_ = tb_y;
    content_width_px_ = content_px;
    toolbar_height_px_ = tb_avail_h;
    scroll_range_px_ = (std::max)(0, content_px - tb_avail_w);
    scroll_pos_px_ = (std::max)(0, (std::min)(scroll_pos_px_, scroll_range_px_));

    reposition_toolbar();

    if (hwnd_txt_dim_) {
        SetWindowPos(hwnd_txt_dim_, nullptr,
                     dim_target_x, dim_target_y,
                     dim_w, dim_h,
                     SWP_NOZORDER | SWP_SHOWWINDOW);
    }

    if (hwnd_scroll_) {
        if (scroll_range_px_ > 0) {
            SetWindowPos(hwnd_scroll_, nullptr,
                         0, cs.height - scrollbar_px,
                         cs.width, scrollbar_px,
                         SWP_NOZORDER | SWP_SHOWWINDOW);
            SCROLLINFO si{sizeof(si)};
            si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
            si.nMin = 0;
            si.nMax = (content_px > 0) ? (content_px - 1) : 0;
            si.nPage = static_cast<UINT>(tb_avail_w);
            si.nPos = scroll_pos_px_;
            SetScrollInfo(hwnd_scroll_, SB_CTL, &si, TRUE);
        } else {
            ShowWindow(hwnd_scroll_, SW_HIDE);
        }
    }
}

void crop_strip::sync_state(const oz::viewer_core::crop_interaction_state* crop_state) noexcept {
    if (!hwnd_strip_ || !crop_state || !crop_state->active) {
        return;
    }

    // 1. Sync ratio buttons exclusive check state
    if (hwnd_toolbar_) {
        const auto& descs = get_crop_button_descriptors();
        const auto& commands = get_crop_command_items();
        for (std::size_t i = 0; i < 9 && i < descs.size(); ++i) {
            const bool is_checked = (commands[i].preset == crop_state->preset);
            SendMessageW(hwnd_toolbar_, TB_CHECKBUTTON, descs[i].command_int_id, MAKELPARAM(is_checked ? TRUE : FALSE, 0));
        }
    }

    // 2. Read-only current crop dimensions summary
    sync_dimension_summary(crop_state);
}

void crop_strip::sync_dimension_summary(const oz::viewer_core::crop_interaction_state* crop_state) noexcept {
    if (!hwnd_txt_dim_ || !crop_state || !crop_state->active) {
        return;
    }

    const std::wstring dim_text =
        crop_state->selection.is_valid()
            ? format_crop_dimensions(crop_state->selection.width,
                                     crop_state->selection.height)
            : viewer_string(IDS_CROP_DIM_EMPTY);
    SetWindowTextW(hwnd_txt_dim_, dim_text.c_str());
}

LRESULT crop_strip::handle_notify(LPARAM lparam) noexcept {
    auto* hdr = reinterpret_cast<NMHDR*>(lparam);
    if (!hdr) return 0;

    if (hdr->hwndFrom == hwnd_toolbar_) {
        if (hdr->code == TBN_HOTITEMCHANGE) {
            auto* hot = reinterpret_cast<NMTBHOTITEM*>(lparam);
            if (hot) {
                ensure_command_visible(static_cast<int>(hot->idNew));
            }
            return 0;
        }
    }

    if (hdr->code == TTN_GETDISPINFOW) {
        auto* disp = reinterpret_cast<NMTTDISPINFOW*>(lparam);
        const int cmd_id = static_cast<int>(hdr->idFrom);
        const auto& descs = get_crop_button_descriptors();
        for (const auto& desc : descs) {
            if (desc.command_int_id == cmd_id) {
                static wchar_t s_tip_buf[256];
                if (desc.shortcut && desc.shortcut[0] != L'\0') {
                    swprintf_s(s_tip_buf, L"%s (%s)", desc.tooltip, desc.shortcut);
                } else {
                    swprintf_s(s_tip_buf, L"%s", desc.tooltip);
                }
                disp->lpszText = s_tip_buf;
                return 0;
            }
        }
    }

    return 0;
}

void crop_strip::handle_hscroll(WPARAM wparam, LPARAM lparam) noexcept {
    if (!hwnd_scroll_ || reinterpret_cast<HWND>(lparam) != hwnd_scroll_ || scroll_range_px_ <= 0) {
        return;
    }

    const int code = LOWORD(wparam);
    int pos = scroll_pos_px_;
    switch (code) {
        case SB_LINELEFT:
            pos -= dip_to_physical_round(24.0f, dpi_);
            break;
        case SB_LINERIGHT:
            pos += dip_to_physical_round(24.0f, dpi_);
            break;
        case SB_PAGELEFT:
            pos -= view_width_px_;
            break;
        case SB_PAGERIGHT:
            pos += view_width_px_;
            break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: {
            SCROLLINFO si{sizeof(si)};
            si.fMask = SIF_TRACKPOS | SIF_POS;
            if (GetScrollInfo(hwnd_scroll_, SB_CTL, &si)) {
                pos = (code == SB_THUMBTRACK) ? si.nTrackPos : si.nPos;
            }
            break;
        }
        case SB_LEFT:
            pos = 0;
            break;
        case SB_RIGHT:
            pos = scroll_range_px_;
            break;
        default:
            return;
    }
    scroll_to_offset(pos);
}

HBRUSH crop_strip::handle_ctl_color(HDC hdc, HWND control) noexcept {
    if (control == hwnd_strip_ || control == hwnd_txt_dim_ || control == hwnd_toolbar_) {
        SetBkColor(hdc, palette_.bg_chrome);
        SetTextColor(hdc, palette_.text_normal);
        return brush_strip_bg_;
    }
    return nullptr;
}

}  // namespace oz::viewer

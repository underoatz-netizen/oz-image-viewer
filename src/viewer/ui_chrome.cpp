// oz/viewer/ui_chrome.cpp - modular native Win32 toolbar and status bar chrome implementation.
#include "ui_chrome.hpp"

#include <shellapi.h>
#include <uxtheme.h>
#include <commctrl.h>

#include <algorithm>
#include <cmath>

namespace oz::viewer {

namespace {

constexpr int kToolbarControlId = 3001;
constexpr int kStatusControlId  = 3002;

}  // namespace

const std::vector<toolbar_button_desc>& get_toolbar_button_descriptors() noexcept {
    static const std::vector<toolbar_button_desc> s_buttons = {
        {ui_command_id::file_open,         101, icon_id::open,         L"Open",         L"Ctrl+O",        L"Open file"},
        {ui_command_id::file_save,         103, icon_id::save,         L"Save",         L"Ctrl+S",        L"Save image"},
        {ui_command_id::edit_undo,         205, icon_id::undo,         L"Undo",         L"Ctrl+Z",        L"Undo edit"},
        {ui_command_id::edit_redo,         206, icon_id::redo,         L"Redo",         L"Ctrl+Y",        L"Redo edit"},
        {ui_command_id::edit_rotate_left,  201, icon_id::rotate_left,  L"Rotate Left",  L"Ctrl+L",        L"Rotate left 90 degrees"},
        {ui_command_id::edit_rotate_right, 202, icon_id::rotate_right, L"Rotate Right", L"Ctrl+R",        L"Rotate right 90 degrees"},
        {ui_command_id::edit_crop,         207, icon_id::crop,         L"Crop",         L"C",             L"Crop mode"},
        {ui_command_id::edit_resize,       211, icon_id::resize,       L"Resize",       L"Ctrl+Alt+R",    L"Resize image"},
        {ui_command_id::view_fit,          302, icon_id::fit,          L"Fit to Window",L"F",             L"Fit image to window", true},
        {ui_command_id::view_actual_size,  301, icon_id::one_to_one,   L"Actual Size",  L"1",             L"Zoom 100 percent",    true},
        {ui_command_id::view_fullscreen,   309, icon_id::fullscreen,   L"Fullscreen",   L"F11",           L"Toggle fullscreen",   true}
    };
    return s_buttons;
}

bool ui_chrome::create(HWND parent, HINSTANCE instance, float dpi) {
    destroy();
    parent_ = parent;
    dpi_ = (dpi > 0.0f) ? dpi : 96.0f;
    theme_mode_ = query_system_theme_mode();
    palette_ = get_theme_palette(theme_mode_);

    INITCOMMONCONTROLSEX icex{sizeof(INITCOMMONCONTROLSEX), ICC_BAR_CLASSES | ICC_TAB_CLASSES};
    InitCommonControlsEx(&icex);

    rebuild_fonts();

    // 1. Create native toolbar
    // Using TBSTYLE_FLAT, TBSTYLE_TOOLTIPS, CCS_NORESIZE, CCS_NODIVIDER, WS_CHILD, WS_TABSTOP
    hwnd_toolbar_ = CreateWindowExW(
        0,
        TOOLBARCLASSNAMEW,
        L"OZ Viewer Toolbar",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS | CCS_NORESIZE | CCS_NODIVIDER,
        0, 0, 0, 0,
        parent_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kToolbarControlId)),
        instance,
        nullptr);

    if (!hwnd_toolbar_) {
        return false;
    }

    SendMessageW(hwnd_toolbar_, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);

    // 2. Create native status bar
    hwnd_status_ = CreateWindowExW(
        0,
        STATUSCLASSNAMEW,
        L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | SBARS_SIZEGRIP | CCS_NORESIZE | CCS_NODIVIDER,
        0, 0, 0, 0,
        parent_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusControlId)),
        instance,
        nullptr);

    if (!hwnd_status_) {
        destroy();
        return false;
    }

    if (font_ui_) {
        SendMessageW(hwnd_toolbar_, WM_SETFONT, reinterpret_cast<WPARAM>(font_ui_), TRUE);
        SendMessageW(hwnd_status_, WM_SETFONT, reinterpret_cast<WPARAM>(font_ui_), TRUE);
    }

    setup_toolbar_buttons();
    setup_tooltips();

    // Set status bar parts: [0: file/detail, 1: dimensions, 2: zoom, 3: nav]
    int parts[] = {300, 480, 600, -1};
    SendMessageW(hwnd_status_, SB_SETPARTS, static_cast<WPARAM>(4), reinterpret_cast<LPARAM>(parts));

    apply_dwm_caption_theme(parent_, theme_mode_);
    return true;
}

void ui_chrome::destroy() noexcept {
    if (himl_) {
        ImageList_Destroy(himl_);
        himl_ = nullptr;
    }
    using_image_list_ = false;

    if (hwnd_toolbar_) {
        DestroyWindow(hwnd_toolbar_);
        hwnd_toolbar_ = nullptr;
    }
    if (hwnd_status_) {
        DestroyWindow(hwnd_status_);
        hwnd_status_ = nullptr;
    }
    if (hwnd_tooltip_) {
        DestroyWindow(hwnd_tooltip_);
        hwnd_tooltip_ = nullptr;
    }
    if (font_ui_) {
        DeleteObject(font_ui_);
        font_ui_ = nullptr;
    }
    if (brush_chrome_bg_) {
        DeleteObject(brush_chrome_bg_);
        brush_chrome_bg_ = nullptr;
    }
    if (brush_status_bg_) {
        DeleteObject(brush_status_bg_);
        brush_status_bg_ = nullptr;
    }
    icon_cache_.clear();
    parent_ = nullptr;
}

void ui_chrome::rebuild_fonts() noexcept {
    if (font_ui_) {
        DeleteObject(font_ui_);
        font_ui_ = nullptr;
    }
    if (brush_chrome_bg_) {
        DeleteObject(brush_chrome_bg_);
        brush_chrome_bg_ = nullptr;
    }
    if (brush_status_bg_) {
        DeleteObject(brush_status_bg_);
        brush_status_bg_ = nullptr;
    }

    brush_chrome_bg_ = CreateSolidBrush(palette_.bg_chrome);
    brush_status_bg_ = CreateSolidBrush(palette_.status_bg);

    const int font_height = dip_to_physical_round(12.0f, dpi_);
    font_ui_ = CreateFontW(
        -font_height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");
}

void ui_chrome::setup_toolbar_buttons() noexcept {
    if (!hwnd_toolbar_) return;

    const auto& descs = get_toolbar_button_descriptors();
    std::vector<TBBUTTON> buttons;
    buttons.reserve(descs.size());

    for (std::size_t i = 0; i < descs.size(); ++i) {
        const auto& desc = descs[i];
        TBBUTTON tb{};
        tb.iBitmap = I_IMAGENONE;
        tb.idCommand = desc.command_int_id;
        tb.fsState = TBSTATE_ENABLED;
        tb.fsStyle = BTNS_BUTTON | BTNS_AUTOSIZE | (desc.is_check_button ? BTNS_CHECK : 0);
        tb.dwData = static_cast<DWORD_PTR>(desc.cmd_id);
        tb.iString = reinterpret_cast<INT_PTR>(desc.tooltip);
        buttons.push_back(tb);
    }

    SendMessageW(hwnd_toolbar_, TB_ADDBUTTONSW, static_cast<WPARAM>(buttons.size()), reinterpret_cast<LPARAM>(buttons.data()));
    SendMessageW(hwnd_toolbar_, TB_SETEXTENDEDSTYLE, 0, TBSTYLE_EX_DRAWDDARROWS);
    SendMessageW(hwnd_toolbar_, TB_AUTOSIZE, 0, 0);
}

void ui_chrome::setup_tooltips() noexcept {
    if (!hwnd_toolbar_) return;
    HWND hTooltip = reinterpret_cast<HWND>(SendMessageW(hwnd_toolbar_, TB_GETTOOLTIPS, 0, 0));
    if (hTooltip) {
        hwnd_tooltip_ = hTooltip;
        SetWindowPos(hTooltip, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void ui_chrome::set_dpi(float dpi) noexcept {
    if (dpi <= 0.0f) return;
    dpi_ = dpi;
    rebuild_fonts();

    if (hwnd_toolbar_) {
        SendMessageW(hwnd_toolbar_, WM_SETFONT, reinterpret_cast<WPARAM>(font_ui_), TRUE);
        SendMessageW(hwnd_toolbar_, TB_AUTOSIZE, 0, 0);
    }
    if (hwnd_status_) {
        SendMessageW(hwnd_status_, WM_SETFONT, reinterpret_cast<WPARAM>(font_ui_), TRUE);
    }
    icon_cache_.clear();
}

void ui_chrome::set_theme(theme_mode mode) noexcept {
    theme_mode_ = mode;
    palette_ = get_theme_palette(theme_mode_);
    rebuild_fonts();
    icon_cache_.clear();

    if (parent_) {
        apply_dwm_caption_theme(parent_, theme_mode_);
    }
    if (hwnd_toolbar_) {
        InvalidateRect(hwnd_toolbar_, nullptr, TRUE);
    }
    if (hwnd_status_) {
        InvalidateRect(hwnd_status_, nullptr, TRUE);
    }
}

void ui_chrome::update_icons(ID2D1DeviceContext* d2d_context) noexcept {
    if (!hwnd_toolbar_) return;

    auto apply_text_fallback = [this]() {
        if (using_image_list_) {
            SendMessageW(hwnd_toolbar_, TB_SETIMAGELIST, 0, reinterpret_cast<LPARAM>(nullptr));
            if (himl_) {
                ImageList_Destroy(himl_);
                himl_ = nullptr;
            }
            using_image_list_ = false;
        }

        const auto& descs = get_toolbar_button_descriptors();
        for (const auto& desc : descs) {
            TBBUTTONINFOW tbinfo{sizeof(TBBUTTONINFOW)};
            tbinfo.dwMask = TBIF_TEXT | TBIF_IMAGE;
            tbinfo.iImage = I_IMAGENONE;
            // Use the full accessible name (e.g. "Rotate left 90 degrees") for screen readers,
            // while preserving readable, complete button text in fallback mode.
            std::wstring label_str(desc.accessible_name ? desc.accessible_name : desc.tooltip);
            tbinfo.pszText = label_str.data();
            SendMessageW(hwnd_toolbar_, TB_SETBUTTONINFOW, desc.command_int_id, reinterpret_cast<LPARAM>(&tbinfo));
        }
        SendMessageW(hwnd_toolbar_, TB_AUTOSIZE, 0, 0);
    };

    // If SVG is not supported or d2d_context is null, ensure buttons use fallback text
    if (!d2d_context || !icon_cache_.is_svg_supported(d2d_context)) {
        apply_text_fallback();
        return;
    }

    // SVG supported: render icons transactionally into a 32-bit ARGB HBITMAP ImageList
    const int icon_size_px = dip_to_physical_round(16.0f, dpi_);
    const auto& descs = get_toolbar_button_descriptors();

    HIMAGELIST new_himl = ImageList_Create(icon_size_px, icon_size_px, ILC_COLOR32 | ILC_MASK, static_cast<int>(descs.size()), 0);
    if (!new_himl) {
        apply_text_fallback();
        return;
    }

    HDC hdc_screen = GetDC(nullptr);
    HDC hdc_mem = CreateCompatibleDC(hdc_screen);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = icon_size_px;
    bmi.bmiHeader.biHeight = -icon_size_px; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    bool all_rendered = true;
    for (std::size_t i = 0; i < descs.size(); ++i) {
        const auto& desc = descs[i];
        void* dib_bits = nullptr;
        HBITMAP hbmp = CreateDIBSection(hdc_mem, &bmi, DIB_RGB_COLORS, &dib_bits, nullptr, 0);
        if (hbmp && dib_bits) {
            std::memset(dib_bits, 0, static_cast<std::size_t>(icon_size_px * icon_size_px * 4));
            const bool ok = icon_cache_.rasterize_icon_pixels(
                d2d_context, desc.icon, icon_size_px, palette_.text_normal, dib_bits);
            if (!ok) {
                all_rendered = false;
                DeleteObject(hbmp);
                break;
            }
            ImageList_Add(new_himl, hbmp, nullptr);
            DeleteObject(hbmp);
        } else {
            all_rendered = false;
            break;
        }
    }

    DeleteDC(hdc_mem);
    ReleaseDC(nullptr, hdc_screen);

    // Transactional check: if any icon failed to rasterize, destroy the partial ImageList
    // and fall back immediately to clean text-labelled buttons. Never show blank buttons!
    if (!all_rendered || ImageList_GetImageCount(new_himl) != static_cast<int>(descs.size())) {
        ImageList_Destroy(new_himl);
        apply_text_fallback();
        return;
    }

    SendMessageW(hwnd_toolbar_, TB_SETIMAGELIST, 0, reinterpret_cast<LPARAM>(new_himl));
    if (himl_) {
        ImageList_Destroy(himl_);
    }
    himl_ = new_himl;
    using_image_list_ = true;

    for (std::size_t i = 0; i < descs.size(); ++i) {
        const auto& desc = descs[i];
        TBBUTTONINFOW tbinfo{sizeof(TBBUTTONINFOW)};
        tbinfo.dwMask = TBIF_IMAGE | TBIF_TEXT;
        tbinfo.iImage = static_cast<int>(i);
        std::wstring acc_name(desc.accessible_name ? desc.accessible_name : desc.tooltip);
        tbinfo.pszText = acc_name.data();
        SendMessageW(hwnd_toolbar_, TB_SETBUTTONINFOW, desc.command_int_id, reinterpret_cast<LPARAM>(&tbinfo));
    }
    SendMessageW(hwnd_toolbar_, TB_AUTOSIZE, 0, 0);
}

void ui_chrome::update_layout(const ui_layout_result& layout, bool is_fullscreen) noexcept {
    if (is_fullscreen) {
        if (hwnd_toolbar_) ShowWindow(hwnd_toolbar_, SW_HIDE);
        if (hwnd_status_) ShowWindow(hwnd_status_, SW_HIDE);
        return;
    }

    if (hwnd_toolbar_) {
        const auto& tb = layout.toolbar_physical;
        if (tb.height > 0) {
            SetWindowPos(hwnd_toolbar_, nullptr, tb.x, tb.y, tb.width, tb.height, SWP_NOZORDER | SWP_SHOWWINDOW);
        } else {
            ShowWindow(hwnd_toolbar_, SW_HIDE);
        }
    }

    if (hwnd_status_) {
        const auto& sb = layout.status_physical;
        if (sb.height > 0) {
            SetWindowPos(hwnd_status_, nullptr, sb.x, sb.y, sb.width, sb.height, SWP_NOZORDER | SWP_SHOWWINDOW);

            // Scale status bar parts proportionally
            const int p1 = dip_to_physical_round(240.0f, dpi_);
            const int p2 = p1 + dip_to_physical_round(160.0f, dpi_);
            const int p3 = p2 + dip_to_physical_round(120.0f, dpi_);
            int parts[] = {p1, p2, p3, -1};
            SendMessageW(hwnd_status_, SB_SETPARTS, static_cast<WPARAM>(4), reinterpret_cast<LPARAM>(parts));
        } else {
            ShowWindow(hwnd_status_, SW_HIDE);
        }
    }
}

void ui_chrome::update_command_states(const ui_command_context& ctx) noexcept {
    if (!hwnd_toolbar_) return;

    const auto& descs = get_toolbar_button_descriptors();
    for (const auto& desc : descs) {
        const auto item_state = derive_command_state(desc.cmd_id, ctx);
        SendMessageW(hwnd_toolbar_, TB_ENABLEBUTTON, desc.command_int_id, item_state.enabled ? TRUE : FALSE);
        if (desc.is_check_button) {
            SendMessageW(hwnd_toolbar_, TB_CHECKBUTTON, desc.command_int_id, item_state.checked ? TRUE : FALSE);
        }
    }
}

void ui_chrome::update_status(const status_bar_info& info) noexcept {
    if (!hwnd_status_) return;

    std::wstring part0 = info.file_name;
    if (!info.state_label.empty()) {
        if (!part0.empty()) part0 += L"  •  ";
        part0 += info.state_label;
    }

    SendMessageW(hwnd_status_, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(part0.c_str()));
    SendMessageW(hwnd_status_, SB_SETTEXTW, 1, reinterpret_cast<LPARAM>(info.dimensions.c_str()));
    SendMessageW(hwnd_status_, SB_SETTEXTW, 2, reinterpret_cast<LPARAM>(info.zoom.c_str()));
    SendMessageW(hwnd_status_, SB_SETTEXTW, 3, reinterpret_cast<LPARAM>(info.navigation.c_str()));
}

LRESULT ui_chrome::handle_notify(LPARAM lparam) noexcept {
    auto* hdr = reinterpret_cast<NMHDR*>(lparam);
    if (!hdr) return 0;

    if (hdr->code == TTN_GETDISPINFOW) {
        auto* disp = reinterpret_cast<NMTTDISPINFOW*>(lparam);
        const int cmd_id = static_cast<int>(hdr->idFrom);
        const auto& descs = get_toolbar_button_descriptors();
        for (const auto& desc : descs) {
            if (desc.command_int_id == cmd_id) {
                static wchar_t s_tip[256];
                if (desc.shortcut && desc.shortcut[0] != L'\0') {
                    swprintf_s(s_tip, L"%s (%s)", desc.tooltip, desc.shortcut);
                } else {
                    swprintf_s(s_tip, L"%s", desc.tooltip);
                }
                disp->lpszText = s_tip;
                return 0;
            }
        }
    }

    // Custom draw for toolbar if needed for high contrast or background colors
    if (hdr->hwndFrom == hwnd_toolbar_ && hdr->code == NM_CUSTOMDRAW) {
        auto* nmcd = reinterpret_cast<NMTBCUSTOMDRAW*>(lparam);
        switch (nmcd->nmcd.dwDrawStage) {
            case CDDS_PREPAINT:
                return CDRF_NOTIFYITEMDRAW;
            case CDDS_ITEMPREPAINT: {
                if (theme_mode_ == theme_mode::high_contrast) {
                    nmcd->clrText = palette_.text_normal;
                    nmcd->clrBtnFace = palette_.bg_chrome;
                    nmcd->clrHighlightHotTrack = palette_.btn_hover;
                } else if (theme_mode_ == theme_mode::dark) {
                    nmcd->clrText = palette_.text_normal;
                    nmcd->clrBtnFace = palette_.bg_chrome;
                    nmcd->clrHighlightHotTrack = palette_.btn_hover;
                }
                return CDRF_DODEFAULT;
            }
            default:
                return CDRF_DODEFAULT;
        }
    }

    return 0;
}

HBRUSH ui_chrome::handle_ctl_color(HDC hdc, HWND control) noexcept {
    if (control == hwnd_toolbar_ || control == hwnd_status_) {
        SetBkColor(hdc, (control == hwnd_status_) ? palette_.status_bg : palette_.bg_chrome);
        SetTextColor(hdc, (control == hwnd_status_) ? palette_.status_text : palette_.text_normal);
        return (control == hwnd_status_) ? brush_status_bg_ : brush_chrome_bg_;
    }
    return nullptr;
}

}  // namespace oz::viewer

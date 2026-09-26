// oz/viewer/ui_chrome.cpp - modular native Win32 toolbar and status bar chrome implementation.
#include "ui_chrome.hpp"

#include "std_icon_bar.hpp"
#include "viewer_strings.hpp"

#include <shellapi.h>
#include <uxtheme.h>
#include <commctrl.h>

#include <algorithm>
#include <cmath>

namespace oz::viewer {

namespace {

constexpr int kToolbarControlId = 3001;
constexpr int kStatusControlId  = 3002;
constexpr int kBarScrollControlId = 3003;

constexpr std::size_t kToolbarButtonCount = 27;

// Stable ownership for retained toolbar/tooltip/accessibility text (Stage
// 10.3): the toolbar keeps raw const wchar_t* pointers (TBBUTTON iString and
// TB_SETBUTTONINFOW pszText are consumed synchronously by SendMessageW), so
// the pointed-to storage must outlive every use. These owning buffers are
// initialized once from the string accessor and never mutated afterwards, so
// no temporary c_str() is ever retained. Entries are in std_icon_bar catalog
// order. Commands without a keyboard shortcut keep an empty shortcut string so
// their tooltip is the plain command name (never a dangling parenthetical).
struct toolbar_text_store {
    std::wstring tooltip[kToolbarButtonCount]{};
    std::wstring shortcut[kToolbarButtonCount]{};
    std::wstring accessible[kToolbarButtonCount]{};
};

const toolbar_text_store& get_toolbar_text_store() noexcept {
    static const toolbar_text_store s_store = [] {
        toolbar_text_store store{};
        try {
            store.tooltip[0]  = viewer_string(IDS_TB_TIP_OPEN);
            store.tooltip[1]  = viewer_string(IDS_TB_TIP_PASTE_IMAGE);
            store.tooltip[2]  = viewer_string(IDS_TB_TIP_SAVE);
            store.tooltip[3]  = viewer_string(IDS_TB_TIP_SAVE_AS);
            store.tooltip[4]  = viewer_string(IDS_TB_TIP_OPEN_LOCATION);
            store.tooltip[5]  = viewer_string(IDS_TB_TIP_COPY_IMAGE);
            store.tooltip[6]  = viewer_string(IDS_TB_TIP_COPY_FILE);
            store.tooltip[7]  = viewer_string(IDS_TB_TIP_COPY_PATH);
            store.tooltip[8]  = viewer_string(IDS_TB_TIP_PRINT);
            store.tooltip[9]  = viewer_string(IDS_TB_TIP_DELETE);
            store.tooltip[10] = viewer_string(IDS_TB_TIP_UNDO);
            store.tooltip[11] = viewer_string(IDS_TB_TIP_REDO);
            store.tooltip[12] = viewer_string(IDS_TB_TIP_ROTATE_LEFT);
            store.tooltip[13] = viewer_string(IDS_TB_TIP_ROTATE_RIGHT);
            store.tooltip[14] = viewer_string(IDS_TB_TIP_FLIP_H);
            store.tooltip[15] = viewer_string(IDS_TB_TIP_FLIP_V);
            store.tooltip[16] = viewer_string(IDS_TB_TIP_CROP);
            store.tooltip[17] = viewer_string(IDS_TB_TIP_RESIZE);
            store.tooltip[18] = viewer_string(IDS_TB_TIP_FIT);
            store.tooltip[19] = viewer_string(IDS_TB_TIP_ACTUAL);
            store.tooltip[20] = viewer_string(IDS_TB_TIP_ZOOM_IN);
            store.tooltip[21] = viewer_string(IDS_TB_TIP_ZOOM_OUT);
            store.tooltip[22] = viewer_string(IDS_TB_TIP_FIRST);
            store.tooltip[23] = viewer_string(IDS_TB_TIP_PREV);
            store.tooltip[24] = viewer_string(IDS_TB_TIP_NEXT);
            store.tooltip[25] = viewer_string(IDS_TB_TIP_LAST);
            store.tooltip[26] = viewer_string(IDS_TB_TIP_FULLSCREEN);

            store.shortcut[0]  = viewer_string(IDS_TB_SHORTCUT_OPEN);
            store.shortcut[1]  = viewer_string(IDS_TB_SHORTCUT_PASTE_IMAGE);
            store.shortcut[2]  = viewer_string(IDS_TB_SHORTCUT_SAVE);
            store.shortcut[3]  = viewer_string(IDS_TB_SHORTCUT_SAVE_AS);
            // 4 Open File Location: no keyboard shortcut.
            store.shortcut[5]  = viewer_string(IDS_TB_SHORTCUT_COPY_IMAGE);
            // 6 Copy File, 7 Copy File Path: no keyboard shortcut.
            store.shortcut[8]  = viewer_string(IDS_TB_SHORTCUT_PRINT);
            store.shortcut[9]  = viewer_string(IDS_TB_SHORTCUT_DELETE);
            store.shortcut[10] = viewer_string(IDS_TB_SHORTCUT_UNDO);
            store.shortcut[11] = viewer_string(IDS_TB_SHORTCUT_REDO);
            store.shortcut[12] = viewer_string(IDS_TB_SHORTCUT_ROTATE_LEFT);
            store.shortcut[13] = viewer_string(IDS_TB_SHORTCUT_ROTATE_RIGHT);
            store.shortcut[14] = viewer_string(IDS_TB_SHORTCUT_FLIP_H);
            store.shortcut[15] = viewer_string(IDS_TB_SHORTCUT_FLIP_V);
            store.shortcut[16] = viewer_string(IDS_TB_SHORTCUT_CROP);
            store.shortcut[17] = viewer_string(IDS_TB_SHORTCUT_RESIZE);
            store.shortcut[18] = viewer_string(IDS_TB_SHORTCUT_FIT);
            store.shortcut[19] = viewer_string(IDS_TB_SHORTCUT_ACTUAL);
            store.shortcut[20] = viewer_string(IDS_TB_SHORTCUT_ZOOM_IN);
            store.shortcut[21] = viewer_string(IDS_TB_SHORTCUT_ZOOM_OUT);
            store.shortcut[22] = viewer_string(IDS_TB_SHORTCUT_FIRST);
            store.shortcut[23] = viewer_string(IDS_TB_SHORTCUT_PREV);
            store.shortcut[24] = viewer_string(IDS_TB_SHORTCUT_NEXT);
            store.shortcut[25] = viewer_string(IDS_TB_SHORTCUT_LAST);
            store.shortcut[26] = viewer_string(IDS_TB_SHORTCUT_FULLSCREEN);

            store.accessible[0]  = viewer_string(IDS_TB_ACC_OPEN);
            store.accessible[1]  = viewer_string(IDS_TB_ACC_PASTE_IMAGE);
            store.accessible[2]  = viewer_string(IDS_TB_ACC_SAVE);
            store.accessible[3]  = viewer_string(IDS_TB_ACC_SAVE_AS);
            store.accessible[4]  = viewer_string(IDS_TB_ACC_OPEN_LOCATION);
            store.accessible[5]  = viewer_string(IDS_TB_ACC_COPY_IMAGE);
            store.accessible[6]  = viewer_string(IDS_TB_ACC_COPY_FILE);
            store.accessible[7]  = viewer_string(IDS_TB_ACC_COPY_PATH);
            store.accessible[8]  = viewer_string(IDS_TB_ACC_PRINT);
            store.accessible[9]  = viewer_string(IDS_TB_ACC_DELETE);
            store.accessible[10] = viewer_string(IDS_TB_ACC_UNDO);
            store.accessible[11] = viewer_string(IDS_TB_ACC_REDO);
            store.accessible[12] = viewer_string(IDS_TB_ACC_ROTATE_LEFT);
            store.accessible[13] = viewer_string(IDS_TB_ACC_ROTATE_RIGHT);
            store.accessible[14] = viewer_string(IDS_TB_ACC_FLIP_H);
            store.accessible[15] = viewer_string(IDS_TB_ACC_FLIP_V);
            store.accessible[16] = viewer_string(IDS_TB_ACC_CROP);
            store.accessible[17] = viewer_string(IDS_TB_ACC_RESIZE);
            store.accessible[18] = viewer_string(IDS_TB_ACC_FIT);
            store.accessible[19] = viewer_string(IDS_TB_ACC_ACTUAL);
            store.accessible[20] = viewer_string(IDS_TB_ACC_ZOOM_IN);
            store.accessible[21] = viewer_string(IDS_TB_ACC_ZOOM_OUT);
            store.accessible[22] = viewer_string(IDS_TB_ACC_FIRST);
            store.accessible[23] = viewer_string(IDS_TB_ACC_PREV);
            store.accessible[24] = viewer_string(IDS_TB_ACC_NEXT);
            store.accessible[25] = viewer_string(IDS_TB_ACC_LAST);
            store.accessible[26] = viewer_string(IDS_TB_ACC_FULLSCREEN);
        } catch (...) {
        }
        return store;
    }();
    return s_store;
}

}  // namespace

const std::vector<toolbar_button_desc>& get_toolbar_button_descriptors() noexcept {
    // The text store is initialized before the descriptor vector (declaration
    // order) so every retained pointer below refers to stable storage. The
    // order MUST match get_std_icon_commands() so the icon image-list index,
    // the group separators, and the command catalog all stay aligned.
    const toolbar_text_store& text = get_toolbar_text_store();
    static const std::vector<toolbar_button_desc> s_buttons = {
        {ui_command_id::file_open,         101, icon_id::open,         text.tooltip[0].c_str(),  text.shortcut[0].c_str(),  text.accessible[0].c_str()},
        {ui_command_id::file_paste_image,  111, icon_id::paste_image,  text.tooltip[1].c_str(),  text.shortcut[1].c_str(),  text.accessible[1].c_str()},
        {ui_command_id::file_save,         103, icon_id::save,         text.tooltip[2].c_str(),  text.shortcut[2].c_str(),  text.accessible[2].c_str()},
        {ui_command_id::file_save_as,      104, icon_id::save_as,      text.tooltip[3].c_str(),  text.shortcut[3].c_str(),  text.accessible[3].c_str()},
        {ui_command_id::file_open_location,105, icon_id::open_location,text.tooltip[4].c_str(),  text.shortcut[4].c_str(),  text.accessible[4].c_str()},
        {ui_command_id::file_copy_image,   108, icon_id::copy_image,   text.tooltip[5].c_str(),  text.shortcut[5].c_str(),  text.accessible[5].c_str()},
        {ui_command_id::file_copy,         106, icon_id::copy_file,    text.tooltip[6].c_str(),  text.shortcut[6].c_str(),  text.accessible[6].c_str()},
        {ui_command_id::file_copy_path,    107, icon_id::copy_path,    text.tooltip[7].c_str(),  text.shortcut[7].c_str(),  text.accessible[7].c_str()},
        {ui_command_id::file_print,        110, icon_id::print,        text.tooltip[8].c_str(),  text.shortcut[8].c_str(),  text.accessible[8].c_str()},
        {ui_command_id::file_delete,       109, icon_id::delete_image, text.tooltip[9].c_str(),  text.shortcut[9].c_str(),  text.accessible[9].c_str()},
        {ui_command_id::edit_undo,         205, icon_id::undo,         text.tooltip[10].c_str(), text.shortcut[10].c_str(), text.accessible[10].c_str()},
        {ui_command_id::edit_redo,         206, icon_id::redo,         text.tooltip[11].c_str(), text.shortcut[11].c_str(), text.accessible[11].c_str()},
        {ui_command_id::edit_rotate_left,  201, icon_id::rotate_left,  text.tooltip[12].c_str(), text.shortcut[12].c_str(), text.accessible[12].c_str()},
        {ui_command_id::edit_rotate_right, 202, icon_id::rotate_right, text.tooltip[13].c_str(), text.shortcut[13].c_str(), text.accessible[13].c_str()},
        {ui_command_id::edit_flip_h,       203, icon_id::flip_h,       text.tooltip[14].c_str(), text.shortcut[14].c_str(), text.accessible[14].c_str()},
        {ui_command_id::edit_flip_v,       204, icon_id::flip_v,       text.tooltip[15].c_str(), text.shortcut[15].c_str(), text.accessible[15].c_str()},
        {ui_command_id::edit_crop,         207, icon_id::crop,         text.tooltip[16].c_str(), text.shortcut[16].c_str(), text.accessible[16].c_str()},
        {ui_command_id::edit_resize,       211, icon_id::resize,       text.tooltip[17].c_str(), text.shortcut[17].c_str(), text.accessible[17].c_str()},
        {ui_command_id::view_fit,          302, icon_id::fit,          text.tooltip[18].c_str(), text.shortcut[18].c_str(), text.accessible[18].c_str(), true},
        {ui_command_id::view_actual_size,  301, icon_id::one_to_one,   text.tooltip[19].c_str(), text.shortcut[19].c_str(), text.accessible[19].c_str(), true},
        {ui_command_id::view_zoom_in,      303, icon_id::zoom_in,      text.tooltip[20].c_str(), text.shortcut[20].c_str(), text.accessible[20].c_str()},
        {ui_command_id::view_zoom_out,     304, icon_id::zoom_out,     text.tooltip[21].c_str(), text.shortcut[21].c_str(), text.accessible[21].c_str()},
        {ui_command_id::nav_first,         307, icon_id::nav_first,    text.tooltip[22].c_str(), text.shortcut[22].c_str(), text.accessible[22].c_str()},
        {ui_command_id::nav_prev,          305, icon_id::nav_prev,     text.tooltip[23].c_str(), text.shortcut[23].c_str(), text.accessible[23].c_str()},
        {ui_command_id::nav_next,          306, icon_id::nav_next,     text.tooltip[24].c_str(), text.shortcut[24].c_str(), text.accessible[24].c_str()},
        {ui_command_id::nav_last,          308, icon_id::nav_last,     text.tooltip[25].c_str(), text.shortcut[25].c_str(), text.accessible[25].c_str()},
        {ui_command_id::view_fullscreen,   309, icon_id::fullscreen,   text.tooltip[26].c_str(), text.shortcut[26].c_str(), text.accessible[26].c_str(), true}
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

    // 1. Create native toolbar. TBSTYLE_WRAPABLE lets the standard surface
    // pack every command into at most two rows; the surface is horizontally
    // scrolled (via the sibling scrollbar below) whenever two rows cannot fit
    // in the available width, so no command is ever hidden.
    const std::wstring toolbar_name = viewer_string(IDS_TOOLBAR_NAME);
    hwnd_toolbar_ = CreateWindowExW(
        0,
        TOOLBARCLASSNAMEW,
        toolbar_name.c_str(),
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP | TBSTYLE_FLAT | TBSTYLE_WRAPABLE | TBSTYLE_TOOLTIPS | CCS_NORESIZE | CCS_NODIVIDER | CCS_NOPARENTALIGN,
        0, 0, 0, 0,
        parent_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kToolbarControlId)),
        instance,
        nullptr);

    if (!hwnd_toolbar_) {
        return false;
    }

    SendMessageW(hwnd_toolbar_, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);

    // 1b. Create the standard-surface horizontal scrollbar (hidden until two
    // rows cannot fit every command). It is a native, keyboard-reachable
    // WS_TABSTOP control so every off-screen command stays reachable.
    hwnd_bar_scroll_ = CreateWindowExW(
        0,
        WC_SCROLLBARW,
        L"",
        WS_CHILD | SBS_HORZ | WS_TABSTOP,
        0, 0, 0, 0,
        parent_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBarScrollControlId)),
        instance,
        nullptr);

    if (!hwnd_bar_scroll_) {
        destroy();
        return false;
    }

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
    if (hwnd_bar_scroll_) {
        DestroyWindow(hwnd_bar_scroll_);
        hwnd_bar_scroll_ = nullptr;
    }
    bar_scroll_pos_px_ = 0;
    bar_scroll_range_px_ = 0;
    bar_view_width_px_ = 0;
    bar_band_x_ = bar_band_y_ = bar_band_w_ = bar_band_h_ = 0;
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

void ui_chrome::apply_command_button_size() noexcept {
    if (!hwnd_toolbar_) return;
    // A fixed, DPI-aware square cell guarantees the 40x40 DIP minimum target
    // and keeps every command equally reachable; BTNS_AUTOSIZE is deliberately
    // not used so text fallback can never shrink a cell below the minimum.
    const int cell_px = dip_to_physical_round(kStdIconCellDip, dpi_);
    SendMessageW(hwnd_toolbar_, TB_SETBUTTONSIZE, 0, MAKELPARAM(cell_px, cell_px));
}

void ui_chrome::setup_toolbar_buttons() noexcept {
    if (!hwnd_toolbar_) return;

    const auto& descs = get_toolbar_button_descriptors();
    const auto& commands = get_std_icon_commands();

    std::vector<TBBUTTON> buttons;
    buttons.reserve(descs.size() + commands.size());

    // Descriptors and the std_icon_bar catalog share the same order, so walk
    // them in lock-step: a separator precedes each later group's first command
    // and never consumes an icon image-list index.
    bool first = true;
    for (std::size_t i = 0; i < descs.size(); ++i) {
        const bool group_start = (i < commands.size()) && commands[i].group_start;
        if (group_start && !first) {
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
        first = false;
    }

    SendMessageW(hwnd_toolbar_, TB_ADDBUTTONSW, static_cast<WPARAM>(buttons.size()), reinterpret_cast<LPARAM>(buttons.data()));
    SendMessageW(hwnd_toolbar_, TB_SETEXTENDEDSTYLE, 0, TBSTYLE_EX_DRAWDDARROWS);
    apply_command_button_size();
    // No TB_AUTOSIZE: update_layout() sizes the band explicitly so the two-row
    // bound and horizontal scroll range stay deterministic.
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
        apply_command_button_size();
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
        apply_command_button_size();
    };

    // If SVG is not supported or d2d_context is null, ensure buttons use fallback text
    if (!d2d_context || !icon_cache_.is_svg_supported(d2d_context)) {
        apply_text_fallback();
        return;
    }

    // SVG supported: render icons transactionally into a 32-bit ARGB HBITMAP ImageList
    const int icon_size_px = dip_to_physical_round(20.0f, dpi_);
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
    apply_command_button_size();
}

void ui_chrome::reposition_bar() noexcept {
    if (!hwnd_toolbar_ || bar_band_w_ <= 0) {
        return;
    }
    const int content_px = bar_band_w_ + bar_scroll_range_px_;
    const int toolbar_w = (std::max)(bar_band_w_, content_px);
    SetWindowPos(hwnd_toolbar_, nullptr,
                 bar_band_x_ - bar_scroll_pos_px_, bar_band_y_,
                 toolbar_w, bar_toolbar_height_px_,
                 SWP_NOZORDER | SWP_SHOWWINDOW);

    if (hwnd_bar_scroll_ && bar_scroll_range_px_ > 0) {
        SCROLLINFO si{sizeof(si)};
        si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        si.nMin = 0;
        si.nMax = content_px - 1;
        si.nPage = static_cast<UINT>(bar_band_w_);
        si.nPos = bar_scroll_pos_px_;
        SetScrollInfo(hwnd_bar_scroll_, SB_CTL, &si, TRUE);
    }
}

void ui_chrome::scroll_to_offset(int pos_px) noexcept {
    const int clamped = (std::max)(0, (std::min)(pos_px, bar_scroll_range_px_));
    if (clamped == bar_scroll_pos_px_) {
        return;
    }
    bar_scroll_pos_px_ = clamped;
    reposition_bar();
}

void ui_chrome::ensure_command_visible(int command_int_id) noexcept {
    if (!hwnd_toolbar_ || bar_scroll_range_px_ <= 0 || bar_view_width_px_ <= 0) {
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
    int pos = bar_scroll_pos_px_;
    if (item_rc.left < pos) {
        pos = item_rc.left;
    } else if (item_rc.right > pos + bar_view_width_px_) {
        pos = item_rc.right - bar_view_width_px_;
    }
    scroll_to_offset(pos);
}

void ui_chrome::update_layout(const ui_layout_result& layout, bool is_fullscreen) noexcept {
    if (is_fullscreen) {
        if (hwnd_toolbar_) ShowWindow(hwnd_toolbar_, SW_HIDE);
        if (hwnd_status_) ShowWindow(hwnd_status_, SW_HIDE);
        if (hwnd_bar_scroll_) ShowWindow(hwnd_bar_scroll_, SW_HIDE);
        return;
    }

    if (hwnd_toolbar_) {
        const auto& tb = layout.toolbar_physical;
        if (tb.height > 0 && tb.width > 0) {
            const float available_dip = physical_to_dip(tb.width, dpi_);
            const auto bar = compute_std_icon_bar_layout(available_dip);

            const int scrollbar_px = bar.needs_scroll
                                         ? dip_to_physical_round(kStdIconScrollbarDip, dpi_)
                                         : 0;
            const int toolbar_h = (std::max)(0, tb.height - scrollbar_px);
            const int content_px = dip_to_physical_round(bar.content_width_dip, dpi_);

            bar_band_x_ = tb.x;
            bar_band_y_ = tb.y;
            bar_band_w_ = tb.width;
            bar_band_h_ = tb.height;
            bar_view_width_px_ = tb.width;
            bar_toolbar_height_px_ = toolbar_h;
            bar_scroll_range_px_ = (std::max)(0, content_px - tb.width);
            bar_scroll_pos_px_ = (std::max)(0, (std::min)(bar_scroll_pos_px_, bar_scroll_range_px_));

            const int toolbar_w = (std::max)(tb.width, content_px);
            SetWindowPos(hwnd_toolbar_, nullptr,
                         tb.x - bar_scroll_pos_px_, tb.y,
                         toolbar_w, toolbar_h,
                         SWP_NOZORDER | SWP_SHOWWINDOW);

            if (hwnd_bar_scroll_) {
                if (bar_scroll_range_px_ > 0) {
                    SetWindowPos(hwnd_bar_scroll_, nullptr,
                                 tb.x, tb.y + tb.height - scrollbar_px,
                                 tb.width, scrollbar_px,
                                 SWP_NOZORDER | SWP_SHOWWINDOW);
                } else {
                    ShowWindow(hwnd_bar_scroll_, SW_HIDE);
                }
            }
            if (hwnd_bar_scroll_) {
                SCROLLINFO si{sizeof(si)};
                si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
                si.nMin = 0;
                si.nMax = (content_px > 0) ? (content_px - 1) : 0;
                si.nPage = static_cast<UINT>(tb.width);
                si.nPos = bar_scroll_pos_px_;
                SetScrollInfo(hwnd_bar_scroll_, SB_CTL, &si, TRUE);
            }
        } else {
            bar_band_x_ = bar_band_y_ = bar_band_w_ = bar_band_h_ = 0;
            bar_view_width_px_ = 0;
            bar_scroll_pos_px_ = 0;
            bar_scroll_range_px_ = 0;
            ShowWindow(hwnd_toolbar_, SW_HIDE);
            if (hwnd_bar_scroll_) ShowWindow(hwnd_bar_scroll_, SW_HIDE);
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

    // Keyboard/mouse focus moves across the standard surface: when horizontal
    // scrolling is active, bring the newly hot command fully into view so focus
    // styling is never clipped and no command becomes unreachable. No-ops when
    // the whole surface fits.
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
        const auto& descs = get_toolbar_button_descriptors();
        for (const auto& desc : descs) {
            if (desc.command_int_id == cmd_id) {
                // Bounded tooltip composition: inputs are concatenated, never
                // interpreted as a format string.
                static wchar_t s_tip[256];
                const std::wstring tip_in(desc.tooltip ? desc.tooltip : L"");
                const std::wstring shortcut_in(desc.shortcut ? desc.shortcut : L"");
                const std::wstring composed =
                    format_tooltip_with_shortcut(tip_in, shortcut_in);
                wcsncpy_s(s_tip, composed.c_str(), _TRUNCATE);
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

void ui_chrome::handle_hscroll(WPARAM wparam, LPARAM lparam) noexcept {
    // Only the standard-surface scrollbar is handled here; the main window
    // forwards every WM_HSCROLL, and other control scrollbars (none are
    // horizontal today) would be rejected by this ownership check.
    if (!hwnd_bar_scroll_ || reinterpret_cast<HWND>(lparam) != hwnd_bar_scroll_ ||
        bar_scroll_range_px_ <= 0) {
        return;
    }

    const int code = LOWORD(wparam);
    int pos = bar_scroll_pos_px_;
    switch (code) {
        case SB_LINELEFT:
            pos -= dip_to_physical_round(24.0f, dpi_);
            break;
        case SB_LINERIGHT:
            pos += dip_to_physical_round(24.0f, dpi_);
            break;
        case SB_PAGELEFT:
            pos -= bar_view_width_px_;
            break;
        case SB_PAGERIGHT:
            pos += bar_view_width_px_;
            break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: {
            SCROLLINFO si{sizeof(si)};
            si.fMask = SIF_TRACKPOS | SIF_POS;
            if (GetScrollInfo(hwnd_bar_scroll_, SB_CTL, &si)) {
                pos = (code == SB_THUMBTRACK) ? si.nTrackPos : si.nPos;
            }
            break;
        }
        case SB_LEFT:
            pos = 0;
            break;
        case SB_RIGHT:
            pos = bar_scroll_range_px_;
            break;
        default:
            return;
    }
    scroll_to_offset(pos);
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

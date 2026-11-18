// oz/viewer/ui_chrome.cpp - Single-row Lucide command bar, overflow menu, and clean status bar chrome.
#include "ui_chrome.hpp"

#include "viewer_strings.hpp"

#include <commctrl.h>

#include <algorithm>
#include <cwchar>

namespace oz::viewer {

namespace {

constexpr int kBandControlId = 3001;
constexpr int kStatusControlId = 3002;
constexpr int kNameLabelId = 3010;
constexpr int kNavLabelId = 3011;
constexpr int kZoomLabelId = 3012;

constexpr UINT kMsgRestoreFocus = WM_APP + 10;

constexpr const wchar_t* kBandClassName = L"PikaCommandBarBand";
constexpr const wchar_t* kStatusClassName = L"PikaCleanStatusBar";

constexpr std::size_t kToolbarButtonCount = 27;

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
            store.shortcut[5]  = viewer_string(IDS_TB_SHORTCUT_COPY_IMAGE);
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

WNDPROC g_button_orig_proc = nullptr;

std::wstring format_button_tip(const toolbar_button_desc& desc) {
    return format_tooltip_with_shortcut(
        desc.tooltip ? std::wstring(desc.tooltip) : std::wstring(),
        desc.shortcut ? std::wstring(desc.shortcut) : std::wstring());
}

std::wstring compact_zoom_text(const std::wstring& raw) {
    if (raw.empty()) {
        return L"--";
    }
    // Convert "Fit (68%)" -> "68%" so the command-bar zoom readout stays crisp and fixed-width.
    const std::size_t open_paren = raw.find(L'(');
    const std::size_t close_paren = raw.rfind(L')');
    if (open_paren != std::wstring::npos && close_paren != std::wstring::npos && close_paren > open_paren + 1) {
        return raw.substr(open_paren + 1, close_paren - open_paren - 1);
    }
    return raw;
}

}  // namespace

const std::vector<toolbar_button_desc>& get_toolbar_button_descriptors() noexcept {
    const toolbar_text_store& text = get_toolbar_text_store();
    static const std::vector<toolbar_button_desc> s_buttons = {
        {ui_command_id::file_open,          101, icon_id::open,          text.tooltip[0].c_str(),  text.shortcut[0].c_str(),  text.accessible[0].c_str()},
        {ui_command_id::file_paste_image,   111, icon_id::paste_image,   text.tooltip[1].c_str(),  text.shortcut[1].c_str(),  text.accessible[1].c_str()},
        {ui_command_id::file_save,          103, icon_id::save,          text.tooltip[2].c_str(),  text.shortcut[2].c_str(),  text.accessible[2].c_str()},
        {ui_command_id::file_save_as,       104, icon_id::save_as,       text.tooltip[3].c_str(),  text.shortcut[3].c_str(),  text.accessible[3].c_str()},
        {ui_command_id::file_open_location, 105, icon_id::open_location, text.tooltip[4].c_str(),  text.shortcut[4].c_str(),  text.accessible[4].c_str()},
        {ui_command_id::file_copy_image,    108, icon_id::copy_image,    text.tooltip[5].c_str(),  text.shortcut[5].c_str(),  text.accessible[5].c_str()},
        {ui_command_id::file_copy,          106, icon_id::copy_file,     text.tooltip[6].c_str(),  text.shortcut[6].c_str(),  text.accessible[6].c_str()},
        {ui_command_id::file_copy_path,     107, icon_id::copy_path,     text.tooltip[7].c_str(),  text.shortcut[7].c_str(),  text.accessible[7].c_str()},
        {ui_command_id::file_print,         110, icon_id::print,         text.tooltip[8].c_str(),  text.shortcut[8].c_str(),  text.accessible[8].c_str()},
        {ui_command_id::file_delete,        109, icon_id::delete_image,  text.tooltip[9].c_str(),  text.shortcut[9].c_str(),  text.accessible[9].c_str()},
        {ui_command_id::edit_undo,          205, icon_id::undo,          text.tooltip[10].c_str(), text.shortcut[10].c_str(), text.accessible[10].c_str()},
        {ui_command_id::edit_redo,          206, icon_id::redo,          text.tooltip[11].c_str(), text.shortcut[11].c_str(), text.accessible[11].c_str()},
        {ui_command_id::edit_rotate_left,   201, icon_id::rotate_left,   text.tooltip[12].c_str(), text.shortcut[12].c_str(), text.accessible[12].c_str()},
        {ui_command_id::edit_rotate_right,  202, icon_id::rotate_right,  text.tooltip[13].c_str(), text.shortcut[13].c_str(), text.accessible[13].c_str()},
        {ui_command_id::edit_flip_h,        203, icon_id::flip_h,        text.tooltip[14].c_str(), text.shortcut[14].c_str(), text.accessible[14].c_str()},
        {ui_command_id::edit_flip_v,        204, icon_id::flip_v,        text.tooltip[15].c_str(), text.shortcut[15].c_str(), text.accessible[15].c_str()},
        {ui_command_id::edit_crop,          207, icon_id::crop,          text.tooltip[16].c_str(), text.shortcut[16].c_str(), text.accessible[16].c_str(), true},
        {ui_command_id::edit_resize,        211, icon_id::resize,        text.tooltip[17].c_str(), text.shortcut[17].c_str(), text.accessible[17].c_str()},
        {ui_command_id::view_fit,           302, icon_id::fit,           text.tooltip[18].c_str(), text.shortcut[18].c_str(), text.accessible[18].c_str(), true},
        {ui_command_id::view_actual_size,   301, icon_id::one_to_one,    text.tooltip[19].c_str(), text.shortcut[19].c_str(), text.accessible[19].c_str(), true},
        {ui_command_id::view_zoom_in,       303, icon_id::zoom_in,       text.tooltip[20].c_str(), text.shortcut[20].c_str(), text.accessible[20].c_str()},
        {ui_command_id::view_zoom_out,      304, icon_id::zoom_out,      text.tooltip[21].c_str(), text.shortcut[21].c_str(), text.accessible[21].c_str()},
        {ui_command_id::nav_first,          307, icon_id::nav_first,     text.tooltip[22].c_str(), text.shortcut[22].c_str(), text.accessible[22].c_str()},
        {ui_command_id::nav_prev,           305, icon_id::nav_prev,      text.tooltip[23].c_str(), text.shortcut[23].c_str(), text.accessible[23].c_str()},
        {ui_command_id::nav_next,           306, icon_id::nav_next,      text.tooltip[24].c_str(), text.shortcut[24].c_str(), text.accessible[24].c_str()},
        {ui_command_id::nav_last,           308, icon_id::nav_last,      text.tooltip[25].c_str(), text.shortcut[25].c_str(), text.accessible[25].c_str()},
        {ui_command_id::view_fullscreen,    309, icon_id::fullscreen,    text.tooltip[26].c_str(), text.shortcut[26].c_str(), text.accessible[26].c_str(), true},
    };
    return s_buttons;
}

const toolbar_button_desc* find_toolbar_button_descriptor(int command_int_id) noexcept {
    const auto& descs = get_toolbar_button_descriptors();
    for (const auto& d : descs) {
        if (d.command_int_id == command_int_id) {
            return &d;
        }
    }
    return nullptr;
}

void ui_chrome::register_window_classes(HINSTANCE instance) noexcept {
    static bool s_registered = false;
    if (s_registered) {
        return;
    }
    WNDCLASSEXW band_wc{sizeof(band_wc)};
    band_wc.lpfnWndProc = &ui_chrome::band_wnd_proc;
    band_wc.hInstance = instance;
    band_wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    band_wc.lpszClassName = kBandClassName;
    RegisterClassExW(&band_wc);

    WNDCLASSEXW status_wc{sizeof(status_wc)};
    status_wc.lpfnWndProc = &ui_chrome::status_wnd_proc;
    status_wc.hInstance = instance;
    status_wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    status_wc.lpszClassName = kStatusClassName;
    RegisterClassExW(&status_wc);

    s_registered = true;
}

bool ui_chrome::create(HWND parent, HINSTANCE instance, float dpi) {
    destroy();
    parent_ = parent;
    dpi_ = (dpi > 0.0f) ? dpi : 96.0f;
    theme_mode_ = query_system_theme_mode();
    palette_ = get_theme_palette(theme_mode_);

    INITCOMMONCONTROLSEX icex{sizeof(INITCOMMONCONTROLSEX), ICC_BAR_CLASSES};
    InitCommonControlsEx(&icex);

    register_window_classes(instance);
    rebuild_fonts_and_brushes();

    const std::wstring toolbar_name = viewer_string(IDS_TOOLBAR_NAME);
    hwnd_band_ = CreateWindowExW(
        0,
        kBandClassName,
        toolbar_name.c_str(),
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, 0, 0,
        parent_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBandControlId)),
        instance,
        this);
    if (!hwnd_band_) {
        return false;
    }

    hwnd_status_ = CreateWindowExW(
        0,
        kStatusClassName,
        L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        0, 0, 0, 0,
        parent_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusControlId)),
        instance,
        this);
    if (!hwnd_status_) {
        destroy();
        return false;
    }

    create_band_controls(instance);
    create_overflow_menu();

    apply_dwm_caption_theme(parent_, theme_mode_);
    return true;
}

void ui_chrome::destroy() noexcept {
    destroy_overflow_menu();

    if (hwnd_tooltip_) {
        DestroyWindow(hwnd_tooltip_);
        hwnd_tooltip_ = nullptr;
    }
    if (hwnd_band_) {
        DestroyWindow(hwnd_band_);
        hwnd_band_ = nullptr;
    }
    if (hwnd_status_) {
        DestroyWindow(hwnd_status_);
        hwnd_status_ = nullptr;
    }

    btn_open_ = lbl_name_ = lbl_nav_ = nullptr;
    btn_prev_ = btn_next_ = nullptr;
    btn_zoom_out_ = lbl_zoom_ = btn_zoom_in_ = nullptr;
    btn_fit_ = btn_actual_ = nullptr;
    btn_rotate_l_ = btn_rotate_r_ = btn_crop_ = nullptr;
    btn_delete_ = btn_print_ = btn_overflow_ = nullptr;
    hover_btn_ = nullptr;

    if (font_ui_) {
        DeleteObject(font_ui_);
        font_ui_ = nullptr;
    }
    if (font_status_) {
        DeleteObject(font_status_);
        font_status_ = nullptr;
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
    bar_layout_ = cmd_bar_layout{};
    last_band_width_px_ = 0;
    parent_ = nullptr;
}

void ui_chrome::rebuild_fonts_and_brushes() noexcept {
    if (font_ui_) {
        DeleteObject(font_ui_);
        font_ui_ = nullptr;
    }
    if (font_status_) {
        DeleteObject(font_status_);
        font_status_ = nullptr;
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

    const int ui_h = dip_to_physical_round(13.0f, dpi_);
    font_ui_ = CreateFontW(
        -ui_h, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");

    const int status_h = dip_to_physical_round(12.0f, dpi_);
    font_status_ = CreateFontW(
        -status_h, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");

    if (font_ui_) {
        for (HWND lbl : {lbl_name_, lbl_nav_, lbl_zoom_}) {
            if (lbl) {
                SendMessageW(lbl, WM_SETFONT, reinterpret_cast<WPARAM>(font_ui_), TRUE);
            }
        }
    }
}

void ui_chrome::create_band_controls(HINSTANCE instance) noexcept {
    if (!hwnd_band_) return;

    const auto make_btn = [&](int cmd_id, const wchar_t* acc_name) -> HWND {
        HWND btn = CreateWindowExW(
            0, L"BUTTON", acc_name ? acc_name : L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0,
            hwnd_band_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(cmd_id)),
            instance, nullptr);
        if (btn) {
            SetWindowLongPtrW(btn, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
            WNDPROC orig = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
                btn, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&ui_chrome::button_subclass_proc)));
            if (g_button_orig_proc == nullptr) {
                g_button_orig_proc = orig;
            }
        }
        return btn;
    };

    const auto acc_for = [](int cmd_id) -> const wchar_t* {
        const auto* d = find_toolbar_button_descriptor(cmd_id);
        return (d && d->accessible_name) ? d->accessible_name : L"";
    };

    btn_open_ = make_btn(static_cast<int>(ui_command_id::file_open), acc_for(101));

    lbl_name_ = CreateWindowExW(
        0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS | SS_NOPREFIX,
        0, 0, 0, 0, hwnd_band_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kNameLabelId)),
        instance, nullptr);

    lbl_nav_ = CreateWindowExW(
        0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE | SS_NOPREFIX,
        0, 0, 0, 0, hwnd_band_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kNavLabelId)),
        instance, nullptr);

    btn_prev_ = make_btn(static_cast<int>(ui_command_id::nav_prev), acc_for(305));
    btn_next_ = make_btn(static_cast<int>(ui_command_id::nav_next), acc_for(306));
    btn_zoom_out_ = make_btn(static_cast<int>(ui_command_id::view_zoom_out), acc_for(304));

    lbl_zoom_ = CreateWindowExW(
        0, L"STATIC", L"--",
        WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE | SS_NOPREFIX,
        0, 0, 0, 0, hwnd_band_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kZoomLabelId)),
        instance, nullptr);

    btn_zoom_in_ = make_btn(static_cast<int>(ui_command_id::view_zoom_in), acc_for(303));
    btn_fit_ = make_btn(static_cast<int>(ui_command_id::view_fit), acc_for(302));
    btn_actual_ = make_btn(static_cast<int>(ui_command_id::view_actual_size), acc_for(301));

#if OZVIEWER_ENABLE_EDITING
    btn_rotate_l_ = make_btn(static_cast<int>(ui_command_id::edit_rotate_left), acc_for(201));
    btn_rotate_r_ = make_btn(static_cast<int>(ui_command_id::edit_rotate_right), acc_for(202));
    btn_crop_ = make_btn(static_cast<int>(ui_command_id::edit_crop), acc_for(207));
#endif

    btn_delete_ = make_btn(static_cast<int>(ui_command_id::file_delete), acc_for(109));
    btn_print_ = make_btn(static_cast<int>(ui_command_id::file_print), acc_for(110));
    btn_overflow_ = make_btn(menu::kIdOverflowMenu, L"More commands");

    if (font_ui_) {
        for (HWND lbl : {lbl_name_, lbl_nav_, lbl_zoom_}) {
            if (lbl) {
                SendMessageW(lbl, WM_SETFONT, reinterpret_cast<WPARAM>(font_ui_), TRUE);
            }
        }
    }

    hwnd_tooltip_ = CreateWindowExW(
        WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        hwnd_band_, nullptr, instance, nullptr);
    if (hwnd_tooltip_) {
        SendMessageW(hwnd_tooltip_, TTM_SETMAXTIPWIDTH, 0,
                     static_cast<LPARAM>(dip_to_physical_round(400.0f, dpi_)));
        const auto add_desc_tip = [&](HWND btn, int cmd_id) {
            if (!btn) return;
            if (const auto* d = find_toolbar_button_descriptor(cmd_id)) {
                const std::wstring tip = format_button_tip(*d);
                add_tooltip(btn, tip.c_str());
            }
        };
        add_desc_tip(btn_open_, 101);
        add_tooltip(lbl_name_, L"Image file name");
        add_tooltip(lbl_nav_, L"Image position in folder");
        add_desc_tip(btn_prev_, 305);
        add_desc_tip(btn_next_, 306);
        add_desc_tip(btn_zoom_out_, 304);
        add_tooltip(lbl_zoom_, L"Current zoom level");
        add_desc_tip(btn_zoom_in_, 303);
        add_desc_tip(btn_fit_, 302);
        add_desc_tip(btn_actual_, 301);
        add_desc_tip(btn_rotate_l_, 201);
        add_desc_tip(btn_rotate_r_, 202);
        add_desc_tip(btn_crop_, 207);
        add_desc_tip(btn_delete_, 109);
        add_desc_tip(btn_print_, 110);
        add_tooltip(btn_overflow_, L"More commands (F10 / Alt)");
    }
}

void ui_chrome::add_tooltip(HWND control, const wchar_t* text) noexcept {
    if (!hwnd_tooltip_ || !control || !text) {
        return;
    }
    TOOLINFOW tip{};
    tip.cbSize = sizeof(tip);
    tip.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    tip.hwnd = hwnd_band_;
    tip.uId = reinterpret_cast<UINT_PTR>(control);
    tip.lpszText = const_cast<LPWSTR>(text);
    SendMessageW(hwnd_tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tip));
}

void ui_chrome::update_tooltip_text(HWND control, const std::wstring& text) noexcept {
    if (!hwnd_tooltip_ || !control) {
        return;
    }
    TOOLINFOW tip{};
    tip.cbSize = sizeof(tip);
    tip.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    tip.hwnd = hwnd_band_;
    tip.uId = reinterpret_cast<UINT_PTR>(control);
    tip.lpszText = const_cast<LPWSTR>(text.c_str());
    SendMessageW(hwnd_tooltip_, TTM_SETTOOLINFOW, 0, reinterpret_cast<LPARAM>(&tip));
}

void ui_chrome::create_overflow_menu() noexcept {
    destroy_overflow_menu();

#if OZVIEWER_ENABLE_EDITING
    constexpr bool kEditing = true;
#else
    constexpr bool kEditing = false;
#endif

    copy_submenu_ = CreatePopupMenu();
    for (std::size_t i = 0; i < menu::kCopySubmenuCount; ++i) {
        const auto& it = menu::kCopySubmenuItems[i];
        AppendMenuW(copy_submenu_, MF_STRING, static_cast<UINT_PTR>(it.command), it.label);
    }

    if (kEditing) {
        crop_ratio_submenu_ = CreatePopupMenu();
        for (std::size_t i = 0; i < menu::kCropRatioSubmenuCount; ++i) {
            const auto& it = menu::kCropRatioSubmenuItems[i];
            AppendMenuW(crop_ratio_submenu_, MF_STRING, static_cast<UINT_PTR>(it.command), it.label);
        }

        edit_submenu_ = CreatePopupMenu();
        for (std::size_t i = 0; i < menu::kEditSubmenuCount; ++i) {
            const auto& it = menu::kEditSubmenuItems[i];
            if (it.command == menu::kMenuSeparator) {
                AppendMenuW(edit_submenu_, MF_SEPARATOR, 0, nullptr);
            } else if (it.command == menu::kMenuCropRatioPopup) {
                AppendMenuW(edit_submenu_, MF_POPUP, reinterpret_cast<UINT_PTR>(crop_ratio_submenu_), it.label);
            } else {
                AppendMenuW(edit_submenu_, MF_STRING, static_cast<UINT_PTR>(it.command), it.label);
            }
        }
    }

    overflow_menu_ = CreatePopupMenu();
    for (std::size_t i = 0; i < menu::kOverflowItemCount; ++i) {
        const auto& it = menu::kOverflowItems[i];
        if constexpr (!kEditing) {
            if (it.editing_only) {
                continue;
            }
        }
        if (it.command == menu::kMenuSeparator) {
            AppendMenuW(overflow_menu_, MF_SEPARATOR, 0, nullptr);
        } else if (it.command == menu::kMenuCopyPopup) {
            AppendMenuW(overflow_menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(copy_submenu_), it.label);
        } else if (it.command == menu::kMenuEditPopup) {
            if (edit_submenu_) {
                AppendMenuW(overflow_menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(edit_submenu_), it.label);
            }
        } else {
            AppendMenuW(overflow_menu_, MF_STRING, static_cast<UINT_PTR>(it.command), it.label);
        }
    }
}

void ui_chrome::destroy_overflow_menu() noexcept {
    if (overflow_menu_) {
        // DestroyMenu recursively destroys attached submenus.
        DestroyMenu(overflow_menu_);
        overflow_menu_ = nullptr;
        copy_submenu_ = nullptr;
        edit_submenu_ = nullptr;
        crop_ratio_submenu_ = nullptr;
    }
}

void ui_chrome::set_dpi(float dpi) noexcept {
    if (dpi <= 0.0f) return;
    dpi_ = dpi;
    rebuild_fonts_and_brushes();
    if (hwnd_tooltip_) {
        SendMessageW(hwnd_tooltip_, TTM_SETMAXTIPWIDTH, 0,
                     static_cast<LPARAM>(dip_to_physical_round(400.0f, dpi_)));
    }
    if (last_band_width_px_ > 0) {
        layout_band_controls(last_band_width_px_);
    }
    if (hwnd_band_) InvalidateRect(hwnd_band_, nullptr, TRUE);
    if (hwnd_status_) InvalidateRect(hwnd_status_, nullptr, TRUE);
}

void ui_chrome::set_theme(theme_mode mode) noexcept {
    theme_mode_ = mode;
    palette_ = get_theme_palette(theme_mode_);
    rebuild_fonts_and_brushes();
    icon_cache_.clear();

    if (parent_) {
        apply_dwm_caption_theme(parent_, theme_mode_);
    }
    if (hwnd_band_) {
        InvalidateRect(hwnd_band_, nullptr, TRUE);
        for (HWND child : {btn_open_, lbl_name_, lbl_nav_, btn_prev_, btn_next_,
                           btn_zoom_out_, lbl_zoom_, btn_zoom_in_, btn_fit_,
                           btn_actual_, btn_rotate_l_, btn_rotate_r_,
                           btn_crop_, btn_delete_, btn_print_, btn_overflow_}) {
            if (child) {
                InvalidateRect(child, nullptr, TRUE);
            }
        }
    }
    if (hwnd_status_) {
        InvalidateRect(hwnd_status_, nullptr, TRUE);
    }
}

void ui_chrome::update_icons(ID2D1DeviceContext* /*d2d_context*/) noexcept {
    if (hwnd_band_) {
        InvalidateRect(hwnd_band_, nullptr, TRUE);
    }
}

HWND ui_chrome::control_for_slot(cmd_bar_item item) const noexcept {
    switch (item) {
        case cmd_bar_item::open: return btn_open_;
        case cmd_bar_item::document_name: return lbl_name_;
        case cmd_bar_item::nav_counter: return lbl_nav_;
        case cmd_bar_item::nav_prev: return btn_prev_;
        case cmd_bar_item::nav_next: return btn_next_;
        case cmd_bar_item::zoom_out: return btn_zoom_out_;
        case cmd_bar_item::zoom_label: return lbl_zoom_;
        case cmd_bar_item::zoom_in: return btn_zoom_in_;
        case cmd_bar_item::view_fit: return btn_fit_;
        case cmd_bar_item::view_actual: return btn_actual_;
        case cmd_bar_item::rotate_left: return btn_rotate_l_;
        case cmd_bar_item::rotate_right: return btn_rotate_r_;
        case cmd_bar_item::edit_crop: return btn_crop_;
        case cmd_bar_item::file_delete: return btn_delete_;
        case cmd_bar_item::file_print: return btn_print_;
        case cmd_bar_item::overflow: return btn_overflow_;
    }
    return nullptr;
}

void ui_chrome::layout_band_controls(int band_width_px) noexcept {
    if (!hwnd_band_ || band_width_px <= 0) {
        return;
    }
    last_band_width_px_ = band_width_px;

    int name_text_w = 0;
    if (!status_info_.file_name.empty() && font_ui_) {
        HDC dc = GetDC(hwnd_band_);
        if (dc) {
            HGDIOBJ old_font = SelectObject(dc, font_ui_);
            SIZE ext{};
            if (GetTextExtentPoint32W(dc, status_info_.file_name.c_str(),
                                      static_cast<int>(status_info_.file_name.size()),
                                      &ext)) {
                name_text_w = ext.cx;
            }
            SelectObject(dc, old_font);
            ReleaseDC(hwnd_band_, dc);
        }
    }

    cmd_bar_input input{};
    input.client_width_px = band_width_px;
    input.dpi = static_cast<int>(std::lround(dpi_));
    input.nav_digits = (std::max)(1, status_info_.nav_digits);
    input.name_text_width_px = name_text_w;
    input.has_nav_counter = status_info_.has_nav_counter;
#if OZVIEWER_ENABLE_EDITING
    input.editing_enabled = true;
#else
    input.editing_enabled = false;
#endif
    bar_layout_ = compute_command_bar_layout(input);

    const int band_h = bar_layout_.height_px;
    const int btn_h = (std::min)(band_h - 4, scale_dip_round(kCommandBarButtonDip, input.dpi));
    const int btn_top = (band_h - btn_h) / 2;
    const int lbl_h = (std::min)(band_h - 4, scale_dip_round(kCommandBarLabelHeightDip, input.dpi));
    const int lbl_top = (band_h - lbl_h) / 2;

    const auto place = [&](HWND ctrl, cmd_bar_item kind, int h, int top) {
        if (!ctrl) return;
        const cmd_bar_slot* slot = bar_layout_.find(kind);
        if (!slot || !slot->visible || slot->width <= 0) {
            ShowWindow(ctrl, SW_HIDE);
            return;
        }
        SetWindowPos(ctrl, nullptr, slot->x, top, slot->width, h, SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(ctrl, SW_SHOW);
        InvalidateRect(ctrl, nullptr, TRUE);
    };

    place(btn_open_, cmd_bar_item::open, btn_h, btn_top);
    place(lbl_name_, cmd_bar_item::document_name, lbl_h, lbl_top);
    place(lbl_nav_, cmd_bar_item::nav_counter, lbl_h, lbl_top);
    place(btn_prev_, cmd_bar_item::nav_prev, btn_h, btn_top);
    place(btn_next_, cmd_bar_item::nav_next, btn_h, btn_top);
    place(btn_zoom_out_, cmd_bar_item::zoom_out, btn_h, btn_top);
    place(lbl_zoom_, cmd_bar_item::zoom_label, lbl_h, lbl_top);
    place(btn_zoom_in_, cmd_bar_item::zoom_in, btn_h, btn_top);
    place(btn_fit_, cmd_bar_item::view_fit, btn_h, btn_top);
    place(btn_actual_, cmd_bar_item::view_actual, btn_h, btn_top);
    place(btn_rotate_l_, cmd_bar_item::rotate_left, btn_h, btn_top);
    place(btn_rotate_r_, cmd_bar_item::rotate_right, btn_h, btn_top);
    place(btn_crop_, cmd_bar_item::edit_crop, btn_h, btn_top);
    place(btn_delete_, cmd_bar_item::file_delete, btn_h, btn_top);
    place(btn_print_, cmd_bar_item::file_print, btn_h, btn_top);
    place(btn_overflow_, cmd_bar_item::overflow, btn_h, btn_top);

    InvalidateRect(hwnd_band_, nullptr, TRUE);
}

void ui_chrome::update_layout(const ui_layout_result& layout, bool is_fullscreen) noexcept {
    if (is_fullscreen) {
        if (hwnd_band_) ShowWindow(hwnd_band_, SW_HIDE);
        if (hwnd_status_) ShowWindow(hwnd_status_, SW_HIDE);
        return;
    }

    if (hwnd_band_) {
        const auto& tb = layout.toolbar_physical;
        if (tb.height > 0 && tb.width > 0) {
            SetWindowPos(hwnd_band_, nullptr, tb.x, tb.y, tb.width, tb.height,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            layout_band_controls(tb.width);
        } else {
            ShowWindow(hwnd_band_, SW_HIDE);
        }
    }

    if (hwnd_status_) {
        const auto& sb = layout.status_physical;
        if (sb.height > 0 && sb.width > 0) {
            SetWindowPos(hwnd_status_, nullptr, sb.x, sb.y, sb.width, sb.height,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            InvalidateRect(hwnd_status_, nullptr, TRUE);
        } else {
            ShowWindow(hwnd_status_, SW_HIDE);
        }
    }
}

void ui_chrome::update_command_states(const ui_command_context& ctx) noexcept {
    last_ctx_ = ctx;

    const auto set_btn_enabled = [&](HWND btn, ui_command_id id) {
        if (!btn) return;
        const auto st = derive_command_state(id, ctx);
        EnableWindow(btn, st.enabled ? TRUE : FALSE);
        InvalidateRect(btn, nullptr, TRUE);
    };

    set_btn_enabled(btn_open_, ui_command_id::file_open);
    set_btn_enabled(btn_prev_, ui_command_id::nav_prev);
    set_btn_enabled(btn_next_, ui_command_id::nav_next);
    set_btn_enabled(btn_zoom_out_, ui_command_id::view_zoom_out);
    set_btn_enabled(btn_zoom_in_, ui_command_id::view_zoom_in);
    set_btn_enabled(btn_fit_, ui_command_id::view_fit);
    set_btn_enabled(btn_actual_, ui_command_id::view_actual_size);
    set_btn_enabled(btn_rotate_l_, ui_command_id::edit_rotate_left);
    set_btn_enabled(btn_rotate_r_, ui_command_id::edit_rotate_right);
    set_btn_enabled(btn_crop_, ui_command_id::edit_crop);
    set_btn_enabled(btn_delete_, ui_command_id::file_delete);
    set_btn_enabled(btn_print_, ui_command_id::file_print);
    if (btn_overflow_) {
        EnableWindow(btn_overflow_, ctx.benchmark_mode ? FALSE : TRUE);
    }

    // Mirror availability and checkmarks in the overflow popup menu and its submenus.
    const auto sync_menu = [&](HMENU hmenu, const menu::item* items, std::size_t count) {
        if (!hmenu) return;
        for (std::size_t i = 0; i < count; ++i) {
            const int cmd = items[i].command;
            if (cmd == menu::kMenuSeparator ||
                cmd == menu::kMenuCopyPopup ||
                cmd == menu::kMenuEditPopup ||
                cmd == menu::kMenuCropRatioPopup) {
                continue;
            }
            const auto st = derive_command_state(static_cast<ui_command_id>(cmd), ctx);
            EnableMenuItem(hmenu, static_cast<UINT>(cmd),
                           MF_BYCOMMAND | (st.enabled ? MF_ENABLED : MF_GRAYED));
            CheckMenuItem(hmenu, static_cast<UINT>(cmd),
                          MF_BYCOMMAND | (st.checked ? MF_CHECKED : MF_UNCHECKED));
        }
    };

    sync_menu(overflow_menu_, menu::kOverflowItems, menu::kOverflowItemCount);
    sync_menu(copy_submenu_, menu::kCopySubmenuItems, menu::kCopySubmenuCount);
    sync_menu(edit_submenu_, menu::kEditSubmenuItems, menu::kEditSubmenuCount);
    sync_menu(crop_ratio_submenu_, menu::kCropRatioSubmenuItems, menu::kCropRatioSubmenuCount);
}

void ui_chrome::update_status(const status_bar_info& info) noexcept {
    const bool name_changed = (status_info_.file_name != info.file_name) ||
                              (status_info_.full_path != info.full_path) ||
                              (status_info_.nav_digits != info.nav_digits) ||
                              (status_info_.has_nav_counter != info.has_nav_counter);
    status_info_ = info;

    const auto set_label_text = [](HWND ctrl, const std::wstring& text) {
        if (!ctrl) return;
        wchar_t buf[512]{};
        GetWindowTextW(ctrl, buf, static_cast<int>(std::size(buf)));
        if (std::wstring(buf) != text) {
            SetWindowTextW(ctrl, text.c_str());
            InvalidateRect(ctrl, nullptr, TRUE);
        }
    };

    set_label_text(lbl_name_, info.file_name);
    set_label_text(lbl_nav_, info.has_nav_counter ? info.navigation : L"");
    set_label_text(lbl_zoom_, compact_zoom_text(info.zoom));

    if (name_changed) {
        const std::wstring tip = !info.full_path.empty()
                                     ? info.full_path
                                     : (!info.file_name.empty() ? info.file_name : std::wstring(L"Image file name"));
        update_tooltip_text(lbl_name_, tip);
        if (last_band_width_px_ > 0) {
            layout_band_controls(last_band_width_px_);
        }
    }

    if (hwnd_status_) {
        InvalidateRect(hwnd_status_, nullptr, TRUE);
    }
}

void ui_chrome::show_overflow_menu() noexcept {
    if (!overflow_menu_ || !parent_) {
        return;
    }

    RECT anchor{};
    if (btn_overflow_ && IsWindowVisible(btn_overflow_)) {
        GetWindowRect(btn_overflow_, &anchor);
    } else if (hwnd_band_) {
        GetWindowRect(hwnd_band_, &anchor);
        anchor.left = anchor.right - 32;
    } else {
        GetWindowRect(parent_, &anchor);
        anchor.left = anchor.right - 32;
        anchor.bottom = anchor.top + 40;
    }

    RECT work_area{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
    const int row_h = (std::max)(1, GetSystemMetrics(SM_CYMENU));
    const int est_h = GetMenuItemCount(overflow_menu_) * row_h + 12;
    const int room_below = work_area.bottom - anchor.bottom;
    const int room_above = anchor.top - work_area.top;
    UINT flags = TPM_RIGHTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD;
    int y = anchor.bottom;
    if (est_h > room_below && room_above > room_below) {
        flags = TPM_RIGHTALIGN | TPM_BOTTOMALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD;
        y = anchor.top;
    }

    const UINT selected = TrackPopupMenu(overflow_menu_, flags, anchor.right, y, 0, parent_, nullptr);
    if (selected != 0) {
        SendMessageW(parent_, WM_COMMAND, MAKEWPARAM(selected, 0), 0);
    }
    if (IsWindow(parent_)) {
        SetFocus(parent_);
    }
}

std::vector<HWND> ui_chrome::visible_enabled_buttons() const noexcept {
    std::vector<HWND> chain;
    for (HWND btn : {btn_open_, btn_prev_, btn_next_, btn_zoom_out_, btn_zoom_in_,
                     btn_fit_, btn_actual_, btn_rotate_l_, btn_rotate_r_,
                     btn_crop_, btn_delete_, btn_print_, btn_overflow_}) {
        if (btn && IsWindowVisible(btn) && IsWindowEnabled(btn)) {
            chain.push_back(btn);
        }
    }
    return chain;
}

HWND ui_chrome::first_tab_stop() const noexcept {
    const auto chain = visible_enabled_buttons();
    return chain.empty() ? hwnd_band_ : chain.front();
}

bool ui_chrome::handle_key_navigation(UINT vk, bool shift) noexcept {
    const auto chain = visible_enabled_buttons();
    if (chain.empty()) {
        return false;
    }
    HWND focused = GetFocus();
    std::size_t idx = 0;
    bool found = false;
    for (std::size_t i = 0; i < chain.size(); ++i) {
        if (chain[i] == focused) {
            idx = i;
            found = true;
            break;
        }
    }
    if (!found) {
        SetFocus(chain.front());
        return true;
    }
    const bool backwards = (vk == VK_LEFT) || (vk == VK_TAB && shift);
    if (vk == VK_TAB) {
        if (backwards && idx == 0) {
            return false;  // Let caller advance out of command bar (or wrap cleanly).
        }
        if (!backwards && idx + 1 >= chain.size()) {
            return false;
        }
    }
    const std::size_t next = backwards
                                 ? (idx == 0 ? chain.size() - 1 : idx - 1)
                                 : ((idx + 1) % chain.size());
    SetFocus(chain[next]);
    return true;
}

LRESULT ui_chrome::handle_notify(LPARAM /*lparam*/) noexcept {
    return 0;
}

void ui_chrome::handle_hscroll(WPARAM /*wparam*/, LPARAM /*lparam*/) noexcept {
}

HBRUSH ui_chrome::handle_ctl_color(HDC hdc, HWND control) noexcept {
    if (control == hwnd_band_ || control == lbl_name_ || control == lbl_nav_ || control == lbl_zoom_) {
        SetBkColor(hdc, palette_.bg_chrome);
        const bool muted = (control == lbl_nav_ || control == lbl_zoom_);
        SetTextColor(hdc, muted ? palette_.text_muted : palette_.text_normal);
        return brush_chrome_bg_;
    }
    if (control == hwnd_status_) {
        SetBkColor(hdc, palette_.status_bg);
        SetTextColor(hdc, palette_.status_text);
        return brush_status_bg_;
    }
    return nullptr;
}

void ui_chrome::draw_band(HDC dc, const RECT& client) noexcept {
    if (!dc) return;
    FillRect(dc, &client, brush_chrome_bg_ ? brush_chrome_bg_ : GetSysColorBrush(COLOR_BTNFACE));

    const int band_h = client.bottom - client.top;
    const int margin_y = dip_to_physical_round(8.0f, dpi_);
    if (!bar_layout_.separators.empty() && band_h > margin_y * 2) {
        HPEN sep_pen = CreatePen(PS_SOLID, 1, palette_.separator);
        HGDIOBJ old_pen = SelectObject(dc, sep_pen);
        for (int x : bar_layout_.separators) {
            MoveToEx(dc, x, margin_y, nullptr);
            LineTo(dc, x, band_h - margin_y);
        }
        SelectObject(dc, old_pen);
        DeleteObject(sep_pen);
    }

    if (band_h > 1) {
        HPEN border_pen = CreatePen(PS_SOLID, 1, palette_.border);
        HGDIOBJ old_pen = SelectObject(dc, border_pen);
        MoveToEx(dc, client.left, client.bottom - 1, nullptr);
        LineTo(dc, client.right, client.bottom - 1);
        SelectObject(dc, old_pen);
        DeleteObject(border_pen);
    }
}

void ui_chrome::draw_status(HDC dc, const RECT& client) noexcept {
    if (!dc) return;
    FillRect(dc, &client, brush_status_bg_ ? brush_status_bg_ : GetSysColorBrush(COLOR_BTNFACE));

    HPEN border_pen = CreatePen(PS_SOLID, 1, palette_.border);
    HGDIOBJ old_pen = SelectObject(dc, border_pen);
    MoveToEx(dc, client.left, client.top, nullptr);
    LineTo(dc, client.right, client.top);
    SelectObject(dc, old_pen);
    DeleteObject(border_pen);

    // Minimal clean status text: dimensions + transient state (or empty-state hint).
    std::wstring left_text;
    if (!status_info_.dimensions.empty()) {
        left_text = status_info_.dimensions;
        if (!status_info_.state_label.empty()) {
            left_text += L"  \u2022  ";
            left_text += status_info_.state_label;
        }
    } else if (!status_info_.state_label.empty()) {
        left_text = status_info_.state_label;
    } else if (!status_info_.file_name.empty()) {
        left_text = status_info_.file_name;
    }

    if (!left_text.empty()) {
        const int pad_x = dip_to_physical_round(10.0f, dpi_);
        RECT text_rc{client.left + pad_x, client.top + 1, (std::max)(client.left + pad_x, client.right - pad_x), client.bottom};
        HFONT font = font_status_ ? font_status_ : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HGDIOBJ old_font = SelectObject(dc, font);
        SetTextColor(dc, palette_.status_text);
        SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, left_text.c_str(), static_cast<int>(left_text.size()),
                  &text_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, old_font);
    }
}

bool ui_chrome::is_button_checked(int command_int_id) const noexcept {
    if (command_int_id == static_cast<int>(ui_command_id::view_fit)) {
        return last_ctx_.have_image && last_ctx_.is_fit_mode;
    }
    if (command_int_id == static_cast<int>(ui_command_id::view_actual_size)) {
        return last_ctx_.have_image && !last_ctx_.is_fit_mode;
    }
    if (command_int_id == static_cast<int>(ui_command_id::edit_crop)) {
        return last_ctx_.crop_active;
    }
    return false;
}

void ui_chrome::draw_button(const DRAWITEMSTRUCT& item) noexcept {
    if (!item.hDC) return;
    const int cmd_id = static_cast<int>(item.CtlID);

    icon_id icon = icon_id::overflow;
    if (cmd_id == menu::kIdOverflowMenu) {
        icon = icon_id::overflow;
    } else if (const auto* desc = find_toolbar_button_descriptor(cmd_id)) {
        icon = desc->icon;
    } else {
        return;
    }

    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool hot = (item.itemState & ODS_HOTLIGHT) != 0 || (item.hwndItem == hover_btn_);
    const bool focused = (item.itemState & ODS_FOCUS) != 0;
    const bool checked = !disabled && is_button_checked(cmd_id);

    FillRect(item.hDC, &item.rcItem, brush_chrome_bg_ ? brush_chrome_bg_ : GetSysColorBrush(COLOR_BTNFACE));

    if (!disabled && (hot || pressed || checked)) {
        RECT plate = item.rcItem;
        InflateRect(&plate, -2, -2);
        const COLORREF plate_color = pressed
                                         ? palette_.btn_pressed
                                         : (checked && !hot ? palette_.btn_checked : palette_.btn_hover);
        HBRUSH plate_brush = CreateSolidBrush(plate_color);
        FillRect(item.hDC, &plate, plate_brush);
        DeleteObject(plate_brush);
    }

    const COLORREF glyph_color = disabled ? palette_.text_disabled : palette_.glyph_normal;
    const int btn_w = item.rcItem.right - item.rcItem.left;
    const int btn_h = item.rcItem.bottom - item.rcItem.top;
    const int glyph_px = (std::min)(dip_to_physical_round(static_cast<float>(kCommandBarGlyphDip), dpi_),
                                    (std::min)(btn_w, btn_h));
    RECT glyph_rc{};
    glyph_rc.left = item.rcItem.left + (btn_w - glyph_px) / 2;
    glyph_rc.top = item.rcItem.top + (btn_h - glyph_px) / 2;
    glyph_rc.right = glyph_rc.left + glyph_px;
    glyph_rc.bottom = glyph_rc.top + glyph_px;

    lucide::DrawIcon(item.hDC, glyph_rc, icon, glyph_color);

    if (focused) {
        RECT focus = item.rcItem;
        InflateRect(&focus, -2, -2);
        DrawFocusRect(item.hDC, &focus);
    }
}

void ui_chrome::restore_focus_after_click(HWND button) noexcept {
    if (!button || !parent_ || !IsWindow(parent_) || GetFocus() != button) {
        return;
    }
    SetFocus(parent_);
}

LRESULT CALLBACK ui_chrome::band_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<ui_chrome*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        self = reinterpret_cast<ui_chrome*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    if (!self) {
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            RECT client{};
            GetClientRect(hwnd, &client);
            self->draw_band(dc, client);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DRAWITEM: {
            if (lparam) {
                self->draw_button(*reinterpret_cast<const DRAWITEMSTRUCT*>(lparam));
                return TRUE;
            }
            break;
        }
        case WM_CTLCOLORSTATIC:
            if (HBRUSH br = self->handle_ctl_color(reinterpret_cast<HDC>(wparam), reinterpret_cast<HWND>(lparam))) {
                return reinterpret_cast<LRESULT>(br);
            }
            break;
        case WM_COMMAND: {
            const int id = LOWORD(wparam);
            if (id == menu::kIdOverflowMenu) {
                self->show_overflow_menu();
                return 0;
            }
            if (self->parent_) {
                SendMessageW(self->parent_, WM_COMMAND, wparam, lparam);
            }
            return 0;
        }
        case kMsgRestoreFocus:
            self->restore_focus_after_click(reinterpret_cast<HWND>(lparam));
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT CALLBACK ui_chrome::status_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<ui_chrome*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        self = reinterpret_cast<ui_chrome*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    if (!self) {
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            RECT client{};
            GetClientRect(hwnd, &client);
            self->draw_status(dc, client);
            EndPaint(hwnd, &ps);
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT CALLBACK ui_chrome::button_subclass_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<ui_chrome*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_MOUSEMOVE && self) {
        if (self->hover_btn_ != hwnd) {
            HWND prev = self->hover_btn_;
            self->hover_btn_ = hwnd;
            if (prev && IsWindow(prev)) {
                InvalidateRect(prev, nullptr, TRUE);
            }
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            InvalidateRect(hwnd, nullptr, TRUE);
        }
    } else if (msg == WM_MOUSELEAVE && self) {
        if (self->hover_btn_ == hwnd) {
            self->hover_btn_ = nullptr;
            InvalidateRect(hwnd, nullptr, TRUE);
        }
    } else if (msg == WM_LBUTTONUP) {
        const LRESULT lr = CallWindowProcW(g_button_orig_proc, hwnd, msg, wparam, lparam);
        if (GetDlgCtrlID(hwnd) != menu::kIdOverflowMenu) {
            PostMessageW(GetParent(hwnd), kMsgRestoreFocus, 0, reinterpret_cast<LPARAM>(hwnd));
        }
        return lr;
    } else if (msg == WM_KEYDOWN && self) {
        if (wparam == VK_LEFT || wparam == VK_RIGHT) {
            self->handle_key_navigation(static_cast<UINT>(wparam), false);
            return 0;
        }
        if (wparam == VK_PRIOR || wparam == VK_NEXT || wparam == VK_HOME ||
            wparam == VK_END || wparam == VK_UP || wparam == VK_DOWN) {
            if (self->parent_) {
                PostMessageW(self->parent_, WM_KEYDOWN, wparam, lparam);
            }
            return 0;
        }
    }
    return CallWindowProcW(g_button_orig_proc, hwnd, msg, wparam, lparam);
}

}  // namespace oz::viewer

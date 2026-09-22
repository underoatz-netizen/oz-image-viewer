// oz/viewer/resize_dialog.cpp - Native modal Win32 Resize dialog (Stage 5 Slice 5).
//
// Modal contract:
// - The dialog is created with DialogBoxIndirectParamW, so owner
//   disable/re-enable, Enter (default button), Esc (IDCANCEL) and Tab
//   navigation are owned by the system dialog manager. The dialog never posts
//   WM_QUIT and never runs a private application message loop.
// - Apply is two-phase: the dialog calls apply_cb(dialog, target) exactly once,
//   freezes its inputs, and stays responsive until
//   notify_resize_dialog_result() reports the matching worker validation
//   outcome. Only then does it close (success) or become usable again with a
//   bounded error (failure).
// - The canvas preview is presentation-only: preview_cb receives the draft
//   target or nullopt; the dialog never mutates edit history or image state.
#include "resize_dialog.hpp"
#include "ui_theme.hpp"

#include <windowsx.h>  // Button_SetCheck / Button_GetCheck

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <string>
#include <string_view>

namespace oz::viewer {

namespace {

using oz::viewer_core::edit_dimensions;

constexpr int kIdRadioPixels  = 1001;
constexpr int kIdRadioPercent = 1002;
constexpr int kIdEditWidth    = 1003;
constexpr int kIdEditHeight   = 1004;
constexpr int kIdEditPercent  = 1005;
constexpr int kIdCheckAspect  = 1006;
constexpr int kIdStaticOutput = 1007;
constexpr int kIdStaticStatus = 1008;
constexpr int kIdBtnReset     = 1009;

// UI-thread notification used by notify_resize_dialog_result():
// wparam != 0 means the validated command committed, lparam is a
// const std::wstring* bounded error message (ignored on success).
constexpr UINT kMsgResizeDialogResult = WM_APP + 4;

// Dialog layout in dialog units (mapped with MapDialogRect per control).
constexpr int kDlgWidthDlu  = 278;
constexpr int kDlgHeightDlu = 152;

struct dialog_state {
    HWND hwnd = nullptr;
    edit_dimensions initial{};
    resize_preview_fn preview_cb;
    resize_apply_fn apply_cb;

    HWND rb_pixels = nullptr;
    HWND rb_percent = nullptr;
    HWND edit_w = nullptr;
    HWND edit_h = nullptr;
    HWND edit_pct = nullptr;
    HWND chk_aspect = nullptr;
    HWND txt_output = nullptr;
    HWND txt_status = nullptr;
    HWND btn_apply = nullptr;
    HWND btn_cancel = nullptr;
    HWND btn_reset = nullptr;

    HFONT font = nullptr;
    bool font_owned = false;
    theme_mode theme = theme_mode::light;
    theme_palette palette{};
    HBRUSH brush_bg = nullptr;
    HBRUSH brush_edit_bg = nullptr;
    bool unit_is_percent = false;
    bool aspect_locked = true;
    // Most recently edited pixel axis: true = height drives width.
    bool last_axis_height = false;
    bool in_update = false;
    bool pending = false;
    bool show_error = false;
    bool accepted = false;
    std::optional<edit_dimensions> calculated;
};

RECT dlu_rect(HWND hwnd, int x, int y, int w, int h) noexcept {
    RECT rc{x, y, x + w, y + h};
    MapDialogRect(hwnd, &rc);
    return rc;
}

HWND create_control(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style,
                    int x, int y, int w, int h, int id) noexcept {
    const RECT rc = dlu_rect(parent, x, y, w, h);
    return CreateWindowExW(0, cls, text, style | WS_CHILD | WS_VISIBLE,
                           rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                           parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           GetModuleHandleW(nullptr), nullptr);
}

// Strict positive-integer parse for the pixel fields. The control restricts
// typing, but paste and IME input still have to be validated here: only an
// all-digit value in [1, kMaxEditDimension] is accepted.
bool parse_dimension_text(HWND edit, std::int32_t& out) noexcept {
    wchar_t buf[32]{};
    const int len = GetWindowTextW(edit, buf, static_cast<int>(std::size(buf)));
    if (len <= 0) {
        return false;
    }
    std::uint64_t value = 0;
    for (int i = 0; i < len; ++i) {
        const wchar_t ch = buf[i];
        if (ch < L'0' || ch > L'9') {
            return false;
        }
        value = value * 10 + static_cast<std::uint64_t>(ch - L'0');
        if (value > static_cast<std::uint64_t>(oz::viewer_core::kMaxEditDimension)) {
            return false;
        }
    }
    if (value == 0) {
        return false;
    }
    out = static_cast<std::int32_t>(value);
    return true;
}

// Strict finite positive percentage parse. Accepts an optional single decimal
// point with at least one digit (for example "37", "12.5", ".5", "200.") so the
// edit control permits exactly the numeric syntax this dialog understands.
bool parse_percent_text(HWND edit, double& out) noexcept {
    wchar_t buf[48]{};
    const int len = GetWindowTextW(edit, buf, static_cast<int>(std::size(buf)));
    if (len <= 0 || len >= static_cast<int>(std::size(buf))) {
        return false;
    }
    bool any_digit = false;
    bool any_dot = false;
    int digit_count = 0;
    for (int i = 0; i < len; ++i) {
        const wchar_t ch = buf[i];
        if (ch == L'.') {
            if (any_dot) {
                return false;
            }
            any_dot = true;
        } else if (ch >= L'0' && ch <= L'9') {
            any_digit = true;
            ++digit_count;
        } else {
            return false;
        }
    }
    if (!any_digit || digit_count > 12) {
        return false;
    }
    // The process never calls setlocale, so wcstod keeps the C-locale '.'
    // contract on the already validated text. Finiteness is checked on the
    // parsed value as well.
    wchar_t* end = nullptr;
    const double value = std::wcstod(buf, &end);
    if (end == buf || *end != L'\0' || !std::isfinite(value) || value <= 0.0) {
        return false;
    }
    out = value;
    return true;
}

void apply_derived_text(HWND edit, std::int32_t value) {
    wchar_t buf[16]{};
    swprintf_s(buf, L"%d", value);
    wchar_t current[16]{};
    GetWindowTextW(edit, current, static_cast<int>(std::size(current)));
    if (wcscmp(buf, current) != 0) {
        SetWindowTextW(edit, buf);
    }
}

void set_status_text(dialog_state& st, std::wstring_view text, bool is_error) {
    // Bounded, single-line status: never surface an unbounded worker message.
    constexpr std::size_t kMaxChars = 140;
    std::wstring bounded(text.substr(0, kMaxChars));
    for (auto& ch : bounded) {
        if (ch == L'\n' || ch == L'\r' || ch == L'\t') {
            ch = L' ';
        }
    }
    st.show_error = is_error;
    if (st.txt_status) {
        SetWindowTextW(st.txt_status, bounded.c_str());
        InvalidateRect(st.txt_status, nullptr, TRUE);
    }
}

// Freezes or restores every interactive control for the two-phase Apply.
// update_dialog_state() applies the mode-specific enables afterwards.
void set_pending_controls(dialog_state& st, bool pending) {
    st.pending = pending;
    for (HWND control : {st.rb_pixels, st.rb_percent, st.edit_w, st.edit_h, st.edit_pct,
                         st.chk_aspect, st.btn_reset}) {
        if (control) {
            EnableWindow(control, pending ? FALSE : TRUE);
        }
    }
    if (st.btn_apply) {
        EnableWindow(st.btn_apply, pending ? FALSE : TRUE);
    }
    if (st.btn_cancel) {
        EnableWindow(st.btn_cancel, TRUE);
    }
}

// Recomputes the output dimensions from the current controls and refreshes the
// dialog. `driver_ctrl_id` is the control that most recently changed (width,
// height, percent, or 0 for mode/reset/checkbox changes). When emit_preview is
// true the valid draft is forwarded to preview_cb; invalid input never alters
// the active draft preview.
void update_dialog_state(dialog_state& st, int driver_ctrl_id, bool emit_preview) {
    if (st.in_update) {
        return;
    }
    st.in_update = true;

    if (st.unit_is_percent) {
        // Uniform scaling: the aspect lock does not apply and is unchecked.
        EnableWindow(st.edit_w, FALSE);
        EnableWindow(st.edit_h, FALSE);
        EnableWindow(st.edit_pct, TRUE);
        EnableWindow(st.chk_aspect, FALSE);
        Button_SetCheck(st.chk_aspect, BST_UNCHECKED);
    } else {
        EnableWindow(st.edit_w, TRUE);
        EnableWindow(st.edit_h, TRUE);
        EnableWindow(st.edit_pct, FALSE);
        EnableWindow(st.chk_aspect, TRUE);
        Button_SetCheck(st.chk_aspect, st.aspect_locked ? BST_CHECKED : BST_UNCHECKED);
    }

    std::optional<edit_dimensions> result;

    if (st.unit_is_percent) {
        double percent = 0.0;
        if (parse_percent_text(st.edit_pct, percent)) {
            oz::viewer_core::resize_calculation_params params{};
            params.current_dims = st.initial;
            params.unit = oz::viewer_core::resize_unit::percent;
            params.maintain_aspect_ratio = true;
            params.requested_percent = percent;
            result = oz::viewer_core::calculate_resize_dimensions(params);
        }
    } else {
        if (driver_ctrl_id == kIdEditHeight) {
            st.last_axis_height = true;
        } else if (driver_ctrl_id == kIdEditWidth) {
            st.last_axis_height = false;
        }

        std::int32_t width = 0;
        std::int32_t height = 0;
        const bool width_ok = parse_dimension_text(st.edit_w, width);
        const bool height_ok = parse_dimension_text(st.edit_h, height);

        if (st.aspect_locked) {
            // The most recently edited axis drives the other through the shared
            // resize calculation so clamping and limits stay in one place.
            oz::viewer_core::resize_calculation_params params{};
            params.current_dims = st.initial;
            params.unit = oz::viewer_core::resize_unit::pixels;
            params.maintain_aspect_ratio = true;
            if (st.last_axis_height) {
                if (height_ok) {
                    params.requested_height = height;
                    result = oz::viewer_core::calculate_resize_dimensions(params);
                    if (result.has_value()) {
                        apply_derived_text(st.edit_w, result->width);
                    }
                }
            } else if (width_ok) {
                params.requested_width = width;
                result = oz::viewer_core::calculate_resize_dimensions(params);
                if (result.has_value()) {
                    apply_derived_text(st.edit_h, result->height);
                }
            }
        } else if (width_ok && height_ok) {
            oz::viewer_core::resize_calculation_params params{};
            params.current_dims = st.initial;
            params.unit = oz::viewer_core::resize_unit::pixels;
            params.maintain_aspect_ratio = false;
            params.requested_width = width;
            params.requested_height = height;
            result = oz::viewer_core::calculate_resize_dimensions(params);
        }
    }

    st.calculated = result;

    if (result.has_value()) {
        wchar_t out[64]{};
        swprintf_s(out, L"Output: %d \u00D7 %d", result->width, result->height);
        SetWindowTextW(st.txt_output, out);
        EnableWindow(st.btn_apply, st.pending ? FALSE : TRUE);
        if (emit_preview && st.preview_cb) {
            if (*result == st.initial) {
                st.preview_cb(std::nullopt);
            } else {
                st.preview_cb(*result);
            }
        }
    } else {
        SetWindowTextW(st.txt_output, L"Output: invalid");
        EnableWindow(st.btn_apply, FALSE);
    }

    st.in_update = false;
}

void reset_dialog_inputs(dialog_state& st) {
    st.in_update = true;
    st.unit_is_percent = false;
    st.aspect_locked = true;
    st.last_axis_height = false;
    Button_SetCheck(st.rb_pixels, BST_CHECKED);
    Button_SetCheck(st.rb_percent, BST_UNCHECKED);
    Button_SetCheck(st.chk_aspect, BST_CHECKED);
    wchar_t buf[16]{};
    swprintf_s(buf, L"%d", st.initial.width);
    SetWindowTextW(st.edit_w, buf);
    swprintf_s(buf, L"%d", st.initial.height);
    SetWindowTextW(st.edit_h, buf);
    SetWindowTextW(st.edit_pct, L"100");
    st.in_update = false;
    set_status_text(st, L"", false);
    // The opening dimensions are not a draft; the callback receives nullopt.
    update_dialog_state(st, 0, true);
}

void handle_apply(dialog_state& st) {
    if (st.pending || !st.calculated.has_value() || !st.calculated->is_valid()) {
        return;
    }
    const edit_dimensions target = *st.calculated;
    if (target == st.initial) {
        // No-op target: close without any history mutation.
        if (st.preview_cb) {
            st.preview_cb(std::nullopt);
        }
        st.accepted = true;
        EndDialog(st.hwnd, IDOK);
        return;
    }
    if (!st.apply_cb) {
        set_status_text(st, L"Resize is unavailable in this build.", true);
        return;
    }

    st.pending = true;
    set_status_text(st, L"Validating resize...", false);
    set_pending_controls(st, true);
    // The displayed draft must match the single in-flight validation target.
    if (st.preview_cb) {
        st.preview_cb(target);
    }
    st.apply_cb(st.hwnd, target);
}

INT_PTR CALLBACK resize_dialog_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    auto* st = reinterpret_cast<dialog_state*>(GetWindowLongPtrW(hwnd, DWLP_USER));

    switch (msg) {
        case WM_INITDIALOG: {
            st = reinterpret_cast<dialog_state*>(lparam);
            if (!st) {
                return FALSE;
            }
            st->hwnd = hwnd;
            SetWindowLongPtrW(hwnd, DWLP_USER, reinterpret_cast<LONG_PTR>(st));

            st->theme = query_system_theme_mode();
            st->palette = get_theme_palette(st->theme);
            apply_dwm_caption_theme(hwnd, st->theme);
            st->brush_bg = CreateSolidBrush(st->palette.bg_chrome);
            st->brush_edit_bg = CreateSolidBrush(st->theme == theme_mode::dark ? RGB(45, 45, 45) : RGB(255, 255, 255));

            NONCLIENTMETRICSW ncm{sizeof(ncm)};
            if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
                st->font = CreateFontIndirectW(&ncm.lfMessageFont);
                st->font_owned = (st->font != nullptr);
            }
            if (!st->font) {
                st->font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
                st->font_owned = false;
            }
            if (st->font) {
                SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(st->font), FALSE);
            }

            create_control(hwnd, L"BUTTON", L"Unit", BS_GROUPBOX,
                           7, 5, 193, 27, 0);
            st->rb_pixels = create_control(hwnd, L"BUTTON", L"&Pixels",
                                           BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP,
                                           13, 16, 52, 12, kIdRadioPixels);
            st->rb_percent = create_control(hwnd, L"BUTTON", L"Per&cent",
                                            BS_AUTORADIOBUTTON | WS_TABSTOP,
                                            72, 16, 64, 12, kIdRadioPercent);

            create_control(hwnd, L"BUTTON", L"Dimensions", BS_GROUPBOX,
                           7, 36, 193, 64, 0);
            create_control(hwnd, L"STATIC", L"&Width:", SS_RIGHT,
                           10, 47, 40, 10, 0);
            st->edit_w = create_control(hwnd, L"EDIT", L"",
                                        WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL | ES_LEFT,
                                        53, 45, 52, 13, kIdEditWidth);
            create_control(hwnd, L"STATIC", L"&Height:", SS_RIGHT,
                           109, 47, 32, 10, 0);
            st->edit_h = create_control(hwnd, L"EDIT", L"",
                                        WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL | ES_LEFT,
                                        144, 45, 52, 13, kIdEditHeight);
            create_control(hwnd, L"STATIC", L"Scale %:", SS_RIGHT,
                           10, 63, 40, 10, 0);
            // No ES_NUMBER here: the parser accepts fractional percentages.
            st->edit_pct = create_control(hwnd, L"EDIT", L"",
                                          WS_TABSTOP | ES_AUTOHSCROLL | ES_LEFT,
                                          53, 61, 52, 13, kIdEditPercent);
            st->chk_aspect = create_control(hwnd, L"BUTTON", L"Maintain &aspect ratio",
                                            BS_AUTOCHECKBOX | WS_TABSTOP,
                                            13, 80, 170, 11, kIdCheckAspect);

            st->txt_output = create_control(hwnd, L"STATIC", L"Output: -",
                                            SS_LEFT, 13, 104, 180, 10, kIdStaticOutput);
            st->txt_status = create_control(hwnd, L"STATIC", L"",
                                            SS_LEFT, 13, 118, 180, 20, kIdStaticStatus);

            st->btn_apply = create_control(hwnd, L"BUTTON", L"Apply",
                                           BS_DEFPUSHBUTTON | WS_TABSTOP,
                                           207, 12, 60, 14, IDOK);
            st->btn_cancel = create_control(hwnd, L"BUTTON", L"Cancel",
                                            BS_PUSHBUTTON | WS_TABSTOP,
                                            207, 30, 60, 14, IDCANCEL);
            st->btn_reset = create_control(hwnd, L"BUTTON", L"&Reset",
                                           BS_PUSHBUTTON | WS_TABSTOP,
                                           207, 48, 60, 14, kIdBtnReset);

            if (st->font) {
                for (HWND control : {st->rb_pixels, st->rb_percent, st->edit_w, st->edit_h,
                                     st->edit_pct, st->chk_aspect, st->txt_output,
                                     st->txt_status, st->btn_apply, st->btn_cancel,
                                     st->btn_reset}) {
                    if (control) {
                        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(st->font), TRUE);
                    }
                }
            }

            st->in_update = true;
            Button_SetCheck(st->rb_pixels, BST_CHECKED);
            Button_SetCheck(st->rb_percent, BST_UNCHECKED);
            Button_SetCheck(st->chk_aspect, BST_CHECKED);
            wchar_t buf[16]{};
            swprintf_s(buf, L"%d", st->initial.width);
            SetWindowTextW(st->edit_w, buf);
            swprintf_s(buf, L"%d", st->initial.height);
            SetWindowTextW(st->edit_h, buf);
            SetWindowTextW(st->edit_pct, L"100");
            st->in_update = false;

            // Initial display is the opening canvas; no draft is emitted.
            update_dialog_state(*st, 0, false);
            if (st->edit_w) {
                SetFocus(st->edit_w);
                SendMessageW(st->edit_w, EM_SETSEL, 0, -1);
            }
            return FALSE;  // focus already set explicitly
        }

        case WM_COMMAND: {
            if (!st) {
                return FALSE;
            }
            const int id = LOWORD(wparam);
            const int code = HIWORD(wparam);

            if (id == IDOK) {
                handle_apply(*st);
                return TRUE;
            }
            if (id == IDCANCEL) {
                // Cancel (button, Esc, or system close) clears the draft and
                // never mutates history, even while a validation is pending.
                if (st->preview_cb) {
                    st->preview_cb(std::nullopt);
                }
                EndDialog(hwnd, IDCANCEL);
                return TRUE;
            }
            if (st->pending) {
                // Only Cancel remains meaningful while one request is in flight.
                return TRUE;
            }
            if (id == kIdBtnReset) {
                reset_dialog_inputs(*st);
                return TRUE;
            }
            if (id == kIdRadioPixels && code == BN_CLICKED) {
                st->unit_is_percent = false;
                set_status_text(*st, L"", false);
                update_dialog_state(*st, 0, true);
                return TRUE;
            }
            if (id == kIdRadioPercent && code == BN_CLICKED) {
                st->unit_is_percent = true;
                set_status_text(*st, L"", false);
                update_dialog_state(*st, 0, true);
                return TRUE;
            }
            if (id == kIdCheckAspect && code == BN_CLICKED) {
                st->aspect_locked = (Button_GetCheck(st->chk_aspect) == BST_CHECKED);
                set_status_text(*st, L"", false);
                update_dialog_state(*st, 0, true);
                return TRUE;
            }
            if (code == EN_CHANGE &&
                (id == kIdEditWidth || id == kIdEditHeight || id == kIdEditPercent)) {
                set_status_text(*st, L"", false);
                update_dialog_state(*st, id, true);
                return TRUE;
            }
            break;
        }

        case kMsgResizeDialogResult: {
            if (!st || !st->pending) {
                return TRUE;
            }
            const bool ok = (wparam != 0);
            const auto* error = reinterpret_cast<const std::wstring*>(lparam);
            st->pending = false;
            if (ok) {
                st->accepted = true;
                EndDialog(hwnd, IDOK);
                return TRUE;
            }
            set_status_text(*st, error != nullptr && !error->empty()
                                     ? std::wstring_view(*error)
                                     : std::wstring_view(L"Resize validation failed."),
                            true);
            // The caller already reverted the canvas draft; restore the
            // controls and refresh output without re-emitting the failed
            // preview (the next input change creates a fresh draft).
            set_pending_controls(*st, false);
            update_dialog_state(*st, 0, false);
            // Restore focus resilience to the active primary edit control
            HWND focus_target = st->unit_is_percent ? st->edit_pct : st->edit_w;
            if (focus_target && IsWindow(focus_target) && IsWindowEnabled(focus_target)) {
                SetFocus(focus_target);
                SendMessageW(focus_target, EM_SETSEL, 0, -1);
            }
            return TRUE;
        }

        case WM_CTLCOLORDLG: {
            if (st != nullptr && st->brush_bg) {
                return reinterpret_cast<INT_PTR>(st->brush_bg);
            }
            return FALSE;
        }

        case WM_CTLCOLORSTATIC: {
            if (st != nullptr) {
                HDC dc = reinterpret_cast<HDC>(wparam);
                if (reinterpret_cast<HWND>(lparam) == st->txt_status && st->show_error) {
                    SetTextColor(dc, RGB(220, 50, 50));
                } else {
                    SetTextColor(dc, st->palette.text_normal);
                }
                SetBkColor(dc, st->palette.bg_chrome);
                return reinterpret_cast<INT_PTR>(st->brush_bg);
            }
            return FALSE;
        }

        case WM_CTLCOLORBTN: {
            if (st != nullptr && st->brush_bg) {
                return reinterpret_cast<INT_PTR>(st->brush_bg);
            }
            return FALSE;
        }

        case WM_CTLCOLOREDIT: {
            if (st != nullptr) {
                HDC dc = reinterpret_cast<HDC>(wparam);
                SetTextColor(dc, st->palette.text_normal);
                SetBkColor(dc, (st->theme == theme_mode::dark) ? RGB(45, 45, 45) : RGB(255, 255, 255));
                return reinterpret_cast<INT_PTR>(st->brush_edit_bg);
            }
            return FALSE;
        }

        case WM_CLOSE: {
            if (!st) {
                return FALSE;
            }
            if (st->preview_cb) {
                st->preview_cb(std::nullopt);
            }
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }

        case WM_DESTROY: {
            if (st != nullptr) {
                if (st->font_owned && st->font) {
                    DeleteObject(st->font);
                }
                st->font = nullptr;
                st->font_owned = false;
                if (st->brush_bg) {
                    DeleteObject(st->brush_bg);
                    st->brush_bg = nullptr;
                }
                if (st->brush_edit_bg) {
                    DeleteObject(st->brush_edit_bg);
                    st->brush_edit_bg = nullptr;
                }
                SetWindowLongPtrW(hwnd, DWLP_USER, 0);
            }
            // Deliberately no PostQuitMessage: this dialog must never end the
            // application message loop or post quit for the main window.
            return TRUE;
        }

        default:
            break;
    }
    return FALSE;
}

}  // namespace

resize_dialog_result show_resize_dialog(
    HWND parent,
    const oz::viewer_core::edit_dimensions& current_dims,
    resize_preview_fn preview_cb,
    resize_apply_fn apply_cb) {
    resize_dialog_result result{};
    if (!parent || !current_dims.is_valid()) {
        return result;
    }

    dialog_state st;
    st.initial = current_dims;
    st.preview_cb = std::move(preview_cb);
    st.apply_cb = std::move(apply_cb);

    // In-memory dialog template: frame only; child controls are created in
    // WM_INITDIALOG so layout uses MapDialogRect (DPI/font aware).
    struct {
        DLGTEMPLATE dlg;
        WORD menu;
        WORD window_class;
        WCHAR title[32];
    } tmpl{};
    tmpl.dlg.style = DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    tmpl.dlg.dwExtendedStyle = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
    tmpl.dlg.cdit = 0;
    tmpl.dlg.x = 0;
    tmpl.dlg.y = 0;
    tmpl.dlg.cx = static_cast<short>(kDlgWidthDlu);
    tmpl.dlg.cy = static_cast<short>(kDlgHeightDlu);
    tmpl.menu = 0;
    tmpl.window_class = 0;
    wcscpy_s(tmpl.title, L"Resize Image");

    const INT_PTR rc = DialogBoxIndirectParamW(
        GetModuleHandleW(nullptr),
        reinterpret_cast<LPCDLGTEMPLATEW>(&tmpl),
        parent,
        resize_dialog_proc,
        reinterpret_cast<LPARAM>(&st));
    if (rc == -1) {
        return result;
    }

    result.accepted = st.accepted;
    if (st.calculated.has_value()) {
        result.target_dimensions = *st.calculated;
    }
    return result;
}

void notify_resize_dialog_result(HWND dialog, bool ok, const std::wstring& error) {
    if (!dialog || !IsWindow(dialog)) {
        return;
    }
    SendMessageW(dialog, kMsgResizeDialogResult, ok ? 1u : 0u,
                 reinterpret_cast<LPARAM>(&error));
}

}  // namespace oz::viewer

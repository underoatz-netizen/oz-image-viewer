// oz/viewer/viewer_strings.cpp - English-only string accessor implementation (Stage 10.3).
#include "viewer_strings.hpp"

#include "ozviewer_resources.h"

#include <cmath>
#include <cstdio>
#include <cwchar>

namespace oz::viewer {

namespace {

// Compiled-in English fallback table. Must match the English STRINGTABLE in
// ozviewer.rc exactly; the stage7/new seam tests enforce parity for the
// seam-visible entries. Filter specs, extensions, class names, DLL names, and
// console diagnostics intentionally have no entries here (non-display).
struct fallback_entry {
    UINT id;
    const wchar_t* text;
};

// clang-format off
constexpr fallback_entry kFallbackTable[] = {
    {IDS_APP_TITLE, L"OZ Image Viewer"},
    {IDS_APP_TITLE_CROP, L"OZ Image Viewer \u2014 Crop (Enter: Apply, Esc: Cancel)"},
    {IDS_FALLBACK_IMAGE_NAME, L"Image"},
    {IDS_STATUS_NO_IMAGE, L"No image open"},
    {IDS_OPEN_BUTTON, L"Open Image..."},
    {IDS_TOOLBAR_NAME, L"OZ Viewer Toolbar"},
    {IDS_STATE_CROP_MODE, L"Crop mode"},
    {IDS_STATE_SAVING, L"Saving..."},
    {IDS_STATE_PRINTING, L"Printing..."},
    {IDS_STATE_RESIZING, L"Resizing..."},
    {IDS_STATE_MODIFIED, L"Modified"},
    {IDS_DIMENSIONS_FMT, L"%d \u00D7 %d"},
    {IDS_ZOOM_FIT_FMT, L"Fit (%.0f%%)"},
    {IDS_ZOOM_FMT, L"%.0f%%"},
    {IDS_NAV_FMT, L"%zu / %zu"},
    {IDS_OUTPUT_DIM_FMT, L"Output: %d \u00D7 %d"},
    {IDS_OUTPUT_INVALID, L"Output: invalid"},
    {IDS_OUTPUT_DASH, L"Output: -"},
    {IDS_CROP_DIM_EMPTY, L"-"},

    {IDS_TB_TIP_OPEN, L"Open"},
    {IDS_TB_TIP_SAVE, L"Save"},
    {IDS_TB_TIP_UNDO, L"Undo"},
    {IDS_TB_TIP_REDO, L"Redo"},
    {IDS_TB_TIP_ROTATE_LEFT, L"Rotate Left"},
    {IDS_TB_TIP_ROTATE_RIGHT, L"Rotate Right"},
    {IDS_TB_TIP_CROP, L"Crop"},
    {IDS_TB_TIP_RESIZE, L"Resize"},
    {IDS_TB_TIP_FIT, L"Fit to Window"},
    {IDS_TB_TIP_ACTUAL, L"Actual Size"},
    {IDS_TB_TIP_FULLSCREEN, L"Fullscreen"},

    {IDS_TB_SHORTCUT_OPEN, L"Ctrl+O"},
    {IDS_TB_SHORTCUT_SAVE, L"Ctrl+S"},
    {IDS_TB_SHORTCUT_UNDO, L"Ctrl+Z"},
    {IDS_TB_SHORTCUT_REDO, L"Ctrl+Y"},
    {IDS_TB_SHORTCUT_ROTATE_LEFT, L"Ctrl+L"},
    {IDS_TB_SHORTCUT_ROTATE_RIGHT, L"Ctrl+R"},
    {IDS_TB_SHORTCUT_CROP, L"C"},
    {IDS_TB_SHORTCUT_RESIZE, L"Ctrl+Alt+R"},
    {IDS_TB_SHORTCUT_FIT, L"F"},
    {IDS_TB_SHORTCUT_ACTUAL, L"1"},
    {IDS_TB_SHORTCUT_FULLSCREEN, L"F11"},

    {IDS_TB_ACC_OPEN, L"Open file"},
    {IDS_TB_ACC_SAVE, L"Save image"},
    {IDS_TB_ACC_UNDO, L"Undo edit"},
    {IDS_TB_ACC_REDO, L"Redo edit"},
    {IDS_TB_ACC_ROTATE_LEFT, L"Rotate left 90 degrees"},
    {IDS_TB_ACC_ROTATE_RIGHT, L"Rotate right 90 degrees"},
    {IDS_TB_ACC_CROP, L"Crop mode"},
    {IDS_TB_ACC_RESIZE, L"Resize image"},
    {IDS_TB_ACC_FIT, L"Fit image to window"},
    {IDS_TB_ACC_ACTUAL, L"Zoom 100 percent"},
    {IDS_TB_ACC_FULLSCREEN, L"Toggle fullscreen"},

    {IDS_MENU_FILE_OPEN, L"&Open...\tCtrl+O"},
    {IDS_MENU_FILE_OPEN_LOCATION, L"Open File &Location"},
    {IDS_MENU_FILE_COPY_IMAGE, L"Copy &Image\tCtrl+C"},
    {IDS_MENU_FILE_COPY_FILE, L"Copy &File"},
    {IDS_MENU_FILE_COPY_PATH, L"Copy File &Path"},
    {IDS_MENU_FILE_DELETE, L"&Delete\tDel"},
    {IDS_MENU_FILE_PRINT, L"&Print...\tCtrl+P"},
    {IDS_MENU_FILE_SAVE, L"&Save\tCtrl+S"},
    {IDS_MENU_FILE_SAVE_AS, L"Save &As...\tCtrl+Shift+S"},
    {IDS_MENU_FILE_POPUP, L"&File"},

    {IDS_MENU_EDIT_UNDO, L"&Undo\tCtrl+Z"},
    {IDS_MENU_EDIT_REDO, L"&Redo\tCtrl+Y"},
    {IDS_MENU_EDIT_ROTATE_LEFT, L"Rotate &Left\tCtrl+L"},
    {IDS_MENU_EDIT_ROTATE_RIGHT, L"Rotate &Right\tCtrl+R"},
    {IDS_MENU_EDIT_FLIP_H, L"Flip &Horizontal\tCtrl+Shift+H"},
    {IDS_MENU_EDIT_FLIP_V, L"Flip &Vertical\tCtrl+Shift+V"},
    {IDS_MENU_EDIT_CROP, L"&Crop...\tC"},
    {IDS_MENU_EDIT_RESIZE, L"&Resize...\tCtrl+Alt+R"},
    {IDS_MENU_EDIT_CROP_APPLY, L"Apply Crop\tEnter"},
    {IDS_MENU_EDIT_CROP_CANCEL, L"Cancel Crop\tEsc"},
    {IDS_MENU_EDIT_CROP_RESET, L"Reset Crop\tR"},
    {IDS_MENU_EDIT_RATIO_POPUP, L"Crop &Ratio"},
    {IDS_MENU_EDIT_POPUP, L"&Edit"},
    {IDS_MENU_RATIO_FREE, L"&Free"},
    {IDS_MENU_RATIO_ORIGINAL, L"&Original"},
    {IDS_MENU_RATIO_1X1, L"1:&1"},
    {IDS_MENU_RATIO_3X2, L"3:2"},
    {IDS_MENU_RATIO_4X3, L"4:3"},
    {IDS_MENU_RATIO_5X4, L"5:4"},
    {IDS_MENU_RATIO_16X9, L"16:9"},
    {IDS_MENU_RATIO_9X16, L"9:16"},
    {IDS_MENU_RATIO_ASERIES, L"A-series"},

    {IDS_CTX_FIT, L"Fit to Window\tF"},
    {IDS_CTX_ACTUAL, L"Actual Size\t1"},
    {IDS_CTX_COPY_IMAGE, L"Copy Image\tCtrl+C"},
    {IDS_CTX_COPY_FILE, L"Copy File"},
    {IDS_CTX_COPY_PATH, L"Copy File Path"},
    {IDS_CTX_OPEN_LOCATION, L"Open File Location"},
    {IDS_CTX_DELETE, L"Delete\tDel"},

    {IDS_CROP_ASPECT_LABEL, L"Aspect Ratio:"},
    {IDS_CROP_BTN_RESET, L"Reset (R)"},
    {IDS_CROP_BTN_APPLY, L"Apply (Enter)"},
    {IDS_CROP_BTN_CANCEL, L"Cancel (Esc)"},
    {IDS_CROP_RATIO_FREE, L"Free"},
    {IDS_CROP_RATIO_ORIGINAL, L"Original"},
    {IDS_CROP_RATIO_1X1, L"1:1 Square"},
    {IDS_CROP_RATIO_3X2, L"3:2"},
    {IDS_CROP_RATIO_4X3, L"4:3"},
    {IDS_CROP_RATIO_5X4, L"5:4"},
    {IDS_CROP_RATIO_16X9, L"16:9"},
    {IDS_CROP_RATIO_9X16, L"9:16"},
    {IDS_CROP_RATIO_ASERIES, L"A-series"},

    {IDS_RESIZE_UNIT, L"Unit"},
    {IDS_RESIZE_PIXELS, L"&Pixels"},
    {IDS_RESIZE_PERCENT, L"Per&cent"},
    {IDS_RESIZE_DIMENSIONS, L"Dimensions"},
    {IDS_RESIZE_WIDTH, L"&Width:"},
    {IDS_RESIZE_HEIGHT, L"&Height:"},
    {IDS_RESIZE_SCALE, L"Scale %:"},
    {IDS_RESIZE_ASPECT, L"Maintain &aspect ratio"},
    {IDS_RESIZE_APPLY, L"Apply"},
    {IDS_RESIZE_CANCEL, L"Cancel"},
    {IDS_RESIZE_RESET, L"&Reset"},
    {IDS_RESIZE_TITLE, L"Resize Image"},
    {IDS_RESIZE_UNAVAILABLE, L"Resize is unavailable in this build."},
    {IDS_RESIZE_VALIDATING, L"Validating resize..."},
    {IDS_RESIZE_FAILED_GENERIC, L"Resize validation failed."},

    {IDS_RESIZE_ERR_SUPERSEDED, L"Resize validation was superseded; no changes were applied."},
    {IDS_RESIZE_ERR_SOURCE_CHANGED, L"The source image changed during resize validation; no changes were applied."},
    {IDS_RESIZE_ERR_UNEXPECTED_DIMS, L"Resize validation produced unexpected dimensions; no changes were applied."},
    {IDS_RESIZE_ERR_PRESENT_FAILED, L"Resize could not be presented; history was left unchanged."},
    {IDS_RESIZE_ERR_SUBMIT_FAILED, L"Resize request could not be submitted."},

    {IDS_SAVE_ERR_GENERIC, L"The image could not be saved. The destination was left unchanged."},
    {IDS_SAVE_ERR_CANCELLED, L"The save was cancelled. No file was written."},
    {IDS_SAVE_ERR_INVALID, L"The save request was not valid. No file was written."},
    {IDS_SAVE_ERR_STALE, L"The source image changed while saving. No file was written."},
    {IDS_SAVE_ERR_CONFLICT, L"The destination file changed or appeared after the save began. No file was written."},
    {IDS_SAVE_ERR_MATERIAL, L"The edited image could not be prepared for saving. No file was written."},
    {IDS_SAVE_ERR_ALPHA, L"The selected format does not support transparency.\n\nUse File -> Save As and choose PNG, WebP, or TIFF to keep the alpha channel."},
    {IDS_SAVE_ERR_ENCODER_UNAVAILABLE, L"The requested image encoder is not available on this system. No file was written."},
    {IDS_SAVE_ERR_ENCODER_CONFIG, L"The image encoder rejected the requested format or quality. No file was written."},
    {IDS_SAVE_ERR_ENCODE, L"Encoding the image failed. No file was written."},
    {IDS_SAVE_ERR_TEMP, L"A temporary file could not be created next to the destination. No file was written."},
    {IDS_SAVE_ERR_FLUSH, L"The encoded file could not be flushed to disk. No file was written."},
    {IDS_SAVE_ERR_REPLACE, L"The encoded file could not be put in place. The destination was left unchanged."},
    {IDS_SAVE_ERR_UNCERTAIN, L"The save could not be confirmed.\n\nThe destination may have been partially replaced. The document remains modified; verify the file before continuing."},
    {IDS_SAVE_ERR_CLEANUP, L"The save failed and its temporary file could not be removed."},

    {IDS_SAVE_PROMPT_EXT_HEAD, L"Unsupported output extension."},
    {IDS_SAVE_PROMPT_EXT_BODY, L"Supported formats are PNG (.png), JPEG (.jpg/.jpeg), WebP (.webp), TIFF (.tif/.tiff), and BMP (.bmp)."},
    {IDS_SAVE_PROMPT_TIFF_HEAD, L"The selected destination is the current TIFF source file."},
    {IDS_SAVE_PROMPT_TIFF_BODY, L"TIFF is supported for Save As export copies only. Choose a different file name."},
    {IDS_SAVE_PROMPT_SAMESRC_HEAD, L"The selected destination is the current source file."},
    {IDS_SAVE_PROMPT_SAMESRC_BODY, L"Choose a different file name or keep the source format."},
    {IDS_SAVE_PROMPT_SRCGONE_HEAD, L"The source file no longer exists at its recorded location."},
    {IDS_SAVE_PROMPT_SRCGONE_BODY, L"Use File -> Save As to write a copy."},
    {IDS_SAVE_PROMPT_NOSOURCE_HEAD, L"No source file is available for this image."},
    {IDS_SAVE_PROMPT_NOSOURCE_BODY, L"The image must be opened from a file before it can be saved."},
    {IDS_SAVE_PROMPT_BUSY_HEAD, L"A save is still in progress."},
    {IDS_SAVE_PROMPT_BUSY_BODY, L"OK waits for the save to finish before continuing.\nCancel stops the save. If the replacement step has already begun it cannot be revoked."},
    {IDS_SAVE_PROMPT_TIMEOUT_HEAD, L"The save did not finish in time."},
    {IDS_SAVE_PROMPT_TIMEOUT_BODY, L"The requested action was not started."},
    {IDS_SAVE_PROMPT_UNVERIFIED_HEAD, L"The image was saved, but the committed file baseline could not be verified."},
    {IDS_SAVE_PROMPT_UNVERIFIED_BODY, L"The document remains modified."},
    {IDS_SAVE_PROMPT_DOCCANGED_HEAD, L"The image was saved, but the document changed during the save."},
    {IDS_SAVE_PROMPT_DOCCANGED_BODY, L"The document remains modified."},

    {IDS_DISCARD_HEAD_CROP, L"A crop selection is in progress."},
    {IDS_DISCARD_HEAD_DIRTY, L"You have unsaved edits in this document."},
    {IDS_DISCARD_BODY_CROP, L"Cancel crop and continue?"},
    {IDS_DISCARD_BODY_DIRTY, L"Discard edits and continue?"},

    {IDS_PRINT_ERR_INVALID, L"The print request was not valid. Nothing was printed."},
    {IDS_PRINT_ERR_ADMISSION, L"The image is too large to prepare for printing. Nothing was printed."},
    {IDS_PRINT_ERR_ACQUIRE, L"The source image could not be read for printing. Nothing was printed."},
    {IDS_PRINT_ERR_MATERIALIZE, L"The edited image could not be prepared for printing. Nothing was printed."},
    {IDS_PRINT_ERR_CAPS, L"The printer reported an unusable page size. Nothing was printed."},
    {IDS_PRINT_ERR_SPOOL, L"The printer did not accept the page. Check the printer queue; the document is unchanged."},
    {IDS_PRINT_TITLE, L"Print"},
    {IDS_PRINT_UNCHANGED, L"The document is unchanged."},

    {IDS_DELETE_TITLE, L"Delete"},
    {IDS_DELETE_HEAD_GENERIC, L"Unable to move the file to the Recycle Bin."},
    {IDS_DELETE_DETAIL_GENERIC, L"Nothing was deleted. The current image is retained."},
    {IDS_DELETE_HEAD_IDENTITY, L"The file changed on disk since it was opened."},
    {IDS_DELETE_HEAD_NOTFOUND, L"The file is no longer present on disk."},
    {IDS_DELETE_HEAD_NOTRECYCLABLE, L"The current document is not a recyclable file."},
    {IDS_DELETE_HEAD_NEXT_FAILED, L"The file was moved to the Recycle Bin, but the next image could not be opened."},
    {IDS_DELETE_DETAIL_NEXT_FAILED, L"The viewer is busy finishing previous work. The current view was cleared."},

    {IDS_DROP_HEAD_UNABLE, L"Unable to open the dropped items."},
    {IDS_DROP_HEAD_INVALID, L"One or more dropped items could not be read."},
    {IDS_DROP_DETAIL_INVALID, L"The drop was ignored. Nothing was opened."},
    {IDS_DROP_HEAD_MIXED, L"Files and folders cannot be dropped together."},
    {IDS_DROP_DETAIL_MIXED, L"Drop image files, or drop a single folder."},
    {IDS_DROP_HEAD_MULTIFOLDER, L"Only one folder can be dropped at a time."},
    {IDS_DROP_DETAIL_MULTIFOLDER, L"Drop a single folder, or drop image files."},
    {IDS_DROP_HEAD_OVERFLOW, L"Too many items were dropped."},
    {IDS_DROP_DETAIL_OVERFLOW, L"The drop exceeds the item limit and was ignored."},
    {IDS_DROP_HEAD_FOLDER_EMPTY, L"The folder contains no supported images."},
    {IDS_DROP_HEAD_FILES_NONE, L"None of the dropped files are supported images."},
    {IDS_DROP_DETAIL_NOTHING, L"Nothing was opened."},
    {IDS_DROP_HEAD_BUSY, L"The viewer is busy finishing previous work."},
    {IDS_DROP_DETAIL_BUSY, L"The drop was ignored. Try again in a moment."},

    {IDS_FILTER_ALL_SUPPORTED, L"All Supported Images"},
    {IDS_FILTER_JPEG_OPEN, L"JPEG Images (*.jpg;*.jpeg)"},
    {IDS_FILTER_PNG_OPEN, L"PNG Images (*.png)"},
    {IDS_FILTER_WEBP_OPEN, L"WebP Images (*.webp)"},
    {IDS_FILTER_AVIF_OPEN, L"AVIF Images (*.avif)"},
    {IDS_FILTER_HEIC_OPEN, L"HEIC Images (*.heic;*.heif)"},
    {IDS_FILTER_BMP_OPEN, L"BMP Images (*.bmp)"},
    {IDS_FILTER_GIF_OPEN, L"GIF Images (*.gif)"},
    {IDS_FILTER_TIFF_OPEN, L"TIFF Images (*.tif;*.tiff)"},
    {IDS_FILTER_ICO_OPEN, L"Icon Files (*.ico)"},
    {IDS_FILTER_CUR_OPEN, L"Cursor Files (*.cur)"},
    {IDS_FILTER_TGA_OPEN, L"TGA Images (*.tga)"},
    {IDS_FILTER_ALL_FILES, L"All Files (*.*)"},
    {IDS_OPEN_TITLE, L"Open Image"},
    {IDS_SAVE_FILTER_PNG, L"PNG (*.png)"},
    {IDS_SAVE_FILTER_JPEG, L"JPEG (*.jpg;*.jpeg)"},
    {IDS_SAVE_FILTER_WEBP, L"WebP (*.webp)"},
    {IDS_SAVE_FILTER_TIFF, L"TIFF (*.tif;*.tiff)"},
    {IDS_SAVE_FILTER_BMP, L"BMP (*.bmp)"},
    {IDS_SAVE_IMAGE_FILTER_PNG, L"PNG Image (*.png)"},
    {IDS_SAVE_IMAGE_FILTER_JPEG, L"JPEG Image (*.jpg;*.jpeg)"},
    {IDS_SAVE_IMAGE_FILTER_WEBP, L"WebP Image (*.webp)"},
    {IDS_SAVE_IMAGE_FILTER_TIFF, L"TIFF Image (*.tif;*.tiff)"},
    {IDS_SAVE_IMAGE_FILTER_BMP, L"BMP Image (*.bmp)"},
    {IDS_SAVE_TITLE, L"Save Image As"},

    {IDS_USAGE_BODY, L"usage: ozviewer.exe [image]\n       ozviewer.exe --benchmark <image>\n       ozviewer.exe --navigation-benchmark --corpus <dir> --out <file.jsonl> [options]\nSupported: JPEG (.jpg/.jpeg), PNG (.png), static WebP (.webp) when enabled,\nstatic AVIF (.avif) when enabled, static HEIC/HEIF (.heic/.heif) when enabled/loaded,\nPhase A: BMP (.bmp), static GIF (.gif), classic TIFF page 0 (.tif/.tiff), ICO (.ico),\nPhase B: CUR (.cur), TGA (.tga)."},

    {IDS_OPEN_LOCATION_TITLE, L"Open File Location"},
    {IDS_OPEN_LOCATION_ERR_HEAD, L"Unable to reveal file in Explorer."},
    {IDS_OPEN_LOCATION_ERR_BODY, L"The file may have been moved, renamed, or deleted."},
    {IDS_COPY_FILE_TITLE, L"Copy File"},
    {IDS_COPY_FILE_ERR_HEAD, L"Unable to copy file to the clipboard."},
    {IDS_COPY_FILE_ERR_BODY, L"The file may have been moved, renamed, deleted, or the clipboard could not be opened."},
    {IDS_COPY_PATH_TITLE, L"Copy File Path"},
    {IDS_COPY_PATH_ERR_HEAD, L"Unable to copy file path to the clipboard."},
    {IDS_COPY_PATH_ERR_BODY, L"The file may have been moved, renamed, deleted, or the clipboard could not be opened."},
    {IDS_COPY_IMAGE_TITLE, L"Copy Image"},
    {IDS_COPY_IMAGE_ERR_HEAD, L"Unable to copy image to the clipboard."},
    {IDS_COPY_IMAGE_ERR_BODY_DECODE, L"The source image could not be decoded or materialized."},
    {IDS_COPY_IMAGE_ERR_BODY_CLIPBOARD, L"The clipboard could not be opened or memory allocation failed."},
    {IDS_OPEN_IMAGE_ERR_HEAD, L"Unable to open image."},
    {IDS_OPEN_IMAGE_ERR_BODY, L"Unsupported or corrupted image format."},
    {IDS_DISPLAY_IMAGE_ERR_HEAD, L"Unable to display image."},
};
// clang-format on

const wchar_t* find_fallback(UINT id) noexcept {
    for (const auto& entry : kFallbackTable) {
        if (entry.id == id) {
            return entry.text;
        }
    }
    return nullptr;
}

bool load_from_module(UINT id, std::wstring& out) noexcept {
    const HMODULE module = GetModuleHandleW(nullptr);
    if (!module) {
        return false;
    }
    wchar_t buf[1024]{};
    const int len = LoadStringW(module, id, buf, static_cast<int>(std::size(buf)));
    if (len <= 0) {
        return false;
    }
    // LoadStringW truncates silently when the buffer is too small; our
    // longest English entry is well under 1024, so a full buffer means the
    // resource table disagrees with the approved text: treat as omission and
    // let the caller fall back deterministically.
    if (len >= static_cast<int>(std::size(buf)) - 1) {
        return false;
    }
    if (buf[0] == L'\0') {
        return false;
    }
    try {
        out.assign(buf, static_cast<std::size_t>(len));
    } catch (...) {
        return false;
    }
    return !out.empty();
}

// A template is well-formed for the int-pair formatters only when it carries
// exactly two %d conversions (no other conversions). Anything else is a
// malformed template: never format, return the failure text.
bool int_pair_template_valid(const std::wstring& tmpl) noexcept {
    if (tmpl.empty() || tmpl.size() > 128) {
        return false;
    }
    int conversions = 0;
    for (std::size_t i = 0; i < tmpl.size(); ++i) {
        if (tmpl[i] != L'%') {
            continue;
        }
        if (i + 1 >= tmpl.size() || tmpl[i + 1] != L'd') {
            return false;
        }
        ++conversions;
        ++i;
    }
    return conversions == 2;
}

bool nav_template_valid(const std::wstring& tmpl) noexcept {
    if (tmpl.empty() || tmpl.size() > 128) {
        return false;
    }
    // Expect exactly two conversions from the set this formatter supports:
    // %zu (MSVC), %llu, %lu, %u. Anything else is malformed.
    int conversions = 0;
    for (std::size_t i = 0; i < tmpl.size(); ++i) {
        if (tmpl[i] != L'%') {
            continue;
        }
        if (i + 2 < tmpl.size() && tmpl.substr(i, 3) == L"%zu") {
            ++conversions;
            i += 2;
        } else if (i + 3 < tmpl.size() && tmpl.substr(i, 4) == L"%llu") {
            ++conversions;
            i += 3;
        } else if (i + 2 < tmpl.size() && tmpl.substr(i, 3) == L"%lu") {
            ++conversions;
            i += 2;
        } else if (i + 1 < tmpl.size() && tmpl[i + 1] == L'u') {
            ++conversions;
            i += 1;
        } else {
            return false;
        }
    }
    return conversions == 2;
}

bool zoom_template_valid(const std::wstring& tmpl) noexcept {
    if (tmpl.empty() || tmpl.size() > 128) {
        return false;
    }
    // Exactly one % conversion and it must be a floating conversion.
    int conversions = 0;
    bool floating = false;
    for (std::size_t i = 0; i < tmpl.size(); ++i) {
        if (tmpl[i] != L'%') {
            continue;
        }
        if (i + 1 >= tmpl.size()) {
            return false;
        }
        if (tmpl[i + 1] == L'%') {
            ++i;  // literal %%
            continue;
        }
        ++conversions;
        // Scan the conversion spec for a floating-point conversion char.
        std::size_t j = i + 1;
        while (j < tmpl.size() && (tmpl[j] == L'.' || (tmpl[j] >= L'0' && tmpl[j] <= L'9'))) {
            ++j;
        }
        if (j < tmpl.size() && (tmpl[j] == L'f' || tmpl[j] == L'g' || tmpl[j] == L'e')) {
            floating = true;
        }
        // Skip to the conversion character.
        while (j < tmpl.size() && tmpl[j] != L'f' && tmpl[j] != L'g' && tmpl[j] != L'e' &&
               tmpl[j] != L'd' && tmpl[j] != L'u' && tmpl[j] != L's' && tmpl[j] != L'c' &&
               tmpl[j] != L'x' && tmpl[j] != L'X' && tmpl[j] != L'p' && tmpl[j] != L'%') {
            ++j;
        }
        i = j;
    }
    return conversions == 1 && floating;
}

std::wstring guard_failure(const wchar_t* failure) noexcept {
    try {
        if (failure && failure[0] != L'\0') {
            return std::wstring(failure);
        }
    } catch (...) {
    }
    return std::wstring(L"-");
}

}  // namespace

bool viewer_confirm_texts_available(std::initializer_list<UINT> ids) noexcept {
    try {
        for (const UINT id : ids) {
            if (viewer_string(id).empty()) {
                return false;
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool viewer_string_from_resources(UINT id, std::wstring& out) noexcept {
    try {
        out.clear();
        return load_from_module(id, out);
    } catch (...) {
        return false;
    }
}

std::wstring viewer_string_fallback(UINT id) noexcept {
    try {
        const wchar_t* text = find_fallback(id);
        if (!text) {
            return {};
        }
        return std::wstring(text);
    } catch (...) {
        return {};
    }
}

bool viewer_string_id_known(UINT id) noexcept {
    return find_fallback(id) != nullptr;
}

std::wstring viewer_string(UINT id) noexcept {
    try {
        std::wstring out;
        if (load_from_module(id, out)) {
            return out;
        }
        const wchar_t* fallback = find_fallback(id);
        if (!fallback) {
            return {};
        }
        return std::wstring(fallback);
    } catch (...) {
        try {
            const wchar_t* fallback = find_fallback(id);
            return fallback ? std::wstring(fallback) : std::wstring{};
        } catch (...) {
            return {};
        }
    }
}

std::wstring format_int_pair_template(const std::wstring& tmpl, int a, int b,
                                      const wchar_t* failure) noexcept {
    const std::wstring fail = guard_failure(failure);
    try {
        if (!int_pair_template_valid(tmpl)) {
            return fail;
        }
        wchar_t buf[128]{};
        const int rc = swprintf_s(buf, tmpl.c_str(), a, b);
        if (rc < 0) {
            return fail;
        }
        std::wstring out(buf, static_cast<std::size_t>(rc));
        if (out.empty()) {
            return fail;
        }
        return out;
    } catch (...) {
        return fail;
    }
}

std::wstring format_nav_template(const std::wstring& tmpl, std::size_t a, std::size_t b,
                                 const wchar_t* failure) noexcept {
    const std::wstring fail = guard_failure(failure);
    try {
        if (!nav_template_valid(tmpl)) {
            return fail;
        }
        wchar_t buf[128]{};
        const int rc = swprintf_s(buf, tmpl.c_str(), a, b);
        if (rc < 0) {
            return fail;
        }
        std::wstring out(buf, static_cast<std::size_t>(rc));
        if (out.empty()) {
            return fail;
        }
        return out;
    } catch (...) {
        return fail;
    }
}

std::wstring format_zoom_template(const std::wstring& tmpl, double percent,
                                  const wchar_t* failure) noexcept {
    const std::wstring fail = guard_failure(failure);
    try {
        if (!zoom_template_valid(tmpl) || !std::isfinite(percent)) {
            return fail;
        }
        if (percent < 0.0 || percent > 1000000.0) {
            return fail;
        }
        wchar_t buf[128]{};
        const int rc = swprintf_s(buf, tmpl.c_str(), percent);
        if (rc < 0) {
            return fail;
        }
        std::wstring out(buf, static_cast<std::size_t>(rc));
        if (out.empty()) {
            return fail;
        }
        return out;
    } catch (...) {
        return fail;
    }
}

std::wstring format_status_dimensions(int width, int height) noexcept {
    if (width <= 0 || height <= 0) {
        return viewer_string(IDS_CROP_DIM_EMPTY);
    }
    return format_int_pair_template(viewer_string(IDS_DIMENSIONS_FMT), width, height,
                                    L"-");
}

std::wstring format_crop_dimensions(int width, int height) noexcept {
    if (width <= 0 || height <= 0) {
        return viewer_string(IDS_CROP_DIM_EMPTY);
    }
    return format_int_pair_template(viewer_string(IDS_DIMENSIONS_FMT), width, height,
                                    L"-");
}

std::wstring format_output_dimensions(int width, int height) noexcept {
    const std::wstring failure = viewer_string(IDS_OUTPUT_INVALID);
    if (width <= 0 || height <= 0) {
        return failure.empty() ? std::wstring(L"Output: invalid") : failure;
    }
    const std::wstring tmpl = viewer_string(IDS_OUTPUT_DIM_FMT);
    const wchar_t* fail_cstr = failure.empty() ? L"Output: invalid" : failure.c_str();
    return format_int_pair_template(tmpl, width, height, fail_cstr);
}

std::wstring format_zoom_fit(double percent) noexcept {
    return format_zoom_template(viewer_string(IDS_ZOOM_FIT_FMT), percent, L"Fit");
}

std::wstring format_zoom_percent(double percent) noexcept {
    return format_zoom_template(viewer_string(IDS_ZOOM_FMT), percent, L"-");
}

std::wstring format_nav_position(std::size_t one_based, std::size_t total) noexcept {
    if (one_based == 0 || total == 0 || one_based > total) {
        return std::wstring(L"-");
    }
    return format_nav_template(viewer_string(IDS_NAV_FMT), one_based, total, L"-");
}

std::wstring format_tooltip_with_shortcut(const std::wstring& tip,
                                          const std::wstring& shortcut) noexcept {
    try {
        if (!shortcut.empty()) {
            std::wstring out(tip);
            out += L" (";
            out += shortcut;
            out += L")";
            if (!out.empty()) {
                return out;
            }
        }
        if (!tip.empty()) {
            return tip;
        }
        return std::wstring(L"-");
    } catch (...) {
        return std::wstring(L"-");
    }
}

}  // namespace oz::viewer

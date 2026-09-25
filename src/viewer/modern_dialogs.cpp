// oz/viewer/modern_dialogs.cpp - Native modern file and task dialog adapters implementation.
#include "modern_dialogs.hpp"

#include "viewer_strings.hpp"

#include <commctrl.h>
#include <commdlg.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>

namespace oz::viewer {

using Microsoft::WRL::ComPtr;

std::vector<open_filter_entry> get_supported_open_filters() noexcept {
    std::vector<open_filter_entry> filters;

    // Build the combined "All Supported Images" filter spec
    std::wstring all_spec = L"*.jpg;*.jpeg;*.png";
#if OZVIEWER_HAVE_WEBP
    all_spec += L";*.webp";
#endif
#if OZVIEWER_HAVE_AVIF
    all_spec += L";*.avif";
#endif
#if OZVIEWER_HAVE_HEIC
    all_spec += L";*.heic;*.heif";
#endif
    all_spec += L";*.bmp;*.gif;*.tif;*.tiff;*.ico;*.cur;*.tga";

    filters.push_back({viewer_string(IDS_FILTER_ALL_SUPPORTED), all_spec});

    // Individual format categories (display names are localized presentation;
    // specs/patterns stay fixed non-display literals).
    filters.push_back({viewer_string(IDS_FILTER_JPEG_OPEN), L"*.jpg;*.jpeg"});
    filters.push_back({viewer_string(IDS_FILTER_PNG_OPEN), L"*.png"});
#if OZVIEWER_HAVE_WEBP
    filters.push_back({viewer_string(IDS_FILTER_WEBP_OPEN), L"*.webp"});
#endif
#if OZVIEWER_HAVE_AVIF
    filters.push_back({viewer_string(IDS_FILTER_AVIF_OPEN), L"*.avif"});
#endif
#if OZVIEWER_HAVE_HEIC
    filters.push_back({viewer_string(IDS_FILTER_HEIC_OPEN), L"*.heic;*.heif"});
#endif
    filters.push_back({viewer_string(IDS_FILTER_BMP_OPEN), L"*.bmp"});
    filters.push_back({viewer_string(IDS_FILTER_GIF_OPEN), L"*.gif"});
    filters.push_back({viewer_string(IDS_FILTER_TIFF_OPEN), L"*.tif;*.tiff"});
    filters.push_back({viewer_string(IDS_FILTER_ICO_OPEN), L"*.ico"});
    filters.push_back({viewer_string(IDS_FILTER_CUR_OPEN), L"*.cur"});
    filters.push_back({viewer_string(IDS_FILTER_TGA_OPEN), L"*.tga"});
    filters.push_back({viewer_string(IDS_FILTER_ALL_FILES), L"*.*"});

    return filters;
}

std::vector<wchar_t> build_legacy_open_filter_string(
    const std::vector<open_filter_entry>& filters) noexcept {
    std::vector<wchar_t> out;
    for (const auto& entry : filters) {
        out.insert(out.end(), entry.display_name.begin(), entry.display_name.end());
        out.push_back(L'\0');
        out.insert(out.end(), entry.spec.begin(), entry.spec.end());
        out.push_back(L'\0');
    }
    out.push_back(L'\0');
    return out;
}

namespace {

// RAII helper to ensure COM is initialized on the current UI thread for dialog operations
class com_scope {
public:
    com_scope() {
        hr_ = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    }
    ~com_scope() {
        if (SUCCEEDED(hr_)) {
            CoUninitialize();
        }
    }
    [[nodiscard]] bool ok() const noexcept {
        return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE;
    }
private:
    HRESULT hr_ = E_FAIL;
};

// Fallback legacy GetOpenFileNameW implementation
std::optional<std::filesystem::path> show_legacy_open_dialog(HWND parent) {
    const auto filters = get_supported_open_filters();
    const auto filter_str = build_legacy_open_filter_string(filters);

    wchar_t filename[32768]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = parent;
    ofn.lpstrFilter = filter_str.data();
    ofn.lpstrFile = filename;
    ofn.nMaxFile = static_cast<DWORD>(std::size(filename));
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT | OFN_EXPLORER;

    if (GetOpenFileNameW(&ofn)) {
        return std::filesystem::path(filename);
    }
    return std::nullopt;
}

// Fallback legacy GetSaveFileNameW implementation
save_as_dialog_result show_legacy_save_dialog(
    HWND parent,
    const std::filesystem::path& default_path,
    bool webp_encoder_available) {
#if !OZVIEWER_ENABLE_EDITING
    // Save is an Editing-ON feature; decline without linking the
    // save-pipeline extension helpers into Editing-OFF builds.
    (void)parent;
    (void)default_path;
    (void)webp_encoder_available;
    return save_as_dialog_result{};
#else
    save_as_dialog_result result{};

    wchar_t filename[32768]{};
    if (!default_path.empty()) {
        const std::wstring stem = default_path.stem().wstring();
        wcscpy_s(filename, stem.c_str());
    }

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = parent;

    std::vector<wchar_t> filter;
    std::vector<oz::viewer_core::save_format> filter_formats;
    const auto append_filter = [&](const wchar_t* display, const wchar_t* spec) {
        while (*display) filter.push_back(*display++);
        filter.push_back(L'\0');
        while (*spec) filter.push_back(*spec++);
        filter.push_back(L'\0');
    };

    // Owning locals: append_filter copies each display/spec pair
    // synchronously into the double-null-terminated buffer.
    const std::wstring legacy_png = viewer_string(IDS_SAVE_FILTER_PNG);
    const std::wstring legacy_jpeg = viewer_string(IDS_SAVE_FILTER_JPEG);
    const std::wstring legacy_webp = viewer_string(IDS_SAVE_FILTER_WEBP);
    const std::wstring legacy_tiff = viewer_string(IDS_SAVE_FILTER_TIFF);
    const std::wstring legacy_bmp = viewer_string(IDS_SAVE_FILTER_BMP);

    append_filter(legacy_png.c_str(), L"*.png");
    filter_formats.push_back(oz::viewer_core::save_format::png);
    append_filter(legacy_jpeg.c_str(), L"*.jpg;*.jpeg");
    filter_formats.push_back(oz::viewer_core::save_format::jpeg);
    if (webp_encoder_available) {
        append_filter(legacy_webp.c_str(), L"*.webp");
        filter_formats.push_back(oz::viewer_core::save_format::webp);
    }
    append_filter(legacy_tiff.c_str(), L"*.tif;*.tiff");
    filter_formats.push_back(oz::viewer_core::save_format::tiff);
    append_filter(legacy_bmp.c_str(), L"*.bmp");
    filter_formats.push_back(oz::viewer_core::save_format::bmp);
    filter.push_back(L'\0');

    ofn.lpstrFilter = filter.data();
    ofn.lpstrFile = filename;
    ofn.nMaxFile = static_cast<DWORD>(std::size(filename));
    ofn.nFilterIndex = 1;  // PNG default
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT | OFN_EXPLORER;

    if (!GetSaveFileNameW(&ofn)) {
        return result;  // Cancelled
    }

    result.accepted = true;
    result.destination = std::filesystem::path(filename);
    const std::wstring ext = result.destination.extension().wstring();
    if (!ext.empty()) {
        result.explicit_extension = true;
        const auto fmt = oz::viewer_core::save_format_for_extension(ext);
        if (fmt.has_value()) {
            result.format = *fmt;
        }
    } else {
        result.explicit_extension = false;
        const DWORD idx = (ofn.nFilterIndex > 0 && ofn.nFilterIndex <= filter_formats.size())
                              ? ofn.nFilterIndex - 1
                              : 0;
        result.format = filter_formats[idx];
        result.destination += oz::viewer_core::preferred_extension(result.format);
    }
    return result;
#endif
}

}  // namespace

std::optional<std::filesystem::path> show_open_image_dialog(HWND parent) {
    com_scope com;
    if (!com.ok()) {
        return show_legacy_open_dialog(parent);
    }

    ComPtr<IFileOpenDialog> dialog;
    HRESULT hr = CoCreateInstance(
        CLSID_FileOpenDialog,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dialog));

    if (FAILED(hr) || !dialog) {
        return show_legacy_open_dialog(parent);
    }

    FILEOPENDIALOGOPTIONS options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT);
    }

    const auto filters = get_supported_open_filters();
    std::vector<COMDLG_FILTERSPEC> specs;
    specs.reserve(filters.size());
    for (const auto& f : filters) {
        specs.push_back({f.display_name.c_str(), f.spec.c_str()});
    }

    dialog->SetFileTypes(static_cast<UINT>(specs.size()), specs.data());
    dialog->SetFileTypeIndex(1);
    const std::wstring open_title = viewer_string(IDS_OPEN_TITLE);
    dialog->SetTitle(open_title.c_str());

    hr = dialog->Show(parent);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        return std::nullopt;
    }
    if (FAILED(hr)) {
        return show_legacy_open_dialog(parent);
    }

    ComPtr<IShellItem> item;
    hr = dialog->GetResult(&item);
    if (FAILED(hr) || !item) {
        return std::nullopt;
    }

    PWSTR path_buf = nullptr;
    hr = item->GetDisplayName(SIGDN_FILESYSPATH, &path_buf);
    if (FAILED(hr) || !path_buf) {
        return std::nullopt;
    }

    std::filesystem::path result(path_buf);
    CoTaskMemFree(path_buf);
    return result;
}

save_as_dialog_result show_save_as_dialog(
    HWND parent,
    const std::filesystem::path& default_path,
    oz::viewer_core::codec_id source_codec,
    bool webp_encoder_available) {
#if !OZVIEWER_ENABLE_EDITING
    // Save is an Editing-ON feature; decline without linking the
    // save-pipeline extension helpers into Editing-OFF builds.
    (void)parent;
    (void)default_path;
    (void)source_codec;
    (void)webp_encoder_available;
    return save_as_dialog_result{};
#else
    (void)source_codec;

    com_scope com;
    if (!com.ok()) {
        return show_legacy_save_dialog(parent, default_path, webp_encoder_available);
    }

    ComPtr<IFileSaveDialog> dialog;
    HRESULT hr = CoCreateInstance(
        CLSID_FileSaveDialog,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dialog));

    if (FAILED(hr) || !dialog) {
        return show_legacy_save_dialog(parent, default_path, webp_encoder_available);
    }

    FILEOPENDIALOGOPTIONS options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT);
    }

    std::vector<COMDLG_FILTERSPEC> filters;
    std::vector<oz::viewer_core::save_format> filter_formats;

    // Display names are localized presentation; specs stay fixed literals.
    // Owning locals: COMDLG_FILTERSPEC only borrows the pointers for the
    // synchronous SetFileTypes call below.
    const std::wstring save_png = viewer_string(IDS_SAVE_IMAGE_FILTER_PNG);
    const std::wstring save_jpeg = viewer_string(IDS_SAVE_IMAGE_FILTER_JPEG);
    const std::wstring save_webp = viewer_string(IDS_SAVE_IMAGE_FILTER_WEBP);
    const std::wstring save_tiff = viewer_string(IDS_SAVE_IMAGE_FILTER_TIFF);
    const std::wstring save_bmp = viewer_string(IDS_SAVE_IMAGE_FILTER_BMP);

    filters.push_back({save_png.c_str(), L"*.png"});
    filter_formats.push_back(oz::viewer_core::save_format::png);

    filters.push_back({save_jpeg.c_str(), L"*.jpg;*.jpeg"});
    filter_formats.push_back(oz::viewer_core::save_format::jpeg);

    if (webp_encoder_available) {
        filters.push_back({save_webp.c_str(), L"*.webp"});
        filter_formats.push_back(oz::viewer_core::save_format::webp);
    }

    filters.push_back({save_tiff.c_str(), L"*.tif;*.tiff"});
    filter_formats.push_back(oz::viewer_core::save_format::tiff);

    filters.push_back({save_bmp.c_str(), L"*.bmp"});
    filter_formats.push_back(oz::viewer_core::save_format::bmp);

    dialog->SetFileTypes(static_cast<UINT>(filters.size()), filters.data());
    dialog->SetFileTypeIndex(1); // Default to PNG
    dialog->SetDefaultExtension(L"png");

    if (!default_path.empty()) {
        const std::wstring stem = default_path.stem().wstring();
        dialog->SetFileName(stem.c_str());
    }

    const std::wstring save_title = viewer_string(IDS_SAVE_TITLE);
    dialog->SetTitle(save_title.c_str());

    hr = dialog->Show(parent);
    save_as_dialog_result res{};
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        return res;
    }
    if (FAILED(hr)) {
        return show_legacy_save_dialog(parent, default_path, webp_encoder_available);
    }

    ComPtr<IShellItem> item;
    hr = dialog->GetResult(&item);
    if (FAILED(hr) || !item) {
        return res;
    }

    PWSTR path_buf = nullptr;
    hr = item->GetDisplayName(SIGDN_FILESYSPATH, &path_buf);
    if (FAILED(hr) || !path_buf) {
        return res;
    }

    res.accepted = true;
    res.destination = std::filesystem::path(path_buf);
    CoTaskMemFree(path_buf);

    UINT filter_index = 1;
    dialog->GetFileTypeIndex(&filter_index);

    const std::wstring ext = res.destination.extension().wstring();
    if (!ext.empty()) {
        res.explicit_extension = true;
        const auto fmt = oz::viewer_core::save_format_for_extension(ext);
        if (fmt.has_value()) {
            res.format = *fmt;
        }
    } else {
        res.explicit_extension = false;
        const UINT idx = (filter_index > 0 && filter_index <= filter_formats.size())
                             ? filter_index - 1
                             : 0;
        res.format = filter_formats[idx];
        res.destination += oz::viewer_core::preferred_extension(res.format);
    }

    return res;
#endif
}

prompt_result show_prompt(
    HWND parent,
    const wchar_t* title,
    const wchar_t* main_instruction,
    const wchar_t* content,
    UINT buttons,
    UINT icon) {
    // Try TaskDialogIndirect first (modern Windows Vista+)
    HMODULE hComCtl = GetModuleHandleW(L"comctl32.dll");
    if (!hComCtl) {
        hComCtl = LoadLibraryW(L"comctl32.dll");
    }

    if (hComCtl) {
        using TaskDialogIndirectFn = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
        auto pfnTaskDialogIndirect = reinterpret_cast<TaskDialogIndirectFn>(
            GetProcAddress(hComCtl, "TaskDialogIndirect"));

        if (pfnTaskDialogIndirect) {
            // The dialog manager copies title/instruction/content
            // synchronously, so owning locals cover this call.
            const std::wstring fallback_title = viewer_string(IDS_APP_TITLE);
            TASKDIALOGCONFIG tc{sizeof(TASKDIALOGCONFIG)};
            tc.hwndParent = parent;
            tc.hInstance = GetModuleHandleW(nullptr);
            tc.pszWindowTitle = title ? title : fallback_title.c_str();
            tc.pszMainInstruction = main_instruction;
            tc.pszContent = content;

            TASKDIALOG_COMMON_BUTTON_FLAGS flags = 0;
            if (buttons == MB_OKCANCEL) {
                flags = TDCBF_OK_BUTTON | TDCBF_CANCEL_BUTTON;
            } else if (buttons == MB_YESNO) {
                flags = TDCBF_YES_BUTTON | TDCBF_NO_BUTTON;
            } else if (buttons == MB_YESNOCANCEL) {
                flags = TDCBF_YES_BUTTON | TDCBF_NO_BUTTON | TDCBF_CANCEL_BUTTON;
            } else if (buttons == MB_OK) {
                flags = TDCBF_OK_BUTTON;
            } else {
                flags = TDCBF_OK_BUTTON | TDCBF_CANCEL_BUTTON;
            }
            tc.dwCommonButtons = flags;

            if (icon == MB_ICONWARNING) {
                tc.pszMainIcon = TD_WARNING_ICON;
            } else if (icon == MB_ICONERROR) {
                tc.pszMainIcon = TD_ERROR_ICON;
            } else if (icon == MB_ICONINFORMATION) {
                tc.pszMainIcon = TD_INFORMATION_ICON;
            }

            int button_pressed = 0;
            const HRESULT hr = pfnTaskDialogIndirect(&tc, &button_pressed, nullptr, nullptr);
            if (SUCCEEDED(hr)) {
                switch (button_pressed) {
                    case IDOK:     return prompt_result::ok;
                    case IDCANCEL: return prompt_result::cancel;
                    case IDYES:    return prompt_result::yes;
                    case IDNO:     return prompt_result::no;
                    case IDRETRY:  return prompt_result::retry;
                    default:       return prompt_result::cancel;
                }
            }
        }
    }

    // Safe standard MessageBox fallback
    std::wstring combined;
    if (main_instruction && main_instruction[0] != L'\0') {
        combined = main_instruction;
        if (content && content[0] != L'\0') {
            combined += L"\n\n";
            combined += content;
        }
    } else if (content) {
        combined = content;
    }

    const std::wstring msg_title = viewer_string(IDS_APP_TITLE);
    const int msg_rc = MessageBoxW(
        parent,
        combined.c_str(),
        title ? title : msg_title.c_str(),
        buttons | icon);

    switch (msg_rc) {
        case IDOK:     return prompt_result::ok;
        case IDCANCEL: return prompt_result::cancel;
        case IDYES:    return prompt_result::yes;
        case IDNO:     return prompt_result::no;
        case IDRETRY:  return prompt_result::retry;
        default:       return prompt_result::cancel;
    }
}

}  // namespace oz::viewer

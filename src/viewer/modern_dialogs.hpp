// oz/viewer/modern_dialogs.hpp - Native modern file and task dialog adapters with safe fallback (Stage 7).
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "oz/viewer_core/codec.hpp"
#include "oz/viewer_core/save_pipeline.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace oz::viewer {

// Prompt choice return codes
enum class prompt_result {
    ok,
    cancel,
    yes,
    no,
    retry
};

// Result of modern Save As dialog
struct save_as_dialog_result {
    bool accepted = false;
    std::filesystem::path destination;
    oz::viewer_core::save_format format = oz::viewer_core::save_format::png;
    bool explicit_extension = false;
};

// Filter entry structure for modern and legacy Open dialogs
struct open_filter_entry {
    std::wstring display_name;
    std::wstring spec;
};

// Returns the authoritative list of supported Open file filters.
// The first entry is always the combined "All Supported Images" filter covering all supported extensions.
[[nodiscard]] std::vector<open_filter_entry> get_supported_open_filters() noexcept;

// Build double-null-terminated string for legacy OPENFILENAMEW from filter list
[[nodiscard]] std::vector<wchar_t> build_legacy_open_filter_string(
    const std::vector<open_filter_entry>& filters) noexcept;

// Interactive-only Open file dialog (IFileOpenDialog with GetOpenFileName fallback)
[[nodiscard]] std::optional<std::filesystem::path> show_open_image_dialog(HWND parent);

// Interactive-only Save As dialog (IFileSaveDialog with GetSaveFileName fallback)
[[nodiscard]] save_as_dialog_result show_save_as_dialog(
    HWND parent,
    const std::filesystem::path& default_path,
    oz::viewer_core::codec_id source_codec,
    bool webp_encoder_available);

// Modern task/message confirmation prompt (TaskDialogIndirect with MessageBox fallback)
[[nodiscard]] prompt_result show_prompt(
    HWND parent,
    const wchar_t* title,
    const wchar_t* main_instruction,
    const wchar_t* content,
    UINT buttons = MB_OKCANCEL,
    UINT icon = MB_ICONWARNING);

}  // namespace oz::viewer

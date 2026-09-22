// oz/viewer/clipboard_output.hpp - dedicated Win32 clipboard export boundary (Stage 9.3).
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace oz::viewer {

enum class clipboard_op_status {
    success,
    invalid_path,
    not_found,
    is_directory,
    payload_allocation_failed,
    open_failed,
    empty_failed,
    set_data_failed,
    invalid_image_dimensions,
    memory_limit_exceeded,
    materialization_failed,
    stale_document
};

struct clipboard_op_result {
    clipboard_op_status status = clipboard_op_status::invalid_path;
    DWORD error_code = 0;

    [[nodiscard]] constexpr bool ok() const noexcept {
        return status == clipboard_op_status::success;
    }
};

// Seam function types for native Win32 clipboard API injection (for testing without touching real OS clipboard):
struct clipboard_api_seam {
    std::function<BOOL(HWND)> open_clipboard;
    std::function<BOOL()> empty_clipboard;
    std::function<BOOL()> close_clipboard;
    std::function<HANDLE(UINT, HANDLE)> set_clipboard_data;
    std::function<UINT(LPCWSTR)> register_clipboard_format_w;
};

// Returns standard Win32 implementation
[[nodiscard]] clipboard_api_seam default_clipboard_api_seam() noexcept;

// Path normalization helper:
// Verifies path non-empty, checks status (must exist and be regular file),
// and returns fully qualified absolute normalized path with standard slashes (no symlink canonicalization).
// If invalid, returns empty path.
[[nodiscard]] std::wstring normalize_and_validate_file_path(const std::filesystem::path& path, clipboard_op_status* out_status = nullptr);

// Construct CF_HDROP memory buffer:
// DROPFILES struct followed by double-NUL-terminated wide string.
[[nodiscard]] std::vector<std::uint8_t> build_hdrop_payload(const std::wstring& normalized_path);

// Construct CFSTR_PREFERREDDROPEFFECT memory buffer:
// DWORD containing DROPEFFECT_COPY.
[[nodiscard]] std::vector<std::uint8_t> build_preferred_drop_effect_payload(DWORD drop_effect = DROPEFFECT_COPY);

// Construct CF_UNICODETEXT memory buffer:
// Exact normalized wide path with single NUL terminator.
[[nodiscard]] std::vector<std::uint8_t> build_unicode_text_payload(const std::wstring& normalized_path);

// Construct CF_DIBV5 memory buffer from canonical straight BGRA8 pixels:
// BITMAPV5HEADER with BI_BITFIELDS, top-down negative height, LCS_sRGB,
// correct color masks, followed by packed 32-bit straight BGRA pixel rows.
[[nodiscard]] std::vector<std::uint8_t> build_dibv5_payload_from_straight_bgra(
    const std::uint8_t* straight_bgra,
    std::int32_t width,
    std::int32_t height,
    std::int32_t src_stride_bytes);

// Converts canonical top-down premultiplied BGRA (PBGRA) buffer to straight BGRA8.
// When alpha == 0, RGB is set to 0. When alpha == 255, RGB is copied exact.
// Otherwise RGB = clamp((RGB * 255 + alpha / 2) / alpha, 0, 255). Exact alpha preserved.
[[nodiscard]] std::vector<std::uint8_t> convert_pbgra_to_straight_bgra(
    const std::uint8_t* pbgra,
    std::int32_t width,
    std::int32_t height,
    std::int32_t src_stride_bytes);

// Platform clipboard operations:
// Requires valid owner_hwnd (never nullptr).
// Validates input, constructs payloads before OpenClipboard/EmptyClipboard,
// uses GMEM_MOVEABLE | GMEM_ZEROINIT, handles RAII locks/allocations and partial publish cleanup.
[[nodiscard]] clipboard_op_result copy_file_to_clipboard(
    HWND owner_hwnd,
    const std::filesystem::path& file_path,
    const clipboard_api_seam* seam = nullptr);

[[nodiscard]] clipboard_op_result copy_file_path_to_clipboard(
    HWND owner_hwnd,
    const std::filesystem::path& file_path,
    const clipboard_api_seam* seam = nullptr);

// Publishes eager CF_DIBV5 straight BGRA image to the clipboard.
// Converts premultiplied BGRA8 input to straight BGRA8 and constructs CF_DIBV5 payload
// before opening the clipboard.
[[nodiscard]] clipboard_op_result copy_image_to_clipboard(
    HWND owner_hwnd,
    const std::uint8_t* pbgra_pixels,
    std::int32_t width,
    std::int32_t height,
    std::int32_t src_stride_bytes,
    const clipboard_api_seam* seam = nullptr);

}  // namespace oz::viewer

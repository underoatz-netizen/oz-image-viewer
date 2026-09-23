// oz/viewer/clipboard_input.hpp - bounded Win32 clipboard image-import boundary (Stage 9.5).
//
// Reads a valid clipboard bitmap/image and decodes it into the canonical
// in-memory contract (top-down premultiplied BGRA8) for presentation as a
// temporary document. Supported native formats:
//
//   CF_DIBV5 (preferred) and CF_DIB, restricted to the 24/32-bit
//   BI_RGB / BI_BITFIELDS DIB family with standard color masks.
//   Alpha contract: a legacy 40-byte 32-bit BI_BITFIELDS header declares
//   RGB only, so the pixel high byte is ignored and alpha is forced to
//   255; BITMAPV4/V5 headers use their explicit alpha mask (full mask
//   preserves/premultiplies alpha, zero mask forces opaque).
//
// CF_BITMAP sources are covered without explicit HBITMAP handling: the OS
// synthesizes CF_DIB/CF_DIBV5 from a clipboard CF_BITMAP on demand, so
// read_clipboard_image observes those formats through the same bounded
// parser. This keeps the module free of device-context (HDC) dependencies
// and fully unit-testable. PNG/JPEG clipboard blobs, RLE compression,
// palettized depths, and non-standard bit masks are rejected quietly as
// unsupported; malformed or overflowing input is rejected safely without
// touching the caller's document state.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace oz::viewer {

enum class paste_status {
    success,
    unavailable,        // clipboard could not be opened, handle lock failed, or no owner window
    no_image,           // no DIB-family format is present on the clipboard
    invalid_bitmap,     // present bytes are malformed or truncated
    unsupported_format, // well-formed header with unsupported depth/compression/masks
    overflow            // dimensions or payload exceed the bounded transfer limits
};

struct paste_result {
    paste_status status = paste_status::no_image;
    DWORD error_code = 0;

    [[nodiscard]] constexpr bool ok() const noexcept {
        return status == paste_status::success;
    }
};

// Canonical pasted bitmap: top-down premultiplied BGRA8, matching the
// decoded_image contract (orientation already applied: bottom-up DIB rows
// are flipped during import).
struct clipboard_bitmap {
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t stride = 0;  // bytes per row (width * 4)
    std::vector<std::uint8_t> pixels;  // premultiplied BGRA8, top-down

    [[nodiscard]] bool is_valid() const noexcept {
        return width > 0 && height > 0 && stride >= width * 4 && !pixels.empty() &&
               pixels.size() >= static_cast<std::size_t>(stride) * static_cast<std::size_t>(height);
    }
};

// Seam function types for native Win32 clipboard read-API injection (for
// testing without touching the real OS clipboard).
struct clipboard_read_seam {
    std::function<BOOL(HWND)> open_clipboard;
    std::function<BOOL()> close_clipboard;
    std::function<BOOL(UINT)> is_format_available;
    std::function<HANDLE(UINT)> get_data;
    std::function<void*(HANDLE)> lock;
    std::function<BOOL(HANDLE)> unlock;
    std::function<SIZE_T(HANDLE)> mem_size;
};

// Returns the standard Win32 implementation.
[[nodiscard]] clipboard_read_seam default_clipboard_read_seam() noexcept;

// Pure parser over an in-memory CF_DIB/CF_DIBV5 byte image (header +
// pixels). Never touches the OS clipboard; `out` is cleared on failure so
// no partial bitmap can escape.
[[nodiscard]] paste_result parse_dib_bytes(const std::uint8_t* data,
                                           std::size_t size,
                                           clipboard_bitmap& out);

// Converts canonical straight BGRA8 rows to premultiplied BGRA8.
// out_rgb = (rgb * alpha + 127) / alpha for 0 < alpha < 255; alpha is
// preserved exactly. Returns an empty vector on invalid input.
[[nodiscard]] std::vector<std::uint8_t> convert_straight_bgra_to_pbgra(
    const std::uint8_t* straight_bgra,
    std::int32_t width,
    std::int32_t height,
    std::int32_t src_stride_bytes);

// Full clipboard read: opens the clipboard, prefers CF_DIBV5 then CF_DIB,
// performs a bounded copy of the handle bytes, closes the clipboard, and
// parses the local copy. Never empties the clipboard, never writes a file,
// and leaves `out` cleared on any failure.
[[nodiscard]] paste_result read_clipboard_image(
    HWND owner_hwnd,
    clipboard_bitmap& out,
    const clipboard_read_seam* seam = nullptr);

}  // namespace oz::viewer

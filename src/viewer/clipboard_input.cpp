// oz/viewer/clipboard_input.cpp - bounded Win32 clipboard image-import implementation (Stage 9.5).
#include "clipboard_input.hpp"

#include <cstring>
#include <new>
#include <stdexcept>

namespace oz::viewer {

namespace {

// Shared bounded limits (mirror the canonical codec policy: 16-bit
// dimension cap, 2 GiB decoded cap).
constexpr std::int64_t kMaxPasteDimension = 65535;
constexpr std::uint64_t kMaxPasteDecodedBytes = 1ull << 31;  // 2 GiB

// Accepted DIB header sizes: BITMAPINFOHEADER (40), BITMAPV4HEADER (108),
// BITMAPV5HEADER (124). Older core headers are rejected as invalid.
constexpr std::uint32_t kDibInfoHeaderSize = 40;
constexpr std::uint32_t kDibV4HeaderSize = 108;
constexpr std::uint32_t kDibV5HeaderSize = 124;

// Standard 32-bit channel masks accepted for BI_BITFIELDS input.
constexpr std::uint32_t kStdRedMask = 0x00FF0000u;
constexpr std::uint32_t kStdGreenMask = 0x0000FF00u;
constexpr std::uint32_t kStdBlueMask = 0x000000FFu;
constexpr std::uint32_t kAlphaMaskFull = 0xFF000000u;

class scoped_clipboard_read_lock {
public:
    explicit scoped_clipboard_read_lock(HWND hwnd, const clipboard_read_seam& seam)
        : seam_(seam) {
        if (seam_.open_clipboard) {
            opened_ = seam_.open_clipboard(hwnd);
        } else {
            opened_ = ::OpenClipboard(hwnd);
        }
    }

    ~scoped_clipboard_read_lock() {
        if (opened_) {
            if (seam_.close_clipboard) {
                seam_.close_clipboard();
            } else {
                ::CloseClipboard();
            }
        }
    }

    scoped_clipboard_read_lock(const scoped_clipboard_read_lock&) = delete;
    scoped_clipboard_read_lock& operator=(const scoped_clipboard_read_lock&) = delete;

    [[nodiscard]] bool is_open() const noexcept {
        return opened_ != FALSE;
    }

private:
    const clipboard_read_seam& seam_;
    BOOL opened_ = FALSE;
};

class scoped_clipboard_handle_lock {
public:
    scoped_clipboard_handle_lock(HANDLE handle, const clipboard_read_seam& seam)
        : handle_(handle), seam_(seam) {
        if (handle_) {
            if (seam_.lock) {
                ptr_ = seam_.lock(handle_);
            } else {
                ptr_ = ::GlobalLock(handle_);
            }
        }
    }

    ~scoped_clipboard_handle_lock() {
        if (ptr_ && handle_) {
            if (seam_.unlock) {
                seam_.unlock(handle_);
            } else {
                ::GlobalUnlock(handle_);
            }
            ptr_ = nullptr;
        }
    }

    scoped_clipboard_handle_lock(const scoped_clipboard_handle_lock&) = delete;
    scoped_clipboard_handle_lock& operator=(const scoped_clipboard_handle_lock&) = delete;

    [[nodiscard]] const void* get() const noexcept {
        return ptr_;
    }

private:
    HANDLE handle_ = nullptr;
    const clipboard_read_seam& seam_;
    void* ptr_ = nullptr;
};

template <typename T>
void read_unaligned(const std::uint8_t* base, std::size_t offset, T& out) noexcept {
    static_assert(std::is_trivially_copyable_v<T>);
    std::memcpy(&out, base + offset, sizeof(T));
}

}  // namespace

clipboard_read_seam default_clipboard_read_seam() noexcept {
    clipboard_read_seam seam;
    seam.open_clipboard = [](HWND hwnd) -> BOOL {
        return ::OpenClipboard(hwnd);
    };
    seam.close_clipboard = []() -> BOOL {
        return ::CloseClipboard();
    };
    seam.is_format_available = [](UINT format) -> BOOL {
        return ::IsClipboardFormatAvailable(format);
    };
    seam.get_data = [](UINT format) -> HANDLE {
        return ::GetClipboardData(format);
    };
    seam.lock = [](HANDLE handle) -> void* {
        return ::GlobalLock(handle);
    };
    seam.unlock = [](HANDLE handle) -> BOOL {
        return ::GlobalUnlock(handle);
    };
    seam.mem_size = [](HANDLE handle) -> SIZE_T {
        return ::GlobalSize(handle);
    };
    return seam;
}

std::vector<std::uint8_t> convert_straight_bgra_to_pbgra(
    const std::uint8_t* straight_bgra,
    std::int32_t width,
    std::int32_t height,
    std::int32_t src_stride_bytes) {
    if (!straight_bgra || width <= 0 || height <= 0 || width > kMaxPasteDimension ||
        height > kMaxPasteDimension) {
        return {};
    }
    const std::int64_t min_src_stride = static_cast<std::int64_t>(width) * 4;
    if (src_stride_bytes < min_src_stride) {
        return {};
    }
    const std::uint64_t dst_row_bytes = static_cast<std::uint64_t>(width) * 4;
    const std::uint64_t total_bytes = dst_row_bytes * static_cast<std::uint64_t>(height);
    if (total_bytes == 0 || total_bytes > kMaxPasteDecodedBytes) {
        return {};
    }

    std::vector<std::uint8_t> out;
    try {
        out.resize(static_cast<std::size_t>(total_bytes));
    } catch (const std::bad_alloc&) {
        return {};
    } catch (const std::length_error&) {
        return {};
    }
    for (std::int32_t y = 0; y < height; ++y) {
        const std::uint8_t* src_row =
            straight_bgra + static_cast<std::size_t>(y) * static_cast<std::size_t>(src_stride_bytes);
        std::uint8_t* dst_row = out.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(dst_row_bytes);
        for (std::int32_t x = 0; x < width; ++x) {
            const std::size_t px = static_cast<std::size_t>(x) * 4;
            const std::uint8_t b = src_row[px + 0];
            const std::uint8_t g = src_row[px + 1];
            const std::uint8_t r = src_row[px + 2];
            const std::uint8_t a = src_row[px + 3];
            dst_row[px + 3] = a;
            if (a == 0) {
                dst_row[px + 0] = 0;
                dst_row[px + 1] = 0;
                dst_row[px + 2] = 0;
            } else if (a == 255) {
                dst_row[px + 0] = b;
                dst_row[px + 1] = g;
                dst_row[px + 2] = r;
            } else {
                dst_row[px + 0] =
                    static_cast<std::uint8_t>((static_cast<std::uint32_t>(b) * a + 127u) / 255u);
                dst_row[px + 1] =
                    static_cast<std::uint8_t>((static_cast<std::uint32_t>(g) * a + 127u) / 255u);
                dst_row[px + 2] =
                    static_cast<std::uint8_t>((static_cast<std::uint32_t>(r) * a + 127u) / 255u);
            }
        }
    }
    return out;
}

paste_result parse_dib_bytes(const std::uint8_t* data,
                             std::size_t size,
                             clipboard_bitmap& out) {
    paste_result result;
    out = clipboard_bitmap{};

    if (!data || size < sizeof(BITMAPINFOHEADER)) {
        result.status = paste_status::invalid_bitmap;
        return result;
    }

    std::uint32_t header_size = 0;
    read_unaligned(data, 0, header_size);
    if (header_size != kDibInfoHeaderSize && header_size != kDibV4HeaderSize &&
        header_size != kDibV5HeaderSize) {
        result.status = paste_status::invalid_bitmap;
        return result;
    }
    if (size < header_size) {
        result.status = paste_status::invalid_bitmap;
        return result;
    }

    std::int32_t width = 0;
    std::int32_t height = 0;
    std::uint16_t planes = 0;
    std::uint16_t bit_count = 0;
    std::uint32_t compression = 0;
    read_unaligned(data, 4, width);
    read_unaligned(data, 8, height);
    read_unaligned(data, 12, planes);
    read_unaligned(data, 14, bit_count);
    read_unaligned(data, 16, compression);

    if (planes != 1) {
        result.status = paste_status::invalid_bitmap;
        return result;
    }
    if (width <= 0 || height == 0) {
        result.status = paste_status::invalid_bitmap;
        return result;
    }
    const std::int64_t abs_width = width > 0 ? static_cast<std::int64_t>(width)
                                             : -static_cast<std::int64_t>(width);
    const std::int64_t abs_height = height > 0 ? static_cast<std::int64_t>(height)
                                               : -static_cast<std::int64_t>(height);
    if (abs_width > kMaxPasteDimension || abs_height > kMaxPasteDimension) {
        result.status = paste_status::overflow;
        return result;
    }

    if (bit_count != 24 && bit_count != 32) {
        // Palettized and sub-byte depths are outside the bounded paste contract.
        result.status = paste_status::unsupported_format;
        return result;
    }
    if (compression != BI_RGB && compression != BI_BITFIELDS) {
        // RLE, JPEG/PNG-embedded, and other compressions are not decoded here.
        result.status = paste_status::unsupported_format;
        return result;
    }
    if (compression == BI_BITFIELDS && bit_count != 32) {
        result.status = paste_status::unsupported_format;
        return result;
    }

    // Resolve channel masks and the pixel-data offset.
    bool force_opaque = false;  // No usable alpha channel: treat pixels as opaque.
    std::size_t pixel_offset = header_size;
    if (compression == BI_BITFIELDS) {
        std::uint32_t red_mask = 0;
        std::uint32_t green_mask = 0;
        std::uint32_t blue_mask = 0;
        std::uint32_t alpha_mask = 0;
        if (header_size == kDibInfoHeaderSize) {
            // BITMAPINFOHEADER is followed by three explicit mask DWORDs.
            if (size < header_size + 3 * sizeof(std::uint32_t)) {
                result.status = paste_status::invalid_bitmap;
                return result;
            }
            read_unaligned(data, header_size + 0, red_mask);
            read_unaligned(data, header_size + 4, green_mask);
            read_unaligned(data, header_size + 8, blue_mask);
            // A 40-byte BI_BITFIELDS header carries no alpha mask field:
            // the three DWORDs declare RGB only. Ignore the pixel high
            // byte and force opaque; no fourth alpha mask is inferred.
            alpha_mask = 0;
            pixel_offset = header_size + 3 * sizeof(std::uint32_t);
        } else {
            // V4/V5 embed the masks (and the V5 alpha mask) in the header.
            read_unaligned(data, 40, red_mask);
            read_unaligned(data, 44, green_mask);
            read_unaligned(data, 48, blue_mask);
            if (header_size >= kDibV5HeaderSize) {
                read_unaligned(data, 52, alpha_mask);
            } else {
                // V4 headers expose bV4AlphaMask at the same offset; accept
                // either a full alpha mask or none.
                read_unaligned(data, 52, alpha_mask);
            }
        }
        if (red_mask != kStdRedMask || green_mask != kStdGreenMask || blue_mask != kStdBlueMask) {
            result.status = paste_status::unsupported_format;
            return result;
        }
        if (alpha_mask == kAlphaMaskFull) {
            force_opaque = false;
        } else if (alpha_mask == 0) {
            force_opaque = true;
        } else {
            result.status = paste_status::unsupported_format;
            return result;
        }
    } else if (bit_count == 32) {
        // BI_RGB 32-bit pixels carry no alpha channel per the DIB contract.
        force_opaque = true;
    }
    if (pixel_offset > size) {
        result.status = paste_status::invalid_bitmap;
        return result;
    }

    // Checked source stride: DWORD-aligned rows.
    const std::uint64_t src_row_bytes =
        ((static_cast<std::uint64_t>(abs_width) * bit_count + 31u) / 32u) * 4u;
    const std::uint64_t src_pixel_bytes = src_row_bytes * static_cast<std::uint64_t>(abs_height);
    if (src_pixel_bytes == 0 || src_pixel_bytes > kMaxPasteDecodedBytes) {
        result.status = paste_status::overflow;
        return result;
    }
    if (static_cast<std::uint64_t>(pixel_offset) + src_pixel_bytes > size) {
        result.status = paste_status::invalid_bitmap;
        return result;
    }

    const std::int32_t out_width = static_cast<std::int32_t>(abs_width);
    const std::int32_t out_height = static_cast<std::int32_t>(abs_height);
    const bool top_down = (height < 0);
    const std::uint64_t dst_row_bytes = static_cast<std::uint64_t>(abs_width) * 4u;
    const std::uint64_t dst_total = dst_row_bytes * static_cast<std::uint64_t>(abs_height);
    if (dst_total == 0 || dst_total > kMaxPasteDecodedBytes) {
        result.status = paste_status::overflow;
        return result;
    }

    // Expand to straight BGRA8 top-down first (flipping bottom-up rows),
    // then premultiply into the canonical contract.
    // Allocation failures are reported as overflow with `out` left cleared;
    // only allocation faults are caught so unrelated exceptions propagate.
    std::vector<std::uint8_t> straight;
    try {
        straight.resize(static_cast<std::size_t>(dst_total));
    } catch (const std::bad_alloc&) {
        result.status = paste_status::overflow;
        return result;
    } catch (const std::length_error&) {
        result.status = paste_status::overflow;
        return result;
    }
    const std::uint8_t* pixel_base = data + pixel_offset;
    const std::size_t bytes_per_pixel = static_cast<std::size_t>(bit_count) / 8;
    for (std::int64_t out_y = 0; out_y < abs_height; ++out_y) {
        const std::uint64_t src_y =
            top_down ? static_cast<std::uint64_t>(out_y)
                     : static_cast<std::uint64_t>(abs_height - 1 - out_y);
        const std::uint8_t* src_row = pixel_base + src_y * src_row_bytes;
        std::uint8_t* dst_row =
            straight.data() + static_cast<std::size_t>(out_y) * static_cast<std::size_t>(dst_row_bytes);
        for (std::int64_t x = 0; x < abs_width; ++x) {
            const std::uint8_t* src_px = src_row + static_cast<std::size_t>(x) * bytes_per_pixel;
            std::uint8_t* dst_px = dst_row + static_cast<std::size_t>(x) * 4;
            dst_px[0] = src_px[0];
            dst_px[1] = src_px[1];
            dst_px[2] = src_px[2];
            if (bit_count == 32 && !force_opaque) {
                dst_px[3] = src_px[3];
            } else {
                dst_px[3] = 255;
            }
        }
    }

    std::vector<std::uint8_t> premul = convert_straight_bgra_to_pbgra(
        straight.data(), out_width, out_height, static_cast<std::int32_t>(dst_row_bytes));
    if (premul.empty()) {
        result.status = paste_status::overflow;
        return result;
    }

    out.width = out_width;
    out.height = out_height;
    out.stride = static_cast<std::int32_t>(dst_row_bytes);
    out.pixels = std::move(premul);
    result.status = paste_status::success;
    result.error_code = 0;
    return result;
}

paste_result read_clipboard_image(
    HWND owner_hwnd,
    clipboard_bitmap& out,
    const clipboard_read_seam* seam) {
    paste_result result;
    out = clipboard_bitmap{};

    if (!owner_hwnd) {
        result.status = paste_status::unavailable;
        result.error_code = ERROR_INVALID_WINDOW_HANDLE;
        return result;
    }

    const clipboard_read_seam active_seam = seam ? *seam : default_clipboard_read_seam();

    // Copy the DIB bytes while the clipboard/GlobalLock scope is held; the
    // scoped locks are destroyed (clipboard closed, handle unlocked) before
    // any pixel parsing runs, per the documented contract.
    std::vector<std::uint8_t> local;
    {
        scoped_clipboard_read_lock clip_lock(owner_hwnd, active_seam);
        if (!clip_lock.is_open()) {
            result.status = paste_status::unavailable;
            result.error_code = ::GetLastError();
            return result;
        }

        UINT format = 0;
        const BOOL has_v5 = active_seam.is_format_available ? active_seam.is_format_available(CF_DIBV5)
                                                            : ::IsClipboardFormatAvailable(CF_DIBV5);
        if (has_v5) {
            format = CF_DIBV5;
        } else {
            const BOOL has_dib = active_seam.is_format_available ? active_seam.is_format_available(CF_DIB)
                                                                 : ::IsClipboardFormatAvailable(CF_DIB);
            if (has_dib) {
                format = CF_DIB;
            }
        }
        if (format == 0) {
            result.status = paste_status::no_image;
            return result;
        }

        HANDLE handle = nullptr;
        if (active_seam.get_data) {
            handle = active_seam.get_data(format);
        } else {
            handle = ::GetClipboardData(format);
        }
        if (!handle) {
            result.status = paste_status::unavailable;
            result.error_code = ::GetLastError();
            return result;
        }

        scoped_clipboard_handle_lock mem_lock(handle, active_seam);
        const void* locked = mem_lock.get();
        if (!locked) {
            result.status = paste_status::unavailable;
            result.error_code = ::GetLastError();
            return result;
        }

        SIZE_T block_size = 0;
        if (active_seam.mem_size) {
            block_size = active_seam.mem_size(handle);
        } else {
            block_size = ::GlobalSize(handle);
        }
        if (block_size < sizeof(BITMAPINFOHEADER) ||
            block_size > static_cast<SIZE_T>(kMaxPasteDecodedBytes) + sizeof(BITMAPV5HEADER)) {
            result.status = block_size < sizeof(BITMAPINFOHEADER) ? paste_status::invalid_bitmap
                                                                   : paste_status::overflow;
            return result;
        }

        // Bounded copy while locks are held; parsing happens after scope exit.
        // Allocation failure is reported as overflow with `out` left cleared;
        // scope destructors still close/unlock the clipboard during unwinding.
        try {
            local.resize(static_cast<std::size_t>(block_size));
        } catch (const std::bad_alloc&) {
            result.status = paste_status::overflow;
            return result;
        } catch (const std::length_error&) {
            result.status = paste_status::overflow;
            return result;
        }
        std::memcpy(local.data(), locked, local.size());
    }  // clip_lock/mem_lock destroyed: GlobalUnlock + CloseClipboard run here.

    // parse_dib_bytes is itself allocation-safe, but guard the final parse
    // boundary so a clipboard-driven allocation fault can never escape to
    // the GUI message loop; `out` is cleared on this path.
    try {
        return parse_dib_bytes(local.data(), local.size(), out);
    } catch (const std::bad_alloc&) {
        out = clipboard_bitmap{};
        result.status = paste_status::overflow;
        return result;
    } catch (const std::length_error&) {
        out = clipboard_bitmap{};
        result.status = paste_status::overflow;
        return result;
    }
}

}  // namespace oz::viewer

// oz/viewer/clipboard_output.cpp - dedicated Win32 clipboard export boundary implementation (Stage 9.3).
#include "clipboard_output.hpp"

#include <cstring>
#include <system_error>

namespace oz::viewer {

namespace {

class scoped_clipboard_lock {
public:
    explicit scoped_clipboard_lock(HWND hwnd, const clipboard_api_seam& seam)
        : seam_(seam) {
        if (seam_.open_clipboard) {
            opened_ = seam_.open_clipboard(hwnd);
        } else {
            opened_ = ::OpenClipboard(hwnd);
        }
    }

    ~scoped_clipboard_lock() {
        if (opened_) {
            if (seam_.close_clipboard) {
                seam_.close_clipboard();
            } else {
                ::CloseClipboard();
            }
        }
    }

    scoped_clipboard_lock(const scoped_clipboard_lock&) = delete;
    scoped_clipboard_lock& operator=(const scoped_clipboard_lock&) = delete;

    [[nodiscard]] bool is_open() const noexcept {
        return opened_;
    }

private:
    const clipboard_api_seam& seam_;
    BOOL opened_ = FALSE;
};

class scoped_hglobal {
public:
    explicit scoped_hglobal(SIZE_T bytes = 0) {
        if (bytes > 0) {
            handle_ = ::GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
        }
    }

    ~scoped_hglobal() {
        if (handle_) {
            ::GlobalFree(handle_);
            handle_ = nullptr;
        }
    }

    scoped_hglobal(scoped_hglobal&& other) noexcept
        : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    scoped_hglobal& operator=(scoped_hglobal&& other) noexcept {
        if (this != &other) {
            if (handle_) {
                ::GlobalFree(handle_);
            }
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    scoped_hglobal(const scoped_hglobal&) = delete;
    scoped_hglobal& operator=(const scoped_hglobal&) = delete;

    [[nodiscard]] HGLOBAL get() const noexcept {
        return handle_;
    }

    [[nodiscard]] HGLOBAL release() noexcept {
        HGLOBAL h = handle_;
        handle_ = nullptr;
        return h;
    }

private:
    HGLOBAL handle_ = nullptr;
};

class scoped_global_lock {
public:
    explicit scoped_global_lock(HGLOBAL handle)
        : handle_(handle) {
        if (handle_) {
            ptr_ = ::GlobalLock(handle_);
        }
    }

    ~scoped_global_lock() {
        if (ptr_ && handle_) {
            ::GlobalUnlock(handle_);
            ptr_ = nullptr;
        }
    }

    scoped_global_lock(const scoped_global_lock&) = delete;
    scoped_global_lock& operator=(const scoped_global_lock&) = delete;

    [[nodiscard]] void* get() const noexcept {
        return ptr_;
    }

private:
    HGLOBAL handle_ = nullptr;
    void* ptr_ = nullptr;
};

[[nodiscard]] scoped_hglobal allocate_and_fill_hglobal(const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) {
        return scoped_hglobal(0);
    }
    scoped_hglobal mem(bytes.size());
    if (!mem.get()) {
        return mem;
    }
    scoped_global_lock lock(mem.get());
    if (!lock.get()) {
        return scoped_hglobal(0);
    }
    std::memcpy(lock.get(), bytes.data(), bytes.size());
    return mem;
}

}  // namespace

clipboard_api_seam default_clipboard_api_seam() noexcept {
    clipboard_api_seam seam;
    seam.open_clipboard = [](HWND hwnd) -> BOOL {
        return ::OpenClipboard(hwnd);
    };
    seam.empty_clipboard = []() -> BOOL {
        return ::EmptyClipboard();
    };
    seam.close_clipboard = []() -> BOOL {
        return ::CloseClipboard();
    };
    seam.set_clipboard_data = [](UINT format, HANDLE data) -> HANDLE {
        return ::SetClipboardData(format, data);
    };
    seam.register_clipboard_format_w = [](LPCWSTR name) -> UINT {
        return ::RegisterClipboardFormatW(name);
    };
    return seam;
}

std::wstring normalize_and_validate_file_path(const std::filesystem::path& path, clipboard_op_status* out_status) {
    if (path.empty()) {
        if (out_status) *out_status = clipboard_op_status::invalid_path;
        return {};
    }

    std::error_code ec;
    const auto st = std::filesystem::status(path, ec);
    if (ec || !std::filesystem::exists(st)) {
        if (out_status) *out_status = clipboard_op_status::not_found;
        return {};
    }

    if (std::filesystem::is_directory(st)) {
        if (out_status) *out_status = clipboard_op_status::is_directory;
        return {};
    }

    if (!std::filesystem::is_regular_file(st)) {
        if (out_status) *out_status = clipboard_op_status::invalid_path;
        return {};
    }

    // Full absolute lexical normalization without symlink resolution:
    // std::filesystem::absolute followed by lexically_normal()
    const auto abs_path = std::filesystem::absolute(path, ec);
    if (ec || abs_path.empty()) {
        if (out_status) *out_status = clipboard_op_status::invalid_path;
        return {};
    }

    const auto norm_path = abs_path.lexically_normal();
    std::wstring wide = norm_path.native();

    // Standardize backslashes for Windows path consistency
    for (auto& ch : wide) {
        if (ch == L'/') {
            ch = L'\\';
        }
    }

    if (out_status) *out_status = clipboard_op_status::success;
    return wide;
}

std::vector<std::uint8_t> build_hdrop_payload(const std::wstring& normalized_path) {
    if (normalized_path.empty()) {
        return {};
    }

    // DROPFILES struct + wide path + double NUL (null char + terminal null char)
    // Checked arithmetic for byte length
    const std::size_t path_chars_with_double_nul = normalized_path.size() + 2;
    const std::size_t string_bytes = path_chars_with_double_nul * sizeof(wchar_t);
    const std::size_t total_bytes = sizeof(DROPFILES) + string_bytes;

    std::vector<std::uint8_t> buffer(total_bytes, 0);

    auto* drop = reinterpret_cast<DROPFILES*>(buffer.data());
    drop->pFiles = sizeof(DROPFILES);
    drop->pt.x = 0;
    drop->pt.y = 0;
    drop->fNC = FALSE;
    drop->fWide = TRUE;

    auto* dest_chars = reinterpret_cast<wchar_t*>(buffer.data() + sizeof(DROPFILES));
    std::memcpy(dest_chars, normalized_path.c_str(), normalized_path.size() * sizeof(wchar_t));
    dest_chars[normalized_path.size()] = L'\0';
    dest_chars[normalized_path.size() + 1] = L'\0';

    return buffer;
}

std::vector<std::uint8_t> build_preferred_drop_effect_payload(DWORD drop_effect) {
    std::vector<std::uint8_t> buffer(sizeof(DWORD), 0);
    std::memcpy(buffer.data(), &drop_effect, sizeof(DWORD));
    return buffer;
}

std::vector<std::uint8_t> build_unicode_text_payload(const std::wstring& normalized_path) {
    if (normalized_path.empty()) {
        return {};
    }

    const std::size_t chars_with_nul = normalized_path.size() + 1;
    const std::size_t total_bytes = chars_with_nul * sizeof(wchar_t);

    std::vector<std::uint8_t> buffer(total_bytes, 0);
    auto* dest_chars = reinterpret_cast<wchar_t*>(buffer.data());
    std::memcpy(dest_chars, normalized_path.c_str(), normalized_path.size() * sizeof(wchar_t));
    dest_chars[normalized_path.size()] = L'\0';

    return buffer;
}

std::vector<std::uint8_t> convert_pbgra_to_straight_bgra(
    const std::uint8_t* pbgra,
    std::int32_t width,
    std::int32_t height,
    std::int32_t src_stride_bytes) {
    if (!pbgra || width <= 0 || height <= 0 || width > 65535 || height > 65535) {
        return {};
    }
    const std::int32_t min_src_stride = width * 4;
    if (src_stride_bytes < min_src_stride) {
        return {};
    }

    const std::size_t dst_row_bytes = static_cast<std::size_t>(width) * 4;
    const std::size_t total_bytes = dst_row_bytes * static_cast<std::size_t>(height);
    if (total_bytes > 0x7FFFFFFF) { // checked arithmetic
        return {};
    }

    std::vector<std::uint8_t> out(total_bytes);
    for (std::int32_t y = 0; y < height; ++y) {
        const std::uint8_t* src_row = pbgra + static_cast<std::size_t>(y) * static_cast<std::size_t>(src_stride_bytes);
        std::uint8_t* dst_row = out.data() + static_cast<std::size_t>(y) * dst_row_bytes;
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
                const auto un_b = (static_cast<std::uint32_t>(b) * 255u + (static_cast<std::uint32_t>(a) / 2u)) / static_cast<std::uint32_t>(a);
                const auto un_g = (static_cast<std::uint32_t>(g) * 255u + (static_cast<std::uint32_t>(a) / 2u)) / static_cast<std::uint32_t>(a);
                const auto un_r = (static_cast<std::uint32_t>(r) * 255u + (static_cast<std::uint32_t>(a) / 2u)) / static_cast<std::uint32_t>(a);

                dst_row[px + 0] = static_cast<std::uint8_t>(un_b > 255u ? 255u : un_b);
                dst_row[px + 1] = static_cast<std::uint8_t>(un_g > 255u ? 255u : un_g);
                dst_row[px + 2] = static_cast<std::uint8_t>(un_r > 255u ? 255u : un_r);
            }
        }
    }
    return out;
}

std::vector<std::uint8_t> build_dibv5_payload_from_straight_bgra(
    const std::uint8_t* straight_bgra,
    std::int32_t width,
    std::int32_t height,
    std::int32_t src_stride_bytes) {
    if (!straight_bgra || width <= 0 || height <= 0 || width > 65535 || height > 65535) {
        return {};
    }
    const std::int32_t min_src_stride = width * 4;
    if (src_stride_bytes < min_src_stride) {
        return {};
    }

    const std::size_t row_bytes = static_cast<std::size_t>(width) * 4;
    const std::size_t pixel_bytes = row_bytes * static_cast<std::size_t>(height);
    const std::size_t total_payload_bytes = sizeof(BITMAPV5HEADER) + pixel_bytes;
    if (pixel_bytes > 0x7FFFFFFF || total_payload_bytes > 0x7FFFFFFF) {
        return {};
    }

    std::vector<std::uint8_t> payload(total_payload_bytes, 0);
    auto* v5 = reinterpret_cast<BITMAPV5HEADER*>(payload.data());
    v5->bV5Size = sizeof(BITMAPV5HEADER);
    v5->bV5Width = width;
    v5->bV5Height = -height; // top-down
    v5->bV5Planes = 1;
    v5->bV5BitCount = 32;
    v5->bV5Compression = BI_BITFIELDS;
    v5->bV5SizeImage = static_cast<DWORD>(pixel_bytes);
    v5->bV5XPelsPerMeter = 0;
    v5->bV5YPelsPerMeter = 0;
    v5->bV5ClrUsed = 0;
    v5->bV5ClrImportant = 0;
    v5->bV5RedMask   = 0x00FF0000;
    v5->bV5GreenMask = 0x0000FF00;
    v5->bV5BlueMask  = 0x000000FF;
    v5->bV5AlphaMask = 0xFF000000;
    v5->bV5CSType = LCS_sRGB;
    v5->bV5Intent = LCS_GM_IMAGES;

    std::uint8_t* dst_pixels = payload.data() + sizeof(BITMAPV5HEADER);
    if (static_cast<std::size_t>(src_stride_bytes) == row_bytes) {
        std::memcpy(dst_pixels, straight_bgra, pixel_bytes);
    } else {
        for (std::int32_t y = 0; y < height; ++y) {
            const std::uint8_t* src_row = straight_bgra + static_cast<std::size_t>(y) * static_cast<std::size_t>(src_stride_bytes);
            std::uint8_t* dst_row = dst_pixels + static_cast<std::size_t>(y) * row_bytes;
            std::memcpy(dst_row, src_row, row_bytes);
        }
    }

    return payload;
}

clipboard_op_result copy_file_to_clipboard(
    HWND owner_hwnd,
    const std::filesystem::path& file_path,
    const clipboard_api_seam* seam) {
    clipboard_op_result result;

    if (!owner_hwnd) {
        result.status = clipboard_op_status::open_failed;
        result.error_code = ERROR_INVALID_WINDOW_HANDLE;
        return result;
    }

    const std::wstring norm_path = normalize_and_validate_file_path(file_path, &result.status);
    if (result.status != clipboard_op_status::success) {
        return result;
    }

    // Build byte payloads before touching the clipboard
    const auto pde_bytes = build_preferred_drop_effect_payload(DROPEFFECT_COPY);
    const auto hdrop_bytes = build_hdrop_payload(norm_path);

    if (pde_bytes.empty() || hdrop_bytes.empty()) {
        result.status = clipboard_op_status::payload_allocation_failed;
        return result;
    }

    scoped_hglobal pde_hglobal = allocate_and_fill_hglobal(pde_bytes);
    scoped_hglobal hdrop_hglobal = allocate_and_fill_hglobal(hdrop_bytes);

    if (!pde_hglobal.get() || !hdrop_hglobal.get()) {
        result.status = clipboard_op_status::payload_allocation_failed;
        result.error_code = ::GetLastError();
        return result;
    }

    const clipboard_api_seam active_seam = seam ? *seam : default_clipboard_api_seam();

    // Register CFSTR_PREFERREDDROPEFFECT format on demand
    constexpr wchar_t kPreferredDropEffectFormat[] = L"Preferred DropEffect";
    UINT cf_pref_drop_effect = 0;
    if (active_seam.register_clipboard_format_w) {
        cf_pref_drop_effect = active_seam.register_clipboard_format_w(kPreferredDropEffectFormat);
    } else {
        cf_pref_drop_effect = ::RegisterClipboardFormatW(kPreferredDropEffectFormat);
    }

    if (cf_pref_drop_effect == 0) {
        result.status = clipboard_op_status::set_data_failed;
        result.error_code = ::GetLastError();
        return result;
    }

    // RAII OpenClipboard guard
    scoped_clipboard_lock clip_lock(owner_hwnd, active_seam);
    if (!clip_lock.is_open()) {
        result.status = clipboard_op_status::open_failed;
        result.error_code = ::GetLastError();
        return result;
    }

    // Empty clipboard
    const BOOL empty_ok = active_seam.empty_clipboard ? active_seam.empty_clipboard() : ::EmptyClipboard();
    if (!empty_ok) {
        result.status = clipboard_op_status::empty_failed;
        result.error_code = ::GetLastError();
        return result;
    }

    // Publish drop effect first and CF_HDROP last
    // SetClipboardData transfers ownership only on success
    auto set_data_fn = active_seam.set_clipboard_data ? active_seam.set_clipboard_data : ::SetClipboardData;

    HANDLE h1 = set_data_fn(cf_pref_drop_effect, pde_hglobal.get());
    if (!h1) {
        result.status = clipboard_op_status::set_data_failed;
        result.error_code = ::GetLastError();
        // pde_hglobal and hdrop_hglobal RAII destructors will free unowned memory
        return result;
    }
    // Ownership transferred to system
    static_cast<void>(pde_hglobal.release());

    HANDLE h2 = set_data_fn(CF_HDROP, hdrop_hglobal.get());
    if (!h2) {
        result.status = clipboard_op_status::set_data_failed;
        result.error_code = ::GetLastError();
        // If second set fails, clipboard was already partially emptied/set with pde.
        // We empty it so no partial invalid format state lingers.
        if (active_seam.empty_clipboard) {
            active_seam.empty_clipboard();
        } else {
            ::EmptyClipboard();
        }
        return result;
    }
    // Ownership transferred to system
    static_cast<void>(hdrop_hglobal.release());

    result.status = clipboard_op_status::success;
    result.error_code = 0;
    return result;
}

clipboard_op_result copy_file_path_to_clipboard(
    HWND owner_hwnd,
    const std::filesystem::path& file_path,
    const clipboard_api_seam* seam) {
    clipboard_op_result result;

    if (!owner_hwnd) {
        result.status = clipboard_op_status::open_failed;
        result.error_code = ERROR_INVALID_WINDOW_HANDLE;
        return result;
    }

    const std::wstring norm_path = normalize_and_validate_file_path(file_path, &result.status);
    if (result.status != clipboard_op_status::success) {
        return result;
    }

    const auto text_bytes = build_unicode_text_payload(norm_path);
    if (text_bytes.empty()) {
        result.status = clipboard_op_status::payload_allocation_failed;
        return result;
    }

    scoped_hglobal text_hglobal = allocate_and_fill_hglobal(text_bytes);
    if (!text_hglobal.get()) {
        result.status = clipboard_op_status::payload_allocation_failed;
        result.error_code = ::GetLastError();
        return result;
    }

    const clipboard_api_seam active_seam = seam ? *seam : default_clipboard_api_seam();

    scoped_clipboard_lock clip_lock(owner_hwnd, active_seam);
    if (!clip_lock.is_open()) {
        result.status = clipboard_op_status::open_failed;
        result.error_code = ::GetLastError();
        return result;
    }

    const BOOL empty_ok = active_seam.empty_clipboard ? active_seam.empty_clipboard() : ::EmptyClipboard();
    if (!empty_ok) {
        result.status = clipboard_op_status::empty_failed;
        result.error_code = ::GetLastError();
        return result;
    }

    auto set_data_fn = active_seam.set_clipboard_data ? active_seam.set_clipboard_data : ::SetClipboardData;
    HANDLE h = set_data_fn(CF_UNICODETEXT, text_hglobal.get());
    if (!h) {
        result.status = clipboard_op_status::set_data_failed;
        result.error_code = ::GetLastError();
        return result;
    }
    // Ownership transferred
    static_cast<void>(text_hglobal.release());

    result.status = clipboard_op_status::success;
    result.error_code = 0;
    return result;
}

clipboard_op_result copy_image_to_clipboard(
    HWND owner_hwnd,
    const std::uint8_t* pbgra_pixels,
    std::int32_t width,
    std::int32_t height,
    std::int32_t src_stride_bytes,
    const clipboard_api_seam* seam) {
    clipboard_op_result result;

    if (!owner_hwnd) {
        result.status = clipboard_op_status::open_failed;
        result.error_code = ERROR_INVALID_WINDOW_HANDLE;
        return result;
    }

    if (!pbgra_pixels || width <= 0 || height <= 0 || width > 65535 || height > 65535) {
        result.status = clipboard_op_status::invalid_image_dimensions;
        return result;
    }

    const std::int32_t min_src_stride = width * 4;
    if (src_stride_bytes < min_src_stride) {
        result.status = clipboard_op_status::invalid_image_dimensions;
        return result;
    }

    const std::size_t row_bytes = static_cast<std::size_t>(width) * 4;
    const std::size_t pixel_bytes = row_bytes * static_cast<std::size_t>(height);
    const std::size_t total_payload_bytes = sizeof(BITMAPV5HEADER) + pixel_bytes;
    if (pixel_bytes > 0x7FFFFFFF || total_payload_bytes > 0x7FFFFFFF) {
        result.status = clipboard_op_status::memory_limit_exceeded;
        return result;
    }

    // Step 1: Unpremultiply PBGRA8 to straight BGRA8
    std::vector<std::uint8_t> straight_bgra = convert_pbgra_to_straight_bgra(pbgra_pixels, width, height, src_stride_bytes);
    if (straight_bgra.empty()) {
        result.status = clipboard_op_status::payload_allocation_failed;
        return result;
    }

    // Step 2: Build complete CF_DIBV5 byte buffer before opening clipboard
    std::vector<std::uint8_t> dibv5_payload = build_dibv5_payload_from_straight_bgra(straight_bgra.data(), width, height, static_cast<std::int32_t>(row_bytes));
    if (dibv5_payload.empty()) {
        result.status = clipboard_op_status::payload_allocation_failed;
        return result;
    }

    // Step 3: Allocate HGLOBAL with GMEM_MOVEABLE | GMEM_ZEROINIT and fill
    scoped_hglobal dib_hglobal = allocate_and_fill_hglobal(dibv5_payload);
    if (!dib_hglobal.get()) {
        result.status = clipboard_op_status::payload_allocation_failed;
        result.error_code = ::GetLastError();
        return result;
    }

    const clipboard_api_seam active_seam = seam ? *seam : default_clipboard_api_seam();

    // Step 4: Open clipboard RAII
    scoped_clipboard_lock clip_lock(owner_hwnd, active_seam);
    if (!clip_lock.is_open()) {
        result.status = clipboard_op_status::open_failed;
        result.error_code = ::GetLastError();
        return result;
    }

    // Step 5: Empty clipboard
    const BOOL empty_ok = active_seam.empty_clipboard ? active_seam.empty_clipboard() : ::EmptyClipboard();
    if (!empty_ok) {
        result.status = clipboard_op_status::empty_failed;
        result.error_code = ::GetLastError();
        return result;
    }

    // Step 6: Publish CF_DIBV5
    auto set_data_fn = active_seam.set_clipboard_data ? active_seam.set_clipboard_data : ::SetClipboardData;
    HANDLE h = set_data_fn(CF_DIBV5, dib_hglobal.get());
    if (!h) {
        result.status = clipboard_op_status::set_data_failed;
        result.error_code = ::GetLastError();
        return result;
    }

    // Ownership transferred to system only after successful SetClipboardData
    static_cast<void>(dib_hglobal.release());

    result.status = clipboard_op_status::success;
    result.error_code = 0;
    return result;
}

}  // namespace oz::viewer

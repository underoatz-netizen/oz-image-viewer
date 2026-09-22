// oz/viewer/source_identity.cpp - always-built stable file-identity helper.
//
// Moved verbatim from edit_controller.cpp so the Editing-OFF viewer (which
// builds copy_controller.cpp but excludes edit_controller.cpp) links.
// Copy Image identity behavior is unchanged; the edit worker links the same
// implementation when editing is enabled.
#include "source_identity.hpp"
#include "oz/viewer_core/codec.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <mutex>

namespace oz::viewer {

bool verify_source_file_metadata(const std::filesystem::path& path,
                                 std::uint64_t expected_size,
                                 std::uint64_t expected_last_write,
                                 std::uint64_t* out_current_size,
                                 std::uint64_t* out_current_last_write) noexcept {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0) {
        CloseHandle(h);
        return false;
    }
    const std::uint64_t cur_sz = static_cast<std::uint64_t>(sz.QuadPart);

    FILETIME ft_write{};
    if (!GetFileTime(h, nullptr, nullptr, &ft_write)) {
        CloseHandle(h);
        return false;
    }
    CloseHandle(h);

    ULARGE_INTEGER ft;
    ft.LowPart = ft_write.dwLowDateTime;
    ft.HighPart = ft_write.dwHighDateTime;
    const std::uint64_t cur_ft = ft.QuadPart;

    if (out_current_size) *out_current_size = cur_sz;
    if (out_current_last_write) *out_current_last_write = cur_ft;

    if (expected_size != 0 && cur_sz != expected_size) {
        return false;
    }
    if (expected_last_write != 0 && cur_ft != expected_last_write) {
        return false;
    }
    return true;
}

namespace {

// Helper to read file bytes safely
bool read_file_all(const std::filesystem::path& path,
                   std::vector<std::uint8_t>& out_data,
                   std::string& err_detail,
                   const std::function<bool()>& check_cancel) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err_detail = "source file open failed";
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 0 ||
        static_cast<std::uint64_t>(size.QuadPart) > oz::viewer_core::kMaxEncodedBytes) {
        CloseHandle(h);
        err_detail = "source file too large";
        return false;
    }

    out_data.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD total = 0;
    const DWORD want = static_cast<DWORD>(out_data.size());
    constexpr DWORD kChunkSize = 64 * 1024;

    while (total < want) {
        if (check_cancel()) {
            CloseHandle(h);
            err_detail = "cancelled";
            return false;
        }
        DWORD to_read = (want - total > kChunkSize) ? kChunkSize : (want - total);
        DWORD n = 0;
        if (!ReadFile(h, out_data.data() + total, to_read, &n, nullptr) || n == 0) {
            CloseHandle(h);
            err_detail = "source file read failed";
            return false;
        }
        total += n;
    }
    CloseHandle(h);
    return true;
}

// Test seam state for the stable-read boundary. Guarded by a mutex so a
// concurrent production read can never observe a half-installed hook.
std::mutex g_stable_read_hook_mutex;
std::function<void()> g_stable_read_hook;

}  // namespace

scoped_stable_read_hook::scoped_stable_read_hook(std::function<void()> hook) {
    std::lock_guard<std::mutex> lock(g_stable_read_hook_mutex);
    previous_ = std::move(g_stable_read_hook);
    g_stable_read_hook = std::move(hook);
}

scoped_stable_read_hook::~scoped_stable_read_hook() {
    std::lock_guard<std::mutex> lock(g_stable_read_hook_mutex);
    g_stable_read_hook = std::move(previous_);
}

bool read_file_stable_identity(const std::filesystem::path& path,
                               std::vector<std::uint8_t>& out_data,
                               std::uint64_t expected_size,
                               std::uint64_t expected_last_write,
                               std::uint64_t& out_stable_size,
                               std::uint64_t& out_stable_last_write,
                               std::string& out_error_detail,
                               const std::function<bool()>& check_cancel) {
    std::uint64_t before_sz = 0;
    std::uint64_t before_ft = 0;
    if (!verify_source_file_metadata(path, expected_size, expected_last_write, &before_sz, &before_ft)) {
        out_error_detail = "source file modified or inaccessible before read";
        return false;
    }

    if (!read_file_all(path, out_data, out_error_detail, check_cancel)) {
        return false;
    }

    // Test seam: deterministic mutation point at the genuine post-first-
    // snapshot/pre-second-snapshot boundary. Production code never installs
    // a hook, so this is a no-op in the viewer.
    {
        std::function<void()> hook;
        {
            std::lock_guard<std::mutex> lock(g_stable_read_hook_mutex);
            hook = g_stable_read_hook;
        }
        if (hook) {
            hook();
        }
    }

    std::uint64_t after_sz = 0;
    std::uint64_t after_ft = 0;
    if (!verify_source_file_metadata(path, before_sz, before_ft, &after_sz, &after_ft)) {
        out_error_detail = "source file modified during read";
        return false;
    }

    out_stable_size = after_sz;
    out_stable_last_write = after_ft;
    return true;
}

}  // namespace oz::viewer

#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace oz::viewer_core {

// True if the path ends with a supported still-image extension
// (.jpg/.jpeg/.png, plus .webp when static WebP is enabled,
// case-insensitive). Extension matching is a sibling
// enumeration hint only; actual decode routing is signature-first.
bool is_supported_image_extension(const std::filesystem::path& path);

// Legacy JPEG-only extension check, preserved for compatibility.
bool is_jpeg_extension(const std::filesystem::path& path);

// Natural sort comparison for filenames / paths: compares numeric components
// numerically and non-numeric components case-insensitively (deterministic).
bool natural_path_less(const std::filesystem::path& a, const std::filesystem::path& b);

class navigation_list {
public:
    navigation_list() = default;

    // Build a sorted navigation list from candidate paths filtering for
    // supported image extensions. Identifies active_path if present;
    // otherwise active_index is std::nullopt.
    static navigation_list build(const std::vector<std::filesystem::path>& candidates,
                                 const std::filesystem::path& active_path);

    bool empty() const noexcept { return items_.empty(); }
    std::size_t size() const noexcept { return items_.size(); }
    const std::vector<std::filesystem::path>& items() const noexcept { return items_; }

    std::optional<std::size_t> active_index() const noexcept { return active_index_; }

    // Path at index, or nullptr if out of bounds.
    const std::filesystem::path* at(std::size_t index) const noexcept;

    // Current active path, or nullptr if none.
    const std::filesystem::path* current() const noexcept;

    // Move active_index to previous (clamped at 0; no wrap).
    // Returns target path if index changed, or nullptr if already at start / empty / no active.
    const std::filesystem::path* select_previous() noexcept;

    // Move active_index to next (clamped at size() - 1; no wrap).
    // Returns target path if index changed, or nullptr if already at end / empty / no active.
    const std::filesystem::path* select_next() noexcept;

    // Move active_index to first item (index 0).
    // Returns target path if index changed, or nullptr if already at 0 or empty.
    const std::filesystem::path* select_first() noexcept;

    // Move active_index to last item (index size() - 1).
    // Returns target path if index changed, or nullptr if already at end or empty.
    const std::filesystem::path* select_last() noexcept;

    // Select specific index. Returns target path or nullptr if out of bounds.
    const std::filesystem::path* select_index(std::size_t index) noexcept;

private:
    std::vector<std::filesystem::path> items_;
    std::optional<std::size_t> active_index_;
};

// Enumerate sibling supported images (.jpg/.jpeg/.png [+ .webp when enabled])
// in the parent directory of target_path. Native Win32 implementation (FindFirstFileW /
// FindNextFileW). Pure data function; does not touch GUI or HWND.
// Enumeration is extension-hinted with no content probing; actual decode
// routing stays signature-first.
navigation_list enumerate_sibling_images(const std::filesystem::path& target_path);

// Legacy JPEG-only enumerator, preserved for compatibility.
navigation_list enumerate_sibling_jpegs(const std::filesystem::path& target_path);

}  // namespace oz::viewer_core

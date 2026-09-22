#include "oz/viewer_core/navigation.hpp"
#include "heif_loader.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlwapi.h>

#include <algorithm>
#include <cwctype>

#pragma comment(lib, "shlwapi.lib")

namespace oz::viewer_core {

bool is_supported_image_extension(const std::filesystem::path& path) {
    const auto ext = path.extension().native();
    if (ext.empty()) {
        return false;
    }
    // Compare wide string case-insensitively
    auto iequals = [](std::wstring_view a, std::wstring_view b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (std::towlower(static_cast<wint_t>(a[i])) != std::towlower(static_cast<wint_t>(b[i]))) {
                return false;
            }
        }
        return true;
    };
    // Discovery hints only: the codec is always selected from bytes, never
    // from the extension. The Common Raster Decode Profile v1 allowlist adds
    // the common-raster extensions; Phase B adds CUR (in-box WIC cursor
    // payload slice) and TGA (native bounded adapter), both always compiled.
    return iequals(ext, L".jpg") || iequals(ext, L".jpeg") || iequals(ext, L".png")
           || iequals(ext, L".bmp") || iequals(ext, L".gif") ||
           iequals(ext, L".tif") || iequals(ext, L".tiff") ||
           iequals(ext, L".ico") || iequals(ext, L".cur") ||
           iequals(ext, L".tga")
#if OZVIEWER_HAVE_WEBP
           || iequals(ext, L".webp")
#endif
#if OZVIEWER_HAVE_AVIF
           || iequals(ext, L".avif")
#endif
#if OZVIEWER_HAVE_HEIC
           || iequals(ext, L".heic") || iequals(ext, L".heif")
#endif
        ;
}

bool is_jpeg_extension(const std::filesystem::path& path) {
    const auto ext = path.extension().native();
    if (ext.empty()) {
        return false;
    }
    auto iequals = [](std::wstring_view a, std::wstring_view b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (std::towlower(static_cast<wint_t>(a[i])) != std::towlower(static_cast<wint_t>(b[i]))) {
                return false;
            }
        }
        return true;
    };
    return iequals(ext, L".jpg") || iequals(ext, L".jpeg");
}

bool natural_path_less(const std::filesystem::path& a, const std::filesystem::path& b) {
    // StrCmpLogicalW provides standard Windows natural sorting ("2" before "10").
    const std::wstring a_str = a.filename().native();
    const std::wstring b_str = b.filename().native();
    int cmp = StrCmpLogicalW(a_str.c_str(), b_str.c_str());
    if (cmp != 0) {
        return cmp < 0;
    }
    // Tie-breaker on full native path for deterministic ordering
    return a.native() < b.native();
}

navigation_list navigation_list::build(const std::vector<std::filesystem::path>& candidates,
                                       const std::filesystem::path& active_path) {
    navigation_list list;
    for (const auto& p : candidates) {
        if (is_supported_image_extension(p)) {
            list.items_.push_back(p);
        }
    }

    std::sort(list.items_.begin(), list.items_.end(), natural_path_less);

    // Locate active_path if provided
    if (!active_path.empty()) {
        const std::wstring active_norm = active_path.lexically_normal().native();
        for (std::size_t i = 0; i < list.items_.size(); ++i) {
            const std::wstring item_norm = list.items_[i].lexically_normal().native();
            // Case-insensitive comparison for paths on Windows
            if (_wcsicmp(active_norm.c_str(), item_norm.c_str()) == 0) {
                list.active_index_ = i;
                break;
            }
        }
    }

    return list;
}

const std::filesystem::path* navigation_list::at(std::size_t index) const noexcept {
    if (index < items_.size()) {
        return &items_[index];
    }
    return nullptr;
}

const std::filesystem::path* navigation_list::current() const noexcept {
    if (active_index_ && *active_index_ < items_.size()) {
        return &items_[*active_index_];
    }
    return nullptr;
}

const std::filesystem::path* navigation_list::select_previous() noexcept {
    if (items_.empty() || !active_index_) {
        return nullptr;
    }
    if (*active_index_ > 0) {
        --(*active_index_);
        return &items_[*active_index_];
    }
    return nullptr;
}

const std::filesystem::path* navigation_list::select_next() noexcept {
    if (items_.empty() || !active_index_) {
        return nullptr;
    }
    if (*active_index_ + 1 < items_.size()) {
        ++(*active_index_);
        return &items_[*active_index_];
    }
    return nullptr;
}

const std::filesystem::path* navigation_list::select_first() noexcept {
    if (items_.empty()) {
        return nullptr;
    }
    if (!active_index_ || *active_index_ != 0) {
        active_index_ = 0;
        return &items_[0];
    }
    return nullptr;
}

const std::filesystem::path* navigation_list::select_last() noexcept {
    if (items_.empty()) {
        return nullptr;
    }
    const std::size_t last = items_.size() - 1;
    if (!active_index_ || *active_index_ != last) {
        active_index_ = last;
        return &items_[last];
    }
    return nullptr;
}

const std::filesystem::path* navigation_list::select_index(std::size_t index) noexcept {
    if (index < items_.size()) {
        active_index_ = index;
        return &items_[index];
    }
    return nullptr;
}

navigation_list enumerate_sibling_images(const std::filesystem::path& target_path) {
    std::vector<std::filesystem::path> candidates;
    const std::filesystem::path parent = target_path.parent_path();
    const std::filesystem::path search_dir = parent.empty() ? std::filesystem::path(L".") : parent;

    std::wstring pattern = search_dir.native();
    if (!pattern.empty() && pattern.back() != L'\\' && pattern.back() != L'/') {
        pattern += L'\\';
    }
    pattern += L"*";

    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                candidates.push_back(search_dir / fd.cFileName);
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }

    return navigation_list::build(candidates, target_path);
}

navigation_list enumerate_sibling_jpegs(const std::filesystem::path& target_path) {
    // Compatibility wrapper: same extension-hinted enumeration entry point.
    return enumerate_sibling_images(target_path);
}

}  // namespace oz::viewer_core

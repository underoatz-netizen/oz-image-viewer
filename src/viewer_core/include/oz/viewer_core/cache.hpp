#pragma once

#include "oz/viewer_core/codec.hpp"
#include "oz/viewer_core/fit.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace oz::viewer_core {

// Normalizes path for case-insensitive lookup and deduplication on Windows.
inline std::wstring normalize_path_key(const std::filesystem::path& path) {
    std::wstring s = path.lexically_normal().wstring();
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return s;
}

// Key identifying a decoded image in the cache or worker queue.
struct cache_key {
    std::wstring normalized_path;
    int scale_den = 1;

    bool operator==(const cache_key& other) const noexcept {
        return scale_den == other.scale_den && normalized_path == other.normalized_path;
    }
};

struct cache_key_hash {
    std::size_t operator()(const cache_key& k) const noexcept {
        std::size_t h1 = std::hash<std::wstring>{}(k.normalized_path);
        std::size_t h2 = std::hash<int>{}(k.scale_den);
        return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
    }
};

// Priority bands for decode work:
// P0: selected / current navigation target
// P1: next (+1)
// P2: previous (-1)
// P3: next+2 (+2)
// P4: previous-2 (-2)
// P5: existing enumeration or background
enum class work_priority : int {
    p0_selected = 0,
    p1_next = 1,
    p2_prev = 2,
    p3_next2 = 3,
    p4_prev2 = 4,
    p5_enum = 5,
};

// Represents a planned item around the desired navigation index.
struct preload_plan_item {
    std::size_t index = 0;
    std::filesystem::path path;
    work_priority priority = work_priority::p0_selected;
    int scale_den = 1;
};

// Generates the deterministic +-2 preload plan given the list size, desired active index,
// and required scaling denominator.
// P0: current; P1: next (+1); P2: prev (-1); P3: next+2 (+2); P4: prev-2 (-2).
// No wrapping. Out-of-bounds indices are omitted.
inline std::vector<preload_plan_item> compute_preload_plan(
    std::size_t total_items,
    std::size_t active_index,
    const std::vector<std::filesystem::path>& items,
    int scale_den) {
    std::vector<preload_plan_item> plan;
    if (total_items == 0 || active_index >= total_items || items.size() < total_items) {
        return plan;
    }

    // P0: current
    plan.push_back({active_index, items[active_index], work_priority::p0_selected, scale_den});

    // P1: next (+1)
    if (active_index + 1 < total_items) {
        plan.push_back({active_index + 1, items[active_index + 1], work_priority::p1_next, scale_den});
    }

    // P2: previous (-1)
    if (active_index > 0) {
        plan.push_back({active_index - 1, items[active_index - 1], work_priority::p2_prev, scale_den});
    }

    // P3: next+2 (+2)
    if (active_index + 2 < total_items) {
        plan.push_back({active_index + 2, items[active_index + 2], work_priority::p3_next2, scale_den});
    }

    // P4: previous-2 (-2)
    if (active_index >= 2) {
        plan.push_back({active_index - 2, items[active_index - 2], work_priority::p4_prev2, scale_den});
    }

    return plan;
}

// Decoded image representation shared between worker, cache, and renderer.
// Canonical profile: top-down SDR sRGB premultiplied BGRA8 (Stage 3.1
// codec registry contract; opaque JPEG pixels carry alpha 255, which is
// identical in straight and premultiplied form).
// scale_den is the actual scaling denominator used for this decode
// (1, 2, 4 or 8). src_width/src_height are the orientation-adjusted source
// dimensions before scaling; they allow the UI to recompute the required
// denominator for the current viewport without file/header I/O. Legacy
// entries with src dims 0 are treated as profile-agnostic for backward
// compatibility.
struct decoded_image_data {
    std::vector<std::uint8_t> pixels;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t stride = 0;
    int scale_den = 1;
    std::int32_t src_width = 0;
    std::int32_t src_height = 0;
    codec_id codec = codec_id::jpeg;
    std::uint64_t source_file_size = 0;
    std::uint64_t source_last_write_time = 0;

    // Checked byte footprint using pixels.capacity()
    std::size_t byte_size() const noexcept {
        const std::size_t cap = pixels.capacity();
        const std::size_t struct_overhead = sizeof(decoded_image_data);
        if (SIZE_MAX - cap < struct_overhead) {
            return SIZE_MAX;
        }
        return cap + struct_overhead;
    }
};

// True when the cached image's actual decode profile matches the denominator
// required by the current viewport. Uses the image's own source dimensions so
// no file/header I/O is needed. Legacy entries without source dims are
// accepted as a match to preserve prior behavior.
inline bool image_profile_matches_viewport(const decoded_image_data& img,
                                           std::int32_t view_w,
                                           std::int32_t view_h) noexcept {
    if (img.src_width > 0 && img.src_height > 0) {
        if (img.scale_den != 1 && img.scale_den != 2 && img.scale_den != 4 &&
            img.scale_den != 8) {
            return false;
        }
        return img.scale_den ==
               choose_scaling_denominator(img.src_width, img.src_height, view_w, view_h);
    }
    return true;
}

// Adaptive CPU decoded-image cache policy and storage.
// Budget rule: min(512 MiB, floor(available_phys_bytes / 10)).
inline std::size_t compute_cpu_cache_budget(std::uint64_t available_phys_bytes) noexcept {
    constexpr std::size_t kMaxBudget = 512ull * 1024ull * 1024ull;  // 512 MiB
    const std::uint64_t tenth = available_phys_bytes / 10ull;
    const std::uint64_t clamped = (std::min)(static_cast<std::uint64_t>(kMaxBudget), tenth);
    return static_cast<std::size_t>(clamped);
}

class cpu_image_cache {
public:
    explicit cpu_image_cache(std::size_t budget_bytes = 0)
        : budget_bytes_(budget_bytes) {}

    void set_budget(std::size_t bytes) noexcept {
        budget_bytes_ = bytes;
    }

    std::size_t budget() const noexcept { return budget_bytes_; }
    std::size_t used_bytes() const noexcept { return current_bytes_; }
    // Number of cached (path, decode-profile) entries.
    std::size_t size() const noexcept { return map_.size(); }

    // Number of distinct decoded source images currently resident. This is a
    // different quantity from size(): a path with both a full-resolution and a
    // scaled variant counts once here and twice in size(). Used by the
    // navigation benchmark so cache_entry_count and resident_decoded_count are
    // never reported as the same number.
    std::size_t resident_image_count() const {
        std::unordered_set<std::wstring> paths;
        paths.reserve(map_.size());
        for (const auto& kv : map_) {
            paths.insert(kv.first.normalized_path);
        }
        return paths.size();
    }

    bool preloads_disabled() const noexcept { return preloads_disabled_; }

    // Pin a specific key as the active/current displayed image.
    // Pinned image will not be evicted unless unpinned or replaced.
    void pin_current(const cache_key& key) {
        pinned_key_ = key;
    }

    void unpin() noexcept {
        pinned_key_.reset();
    }

    const std::optional<cache_key>& pinned_key() const noexcept {
        return pinned_key_;
    }

    // Lookup matching source path and scaling denominator.
    // Touches LRU order on hit.
    std::shared_ptr<decoded_image_data> get(const cache_key& key) {
        auto it = map_.find(key);
        if (it == map_.end()) {
            return nullptr;
        }
        // Move to most-recently-used (front)
        lru_list_.splice(lru_list_.begin(), lru_list_, it->second.list_iter);
        return it->second.image;
    }

    // Lookup the cached image for a path whose actual profile matches the
    // current viewport. Scans scale variants for the path and accepts only
    // the entry whose profile matches; touches LRU on hit. Returns nullptr
    // when no variant matches (viewport change or miss) without file I/O.
    std::shared_ptr<decoded_image_data> find_for_viewport(
        const std::wstring& normalized_path, std::int32_t view_w,
        std::int32_t view_h) {
        for (auto& kv : map_) {
            if (kv.first.normalized_path != normalized_path) {
                continue;
            }
            if (kv.second.image &&
                image_profile_matches_viewport(*kv.second.image, view_w, view_h)) {
                lru_list_.splice(lru_list_.begin(), lru_list_, kv.second.list_iter);
                return kv.second.image;
            }
        }
        return nullptr;
    }

    // Key for the viewport-matching entry, if any (for pinning).
    std::optional<cache_key> find_key_for_viewport(
        const std::wstring& normalized_path, std::int32_t view_w,
        std::int32_t view_h) const {
        for (const auto& kv : map_) {
            if (kv.first.normalized_path != normalized_path) {
                continue;
            }
            if (kv.second.image &&
                image_profile_matches_viewport(*kv.second.image, view_w, view_h)) {
                return kv.first;
            }
        }
        return std::nullopt;
    }

    // Source dimensions for a path from any cached variant (invariant per file).
    bool try_get_source_dims(const std::wstring& normalized_path, std::int32_t& out_w,
                             std::int32_t& out_h) const {
        for (const auto& kv : map_) {
            if (kv.first.normalized_path != normalized_path) {
                continue;
            }
            const auto& img = kv.second.image;
            if (img && img->src_width > 0 && img->src_height > 0) {
                out_w = img->src_width;
                out_h = img->src_height;
                return true;
            }
        }
        return false;
    }

    // Check if admitting a preload of estimated_bytes would exceed budget.
    // If current alone exceeds budget, preloads are disabled.
    bool can_admit_preload(std::size_t estimated_bytes) const noexcept {
        if (preloads_disabled_) {
            return false;
        }
        if (estimated_bytes > budget_bytes_) {
            return false;
        }
        // If admitted, would eviction of unpinned allow it?
        // Count unpinned bytes that can be evicted
        std::size_t pinned_bytes = 0;
        if (pinned_key_) {
            auto it = map_.find(*pinned_key_);
            if (it != map_.end()) {
                pinned_bytes = it->second.bytes;
            }
        }
        if (pinned_bytes + estimated_bytes > budget_bytes_) {
            return false;
        }
        return true;
    }

    // Insert or update decoded image in cache.
    // is_current: true if this is the active/displayed image.
    // desired_window: keys within latest +-2 navigation window.
    void insert(const cache_key& key,
                std::shared_ptr<decoded_image_data> image,
                bool is_current,
                const std::vector<cache_key>& desired_window = {}) {
        if (!image) {
            return;
        }

        const std::size_t img_bytes = image->byte_size();

        if (is_current) {
            if (img_bytes > budget_bytes_) {
                // Oversized current: retain it alone as the sole pinned entry.
                // Clear first while the new key is not yet in the map so the
                // pin is not lost, then fall through to emplace below.
                map_.clear();
                lru_list_.clear();
                current_bytes_ = 0;
                pinned_key_ = key;
                preloads_disabled_ = true;
            } else {
                pinned_key_ = key;
                preloads_disabled_ = false;
            }
        } else {
            // Preload entry
            if (preloads_disabled_ || img_bytes > budget_bytes_) {
                return;  // Do not admit preload if preloads disabled or alone exceeds budget
            }
        }

        // If existing entry, subtract old size
        auto it = map_.find(key);
        if (it != map_.end()) {
            current_bytes_ = (current_bytes_ >= it->second.bytes) ? (current_bytes_ - it->second.bytes) : 0;
            lru_list_.erase(it->second.list_iter);
            map_.erase(it);
        }

        // Evict if needed to make room for img_bytes
        evict_to_fit(img_bytes, desired_window);

        // One in-flight decoded result may transiently exceed settled budget if current
        lru_list_.push_front(key);
        entry e;
        e.image = image;
        e.bytes = img_bytes;
        e.list_iter = lru_list_.begin();
        map_.emplace(key, std::move(e));
        current_bytes_ += img_bytes;
    }

    // Evicts entries outside desired window, then lowest priority/farthest, then LRU,
    // until used_bytes + needed_bytes <= budget_bytes_ (pinned entry is never evicted).
    // Window membership is path-based (ignores scale_den) so per-image decode
    // profiles derived by the worker remain protected without UI-side header I/O.
    void evict_to_fit(std::size_t needed_bytes, const std::vector<cache_key>& desired_window) {
        if (current_bytes_ + needed_bytes <= budget_bytes_) {
            return;
        }

        auto in_window = [&](const cache_key& k) {
            for (const auto& w : desired_window) {
                if (w.normalized_path == k.normalized_path) return true;
            }
            return false;
        };

        // Pass 1: Evict unpinned items outside desired_window (from back of LRU)
        auto it = lru_list_.rbegin();
        while (it != lru_list_.rend() && current_bytes_ + needed_bytes > budget_bytes_) {
            const cache_key& k = *it;
            if ((!pinned_key_ || !(*pinned_key_ == k)) && !in_window(k)) {
                auto map_it = map_.find(k);
                if (map_it != map_.end()) {
                    current_bytes_ = (current_bytes_ >= map_it->second.bytes) ? (current_bytes_ - map_it->second.bytes) : 0;
                    map_.erase(map_it);
                }
                auto to_erase = std::next(it).base();
                it = std::reverse_iterator(lru_list_.erase(to_erase));
            } else {
                ++it;
            }
        }

        // Pass 2: If still needed, evict unpinned items inside desired_window
        // Prioritize farthest in desired_window (i.e. P4, P3, P2, P1)
        // desired_window ordering: 0 is P0, 1 is P1, 2 is P2, 3 is P3, 4 is P4
        // Evict from back of desired_window towards front (path-based so any
        // scale variant of the window path is considered).
        for (auto w_it = desired_window.rbegin(); w_it != desired_window.rend() && current_bytes_ + needed_bytes > budget_bytes_; ++w_it) {
            const std::wstring& want_path = w_it->normalized_path;
            // Collect unpinned variants for this path first.
            std::vector<cache_key> variants;
            for (const auto& kv : map_) {
                if (kv.first.normalized_path == want_path &&
                    (!pinned_key_ || !(*pinned_key_ == kv.first))) {
                    variants.push_back(kv.first);
                }
            }
            for (const auto& k : variants) {
                if (current_bytes_ + needed_bytes <= budget_bytes_) {
                    break;
                }
                auto map_it = map_.find(k);
                if (map_it != map_.end()) {
                    current_bytes_ = (current_bytes_ >= map_it->second.bytes) ? (current_bytes_ - map_it->second.bytes) : 0;
                    lru_list_.erase(map_it->second.list_iter);
                    map_.erase(map_it);
                }
            }
        }

        // Pass 3: Any remaining unpinned items (LRU order)
        it = lru_list_.rbegin();
        while (it != lru_list_.rend() && current_bytes_ + needed_bytes > budget_bytes_) {
            const cache_key& k = *it;
            if (!pinned_key_ || !(*pinned_key_ == k)) {
                auto map_it = map_.find(k);
                if (map_it != map_.end()) {
                    current_bytes_ = (current_bytes_ >= map_it->second.bytes) ? (current_bytes_ - map_it->second.bytes) : 0;
                    map_.erase(map_it);
                }
                auto to_erase = std::next(it).base();
                it = std::reverse_iterator(lru_list_.erase(to_erase));
            } else {
                ++it;
            }
        }
    }

    void clear_except_pinned() {
        if (!pinned_key_) {
            map_.clear();
            lru_list_.clear();
            current_bytes_ = 0;
            return;
        }
        auto it = map_.find(*pinned_key_);
        if (it == map_.end()) {
            map_.clear();
            lru_list_.clear();
            current_bytes_ = 0;
            pinned_key_.reset();
            return;
        }
        entry pinned_entry = std::move(it->second);
        cache_key pinned_k = *pinned_key_;
        map_.clear();
        lru_list_.clear();
        lru_list_.push_front(pinned_k);
        pinned_entry.list_iter = lru_list_.begin();
        current_bytes_ = pinned_entry.bytes;
        map_.emplace(pinned_k, std::move(pinned_entry));
    }

    void clear() noexcept {
        map_.clear();
        lru_list_.clear();
        current_bytes_ = 0;
        pinned_key_.reset();
        preloads_disabled_ = false;
    }

private:
    struct entry {
        std::shared_ptr<decoded_image_data> image;
        std::size_t bytes = 0;
        std::list<cache_key>::iterator list_iter;
    };

    std::size_t budget_bytes_ = 0;
    std::size_t current_bytes_ = 0;
    bool preloads_disabled_ = false;
    std::optional<cache_key> pinned_key_;
    std::list<cache_key> lru_list_;
    std::unordered_map<cache_key, entry, cache_key_hash> map_;
};

}  // namespace oz::viewer_core

// oz/viewer/recycle_delete.hpp - Stage 9.7 post-recycle navigation policy.
//
// Pure, dependency-free policy layer between the Shell Recycle Bin boundary
// (shell_integration.hpp) and the Delete wiring in main.cpp. After the shell
// reports a successful recycle, the viewer navigates deterministically:
//
//   - next image when one follows the recycled file in presentation order,
//   - previous image when the recycled file was last,
//   - blank state when the recycled file was the only entry (or no navigation
//     entries exist).
//
// No filesystem access, no decode, no presentation work, and no async
// scheduling happen here. The caller supplies the pre-delete navigation
// snapshot in presentation order; these helpers never reorder, never throw,
// and preserve Unicode paths verbatim (std::filesystem::path equality only).
#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <vector>

namespace oz::viewer {

// Deterministic successor for a successfully recycled document.
struct recycle_successor {
    bool has_successor = false;
    std::filesystem::path path{};
};

// Choose the post-recycle successor from a pre-delete navigation snapshot.
// `items` is in presentation order and `deleted_index` is the position of the
// recycled file within it. Out-of-range input (including an empty list)
// yields no successor (blank state); callers must only pass the genuine
// deleted position. Never throws.
[[nodiscard]] inline recycle_successor decide_recycle_successor(
    const std::vector<std::filesystem::path>& items,
    std::size_t deleted_index) noexcept {
    recycle_successor out;
    try {
        if (items.empty() || deleted_index >= items.size()) {
            return out;
        }
        if (items.size() == 1) {
            return out;  // sole entry recycled -> blank state
        }
        if (deleted_index + 1 < items.size()) {
            out.has_successor = true;
            out.path = items[deleted_index + 1];
            return out;
        }
        // Recycled file was last -> previous entry.
        out.has_successor = true;
        out.path = items[deleted_index - 1];
        return out;
    } catch (...) {
        out.has_successor = false;
        out.path.clear();
        return out;
    }
}

// Locate the recycled path within a navigation snapshot by path equality.
// Returns nullopt when absent (stale or pathless navigation). Never throws.
[[nodiscard]] inline std::optional<std::size_t> find_recycled_index(
    const std::vector<std::filesystem::path>& items,
    const std::filesystem::path& deleted) noexcept {
    try {
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (items[i] == deleted) {
                return i;
            }
        }
    } catch (...) {
    }
    return std::nullopt;
}

// Remove exactly the first entry equal to `deleted`, preserving the order of
// the survivors (drop-order and sibling-sorted lists alike). A path that is
// absent yields the input unchanged. Never throws: an append fault stops with
// the prefix kept.
[[nodiscard]] inline std::vector<std::filesystem::path> prune_recycled_path(
    const std::vector<std::filesystem::path>& items,
    const std::filesystem::path& deleted) noexcept {
    std::vector<std::filesystem::path> kept;
    try {
        kept.reserve(items.size());
    } catch (...) {
        return kept;
    }
    bool removed = false;
    for (const auto& entry : items) {
        bool match = false;
        try {
            match = (!removed && entry == deleted);
        } catch (...) {
            match = false;
        }
        if (match) {
            removed = true;
            continue;
        }
        try {
            kept.push_back(entry);
        } catch (...) {
            break;
        }
    }
    return kept;
}

}  // namespace oz::viewer

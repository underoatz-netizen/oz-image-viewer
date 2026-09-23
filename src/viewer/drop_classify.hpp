// oz/viewer/drop_classify.hpp - Stage 9.6A multi-drop classification seam.
//
// Deterministic, side-effect-free classification of a WM_DROPFILES path batch
// into the approved navigation-policy outcome:
//
//   (1) multiple dropped files  -> bounded temporary navigation list that
//       preserves the original drop order (no sorting here);
//   (2) exactly one folder      -> folder intent only; Stage 9.6B owns the
//       non-recursive supported-image enumeration (natural-sorted) and the
//       WM_DROPFILES wiring, so this seam never touches the filesystem;
//   (3) mixed files and folders -> clear rejection.
//
// Contract notes:
//   - Pure classifier: the caller supplies each entry's kind (file vs folder)
//     from a bounded attribute query at the future 9.6B call site. This seam
//     performs no attribute reads, no folder listing, no sorting, no decode,
//     no presentation work, and starts no async work.
//   - Single-file results preserve the existing WM_DROPFILES behavior: the
//     caller opens that path exactly as today.
//   - Bounds: at most kMaxDropEntries input entries; the output list can never
//     exceed the input, so one bound covers both. Over-limit input is rejected
//     with `overflow` and yields no partial list.
//   - Validation order is structural first: empty input, over-limit input,
//     then per-entry shape (unknown kind or empty path), then the file/folder
//     shape policy. The first failure wins; `out` fields stay empty unless the
//     status is `ok`.
//   - File lists dedupe case-insensitively (Windows path semantics),
//     keeping the first occurrence so the surviving order is still the
//     original drop order.
#pragma once

#include <cstddef>
#include <cwctype>
#include <filesystem>
#include <vector>

namespace oz::viewer {

// Maximum accepted drop entries (input) and navigation items (output).
constexpr std::size_t kMaxDropEntries = 1024;

// Caller-supplied entry shape. `unknown` covers anything that is neither a
// regular file nor a folder (vanished paths, devices, reparse targets the
// caller could not resolve, ...); such entries reject the whole batch.
enum class drop_entry_kind {
    file,
    directory,
    unknown
};

struct drop_entry {
    std::filesystem::path path;
    drop_entry_kind kind = drop_entry_kind::unknown;
};

// Policy outcome for an accepted batch.
enum class drop_intent {
    none,         // rejected batch; see status
    single_file,  // one file: open it via the existing single-file path
    file_list,    // several files: temporary navigation list in drop order
    folder        // exactly one folder: Stage 9.6B enumerates it on demand
};

enum class drop_classify_status {
    ok,
    empty,             // no entries at all
    invalid_entry,     // an entry has an unknown kind or an empty path
    mixed,             // files and folders combined
    multiple_folders,  // more than one folder and no files
    overflow           // more than kMaxDropEntries entries
};

struct drop_classify_result {
    drop_classify_status status = drop_classify_status::empty;
    drop_intent intent = drop_intent::none;
    std::vector<std::filesystem::path> files;  // drop order, deduped
    std::filesystem::path folder;              // set only for folder intent

    [[nodiscard]] constexpr bool ok() const noexcept {
        return status == drop_classify_status::ok;
    }
};

namespace drop_detail {

[[nodiscard]] inline bool path_equal_folded(const std::filesystem::path& a,
                                            const std::filesystem::path& b) noexcept {
    try {
        const std::wstring an = a.lexically_normal().native();
        const std::wstring bn = b.lexically_normal().native();
        if (an.size() != bn.size()) {
            return false;
        }
        for (std::size_t i = 0; i < an.size(); ++i) {
            if (std::towlower(static_cast<wint_t>(an[i])) !=
                std::towlower(static_cast<wint_t>(bn[i]))) {
                return false;
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace drop_detail

// Classifies a drop batch per the policy above. Never throws: path
// comparisons are guarded and the only allocations are the bounded output
// list, whose faults report `overflow` with empty fields.
[[nodiscard]] inline drop_classify_result classify_drop_paths(
    const std::vector<drop_entry>& entries) {
    drop_classify_result out;
    if (entries.empty()) {
        out.status = drop_classify_status::empty;
        return out;
    }
    if (entries.size() > kMaxDropEntries) {
        out.status = drop_classify_status::overflow;
        return out;
    }

    std::size_t file_count = 0;
    std::size_t dir_count = 0;
    for (const auto& entry : entries) {
        if (entry.kind != drop_entry_kind::file && entry.kind != drop_entry_kind::directory) {
            out.status = drop_classify_status::invalid_entry;
            return out;
        }
        if (entry.path.empty()) {
            out.status = drop_classify_status::invalid_entry;
            return out;
        }
        if (entry.kind == drop_entry_kind::file) {
            ++file_count;
        } else {
            ++dir_count;
        }
    }

    if (file_count > 0 && dir_count > 0) {
        out.status = drop_classify_status::mixed;
        return out;
    }
    if (dir_count > 1) {
        out.status = drop_classify_status::multiple_folders;
        return out;
    }
    if (dir_count == 1) {
        // Exactly one entry (a lone folder: files are absent by the guards).
        out.status = drop_classify_status::ok;
        out.intent = drop_intent::folder;
        out.folder = entries[0].path;
        return out;
    }
    if (file_count == 1) {
        // Exactly one entry (a lone file): preserve the single-file path.
        out.status = drop_classify_status::ok;
        out.intent = drop_intent::single_file;
        out.files.push_back(entries[0].path);
        return out;
    }

    // Several files: bounded temporary list in original drop order,
    // deduped case-insensitively keeping the first occurrence.
    std::vector<std::filesystem::path> kept;
    try {
        kept.reserve(entries.size());
        for (const auto& entry : entries) {
            bool seen = false;
            for (const auto& prior : kept) {
                if (drop_detail::path_equal_folded(prior, entry.path)) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                kept.push_back(entry.path);
            }
        }
    } catch (...) {
        out.status = drop_classify_status::overflow;
        return out;
    }
    out.status = drop_classify_status::ok;
    out.intent = drop_intent::file_list;
    out.files = std::move(kept);
    return out;
}

}  // namespace oz::viewer

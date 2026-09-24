// oz/viewer/drop_flow.hpp - Stage 9.6B drop-open policy helpers.
//
// Pure, dependency-free policy layer between the approved Stage 9.6A
// classifier (drop_classify.hpp) and the WM_DROPFILES wiring in main.cpp:
//   - explicit user-facing rejection text for every rejected batch shape,
//     including the two supported-image shortfalls (a file batch or a
//     folder yielding no openable image);
//   - drop-order supported-image selection for multi-file batches.
//
// No attribute reads, no folder listing, no sorting, no decode, no
// presentation work, and no async scheduling happen here. The caller
// supplies openability through a predicate so this header stays free of
// codec and extension knowledge (production passes the shared
// supported-extension check; tests inject a stub).
#pragma once

#include <filesystem>
#include <vector>

#include "drop_classify.hpp"

namespace oz::viewer {

// User-facing text for a rejected drop batch. All pointers refer to string
// literals; `title` is always the application title.
struct drop_reject_text {
    const wchar_t* title = L"OZ Image Viewer";
    const wchar_t* headline = L"Unable to open the dropped items.";
    const wchar_t* detail = nullptr;
};

// True for classifier outcomes that deserve an explicit prompt. `ok`
// proceeds to open, and `empty` is silently ignored (nothing was dropped).
[[nodiscard]] inline bool is_promptable_drop_rejection(
    drop_classify_status status) noexcept {
    return status == drop_classify_status::invalid_entry ||
           status == drop_classify_status::mixed ||
           status == drop_classify_status::multiple_folders ||
           status == drop_classify_status::overflow;
}

// Explicit rejection text for a promptable classifier outcome. Callers must
// only invoke this for statuses where is_promptable_drop_rejection holds;
// other inputs yield an empty headline and no detail.
[[nodiscard]] inline drop_reject_text describe_drop_rejection(
    drop_classify_status status) {
    drop_reject_text out;
    switch (status) {
        case drop_classify_status::invalid_entry:
            out.headline = L"One or more dropped items could not be read.";
            out.detail = L"The drop was ignored. Nothing was opened.";
            break;
        case drop_classify_status::mixed:
            out.headline = L"Files and folders cannot be dropped together.";
            out.detail = L"Drop image files, or drop a single folder.";
            break;
        case drop_classify_status::multiple_folders:
            out.headline = L"Only one folder can be dropped at a time.";
            out.detail = L"Drop a single folder, or drop image files.";
            break;
        case drop_classify_status::overflow:
            out.headline = L"Too many items were dropped.";
            out.detail = L"The drop exceeds the item limit and was ignored.";
            break;
        case drop_classify_status::ok:
        case drop_classify_status::empty:
            out.headline = L"";
            out.detail = nullptr;
            break;
    }
    return out;
}

// Explicit rejection text when an accepted batch yields no supported image:
// a multi-file batch whose entries are all outside the supported set, or a
// folder whose listing holds no supported image. The current document is
// retained in both cases.
[[nodiscard]] inline drop_reject_text describe_no_supported_images(bool folder_drop) {
    drop_reject_text out;
    if (folder_drop) {
        out.headline = L"The folder contains no supported images.";
    } else {
        out.headline = L"None of the dropped files are supported images.";
    }
    out.detail = L"Nothing was opened.";
    return out;
}

// Explicit rejection text when the viewer cannot accept a new drop because
// the bounded async-worker retirement budget is exhausted: a superseded
// folder/sibling traversal has not finished winding down (or the reaper could
// not start). The current document and navigation are retained and the drop is
// ignored; it can be retried once the backlog drains.
[[nodiscard]] inline drop_reject_text describe_drop_overload() {
    drop_reject_text out;
    out.headline = L"The viewer is busy finishing previous work.";
    out.detail = L"The drop was ignored. Try again in a moment.";
    return out;
}

// Keep the drop order, retaining only entries the caller deems openable.
// Order and duplicates are the classifier's policy; this helper never
// reorders, never dedupes, and never throws: a predicate fault treats that
// entry as not openable, and an append fault stops with the prefix kept.
template <typename IsSupported>
[[nodiscard]] inline std::vector<std::filesystem::path> select_supported_in_drop_order(
    const std::vector<std::filesystem::path>& files, IsSupported&& is_supported) {
    std::vector<std::filesystem::path> kept;
    for (const auto& path : files) {
        bool supported = false;
        try {
            supported = static_cast<bool>(is_supported(path));
        } catch (...) {
            supported = false;
        }
        if (supported) {
            try {
                kept.push_back(path);
            } catch (...) {
                break;
            }
        }
    }
    return kept;
}

}  // namespace oz::viewer

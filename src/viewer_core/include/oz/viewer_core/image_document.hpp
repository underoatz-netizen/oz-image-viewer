// oz/viewer_core/image_document.hpp - lightweight domain image document state
// (Stage 5 Slice 1; Slice 6 saved-active-command checkpoint; Sol audit authoritative source baseline).
#pragma once

#include "oz/viewer_core/codec.hpp"
#include "oz/viewer_core/edit_state.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace oz::viewer_core {

// Monotonically increasing document instance ID for tracking document lifecycle across opens/drops.
inline std::uint64_t generate_next_document_instance_id() noexcept {
    static std::atomic<std::uint64_t> s_id{1};
    return s_id.fetch_add(1, std::memory_order_relaxed);
}

// Authoritative source baseline recorded at document construction or updated
// upon confirmed successful in-place Save.
struct document_source_baseline {
    std::filesystem::path path{};
    std::uint64_t file_size = 0;
    std::uint64_t last_write_time = 0;
    bool valid = false;
};

// Presentation/source metadata associated with a loaded document.
struct document_metadata {
    std::filesystem::path source_path{};
    std::uint64_t file_size = 0;
    std::uint64_t last_write_time = 0;
    edit_dimensions source_dimensions{};
    codec_id codec = codec_id::jpeg;
    std::string codec_name{};
};

// Pure domain document model for Stage 5 editing foundation.
// Holds the document instance identity, source metadata, authoritative source baseline,
// edit history, and the saved-active-command checkpoint. Does not own HWND, Direct2D/D3D device,
// render target, worker threads, or cache.
//
// Dirty semantics (Slice 6): a document is dirty when its current active
// command sequence differs by command-value equality from the saved checkpoint.
// The initial checkpoint is empty, so an unedited document is clean. Undo/redo
// that lands on the same command values as the checkpoint is clean even at a
// later revision; moving the history cursor alone never changes dirty state.
class image_document {
public:
    image_document()
        : instance_id_(generate_next_document_instance_id()) {}

    explicit image_document(document_metadata meta)
        : instance_id_(generate_next_document_instance_id()),
          metadata_(std::move(meta)),
          source_baseline_{metadata_.source_path, metadata_.file_size, metadata_.last_write_time,
                           !metadata_.source_path.empty() &&
                               (metadata_.file_size != 0 || metadata_.last_write_time != 0)},
          history_(metadata_.source_dimensions) {}

    [[nodiscard]] std::uint64_t instance_id() const noexcept {
        return instance_id_;
    }

    [[nodiscard]] const document_metadata& metadata() const noexcept {
        return metadata_;
    }

    [[nodiscard]] const document_source_baseline& source_baseline() const noexcept {
        return source_baseline_;
    }

    [[nodiscard]] const edit_history& history() const noexcept {
        return history_;
    }

    [[nodiscard]] edit_history& history() noexcept {
        return history_;
    }

    [[nodiscard]] bool is_dirty() const noexcept {
        const auto active = history_.active_commands();
        if (active.size() != saved_commands_.size()) {
            return true;
        }
        return !std::equal(active.begin(), active.end(), saved_commands_.begin());
    }

    // Exact active-command snapshot recorded by the last successful save to the
    // current source. Empty until the first such save.
    [[nodiscard]] std::span<const edit_command> saved_commands() const noexcept {
        return saved_commands_;
    }

    // Record the current active command sequence as the clean checkpoint.
    // Never clears, rewrites, or truncates edit history/revisions.
    void mark_saved() {
        const auto active = history_.active_commands();
        saved_commands_.assign(active.begin(), active.end());
    }

    // Atomically advance the clean checkpoint and authoritative source baseline
    // together using the committed output fingerprint from a confirmed same-source save.
    void mark_saved_with_baseline(std::uint64_t file_size,
                                  std::uint64_t last_write_time) noexcept {
        const auto active = history_.active_commands();
        saved_commands_.assign(active.begin(), active.end());
        metadata_.file_size = file_size;
        metadata_.last_write_time = last_write_time;
        source_baseline_.file_size = file_size;
        source_baseline_.last_write_time = last_write_time;
        source_baseline_.valid = true;
    }

    // Refresh the on-disk source fingerprint and authoritative baseline after
    // confirmed in-place save. Pixels, history, and the saved checkpoint are untouched.
    void update_source_file_metadata(std::uint64_t file_size,
                                     std::uint64_t last_write_time) noexcept {
        metadata_.file_size = file_size;
        metadata_.last_write_time = last_write_time;
        source_baseline_.file_size = file_size;
        source_baseline_.last_write_time = last_write_time;
        source_baseline_.valid = true;
    }

    [[nodiscard]] std::uint64_t edit_revision() const noexcept {
        return history_.revision();
    }

    [[nodiscard]] std::optional<edit_dimensions> current_dimensions() const noexcept {
        return history_.current_dimensions();
    }

    // Reset document to new metadata, clear history, and reset the checkpoint.
    void reset(document_metadata meta) {
        instance_id_ = generate_next_document_instance_id();
        metadata_ = std::move(meta);
        source_baseline_ = {metadata_.source_path, metadata_.file_size, metadata_.last_write_time,
                            !metadata_.source_path.empty() &&
                                (metadata_.file_size != 0 || metadata_.last_write_time != 0)};
        saved_commands_.clear();
        history_.set_source_dimensions(metadata_.source_dimensions);
    }

    void reset_to_empty() {
        instance_id_ = generate_next_document_instance_id();
        metadata_ = {};
        source_baseline_ = {};
        saved_commands_.clear();
        history_.clear();
    }

private:
    std::uint64_t instance_id_ = 0;
    document_metadata metadata_{};
    document_source_baseline source_baseline_{};
    edit_history history_{};
    std::vector<edit_command> saved_commands_{};
};

}  // namespace oz::viewer_core

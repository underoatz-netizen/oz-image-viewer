// oz/viewer_core/document_state.hpp - UI-thread document state and generation model (Stage 4).
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace oz::viewer_core {

// Document generation is a monotonic integer owned strictly by the UI thread.
// It is incremented on every user Open/drop request.
// Background workers and completions carry the generation under which they were launched;
// completions whose generation does not match the active generation are rejected.
struct document_state {
    std::uint64_t generation = 1;
    bool has_document = false;
    std::filesystem::path active_path;

    std::uint64_t next_generation() noexcept {
        return ++generation;
    }

    void reset_to_empty() noexcept {
        ++generation;
        has_document = false;
        active_path.clear();
    }

    void commit_document(const std::filesystem::path& path) noexcept {
        has_document = true;
        active_path = path;
    }
};

}  // namespace oz::viewer_core

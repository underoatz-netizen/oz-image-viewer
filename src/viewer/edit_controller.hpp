// oz/viewer/edit_controller.hpp - dedicated latest-only edit worker and materialization controller (Stage 5 Slice 3).
#pragma once

#include "oz/viewer_core/edit_pipeline.hpp"
#include "oz/viewer_core/edit_state.hpp"
#include "source_identity.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace oz::viewer {

// Canonical full-resolution source image retained for editing
struct edit_source_buffer {
    std::uint64_t document_instance_id = 0;
    std::filesystem::path source_path{};
    std::uint64_t file_size = 0;
    std::uint64_t last_write_time = 0;
    oz::viewer_core::edit_dimensions dimensions{};
    std::int32_t stride = 0;
    std::vector<std::uint8_t> pixels; // canonical PBGRA8

    [[nodiscard]] bool is_valid() const noexcept {
        return document_instance_id != 0 &&
               dimensions.is_valid() &&
               stride >= dimensions.width * 4 &&
               pixels.size() >= static_cast<std::size_t>(stride) * static_cast<std::size_t>(dimensions.height);
    }

    [[nodiscard]] oz::viewer_core::edit_source_view make_view() const noexcept {
        return oz::viewer_core::edit_source_view{
            .pixels = std::span<const std::uint8_t>(pixels.data(), pixels.size()),
            .dimensions = dimensions,
            .stride = stride
        };
    }
};

// Completion result dispatched to UI thread
struct edit_completion_msg {
    std::uint64_t document_instance_id = 0;
    std::uint64_t edit_revision = 0;
    std::uint64_t request_id = 0;
    bool ok = false;
    std::string error_detail;

    // If source acquisition was performed / completed
    std::shared_ptr<const edit_source_buffer> source;

    // If edit materialization was performed / completed
    oz::viewer_core::edit_pipeline_result result;
};

// Work request sent to the edit worker
struct edit_work_request {
    std::uint64_t document_instance_id = 0;
    std::uint64_t edit_revision = 0;
    std::uint64_t request_id = 0;

    // Source acquisition details
    bool needs_source_acquisition = false;
    std::filesystem::path source_path;
    std::uint64_t expected_file_size = 0;
    std::uint64_t expected_last_write_time = 0;
    oz::viewer_core::edit_dimensions expected_dimensions{};

    // Materialization details
    bool needs_materialization = false;
    std::shared_ptr<const edit_source_buffer> existing_source;
    std::vector<oz::viewer_core::edit_command> commands;
    std::size_t memory_budget = 0;
};

// Controller managing background edit acquisition and materialization
class edit_controller {
public:
    edit_controller() = default;
    ~edit_controller() {
        stop();
    }

    // Non-copyable, non-movable
    edit_controller(const edit_controller&) = delete;
    edit_controller& operator=(const edit_controller&) = delete;
    edit_controller(edit_controller&&) = delete;
    edit_controller& operator=(edit_controller&&) = delete;

    void start(void* hwnd_handle, unsigned int notify_msg) {
        hwnd_ = hwnd_handle;
        notify_msg_ = notify_msg;
        stop_requested_.store(false, std::memory_order_relaxed);
        worker_thread_ = std::jthread([this] { worker_loop(); });
    }

    void stop() {
        stop_requested_.store(true, std::memory_order_relaxed);
        cv_.notify_all();
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }
    }

    // Submit latest-only work request. Replaces any pending request.
    std::uint64_t submit_request(edit_work_request req) {
        std::uint64_t req_id = next_req_id_.fetch_add(1, std::memory_order_relaxed);
        req.request_id = req_id;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_request_ = std::move(req);
            has_pending_ = true;
        }
        cv_.notify_one();
        return req_id;
    }

    // Retrieve all completed messages (UI thread call)
    std::vector<edit_completion_msg> pop_completions() {
        std::vector<edit_completion_msg> out;
        {
            std::lock_guard<std::mutex> lock(comp_mutex_);
            out.swap(completions_);
        }
        return out;
    }

private:
    void worker_loop();

    void* hwnd_ = nullptr;
    unsigned int notify_msg_ = 0;

    std::atomic<bool> stop_requested_{false};
    std::atomic<std::uint64_t> next_req_id_{1};

    std::mutex mutex_;
    std::condition_variable cv_;
    edit_work_request pending_request_{};
    bool has_pending_ = false;

    std::mutex comp_mutex_;
    std::vector<edit_completion_msg> completions_;

    std::jthread worker_thread_;
};

// Stable file-identity helpers (verify_source_file_metadata,
// read_file_stable_identity, scoped_stable_read_hook) are declared in
// source_identity.hpp and defined in the always-built source_identity.cpp so
// the Editing-OFF copy path links. They remain visible through this header so
// existing Editing-ON includes are unchanged.

}  // namespace oz::viewer

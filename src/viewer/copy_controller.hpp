// oz/viewer/copy_controller.hpp - dedicated latest-only Copy Image worker and controller (Stage 9.4).
#pragma once

#include "edit_controller.hpp"
#include "oz/viewer_core/edit_pipeline.hpp"
#include "oz/viewer_core/edit_state.hpp"

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

// Completion message dispatched to the UI thread
struct copy_completion_msg {
    std::uint64_t request_id = 0;
    std::uint64_t document_instance_id = 0;
    std::uint64_t diagnostic_revision = 0;
    std::uint64_t document_generation = 0;
    bool ok = false;
    std::string error_detail;

    // Materialized pixels in canonical PBGRA8 (top-down)
    std::vector<std::uint8_t> pixels;
    oz::viewer_core::edit_dimensions dimensions{};
    std::int32_t stride = 0;
};

// Work request sent to the copy worker
struct copy_work_request {
    std::uint64_t request_id = 0;
    std::uint64_t document_instance_id = 0;
    std::uint64_t diagnostic_revision = 0;
    std::uint64_t document_generation = 0;

    // Source acquisition details (used if no full_res_source provided or decode needed)
    bool needs_source_acquisition = false;
    std::filesystem::path source_path;
    std::uint64_t expected_file_size = 0;
    std::uint64_t expected_last_write_time = 0;
    oz::viewer_core::edit_dimensions expected_dimensions{};

    // Materialization details
    std::shared_ptr<const edit_source_buffer> existing_source;
    std::vector<oz::viewer_core::edit_command> commands;
    std::size_t memory_budget = 0;
};

// Controller managing background copy materialization (latest-only)
class copy_controller {
public:
    copy_controller() = default;
    ~copy_controller() {
        stop();
    }

    copy_controller(const copy_controller&) = delete;
    copy_controller& operator=(const copy_controller&) = delete;
    copy_controller(copy_controller&&) = delete;
    copy_controller& operator=(copy_controller&&) = delete;

    void start(void* hwnd_handle, unsigned int notify_msg) {
        hwnd_ = hwnd_handle;
        notify_msg_ = notify_msg;
        stop_requested_.store(false, std::memory_order_relaxed);
        cancel_requested_.store(false, std::memory_order_relaxed);
        worker_thread_ = std::jthread([this] { worker_loop(); });
    }

    void stop() {
        stop_requested_.store(true, std::memory_order_relaxed);
        cancel_requested_.store(true, std::memory_order_relaxed);
        cv_.notify_all();
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }
    }

    // Submit work request (latest-only semantics).
    // If a request is already queued, it is replaced by this new request.
    std::uint64_t submit_request(copy_work_request req) {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::uint64_t req_id = next_req_id_.fetch_add(1, std::memory_order_relaxed);
        req.request_id = req_id;
        pending_request_ = std::move(req);
        has_pending_ = true;
        is_active_.store(true, std::memory_order_release);
        cv_.notify_one();
        return req_id;
    }

    void request_cancel() noexcept {
        cancel_requested_.store(true, std::memory_order_release);
    }

    [[nodiscard]] bool is_active() const noexcept {
        return is_active_.load(std::memory_order_acquire);
    }

    // Retrieve all completed messages (UI thread call)
    std::vector<copy_completion_msg> pop_completions() {
        std::vector<copy_completion_msg> out;
        {
            std::lock_guard<std::mutex> lock(comp_mutex_);
            out.swap(completions_);
        }
        if (!has_pending_) {
            is_active_.store(false, std::memory_order_release);
        }
        return out;
    }

private:
    void worker_loop();

    void* hwnd_ = nullptr;
    unsigned int notify_msg_ = 0;

    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> cancel_requested_{false};
    std::atomic<bool> is_active_{false};
    std::atomic<std::uint64_t> next_req_id_{1};

    std::mutex mutex_;
    std::condition_variable cv_;
    copy_work_request pending_request_{};
    bool has_pending_ = false;

    std::mutex comp_mutex_;
    std::vector<copy_completion_msg> completions_;

    std::jthread worker_thread_;
};

}  // namespace oz::viewer

// oz/viewer/save_controller.hpp - dedicated single-flight Save worker
// (Stage 5 Slice 6).
//
// Deliberately separate from edit_controller:
// - edit_controller is latest-only-replace for preview/materialization work;
// - save_controller is single-flight: at most one pending/in-flight save, and
//   a second submit is rejected rather than replacing the first.
// The worker materializes the captured active commands exactly once, encodes
// to a unique same-directory temp file, and atomically commits. It never
// touches the renderer, the retained source bitmap, the CPU cache, or history.
#pragma once

#include "edit_controller.hpp"
#include "oz/viewer_core/save_pipeline.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace oz::viewer {

// Work request sent to the save worker.
struct save_work_request {
    std::uint64_t request_id = 0;  // assigned by the controller
    std::uint64_t document_instance_id = 0;
    std::uint64_t diagnostic_revision = 0;

    // Immutable retained source supplied by the UI when available. The worker
    // keeps the shared_ptr alive for the whole request.
    std::shared_ptr<const edit_source_buffer> existing_source;

    // Otherwise the worker acquires a full-resolution immutable source from
    // the original file exactly like the edit controller (source-only
    // full-resolution upgrade; never a materialized edit result).
    bool needs_source_acquisition = false;
    std::filesystem::path source_path;
    std::uint64_t expected_file_size = 0;
    std::uint64_t expected_last_write_time = 0;
    oz::viewer_core::edit_dimensions expected_dimensions{};

    // Copied active command sequence captured on the UI thread.
    std::vector<oz::viewer_core::edit_command> commands;

    // Destination/format/baseline/source-fingerprint/admission details.
    oz::viewer_core::save_request request{};
};

// Completion dispatched to the UI thread.
struct save_completion_msg {
    std::uint64_t request_id = 0;
    std::uint64_t document_instance_id = 0;
    std::uint64_t diagnostic_revision = 0;

    oz::viewer_core::save_error error = oz::viewer_core::save_error::none;
    std::string detail;
    bool committed = false;
    std::filesystem::path destination;
    oz::viewer_core::file_fingerprint committed_fingerprint{};

    // Source-only upgrade artifact when the worker had to acquire the
    // full-resolution source. Never a materialized edit result.
    std::shared_ptr<const edit_source_buffer> acquired_source;
};

// Single-flight save worker with cooperative cancellation.
class save_controller {
public:
    save_controller() = default;
    ~save_controller() {
        stop();
    }

    save_controller(const save_controller&) = delete;
    save_controller& operator=(const save_controller&) = delete;
    save_controller(save_controller&&) = delete;
    save_controller& operator=(save_controller&&) = delete;

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

    // Single-flight submit. Returns 0 when a save is already pending or in
    // flight; the caller must keep its captured state until the matching
    // completion is consumed.
    std::uint64_t submit_request(save_work_request req) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (request_active_.load(std::memory_order_acquire) || has_pending_) {
            return 0;
        }
        cancel_requested_.store(false, std::memory_order_release);
        const std::uint64_t req_id = next_req_id_.fetch_add(1, std::memory_order_relaxed);
        req.request_id = req_id;
        pending_request_ = std::move(req);
        has_pending_ = true;
        pending_flag_.store(true, std::memory_order_release);
        request_active_.store(true, std::memory_order_release);
        cv_.notify_one();
        return req_id;
    }

    // Cooperative cancellation of the pending/in-flight save. Cancellation is
    // checked before the replacement step; a save that has already entered
    // ReplaceFileW/MoveFileExW cannot be revoked.
    void request_cancel() noexcept {
        cancel_requested_.store(true, std::memory_order_release);
        cv_.notify_all();
    }

    // True while a save is pending, in flight, or has a completion that the UI
    // thread has not consumed yet. The UI must not switch documents until this
    // is false.
    [[nodiscard]] bool is_active() const noexcept {
        if (request_active_.load(std::memory_order_acquire)) {
            return true;
        }
        return pending_flag_.load(std::memory_order_acquire);
    }

    // Retrieve all completed messages (UI thread call). Consuming the last
    // completion releases the single-flight slot.
    std::vector<save_completion_msg> pop_completions() {
        std::vector<save_completion_msg> out;
        {
            std::lock_guard<std::mutex> lock(comp_mutex_);
            out.swap(completions_);
        }
        if (!out.empty()) {
            request_active_.store(false, std::memory_order_release);
        }
        return out;
    }

private:
    void worker_loop();

    void* hwnd_ = nullptr;
    unsigned int notify_msg_ = 0;

    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> cancel_requested_{false};
    std::atomic<bool> request_active_{false};
    std::atomic<bool> pending_flag_{false};
    std::atomic<std::uint64_t> next_req_id_{1};

    std::mutex mutex_;
    std::condition_variable cv_;
    save_work_request pending_request_{};
    bool has_pending_ = false;

    std::mutex comp_mutex_;
    std::vector<save_completion_msg> completions_;

    std::jthread worker_thread_;
};

}  // namespace oz::viewer

// oz/viewer/print_controller.hpp - dedicated single-flight Print worker
// (Stage 9.8).
//
// Deliberately separate from save_controller, copy_controller, and the
// renderer:
// - exactly one pending/in-flight print; a second submit is rejected rather
//   than replacing the first (the caller keeps HDC ownership on rejection);
// - the worker source-acquires/materializes the full-resolution edited result
//   exactly once from the immutable canonical source, composites it over
//   opaque white to a 24-bpp BGR DIB, and spools a single fit-to-page page;
// - it never touches the renderer, the retained source bitmap, the CPU cache,
//   edit history, dirty state, or the save checkpoint.
#pragma once

#include "edit_controller.hpp"
#include "print_job.hpp"

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

// Release helper for a transferred printer DC (DeleteDC). Defined in
// print_controller.cpp; used by submit_request on the reject path.
void release_print_dc(void* printer_dc) noexcept;

// Work request sent to the print worker.
struct print_work_request {
    std::uint64_t request_id = 0;  // assigned by the controller
    std::uint64_t document_instance_id = 0;
    std::uint64_t diagnostic_revision = 0;
    std::uint64_t document_generation = 0;

    // Immutable retained source supplied by the UI when available. The worker
    // keeps the shared_ptr alive for the whole request.
    std::shared_ptr<const edit_source_buffer> existing_source;

    // Otherwise the worker acquires a full-resolution immutable source from
    // the original file exactly like the copy controller (source-only
    // full-resolution upgrade; never a materialized edit result).
    bool needs_source_acquisition = false;
    std::filesystem::path source_path;
    std::uint64_t expected_file_size = 0;
    std::uint64_t expected_last_write_time = 0;
    oz::viewer_core::edit_dimensions expected_dimensions{};

    // Copied active command sequence captured on the UI thread.
    std::vector<oz::viewer_core::edit_command> commands;
    std::size_t memory_budget = 0;

    // Printer DC from PrintDlgExW with PD_RETURNDC. Ownership transfers to
    // the controller on submit: an accepted request is DeleteDC'd by the
    // worker exactly once the job settles (success, failure, or cancel); a
    // rejected request (return 0) is DeleteDC'd by the controller before it
    // returns, so the caller never retains HDC ownership after submit.
    void* printer_dc = nullptr;  // HDC
};

// Completion dispatched to the UI thread.
struct print_completion_msg {
    std::uint64_t request_id = 0;
    std::uint64_t document_instance_id = 0;
    std::uint64_t diagnostic_revision = 0;
    std::uint64_t document_generation = 0;

    bool ok = false;
    print_error error = print_error::none;
    std::string detail;
};

// Single-flight print worker with cooperative cancellation.
class print_controller {
public:
    print_controller() = default;
    ~print_controller() {
        stop();
    }

    print_controller(const print_controller&) = delete;
    print_controller& operator=(const print_controller&) = delete;
    print_controller(print_controller&&) = delete;
    print_controller& operator=(print_controller&&) = delete;

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

    // Single-flight submit. Returns 0 when a print is already pending or in
    // flight, or when no printer DC was supplied. The controller always
    // consumes the request: the worker owns an accepted HDC, and a rejected
    // HDC is released before returning, so the caller keeps no ownership.
    std::uint64_t submit_request(print_work_request req) {
        if (req.printer_dc == nullptr) {
            return 0;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (request_active_.load(std::memory_order_acquire) || has_pending_) {
            release_print_dc(req.printer_dc);
            req.printer_dc = nullptr;
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

    // Cooperative cancellation of the pending/in-flight print. A job that
    // already entered StartDoc is aborted with AbortDoc; a job that has
    // already returned from EndDoc cannot be revoked.
    void request_cancel() noexcept {
        cancel_requested_.store(true, std::memory_order_release);
        cv_.notify_all();
    }

    // True while a print is pending, in flight, or has a completion that the
    // UI thread has not consumed yet.
    [[nodiscard]] bool is_active() const noexcept {
        if (request_active_.load(std::memory_order_acquire)) {
            return true;
        }
        return pending_flag_.load(std::memory_order_acquire);
    }

    // Retrieve all completed messages (UI thread call). Consuming the last
    // completion releases the single-flight slot.
    std::vector<print_completion_msg> pop_completions() {
        std::vector<print_completion_msg> out;
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
    print_work_request pending_request_{};
    bool has_pending_ = false;

    std::mutex comp_mutex_;
    std::vector<print_completion_msg> completions_;

    std::jthread worker_thread_;
};

}  // namespace oz::viewer

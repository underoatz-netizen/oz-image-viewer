#pragma once

#include "oz/viewer_core/cache.hpp"

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace oz::viewer_core {

// Work request for the decode worker.
struct decode_request {
    std::uint64_t epoch = 0;
    work_priority priority = work_priority::p0_selected;
    cache_key key;
    std::filesystem::path path;
    std::int32_t view_w = 0;
    std::int32_t view_h = 0;
    bool is_initial = false;
};

// Result published by the decode worker to the completion queue.
// src_width/src_height are the source JPEG dimensions before scaling;
// actual_scale_den is the denominator derived by the worker after the header
// parse from the request viewport. It must match the denominator required by
// the current viewport before the UI commits the image.
struct decode_result_msg {
    std::uint64_t epoch = 0;
    work_priority priority = work_priority::p0_selected;
    cache_key key;
    std::filesystem::path path;
    bool ok = false;
    std::shared_ptr<decoded_image_data> image;
    std::string detail;
    std::int32_t src_width = 0;
    std::int32_t src_height = 0;
    int actual_scale_den = 1;
    std::uint64_t source_file_size = 0;
    std::uint64_t source_last_write_time = 0;
};

// Epoch-agnostic commit decision: a completion may commit when its normalized
// path matches the latest desired target AND its actual decode profile matches
// the denominator required by the current viewport. It must never commit a
// wrong path/profile. Epoch equality alone is not required (stale in-flight
// work for the same path/profile is coalesced/promoted instead of re-decoded).
inline bool should_commit_completion(const decode_result_msg& msg,
                                     const std::wstring& desired_normalized_path,
                                     std::int32_t view_w, std::int32_t view_h) noexcept {
    if (!msg.ok || msg.image == nullptr) {
        return false;
    }
    if (msg.key.normalized_path != desired_normalized_path) {
        return false;
    }
    if (msg.src_width <= 0 || msg.src_height <= 0) {
        return false;
    }
    if (msg.actual_scale_den != msg.key.scale_den) {
        return false;
    }
    if (msg.image->scale_den != msg.actual_scale_den) {
        return false;
    }
    return msg.actual_scale_den ==
           choose_scaling_denominator(msg.src_width, msg.src_height, view_w, view_h);
}

// Bounded replaceable latest-plan queue for the decode worker.
// The worker thread pops the highest priority task available.
// When a new navigation epoch is set, the queue of unstarted items is replaced
// with the new +-2 plan (P0, P1, P2, P3, P4).
class decode_work_queue {
public:
    decode_work_queue() = default;

    // Shutdown and wake worker
    void request_stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        cv_.notify_all();
    }

    bool is_stopped() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stop_;
    }

    // Set new epoch and replace queued pending work with newest plan items.
    // If an item in the new plan matches the in-flight key, it can be marked/promoted.
    void replace_plan(std::uint64_t new_epoch, std::vector<decode_request> new_items) {
        std::lock_guard<std::mutex> lock(mutex_);
        current_epoch_ = new_epoch;
        // Sort new items by priority ascending (P0 first, then P1, etc.)
        std::sort(new_items.begin(), new_items.end(), [](const decode_request& a, const decode_request& b) {
            return static_cast<int>(a.priority) < static_cast<int>(b.priority);
        });
        pending_items_ = std::move(new_items);
        cv_.notify_one();
    }

    // Push a single request (e.g. initial decode)
    void push(decode_request req) {
        std::lock_guard<std::mutex> lock(mutex_);
        current_epoch_ = req.epoch;
        pending_items_.push_back(std::move(req));
        std::sort(pending_items_.begin(), pending_items_.end(), [](const decode_request& a, const decode_request& b) {
            return static_cast<int>(a.priority) < static_cast<int>(b.priority);
        });
        cv_.notify_one();
    }

    // Worker waits for work or stop signal.
    // Returns std::nullopt if stopping.
    std::optional<decode_request> pop_work(uint64_t& out_epoch) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] {
            return stop_ || !pending_items_.empty();
        });
        if (stop_ && pending_items_.empty()) {
            return std::nullopt;
        }
        if (pending_items_.empty()) {
            return std::nullopt;
        }

        decode_request req = std::move(pending_items_.front());
        pending_items_.erase(pending_items_.begin());
        out_epoch = current_epoch_;
        return req;
    }

    // Get current active epoch under lock
    std::uint64_t get_epoch() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return current_epoch_;
    }

    // Number of queued (not yet started) requests. Used by the test-only
    // navigation benchmark to drain decode work before verified-miss setup.
    std::size_t pending_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pending_items_.size();
    }

    // Clear all pending items
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_items_.clear();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::uint64_t current_epoch_ = 0;
    std::vector<decode_request> pending_items_;
};

// Thread-safe bounded completion queue for passing outcomes to UI thread.
// Policy: when full, drop the oldest non-P0 completion first to retain the
// selected image; drop the oldest entry only when every queued entry is P0.
class completion_queue {
public:
    static constexpr std::size_t kMaxCapacity = 32;

    void push(decode_result_msg msg) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= kMaxCapacity) {
            auto it = queue_.begin();
            for (; it != queue_.end(); ++it) {
                if (it->priority != work_priority::p0_selected) {
                    break;
                }
            }
            if (it != queue_.end()) {
                queue_.erase(it);
            } else {
                queue_.pop_front();
            }
        }
        queue_.push_back(std::move(msg));
    }

    // True when a queued valid completion already covers the desired path for
    // the current viewport (path + actual profile match). Used by the UI to
    // avoid scheduling a duplicate P0 decode when valid work is already queued.
    bool has_valid_for_path(const std::wstring& desired_normalized_path,
                            std::int32_t view_w, std::int32_t view_h) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& m : queue_) {
            if (should_commit_completion(m, desired_normalized_path, view_w, view_h)) {
                return true;
            }
        }
        return false;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    std::vector<decode_result_msg> pop_all() {
        std::vector<decode_result_msg> out;
        std::lock_guard<std::mutex> lock(mutex_);
        out.reserve(queue_.size());
        while (!queue_.empty()) {
            out.push_back(std::move(queue_.front()));
            queue_.pop_front();
        }
        return out;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
    }

private:
    mutable std::mutex mutex_;
    std::deque<decode_result_msg> queue_;
};

}  // namespace oz::viewer_core

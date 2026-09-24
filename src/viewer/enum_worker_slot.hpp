// oz/viewer/enum_worker_slot.hpp - Stage 9.6B MUST_FIX: asynchronous
// retirement for the one-shot worker slots (enumeration and fast-first initial
// decode) plus the newest-launch-wins publication and cancellation rules those
// workers use.
//
// The UI thread (WM_DROPFILES and decode/enumeration completion handlers) must
// never join or wait on an enumeration worker that may still be running. A
// per-worker "body finished" flag cannot prove that: the worker's callable and
// thread bookkeeping are destroyed after the body returns, so `join()` on a
// flagged worker can still block. This slot therefore owns one dedicated
// reaper thread; retiring a worker moves its std::jthread into the reaper
// queue, and only the reaper (or teardown) ever joins.
//
// UI-path contract:
//   - launch(): stop-requests the previous active worker and hands it to the
//     reaper. Never joins.
//   - request_stop_all(): stops the active worker. Never joins. Retired
//     workers were already stop-requested when they were retired, and the
//     stop request is sticky per worker.
//   - reap_finished(): erases bookkeeping for workers the reaper has already
//     joined. The flag it consults is set only after join() returns, so this
//     call performs no join and is provably non-blocking.
//   - join_all(): teardown only. Stops and joins the active worker, closes and
//     joins the reaper, and drains any fallback queue. The caller must
//     guarantee no further launch() and that referenced state outlives the
//     call.
//
// The reaper joins in retirement order. A worker that ignores cancellation
// delays only the reaping of workers retired after it, never a UI path.
//
// Bounded overload rule (third MUST_FIX): the reaper can fail to start
// (resource failure) or fail to progress (a retired worker that ignores
// cancellation blocks every join behind it). Without a bound, repeated UI
// launches would grow retired_/reaper_queue_ without limit. Every launch
// therefore evaluates the finite, non-waiting budget `can_accept_launch()`
// before any mutation. When the pending-retirement backlog is at
// kMaxPendingRetirements, launch() returns launch_status::overloaded and
// leaves the active worker, the queue, and the bookkeeping untouched; the
// caller rejects or defers the new request with its own error UX and keeps
// its current task. Teardown may wait, so join_all() still drains everything.
//
// Dependency-free and GUI-free so the lifecycle contract is unit-testable.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace oz::viewer {

// Newest-launch-wins publication rule for a shared enumeration result slot.
// A superseded worker may still be winding down while a newer launch owns the
// slot; its older launch generation must never overwrite (or hide) the newer
// result. Equal generations re-publish, preserving the single-worker
// overwrite semantics.
[[nodiscard]] inline bool should_publish_enumeration_result(
    std::uint64_t launch_generation, std::uint64_t stored_generation) noexcept {
    return launch_generation >= stored_generation;
}

// Newest-launch-wins publication rule for the fast-first initial-decode result
// slot. Same contract as should_publish_enumeration_result: a superseded
// decode may still be winding down (decode_image() itself is not cancellable)
// and its older launch generation must never overwrite (or hide) a newer
// result.
[[nodiscard]] inline bool should_publish_initial_decode_result(
    std::uint64_t launch_generation, std::uint64_t stored_generation) noexcept {
    return launch_generation >= stored_generation;
}

// Cooperative cancellation predicate for the fast-first initial-decode worker:
// sticky per-thread stop request (set when the slot retires this worker) OR the
// shared flag (set by Open/Paste/shutdown intents and cleared for the next
// task). The stop token is what makes replacement cancellation-safe: the
// launching path clears the shared flag before a superseded worker may poll
// it, so only the stop request is guaranteed to survive.
[[nodiscard]] inline bool initial_decode_cancel_requested(
    const std::stop_token& stop, const std::atomic<bool>& shared_cancel) noexcept {
    return stop.stop_requested() || shared_cancel.load(std::memory_order_relaxed);
}

// Single-slot one-shot worker with non-blocking replacement.
class async_worker_slot {
public:
    // Finite bound on superseded workers awaiting reaping. A worker that
    // observes cancellation drains immediately, so this budget is only
    // reached when the reaper cannot start or a retired worker ignores
    // cancellation. Four pending retirements is ample for normal bursts and
    // caps the thread bookkeeping at a small, fixed size (budget + active +
    // reaper).
    static constexpr std::size_t kMaxPendingRetirements = 4;
    static_assert(kMaxPendingRetirements > 0);

    enum class launch_status {
        launched,          // the new worker is now the active worker
        overloaded,        // finite retirement budget exhausted; nothing changed
        resource_failure   // worker/bookkeeping allocation failed; nothing else changed
    };

    // `start_reaper == false` is a fault-injection seam: it simulates a
    // reaper-start resource failure so the bounded-overload and
    // teardown-drain contracts are testable. Production uses the default.
    explicit async_worker_slot(bool start_reaper = true) noexcept
        : reaper_enabled_(start_reaper) {}

    ~async_worker_slot() { join_all(); }

    async_worker_slot(const async_worker_slot&) = delete;
    async_worker_slot& operator=(const async_worker_slot&) = delete;
    async_worker_slot(async_worker_slot&&) = delete;
    async_worker_slot& operator=(async_worker_slot&&) = delete;

    // Launch `fn(std::stop_token)` as the active worker. A previous active
    // worker is stop-requested and handed to the reaper first; this call never
    // waits for it to finish. The finite overload gate is evaluated before
    // any retirement or thread-creation mutation: at the budget bound the
    // launch is rejected with launch_status::overloaded and the slot is left
    // exactly as it was (active worker keeps running).
    template <typename Fn>
    [[nodiscard]] launch_status launch(Fn&& fn) {
        if (!can_accept_launch()) {
            return launch_status::overloaded;
        }
        if (!retire_active()) {
            return launch_status::resource_failure;
        }
        try {
            active_ = std::jthread(std::forward<Fn>(fn));
        } catch (...) {
            return launch_status::resource_failure;
        }
        return launch_status::launched;
    }

    [[nodiscard]] bool active_joinable() const noexcept { return active_.joinable(); }

    [[nodiscard]] std::size_t retired_count() const noexcept { return retired_.size(); }

    // Read-only, non-waiting preflight for callers that must decide before
    // mutating their own state: true when launch() would accept right now.
    // Evaluated on the UI thread (the only writer of retired_); reap_finished()
    // can only free budget, never consume it, so a caller that gates on this
    // cannot observe an overload rejection from the follow-up launch.
    [[nodiscard]] bool can_accept_launch() const noexcept {
        const std::size_t retiring = active_.joinable() ? 1u : 0u;
        return retired_.size() + retiring <= kMaxPendingRetirements;
    }

    // Sticky stop request for the active worker. Retired workers were already
    // stop-requested at retirement, so this is the only worker that can still
    // need one. Never joins.
    void request_stop_all() noexcept {
        if (active_.joinable()) {
            active_.request_stop();
        }
    }

    // UI-thread bookkeeping cleanup only: drops entries for workers the reaper
    // has already joined (the flag is set after join() returned). It never
    // joins and never waits, so it is safe on WM_DROPFILES and completion
    // handlers even while a retired worker is still running.
    void reap_finished() {
        auto it = retired_.begin();
        while (it != retired_.end()) {
            const std::shared_ptr<std::atomic<bool>>& joined = *it;
            if (joined && joined->load(std::memory_order_acquire)) {
                it = retired_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Teardown only: stop and join the active worker, then drain the reaper
    // queue. Waiting here is required so no worker can access state destroyed
    // after this call.
    void join_all() {
        if (active_.joinable()) {
            active_.request_stop();
            active_.join();
        }
        {
            std::lock_guard<std::mutex> lock(reaper_mutex_);
            reaper_closing_ = true;
        }
        reaper_cv_.notify_all();
        if (reaper_thread_.joinable()) {
            reaper_thread_.join();
        }
        // Fallback for a reaper that could not be started (resource failure):
        // teardown may wait, so drain the queue on this thread.
        for (auto& entry : reaper_queue_) {
            if (entry.thread.joinable()) {
                entry.thread.join();
            }
        }
        reaper_queue_.clear();
        retired_.clear();
    }

private:
    struct reaper_entry {
        std::jthread thread;
        std::shared_ptr<std::atomic<bool>> joined;
    };

    // Retire the active worker into the reaper queue without joining. Returns
    // false when retirement bookkeeping cannot be allocated; in that case the
    // active worker stays active (a stop request may already have been issued)
    // and the caller must treat the launch as rejected. Never joins.
    bool retire_active() {
        if (!active_.joinable()) {
            return true;
        }
        // Reserve UI bookkeeping before touching the active worker. On failure
        // the worker keeps running and nothing in the slot has changed.
        std::shared_ptr<std::atomic<bool>> joined;
        try {
            joined = std::make_shared<std::atomic<bool>>(false);
        } catch (...) {
            return false;
        }
        try {
            retired_.push_back(joined);
        } catch (...) {
            return false;
        }
        active_.request_stop();
        // The deque push is the only remaining throwing step; it happens
        // before the jthread move, so a fault here leaves active_ intact.
        try {
            std::lock_guard<std::mutex> lock(reaper_mutex_);
            reaper_queue_.emplace_back();
            reaper_queue_.back().thread = std::move(active_);
            reaper_queue_.back().joined = std::move(joined);
        } catch (...) {
            retired_.pop_back();
            return false;
        }
        ensure_reaper_started();
        reaper_cv_.notify_one();
        return true;
    }

    void ensure_reaper_started() {
        if (!reaper_enabled_ || reaper_thread_.joinable()) {
            return;
        }
        try {
            reaper_thread_ = std::jthread([this](std::stop_token stop) { reaper_main(stop); });
        } catch (...) {
            // Resource failure: the queued worker stays pending and the finite
            // budget stops further retirement; join_all() drains at teardown.
        }
    }

    void reaper_main(std::stop_token stop) {
        for (;;) {
            std::jthread worker;
            std::shared_ptr<std::atomic<bool>> joined;
            {
                std::unique_lock<std::mutex> lock(reaper_mutex_);
                reaper_cv_.wait(lock, stop, [this] {
                    return reaper_closing_ || !reaper_queue_.empty();
                });
                if (reaper_queue_.empty()) {
                    return;  // closing (or stop request) observed with nothing left
                }
                worker = std::move(reaper_queue_.front().thread);
                joined = std::move(reaper_queue_.front().joined);
                reaper_queue_.pop_front();
            }
            if (worker.joinable()) {
                worker.join();  // blocking is safe: this is the reaper thread
            }
            if (joined) {
                joined->store(true, std::memory_order_release);
            }
        }
    }

    std::jthread active_;
    // UI-thread owned: one entry per retired-but-not-yet-confirmed-joined
    // worker, erased by reap_finished() only after the reaper sets the flag.
    // Its size is the finite retirement budget consumed by can_accept_launch().
    std::vector<std::shared_ptr<std::atomic<bool>>> retired_;
    std::mutex reaper_mutex_;
    std::condition_variable_any reaper_cv_;
    std::deque<reaper_entry> reaper_queue_;  // guarded by reaper_mutex_
    std::jthread reaper_thread_;
    bool reaper_closing_ = false;  // guarded by reaper_mutex_
    bool reaper_enabled_ = true;   // fault-injection seam; see constructor
};

}  // namespace oz::viewer

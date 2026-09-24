// async_worker_slot_tests.cpp - Stage 9.6B MUST_FIX concurrency contract
// tests for src/viewer/enum_worker_slot.hpp.
//
// Dependency-free header-only seam; builds in every preset including no-vcpkg.
// The tests exercise the real std::jthread lifecycle (no GUI).
//
// Defect pinned by test 3: a worker's "body finished" flag is set before the
// worker's closure and thread bookkeeping are destroyed, so joining a worker
// whose flag is already set can still block. The corrected contract is that no
// UI-path call joins or waits on an enumeration worker: a dedicated reaper
// thread owns every join, and reap_finished() only drops bookkeeping for
// workers whose join has already returned.
//
// Tests 10-11 extend the same contract to the fast-first initial decode: its
// replacement is non-blocking, its cancellation must survive the shared flag
// being cleared for the replacement (sticky stop token), and its stale
// completion must not hide a newer result (newest-launch-wins publication).
#include "enum_worker_slot.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(expr) do { \
    ++g_checks; \
    if (!(expr)) { \
        ++g_failures; \
        std::fprintf(stderr, "FAIL: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
    } \
} while (0)

using oz::viewer::async_worker_slot;
using oz::viewer::initial_decode_cancel_requested;
using oz::viewer::should_publish_enumeration_result;
using oz::viewer::should_publish_initial_decode_result;

using launch_status = async_worker_slot::launch_status;

// A launch the finite budget accepts must report `launched`; anything else is
// a contract violation at these call sites.
#define CHECK_LAUNCHED(expr) CHECK((expr) == launch_status::launched)

using clock_type = std::chrono::steady_clock;

bool wait_for(const std::atomic<bool>& flag, int timeout_ms = 5000) {
    const auto deadline = clock_type::now() + std::chrono::milliseconds(timeout_ms);
    while (!flag.load(std::memory_order_acquire)) {
        if (clock_type::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// Reap until the retired bookkeeping drains; reap_finished() must never join,
// so a timeout means the reaper never finished a retired worker (test bug).
bool wait_retired_empty(async_worker_slot& slot, int timeout_ms = 5000) {
    const auto deadline = clock_type::now() + std::chrono::milliseconds(timeout_ms);
    while (slot.retired_count() != 0) {
        slot.reap_finished();
        if (clock_type::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

long long elapsed_ms(clock_type::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() - start).count();
}

// Release guard so a regression cannot hang the suite: if a UI-path call under
// test blocks, the guard releases the blocked worker after a few seconds and
// the elapsed-time assertion fails instead of the whole run hanging.
class safety_release {
public:
    safety_release(std::atomic<bool>& flag, int delay_ms)
        : thread_([&flag, delay_ms](std::stop_token stop) {
              const auto deadline = clock_type::now() + std::chrono::milliseconds(delay_ms);
              while (!stop.stop_requested() && clock_type::now() < deadline) {
                  std::this_thread::sleep_for(std::chrono::milliseconds(1));
              }
              flag.store(true, std::memory_order_release);
          }) {}
    safety_release(const safety_release&) = delete;
    safety_release& operator=(const safety_release&) = delete;
    ~safety_release() {
        thread_.request_stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    std::jthread thread_;
};

// Runs its destructor on the worker thread after the callable body returned
// (std::jthread destroys the decayed callable on the worker thread). It blocks
// there until released: exactly the window in which a "body finished" flag is
// already set while the thread has not terminated.
struct blocking_state {
    std::atomic<bool>* entered = nullptr;
    std::atomic<bool>* release = nullptr;

    blocking_state(std::atomic<bool>* entered_in, std::atomic<bool>* release_in)
        : entered(entered_in), release(release_in) {}

    ~blocking_state() {
        if (entered) {
            entered->store(true, std::memory_order_release);
        }
        if (release) {
            while (!release->load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }
};

struct lifetime_probe {
    std::atomic<bool>* destroyed = nullptr;
    explicit lifetime_probe(std::atomic<bool>* destroyed_in) : destroyed(destroyed_in) {}
    ~lifetime_probe() {
        if (destroyed) {
            destroyed->store(true, std::memory_order_release);
        }
    }
};

// 1. Newest-launch-wins publication guard: a superseded worker's older
// generation may never overwrite (or hide) a newer result.
void test_publish_guard() {
    CHECK(should_publish_enumeration_result(0, 0));
    CHECK(should_publish_enumeration_result(1, 0));
    CHECK(should_publish_enumeration_result(7, 6));
    CHECK(!should_publish_enumeration_result(6, 7));
    CHECK(!should_publish_enumeration_result(0, 1));
    CHECK(should_publish_enumeration_result(7, 7));
}

// 2. Replacing an active worker must not wait for it to finish, and reaping
// must not wait for a retired worker that has not finished either.
void test_launch_and_reap_do_not_wait_for_active_worker() {
    async_worker_slot slot;
    std::atomic<bool> a_started{false};
    std::atomic<bool> a_done{false};
    std::atomic<bool> release_a{false};
    std::atomic<bool> b_done{false};

    // A simulates a traversal that has not reached a cancellation check point.
    CHECK_LAUNCHED(slot.launch([&](std::stop_token) {
        a_started.store(true, std::memory_order_release);
        while (!release_a.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        a_done.store(true, std::memory_order_release);
    }));
    CHECK(wait_for(a_started));

    // Launching B retires A; because A cannot finish until released, B's
    // launch returning proves it did not join A.
    const auto launch_start = clock_type::now();
    CHECK_LAUNCHED(
        slot.launch([&](std::stop_token) { b_done.store(true, std::memory_order_release); }));
    const auto launch_ms = elapsed_ms(launch_start);
    CHECK(launch_ms < 1000);
    CHECK(!a_done.load(std::memory_order_acquire));
    CHECK(wait_for(b_done));
    CHECK(slot.retired_count() == 1);

    // Reap must not wait for the unfinished retired worker.
    const auto reap_start = clock_type::now();
    slot.reap_finished();
    const auto reap_ms = elapsed_ms(reap_start);
    CHECK(reap_ms < 1000);
    CHECK(slot.retired_count() == 1);
    CHECK(!a_done.load(std::memory_order_acquire));

    release_a.store(true, std::memory_order_release);
    CHECK(wait_for(a_done));
    CHECK(wait_retired_empty(slot));
    slot.join_all();
}

// 3. The MUST_FIX regression: the worker body has returned (any "finished"
// flag is set) but its closure destructor is still running on the worker
// thread. Every UI-path call must return promptly, and bookkeeping must stay
// until the reaper's join has actually returned.
void test_ui_paths_never_wait_for_blocked_worker_destructor() {
    async_worker_slot slot;
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    safety_release guard(release, 5000);

    auto state = std::make_shared<blocking_state>(&entered, &release);
    CHECK_LAUNCHED(slot.launch([state](std::stop_token) {}));
    state.reset();  // the worker closure now owns the last reference

    CHECK(wait_for(entered));  // body returned; destructor is blocked
    CHECK(!release.load());

    std::atomic<bool> b_done{false};
    const auto launch_start = clock_type::now();
    CHECK_LAUNCHED(
        slot.launch([&b_done](std::stop_token) { b_done.store(true, std::memory_order_release); }));
    const auto launch_ms = elapsed_ms(launch_start);
    CHECK(wait_for(b_done));
    CHECK(launch_ms < 1000);  // retiring a blocked worker never waits
    CHECK(slot.retired_count() == 1);
    CHECK(!release.load());

    const auto reap_start = clock_type::now();
    slot.reap_finished();  // the defect joined here and blocked
    slot.request_stop_all();
    const auto reap_ms = elapsed_ms(reap_start);
    CHECK(reap_ms < 1000);
    CHECK(!release.load());            // still blocked: reap did not wait for it
    CHECK(slot.retired_count() == 1);  // only a confirmed join may erase bookkeeping

    release.store(true, std::memory_order_release);
    CHECK(wait_retired_empty(slot));
    slot.join_all();
    CHECK(!slot.active_joinable());
}

// 4. The stop request is sticky per worker: retirement stops the superseded
// worker, request_stop_all stops the active one, and a fresh launch starts
// unstopped.
void test_stop_request_sticky_and_observed() {
    async_worker_slot slot;
    std::atomic<bool> a_started{false};
    std::atomic<bool> a_observed{false};
    std::atomic<bool> b_started{false};
    std::atomic<bool> b_observed{false};
    std::atomic<bool> b_stopped_at_entry{true};

    CHECK_LAUNCHED(slot.launch([&](std::stop_token stop) {
        a_started.store(true, std::memory_order_release);
        while (!stop.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        a_observed.store(true, std::memory_order_release);
    }));
    CHECK(wait_for(a_started));

    // Retiring A (by launching B) is itself the stop request for A.
    CHECK_LAUNCHED(slot.launch([&](std::stop_token stop) {
        b_stopped_at_entry.store(stop.stop_requested(), std::memory_order_release);
        b_started.store(true, std::memory_order_release);
        while (!stop.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        b_observed.store(true, std::memory_order_release);
    }));
    CHECK(wait_for(b_started));
    CHECK(!b_stopped_at_entry.load(std::memory_order_acquire));  // fresh token
    CHECK(wait_for(a_observed));                                 // A observed retirement stop
    CHECK(!b_observed.load());

    slot.request_stop_all();
    CHECK(wait_for(b_observed));
    CHECK(wait_retired_empty(slot));
    slot.join_all();
}

// 5. A run of retirements behind a stuck worker stays bounded and drains once
// the stuck worker finishes; reaping never removes an unjoined worker.
void test_reaper_drains_multiple_retired_workers() {
    async_worker_slot slot;
    std::atomic<bool> a_started{false};
    std::atomic<bool> release_a{false};
    std::atomic<bool> quick_done{false};

    CHECK_LAUNCHED(slot.launch([&](std::stop_token) {
        a_started.store(true, std::memory_order_release);
        while (!release_a.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }));
    CHECK(wait_for(a_started));

    for (int i = 0; i < 3; ++i) {
        CHECK_LAUNCHED(
            slot.launch([&](std::stop_token) { quick_done.store(true, std::memory_order_release); }));
    }
    CHECK(wait_for(quick_done));
    CHECK(slot.retired_count() == 3);

    const auto reap_start = clock_type::now();
    slot.reap_finished();
    const auto reap_ms = elapsed_ms(reap_start);
    CHECK(reap_ms < 1000);
    CHECK(slot.retired_count() == 3);  // A still unjoined; later joins queue behind it

    release_a.store(true, std::memory_order_release);
    CHECK(wait_retired_empty(slot));
    slot.join_all();
}

// 6. Teardown join_all waits for workers that ignore the stop token.
void test_join_all_waits_for_completion() {
    async_worker_slot slot;
    std::atomic<bool> done{false};
    CHECK_LAUNCHED(slot.launch([&](std::stop_token) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        done.store(true, std::memory_order_release);
    }));
    slot.join_all();
    CHECK(done.load(std::memory_order_acquire));
    CHECK(!slot.active_joinable());
    CHECK(slot.retired_count() == 0);
}

// 7. Teardown lifetime: join_all must not return while a worker can still
// touch state captured by its closure.
void test_join_all_releases_worker_lifetime() {
    std::atomic<bool> destroyed{false};
    auto probe = std::make_shared<lifetime_probe>(&destroyed);
    {
        async_worker_slot slot;
        CHECK_LAUNCHED(slot.launch([probe](std::stop_token stop) {
            while (!stop.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }));
        probe.reset();  // only the worker closure owns the probe now
        CHECK(!destroyed.load());
        slot.join_all();
        CHECK(destroyed.load(std::memory_order_acquire));
    }
    CHECK(destroyed.load(std::memory_order_acquire));
}

// 8. Third MUST_FIX: a repeated launch storm behind a stuck worker stays
// inside the finite retirement budget. Once the budget is full, launch()
// returns overloaded quickly without stopping, replacing, or joining the
// active worker, and can_accept_launch() reports the same decision callers
// gate on before mutating their own state. Releasing the stuck worker restores
// acceptance.
void test_launch_storm_bounded_and_rejection_inert() {
    async_worker_slot slot;
    std::atomic<bool> a_started{false};
    std::atomic<bool> release_a{false};
    std::atomic<bool> watcher_started{false};
    std::atomic<bool> watcher_stopped{false};

    CHECK_LAUNCHED(slot.launch([&](std::stop_token) {
        a_started.store(true, std::memory_order_release);
        while (!release_a.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }));
    CHECK(wait_for(a_started));

    // Retire quick workers until one budget slot remains. Stuck A blocks the
    // reaper, so nothing drains and each accepted launch consumes one slot.
    for (int i = 0; i < 64 && slot.retired_count() + 1 < async_worker_slot::kMaxPendingRetirements;
         ++i) {
        CHECK_LAUNCHED(slot.launch([&](std::stop_token) {}));
    }
    CHECK(slot.retired_count() + 1 == async_worker_slot::kMaxPendingRetirements);
    CHECK(slot.can_accept_launch());

    // The last accepted launch is the active worker under observation.
    CHECK_LAUNCHED(slot.launch([&](std::stop_token stop) {
        watcher_started.store(true, std::memory_order_release);
        while (!stop.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        watcher_stopped.store(true, std::memory_order_release);
    }));
    CHECK(wait_for(watcher_started));
    CHECK(slot.retired_count() == async_worker_slot::kMaxPendingRetirements);
    CHECK(!slot.can_accept_launch());

    // Launch storm: every further launch is rejected, promptly, with no
    // observable effect on the active worker or the backlog.
    const auto storm_start = clock_type::now();
    for (int i = 0; i < 64; ++i) {
        CHECK(slot.launch([&](std::stop_token) {}) == launch_status::overloaded);
    }
    const auto storm_ms = elapsed_ms(storm_start);
    CHECK(storm_ms < 2000);
    CHECK(!watcher_stopped.load(std::memory_order_acquire));
    CHECK(slot.active_joinable());
    CHECK(slot.retired_count() == async_worker_slot::kMaxPendingRetirements);

    // Recoverable: release the stuck worker, drain, and the slot accepts again.
    release_a.store(true, std::memory_order_release);
    CHECK(wait_retired_empty(slot));
    CHECK(slot.can_accept_launch());
    CHECK_LAUNCHED(slot.launch([&](std::stop_token) {}));
    slot.request_stop_all();
    CHECK(wait_for(watcher_stopped));
    slot.join_all();
}

// 9. Third MUST_FIX, reaper-start failure injection: with no reaper thread the
// backlog still stops at the same finite budget, no UI-path call waits, and
// teardown drains the fallback queue (teardown may wait).
void test_reaper_start_failure_is_bounded_and_drained_at_teardown() {
    async_worker_slot slot(/*start_reaper=*/false);
    std::atomic<bool> a_started{false};
    std::atomic<bool> release_a{false};

    CHECK_LAUNCHED(slot.launch([&](std::stop_token) {
        a_started.store(true, std::memory_order_release);
        while (!release_a.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }));
    CHECK(wait_for(a_started));

    // Without a reaper, retirements never drain; the finite budget still stops
    // the storm and every call stays non-waiting.
    std::size_t accepted = 1;
    bool saw_overload = false;
    const auto storm_start = clock_type::now();
    for (int i = 0; i < 64; ++i) {
        if (slot.launch([&](std::stop_token) {}) == launch_status::launched) {
            ++accepted;
        } else {
            saw_overload = true;
        }
    }
    const auto storm_ms = elapsed_ms(storm_start);
    CHECK(saw_overload);
    CHECK(storm_ms < 2000);
    CHECK(accepted == async_worker_slot::kMaxPendingRetirements + 1);
    CHECK(slot.retired_count() == async_worker_slot::kMaxPendingRetirements);
    CHECK(!slot.can_accept_launch());

    // Reaping is still non-waiting and cannot confirm a join that never ran.
    const auto reap_start = clock_type::now();
    slot.reap_finished();
    const auto reap_ms = elapsed_ms(reap_start);
    CHECK(reap_ms < 1000);
    CHECK(slot.retired_count() == async_worker_slot::kMaxPendingRetirements);

    release_a.store(true, std::memory_order_release);
    slot.join_all();  // teardown may wait; drains the fallback queue
    CHECK(slot.retired_count() == 0);
    CHECK(!slot.active_joinable());
}

// 10. Initial-decode publication guard and cancellation composition: the
// fast-first decode uses the same newest-launch-wins rule as enumeration, and
// its cancel predicate keeps the sticky stop request even after the shared
// flag is cleared for the replacement.
void test_initial_decode_cancel_and_publish_guards() {
    CHECK(should_publish_initial_decode_result(0, 0));
    CHECK(should_publish_initial_decode_result(1, 0));
    CHECK(should_publish_initial_decode_result(7, 6));
    CHECK(!should_publish_initial_decode_result(6, 7));
    CHECK(!should_publish_initial_decode_result(0, 1));
    CHECK(should_publish_initial_decode_result(7, 7));

    std::atomic<bool> shared_cancel{false};
    const std::stop_token no_stop;
    CHECK(!initial_decode_cancel_requested(no_stop, shared_cancel));
    shared_cancel.store(true, std::memory_order_release);
    CHECK(initial_decode_cancel_requested(no_stop, shared_cancel));

    std::stop_source source;
    source.request_stop();
    const std::stop_token stopped = source.get_token();
    shared_cancel.store(false, std::memory_order_release);
    // The launching path clears the shared flag for the new task; only the
    // sticky stop request still cancels the superseded worker.
    CHECK(initial_decode_cancel_requested(stopped, shared_cancel));
}

// 11. Initial-decode slot model: replacement never joins the superseded
// decode, the superseded decode observes the sticky stop after the shared
// flag is cleared, its late stale completion is rejected by the publication
// guard, and teardown drains the slot.
void test_initial_decode_slot_composition() {
    async_worker_slot slot;
    std::atomic<bool> shared_cancel{false};
    std::mutex result_mutex;
    std::uint64_t stored_generation = 0;

    auto publish = [&](std::uint64_t gen) {
        std::lock_guard<std::mutex> lock(result_mutex);
        if (!should_publish_initial_decode_result(gen, stored_generation)) {
            return false;
        }
        stored_generation = gen;
        return true;
    };

    std::atomic<bool> a_started{false};
    std::atomic<bool> a_observed_stop{false};
    std::atomic<bool> a_release{false};
    std::atomic<bool> a_attempted{false};
    std::atomic<bool> a_stale_publish_result{false};

    CHECK_LAUNCHED(slot.launch([&](std::stop_token stop) {
        a_started.store(true, std::memory_order_release);
        while (!initial_decode_cancel_requested(stop, shared_cancel)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        a_observed_stop.store(true, std::memory_order_release);
        // Hold the stale completion until the replacement has published.
        while (!a_release.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const bool published = publish(1);
        a_stale_publish_result.store(published, std::memory_order_release);
        a_attempted.store(true, std::memory_order_release);
    }));
    CHECK(wait_for(a_started));

    // The launching path clears the shared flag for the new task before the
    // replacement starts. Keep it cleared here, exactly as the superseded
    // worker observes it: only the sticky stop token can still cancel A.
    CHECK(!shared_cancel.load(std::memory_order_acquire));

    std::atomic<bool> b_done{false};
    const auto replace_start = clock_type::now();
    CHECK_LAUNCHED(slot.launch([&](std::stop_token) {
        publish(2);
        b_done.store(true, std::memory_order_release);
    }));
    const auto replace_ms = elapsed_ms(replace_start);
    CHECK(replace_ms < 1000);  // never joined the superseded decode
    CHECK(wait_for(b_done));
    {
        std::lock_guard<std::mutex> lock(result_mutex);
        CHECK(stored_generation == 2);
    }
    CHECK(wait_for(a_observed_stop));  // sticky stop observed with shared flag clear
    CHECK(!shared_cancel.load(std::memory_order_acquire));

    a_release.store(true, std::memory_order_release);
    CHECK(wait_for(a_attempted));
    CHECK(!a_stale_publish_result.load(std::memory_order_acquire));  // gen 1 cannot hide gen 2
    {
        std::lock_guard<std::mutex> lock(result_mutex);
        CHECK(stored_generation == 2);
    }

    CHECK(wait_retired_empty(slot));
    slot.join_all();
    CHECK(!slot.active_joinable());
}

}  // namespace

int main() {
    test_publish_guard();
    test_launch_and_reap_do_not_wait_for_active_worker();
    test_ui_paths_never_wait_for_blocked_worker_destructor();
    test_stop_request_sticky_and_observed();
    test_reaper_drains_multiple_retired_workers();
    test_join_all_waits_for_completion();
    test_join_all_releases_worker_lifetime();
    test_launch_storm_bounded_and_rejection_inert();
    test_reaper_start_failure_is_bounded_and_drained_at_teardown();
    test_initial_decode_cancel_and_publish_guards();
    test_initial_decode_slot_composition();
    std::printf("async_worker_slot_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

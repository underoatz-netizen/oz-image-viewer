// tests/print_controller_tests.cpp - unit and regression tests for print_controller lifecycle
// and printer DC release ordering (guaranteeing DC release before completion notification).

#include "print_controller.hpp"
#include "print_job.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

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

using oz::viewer::print_controller;
using oz::viewer::print_work_request;
using oz::viewer::print_error;

struct test_deleter_context {
    std::mutex mtx;
    std::condition_variable cv_entered;
    std::condition_variable cv_release;
    std::atomic<bool> release_entered{false};
    std::atomic<bool> release_returned{false};
    bool allow_release = false;
    std::atomic<bool> worker_timed_out{false};
    std::atomic<int> delete_count{0};
    std::vector<void*> deleted_ptrs;

    void reset() {
        std::lock_guard<std::mutex> lock(mtx);
        release_entered.store(false, std::memory_order_relaxed);
        release_returned.store(false, std::memory_order_relaxed);
        allow_release = false;
        worker_timed_out.store(false, std::memory_order_relaxed);
        delete_count.store(0, std::memory_order_relaxed);
        deleted_ptrs.clear();
    }

    void unblock() {
        {
            std::lock_guard<std::mutex> lock(mtx);
            allow_release = true;
        }
        cv_release.notify_all();
    }
};

static test_deleter_context g_deleter_ctx;

// RAII latch guard ensuring test_deleter_context is unblocked on all exit paths,
// so worker_thread_ never hangs in join() if an assertion or exception triggers.
struct auto_unblock_latch {
    ~auto_unblock_latch() {
        g_deleter_ctx.unblock();
    }
};

// Token 0x1000 is our accepted test token that blocks on allow_release.
// Any other token (e.g. 0x2000 from rejected submit) does not block.
void custom_test_deleter(void* dc) noexcept {
    if (!dc) return;
    if (dc == reinterpret_cast<void*>(0x1000)) {
        {
            std::lock_guard<std::mutex> lock(g_deleter_ctx.mtx);
            g_deleter_ctx.release_entered.store(true, std::memory_order_release);
        }
        g_deleter_ctx.cv_entered.notify_all();

        {
            std::unique_lock<std::mutex> lock(g_deleter_ctx.mtx);
            const bool signaled = g_deleter_ctx.cv_release.wait_for(
                lock, std::chrono::seconds(10), [] {
                    return g_deleter_ctx.allow_release;
                });
            if (!signaled) {
                g_deleter_ctx.worker_timed_out.store(true, std::memory_order_release);
            }
            g_deleter_ctx.deleted_ptrs.push_back(dc);
            g_deleter_ctx.delete_count.fetch_add(1, std::memory_order_relaxed);
        }

        g_deleter_ctx.release_returned.store(true, std::memory_order_release);
    } else {
        std::unique_lock<std::mutex> lock(g_deleter_ctx.mtx);
        g_deleter_ctx.deleted_ptrs.push_back(dc);
        g_deleter_ctx.delete_count.fetch_add(1, std::memory_order_relaxed);
    }
}

static std::atomic<int> g_simple_delete_count{0};
void simple_counting_deleter(void* dc) noexcept {
    if (!dc) return;
    g_simple_delete_count.fetch_add(1, std::memory_order_relaxed);
}

// Test 1: Invariant proof - DC deleter returns BEFORE completion is published / observable.
// While deleter is blocked, pop_completions MUST be empty and is_active MUST be true.
void test_dc_release_before_completion_order() {
    g_deleter_ctx.reset();

    print_controller ctrl;
    auto_unblock_latch unblocker;

    // Notification message is 0 and hwnd is nullptr, meaning no PostMessage calls will happen.
    ctrl.start(nullptr, 0, custom_test_deleter);

    void* fake_dc_1 = reinterpret_cast<void*>(0x1000);
    print_work_request req1;
    req1.printer_dc = fake_dc_1;
    req1.document_instance_id = 101;
    req1.document_generation = 1;
    // Without existing_source and with needs_source_acquisition = false,
    // the worker reaches the (!source || !source->is_valid()) error branch,
    // which triggers finish(comp) without calling GDI.

    const uint64_t id1 = ctrl.submit_request(std::move(req1));
    CHECK(id1 != 0);

    // Wait until worker enters custom_test_deleter for fake_dc_1
    {
        std::unique_lock<std::mutex> lock(g_deleter_ctx.mtx);
        const bool entered = g_deleter_ctx.cv_entered.wait_for(
            lock, std::chrono::seconds(5), [] {
                return g_deleter_ctx.release_entered.load(std::memory_order_acquire);
            });
        CHECK(entered);
    }

    // While the deleter is blocked inside release_owned_dc:
    // 1. pop_completions MUST be completely empty (completion not published yet)
    auto comps_early = ctrl.pop_completions();
    CHECK(comps_early.empty());

    // 2. ctrl.is_active() MUST still be true
    CHECK(ctrl.is_active());

    // 3. A second submit attempt while the first request's DC is releasing MUST be rejected
    void* fake_dc_2 = reinterpret_cast<void*>(0x2000);
    print_work_request req2;
    req2.printer_dc = fake_dc_2;
    req2.document_instance_id = 102;
    req2.document_generation = 2;
    const uint64_t id2 = ctrl.submit_request(std::move(req2));
    CHECK(id2 == 0);

    // Verify rejected fake_dc_2 was immediately released via custom deleter
    {
        std::unique_lock<std::mutex> lock(g_deleter_ctx.mtx);
        bool found_dc2 = false;
        for (void* p : g_deleter_ctx.deleted_ptrs) {
            if (p == fake_dc_2) {
                found_dc2 = true;
                break;
            }
        }
        CHECK(found_dc2);
    }

    // Unblock the latch for fake_dc_1 under mutex and notify
    g_deleter_ctx.unblock();

    // Now wait for the worker to finish and post completion
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::vector<oz::viewer::print_completion_msg> comps;
    while (std::chrono::steady_clock::now() < deadline) {
        comps = ctrl.pop_completions();
        if (!comps.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    CHECK(!g_deleter_ctx.worker_timed_out.load(std::memory_order_acquire));
    CHECK(g_deleter_ctx.release_returned.load(std::memory_order_acquire));
    CHECK(comps.size() == 1);
    if (!comps.empty()) {
        CHECK(comps[0].request_id == id1);
        CHECK(comps[0].document_instance_id == 101);
        CHECK(comps[0].error == print_error::invalid_request);
    }

    // After pop_completions consumed, controller is inactive and ready for next request
    CHECK(!ctrl.is_active());

    ctrl.stop();
}

// Test 2: Sequential execution (print -> print) through actual controller
void test_sequential_requests() {
    g_simple_delete_count.store(0);

    print_controller ctrl;
    ctrl.start(nullptr, 0, simple_counting_deleter);

    for (uint64_t i = 1; i <= 3; ++i) {
        void* fake_dc = reinterpret_cast<void*>(0x3000 + i);
        print_work_request req;
        req.printer_dc = fake_dc;
        req.document_instance_id = 200 + i;
        req.document_generation = i;

        const uint64_t id = ctrl.submit_request(std::move(req));
        CHECK(id != 0);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        std::vector<oz::viewer::print_completion_msg> comps;
        while (std::chrono::steady_clock::now() < deadline) {
            comps = ctrl.pop_completions();
            if (!comps.empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        CHECK(comps.size() == 1);
        if (!comps.empty()) {
            CHECK(comps[0].request_id == id);
            CHECK(comps[0].document_instance_id == 200 + i);
            CHECK(comps[0].error == print_error::invalid_request);
        }
        CHECK(!ctrl.is_active());
    }

    CHECK(g_simple_delete_count.load() == 3);
    ctrl.stop();
}

// Test 3: Resource release on error / invalid request
// Explicit pure-spool cancellation coverage (StartDoc/Page/EndDoc/AbortDoc) is
// deterministically verified in print_job_tests.cpp (test_spool_cancel_before_start_makes_no_call,
// test_spool_failure_abort_paths). This test verifies that error/early-exit branches
// in the actual controller worker unconditionally execute DC release before completion.
void test_error_path_dc_release() {
    g_simple_delete_count.store(0);

    print_controller ctrl;
    ctrl.start(nullptr, 0, simple_counting_deleter);

    void* fake_dc = reinterpret_cast<void*>(0x4000);
    print_work_request req;
    req.printer_dc = fake_dc;
    req.document_instance_id = 301;
    req.document_generation = 1;

    const uint64_t id = ctrl.submit_request(std::move(req));
    CHECK(id != 0);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::vector<oz::viewer::print_completion_msg> comps;
    while (std::chrono::steady_clock::now() < deadline) {
        comps = ctrl.pop_completions();
        if (!comps.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    CHECK(comps.size() == 1);
    if (!comps.empty()) {
        CHECK(comps[0].request_id == id);
        CHECK(comps[0].error == print_error::invalid_request);
    }

    CHECK(g_simple_delete_count.load() == 1);
    CHECK(!ctrl.is_active());

    ctrl.stop();
}

} // namespace

int main() {
    test_dc_release_before_completion_order();
    test_sequential_requests();
    test_error_path_dc_release();

    std::printf("print_controller_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

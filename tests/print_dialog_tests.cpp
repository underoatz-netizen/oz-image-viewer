// tests/print_dialog_tests.cpp - unit and regression tests for show_print_dialog helper:
// COM STA initialization lifecycle, PrintDlgExW result ownership, handle cleanup,
// and attempt-level reentrancy guards.

#include "print_dialog.hpp"
#include "ui_command_state.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
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

using namespace oz::viewer;

enum class mock_stage {
    init,
    dialog,
    cleanup,
    uninit,
    done
};

struct mock_dialog_state {
    HRESULT co_init_ret = S_OK;
    int co_init_count = 0;
    int co_uninit_count = 0;
    int print_dlg_ex_count = 0;
    int global_free_count = 0;
    int delete_dc_count = 0;

    HRESULT print_dlg_hr = S_OK;
    DWORD print_dlg_action = PD_RESULT_PRINT;
    HDC print_dlg_hdc = reinterpret_cast<HDC>(0x5000);
    HGLOBAL print_dlg_devmode = reinterpret_cast<HGLOBAL>(0x6000);
    HGLOBAL print_dlg_devnames = reinterpret_cast<HGLOBAL>(0x7000);

    mock_stage current_stage = mock_stage::init;
    std::function<void(mock_stage)> on_stage_hook;

    std::vector<HGLOBAL> freed_globals;
    std::vector<HDC> deleted_dcs;
    std::vector<std::string> event_order;

    void reset() {
        co_init_ret = S_OK;
        co_init_count = 0;
        co_uninit_count = 0;
        print_dlg_ex_count = 0;
        global_free_count = 0;
        delete_dc_count = 0;
        print_dlg_hr = S_OK;
        print_dlg_action = PD_RESULT_PRINT;
        print_dlg_hdc = reinterpret_cast<HDC>(0x5000);
        print_dlg_devmode = reinterpret_cast<HGLOBAL>(0x6000);
        print_dlg_devnames = reinterpret_cast<HGLOBAL>(0x7000);
        current_stage = mock_stage::init;
        on_stage_hook = nullptr;
        freed_globals.clear();
        deleted_dcs.clear();
        event_order.clear();
    }
};

static mock_dialog_state g_mock;

HRESULT mock_co_init(void*, DWORD) noexcept {
    g_mock.co_init_count++;
    g_mock.event_order.push_back("co_init");
    g_mock.current_stage = mock_stage::init;
    if (g_mock.on_stage_hook) {
        g_mock.on_stage_hook(mock_stage::init);
    }
    return g_mock.co_init_ret;
}

void mock_co_uninit() noexcept {
    g_mock.co_uninit_count++;
    g_mock.event_order.push_back("co_uninit");
    g_mock.current_stage = mock_stage::uninit;
    if (g_mock.on_stage_hook) {
        g_mock.on_stage_hook(mock_stage::uninit);
    }
}

HRESULT mock_print_dlg_ex(LPPRINTDLGEXW lppd) noexcept {
    g_mock.print_dlg_ex_count++;
    g_mock.event_order.push_back("print_dlg_ex");
    g_mock.current_stage = mock_stage::dialog;
    if (g_mock.on_stage_hook) {
        g_mock.on_stage_hook(mock_stage::dialog);
    }
    if (lppd) {
        lppd->dwResultAction = g_mock.print_dlg_action;
        lppd->hDC = g_mock.print_dlg_hdc;
        lppd->hDevMode = g_mock.print_dlg_devmode;
        lppd->hDevNames = g_mock.print_dlg_devnames;
    }
    return g_mock.print_dlg_hr;
}

HGLOBAL mock_global_free(HGLOBAL h) noexcept {
    if (h) {
        g_mock.global_free_count++;
        g_mock.freed_globals.push_back(h);
        g_mock.event_order.push_back("global_free");
    }
    g_mock.current_stage = mock_stage::cleanup;
    if (g_mock.on_stage_hook) {
        g_mock.on_stage_hook(mock_stage::cleanup);
    }
    return nullptr;
}

BOOL mock_delete_dc(HDC hdc) noexcept {
    if (hdc) {
        g_mock.delete_dc_count++;
        g_mock.deleted_dcs.push_back(hdc);
        g_mock.event_order.push_back("delete_dc");
    }
    g_mock.current_stage = mock_stage::cleanup;
    if (g_mock.on_stage_hook) {
        g_mock.on_stage_hook(mock_stage::cleanup);
    }
    return TRUE;
}

HRESULT mock_co_get_apartment(APTTYPE* pType, APTTYPEQUALIFIER* pQual) noexcept {
    if (pType) *pType = APTTYPE_MAINSTA;
    if (pQual) *pQual = APTTYPEQUALIFIER_NONE;
    return S_OK;
}

print_dialog_api_seam make_mock_seam() {
    print_dialog_api_seam seam;
    seam.co_init = mock_co_init;
    seam.co_uninit = mock_co_uninit;
    seam.print_dlg_ex = mock_print_dlg_ex;
    seam.global_free = mock_global_free;
    seam.delete_dc = mock_delete_dc;
    seam.co_get_apartment = mock_co_get_apartment;
    return seam;
}

// Minimal mock state type mirroring app_state for testing basic_print_attempt_guard
struct mock_app_state {
    bool print_dialog_in_progress = false;
    bool pending_print_installed = false;
    int refresh_count = 0;

    [[nodiscard]] bool is_print_active() const noexcept {
        return print_dialog_in_progress || pending_print_installed;
    }
};

// Test 1: S_OK and S_FALSE both balance CoUninitialize exactly once.
// Tests drop unconsumed result and verifies custom deleter invokes without OS DeleteDC.
void test_com_init_balance_and_unconsumed_destructor() {
    auto seam = make_mock_seam();

    // S_OK case: unconsumed result drops and destructor frees hdc via deleter seam
    {
        g_mock.reset();
        g_mock.co_init_ret = S_OK;
        {
            auto res = show_print_dialog(nullptr, &seam);
            CHECK(res.outcome == print_dialog_outcome::success_print);
            CHECK(g_mock.co_init_count == 1);
            CHECK(g_mock.co_uninit_count == 1);
            CHECK(g_mock.print_dlg_ex_count == 1);
            CHECK(res.hdc == reinterpret_cast<HDC>(0x5000));
            CHECK(g_mock.delete_dc_count == 0); // Not deleted while res is alive
        }
        // Exiting scope drops res: destructor MUST invoke mock_delete_dc exact-once
        CHECK(g_mock.delete_dc_count == 1);
        CHECK(!g_mock.deleted_dcs.empty());
        CHECK(g_mock.deleted_dcs[0] == reinterpret_cast<HDC>(0x5000));
    }

    // S_FALSE case: tests move-construction and move-assignment preserving deleter
    {
        g_mock.reset();
        g_mock.co_init_ret = S_FALSE;
        {
            auto res1 = show_print_dialog(nullptr, &seam);
            CHECK(res1.outcome == print_dialog_outcome::success_print);
            CHECK(g_mock.co_uninit_count == 1);
            print_dialog_result res2 = std::move(res1);
            CHECK(res1.hdc == nullptr);
            CHECK(res2.hdc == reinterpret_cast<HDC>(0x5000));

            print_dialog_result res3;
            res3 = std::move(res2);
            CHECK(res2.hdc == nullptr);
            CHECK(res3.hdc == reinterpret_cast<HDC>(0x5000));
            CHECK(g_mock.delete_dc_count == 0);
        }
        CHECK(g_mock.delete_dc_count == 1);
    }
}

// Test 2: RPC_E_CHANGED_MODE or failure: NO Dialog, NO Uninit, returns com_init_failed
void test_com_init_failure_no_dialog_no_uninit() {
    auto seam = make_mock_seam();
    g_mock.reset();
    g_mock.co_init_ret = RPC_E_CHANGED_MODE;

    auto res = show_print_dialog(nullptr, &seam);
    CHECK(res.outcome == print_dialog_outcome::com_init_failed);
    CHECK(res.hr == RPC_E_CHANGED_MODE);
    CHECK(g_mock.co_init_count == 1);
    CHECK(g_mock.co_uninit_count == 0);      // NEVER uninitialize on failure!
    CHECK(g_mock.print_dlg_ex_count == 0);   // NEVER enter dialog on COM failure!
    CHECK(res.hdc == nullptr);
}

// Test 3: Cancellation, Apply, and Dialog failure all clean DevMode, DevNames, HDC, and balance uninit
void test_cancel_apply_failure_cleanup_and_order() {
    auto seam = make_mock_seam();

    // Cancel: verify resource cleanup order precedes co_uninit
    {
        g_mock.reset();
        g_mock.print_dlg_action = PD_RESULT_CANCEL;
        auto res = show_print_dialog(nullptr, &seam);
        CHECK(res.outcome == print_dialog_outcome::cancelled);
        CHECK(res.hdc == nullptr);
        CHECK(g_mock.global_free_count == 2); // DevMode + DevNames
        CHECK(g_mock.delete_dc_count == 1);   // HDC freed
        CHECK(g_mock.co_uninit_count == 1);

        // Verification of exact order: cleanup must happen BEFORE co_uninit
        bool found_cleanup = false;
        bool uninit_after_cleanup = false;
        for (const auto& ev : g_mock.event_order) {
            if (ev == "global_free" || ev == "delete_dc") {
                found_cleanup = true;
            }
            if (found_cleanup && ev == "co_uninit") {
                uninit_after_cleanup = true;
            }
        }
        CHECK(found_cleanup);
        CHECK(uninit_after_cleanup);
    }

    // Apply only
    {
        g_mock.reset();
        g_mock.print_dlg_action = PD_RESULT_APPLY;
        auto res = show_print_dialog(nullptr, &seam);
        CHECK(res.outcome == print_dialog_outcome::apply_only);
        CHECK(res.hdc == nullptr);
        CHECK(g_mock.global_free_count == 2);
        CHECK(g_mock.delete_dc_count == 1);
        CHECK(g_mock.co_uninit_count == 1);
    }

    // HRESULT Dialog failure
    {
        g_mock.reset();
        g_mock.print_dlg_hr = E_FAIL;
        auto res = show_print_dialog(nullptr, &seam);
        CHECK(res.outcome == print_dialog_outcome::dialog_failed);
        CHECK(res.hdc == nullptr);
        CHECK(g_mock.global_free_count == 2);
        CHECK(g_mock.delete_dc_count == 1);
        CHECK(g_mock.co_uninit_count == 1);
    }

    // PD_RESULT_PRINT with null DC treated as explicit dialog_failed
    {
        g_mock.reset();
        g_mock.print_dlg_action = PD_RESULT_PRINT;
        g_mock.print_dlg_hdc = nullptr;
        auto res = show_print_dialog(nullptr, &seam);
        CHECK(res.outcome == print_dialog_outcome::dialog_failed);
        CHECK(res.hdc == nullptr);
        CHECK(g_mock.co_uninit_count == 1);
    }
}

// Test 4: Real basic_print_attempt_guard reentrancy across all stages
void test_reentrancy_with_actual_guard() {
    auto seam = make_mock_seam();
    g_mock.reset();

    mock_app_state state;
    CHECK(!state.is_print_active());

    int stages_hooked = 0;

    auto try_second_start_print = [&](const mock_app_state& s) -> bool {
        // Mirrors main.cpp:
        if (s.is_print_active()) {
            return false; // Rejected
        }
        ui_command_context ctx{};
        ctx.have_image = true;
        ctx.has_document = true;
        ctx.has_source = true;
        ctx.has_file_path = true;
        ctx.print_busy = s.is_print_active();
        const auto st = derive_command_state(ui_command_id::file_print, ctx);
        return st.enabled;
    };

    g_mock.on_stage_hook = [&](mock_stage /*stage*/) {
        stages_hooked++;
        // 1. Guard has state.print_dialog_in_progress active during this stage:
        CHECK(state.print_dialog_in_progress);
        CHECK(state.is_print_active());

        // 2. ui_command_context command state is disabled:
        ui_command_context ctx{};
        ctx.have_image = true;
        ctx.has_document = true;
        ctx.has_source = true;
        ctx.has_file_path = true;
        ctx.print_busy = state.is_print_active();
        const auto st = derive_command_state(ui_command_id::file_print, ctx);
        CHECK(!st.enabled);

        // 3. Second start_print attempt is rejected:
        CHECK(!try_second_start_print(state));
    };

    // Execute with actual basic_print_attempt_guard
    {
        basic_print_attempt_guard guard(state, [](mock_app_state& s) {
            s.refresh_count++;
        });

        CHECK(guard.is_active());
        CHECK(state.print_dialog_in_progress);
        CHECK(state.is_print_active());
        CHECK(state.refresh_count == 1);

        auto res = show_print_dialog(nullptr, &seam);
        CHECK(res.outcome == print_dialog_outcome::success_print);
        CHECK(stages_hooked >= 4); // init, dialog, cleanup, uninit

        // Assert busy remains while caller prepares submission
        CHECK(state.is_print_active());

        // Simulate successful controller submission transition:
        state.pending_print_installed = true;
        guard.disengage_and_preserve();
        CHECK(!guard.is_active());
        CHECK(!state.print_dialog_in_progress);
        // Busy state seamlessly preserved through pending_print_installed:
        CHECK(state.is_print_active());
        CHECK(!try_second_start_print(state));

        HDC hdc_owned = res.release_hdc();
        CHECK(hdc_owned != nullptr);
    }

    // Simulate completion consumed:
    state.pending_print_installed = false;
    CHECK(!state.is_print_active());
    CHECK(try_second_start_print(state));

    // Destructor path (early-return/error cleanup) clears the flag and refreshes.
    mock_app_state early_state;
    {
        basic_print_attempt_guard early_guard(early_state, [](mock_app_state& s) {
            s.refresh_count++;
        });
        CHECK(early_state.print_dialog_in_progress);
        CHECK(early_state.is_print_active());
    }
    CHECK(!early_state.print_dialog_in_progress);
    CHECK(!early_state.is_print_active());
    CHECK(early_state.refresh_count == 2);
}

// Test 5: Sequential attempts (Cancel -> Cancel -> Success, InitFail -> Success, Fail -> Success)
void test_sequential_lifecycle_and_recovery() {
    auto seam = make_mock_seam();

    // 1. Cancel -> Cancel -> Success
    {
        g_mock.reset();
        g_mock.print_dlg_action = PD_RESULT_CANCEL;
        auto res1 = show_print_dialog(nullptr, &seam);
        CHECK(res1.outcome == print_dialog_outcome::cancelled);
        CHECK(g_mock.co_uninit_count == 1);

        g_mock.reset();
        g_mock.print_dlg_action = PD_RESULT_CANCEL;
        auto res2 = show_print_dialog(nullptr, &seam);
        CHECK(res2.outcome == print_dialog_outcome::cancelled);
        CHECK(g_mock.co_uninit_count == 1);

        g_mock.reset();
        g_mock.print_dlg_action = PD_RESULT_PRINT;
        auto res3 = show_print_dialog(nullptr, &seam);
        CHECK(res3.outcome == print_dialog_outcome::success_print);
        CHECK(g_mock.co_uninit_count == 1);
        HDC hdc_owned = res3.release_hdc();
        CHECK(hdc_owned != nullptr);
    }

    // 2. Init failure (RPC_E_CHANGED_MODE) then recoverable retry (S_OK)
    {
        g_mock.reset();
        g_mock.co_init_ret = RPC_E_CHANGED_MODE;
        auto res1 = show_print_dialog(nullptr, &seam);
        CHECK(res1.outcome == print_dialog_outcome::com_init_failed);
        CHECK(g_mock.co_uninit_count == 0);

        g_mock.reset();
        g_mock.co_init_ret = S_OK;
        auto res2 = show_print_dialog(nullptr, &seam);
        CHECK(res2.outcome == print_dialog_outcome::success_print);
        CHECK(g_mock.co_uninit_count == 1);
        HDC hdc_owned = res2.release_hdc();
        CHECK(hdc_owned != nullptr);
    }

    // 3. General failure (E_FAIL) then recoverable retry (S_OK)
    {
        g_mock.reset();
        g_mock.print_dlg_hr = E_FAIL;
        auto res1 = show_print_dialog(nullptr, &seam);
        CHECK(res1.outcome == print_dialog_outcome::dialog_failed);
        CHECK(g_mock.co_uninit_count == 1);

        g_mock.reset();
        g_mock.print_dlg_hr = S_OK;
        g_mock.print_dlg_action = PD_RESULT_PRINT;
        auto res2 = show_print_dialog(nullptr, &seam);
        CHECK(res2.outcome == print_dialog_outcome::success_print);
        CHECK(g_mock.co_uninit_count == 1);
        HDC hdc_owned = res2.release_hdc();
        CHECK(hdc_owned != nullptr);
    }
}

// Test 6: Lock-free diagnostic buffer concurrent producer/copier, quiescent reset, and capacity bounds
void test_diagnostic_buffer_concurrency_and_bounds() {
    quiescent_reset_print_dialog_trace_buffer();

    print_dialog_trace_record recs[10];
    const size_t initial_copied = copy_print_dialog_trace_buffer(recs, 10);
#if defined(OZVIEWER_PRINT_DIAGNOSTICS)
    CHECK(initial_copied == 0);

    auto seam = make_mock_seam();
    g_mock.reset();

    // Concurrently trigger show_print_dialog attempts while a copier thread continuously reads
    std::atomic<bool> stop_copier{false};
    std::atomic<size_t> max_seen_valid{0};
    std::atomic<bool> copier_started{false};
    std::atomic<bool> invalid_stage_seen{false};

    std::thread copier([&] {
        copier_started.store(true, std::memory_order_release);
        print_dialog_trace_record local_buf[kMaxDiagTraceRecords];
        while (!stop_copier.load(std::memory_order_relaxed)) {
            size_t n = copy_print_dialog_trace_buffer(local_buf, kMaxDiagTraceRecords);
            if (n > 0) {
                // Verify every copied record is structurally valid (stage <= 11)
                for (size_t i = 0; i < n; ++i) {
                    if (local_buf[i].stage > 11) {
                        invalid_stage_seen.store(true, std::memory_order_relaxed);
                    }
                }
                size_t prev = max_seen_valid.load(std::memory_order_relaxed);
                while (n > prev && !max_seen_valid.compare_exchange_weak(prev, n, std::memory_order_relaxed)) {}
            }
            std::this_thread::yield();
        }
        // One final copy after stop signaled to capture published records
        size_t n = copy_print_dialog_trace_buffer(local_buf, kMaxDiagTraceRecords);
        if (n > 0) {
            size_t prev = max_seen_valid.load(std::memory_order_relaxed);
            while (n > prev && !max_seen_valid.compare_exchange_weak(prev, n, std::memory_order_relaxed)) {}
        }
    });

    while (!copier_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    // Run 3 dialog attempts
    for (int i = 0; i < 3; ++i) {
        g_mock.reset();
        auto res = show_print_dialog(nullptr, &seam);
        HDC hdc_owned = res.release_hdc();
        CHECK(hdc_owned != nullptr);
    }

    stop_copier.store(true, std::memory_order_relaxed);
    copier.join();

    CHECK(!invalid_stage_seen.load(std::memory_order_relaxed));

    print_dialog_trace_record final_buf[kMaxDiagTraceRecords];
    size_t final_copied = copy_print_dialog_trace_buffer(final_buf, kMaxDiagTraceRecords);
    // 3 attempts * 12 stages = 36 records published
    CHECK(final_copied == 36);
    CHECK(max_seen_valid.load() > 0);

    // Verify apartment post-query is recorded
    bool found_pre_query = false;
    bool found_post_query = false;
    for (size_t i = 0; i < final_copied; ++i) {
        if (final_buf[i].stage == static_cast<uint32_t>(print_dialog_trace_stage::co_get_apartment_pre_query)) {
            found_pre_query = true;
        }
        if (final_buf[i].stage == static_cast<uint32_t>(print_dialog_trace_stage::co_get_apartment_post_query)) {
            found_post_query = true;
        }
    }
    CHECK(found_pre_query);
    CHECK(found_post_query);

    // Quiescent reset
    quiescent_reset_print_dialog_trace_buffer();
    size_t after_reset = copy_print_dialog_trace_buffer(final_buf, kMaxDiagTraceRecords);
    CHECK(after_reset == 0);
#else
    CHECK(initial_copied == 0);
#endif
}

} // namespace

int main() {
    test_com_init_balance_and_unconsumed_destructor();
    test_com_init_failure_no_dialog_no_uninit();
    test_cancel_apply_failure_cleanup_and_order();
    test_reentrancy_with_actual_guard();
    test_sequential_lifecycle_and_recovery();
    test_diagnostic_buffer_concurrency_and_bounds();

    std::printf("print_dialog_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

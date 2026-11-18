// src/viewer/print_dialog.cpp - implementation of show_print_dialog with strict local STA
// apartment lifecycle, complete handle cleanup, and lock-free in-memory diagnostic buffer.
#include "print_dialog.hpp"

#include <atomic>

namespace oz::viewer {

#if defined(OZVIEWER_PRINT_DIAGNOSTICS)
// Private exported DATA symbol: fixed ABI, zero heap allocation, zero blocking I/O
extern "C" OZVIEWER_PRINT_DIAG_EXPORT print_dialog_diagnostic_buffer g_ozviewer_print_diag_data = {
    1,                                             // abi_version = 1
    static_cast<uint32_t>(sizeof(print_dialog_trace_record)), // record_size = 32
    static_cast<uint32_t>(kMaxDiagTraceRecords),   // capacity = 128
    0,                                             // reserved
    0,                                             // reservations
    {},                                            // records
    {}                                             // ready
};

void record_to_diag_buffer(const print_dialog_trace_record& rec) noexcept {
    const uint32_t idx = g_ozviewer_print_diag_data.reservations.fetch_add(1, std::memory_order_relaxed);
    if (idx < kMaxDiagTraceRecords) {
        // Write record contents BEFORE marking slot as ready
        g_ozviewer_print_diag_data.records[idx] = rec;
        // Publish with release ordering
        g_ozviewer_print_diag_data.ready[idx].store(1, std::memory_order_release);
    }
}
#endif

namespace {

static std::atomic<uint32_t> g_print_attempt_counter{0};

inline HRESULT default_co_init(void* pvReserved, DWORD dwCoInit) noexcept {
    return CoInitializeEx(pvReserved, dwCoInit);
}

inline void default_co_uninit() noexcept {
    CoUninitialize();
}

inline HRESULT default_print_dlg_ex(LPPRINTDLGEXW lppd) noexcept {
    return PrintDlgExW(lppd);
}

inline HGLOBAL default_global_free(HGLOBAL hMem) noexcept {
    return GlobalFree(hMem);
}

inline BOOL default_delete_dc(HDC hdc) noexcept {
    return DeleteDC(hdc);
}

inline HRESULT default_co_get_apartment(APTTYPE* pAptType, APTTYPEQUALIFIER* pAptQualifier) noexcept {
    return CoGetApartmentType(pAptType, pAptQualifier);
}

}  // namespace

size_t copy_print_dialog_trace_buffer(print_dialog_trace_record* out_dest, size_t max_count) noexcept {
#if defined(OZVIEWER_PRINT_DIAGNOSTICS)
    if (!out_dest || max_count == 0) return 0;
    const uint32_t reserved = g_ozviewer_print_diag_data.reservations.load(std::memory_order_relaxed);
    const size_t bound = (reserved < kMaxDiagTraceRecords) ? reserved : kMaxDiagTraceRecords;
    const size_t limit = (bound < max_count) ? bound : max_count;
    size_t copied = 0;
    for (size_t i = 0; i < limit; ++i) {
        // Only read slots that are fully published
        if (g_ozviewer_print_diag_data.ready[i].load(std::memory_order_acquire) == 1) {
            out_dest[copied] = g_ozviewer_print_diag_data.records[i];
            copied++;
        }
    }
    return copied;
#else
    (void)out_dest;
    (void)max_count;
    return 0;
#endif
}

void quiescent_reset_print_dialog_trace_buffer() noexcept {
#if defined(OZVIEWER_PRINT_DIAGNOSTICS)
    // For quiescent test setup only
    for (size_t i = 0; i < kMaxDiagTraceRecords; ++i) {
        g_ozviewer_print_diag_data.ready[i].store(0, std::memory_order_relaxed);
    }
    g_ozviewer_print_diag_data.reservations.store(0, std::memory_order_release);
#endif
}

print_dialog_result show_print_dialog(
    HWND hwnd_owner,
    const print_dialog_api_seam* custom_seam,
    print_dialog_trace_callback trace_cb,
    void* trace_user_data) noexcept {

    const uint32_t attempt_id = g_print_attempt_counter.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint32_t thread_id = static_cast<uint32_t>(GetCurrentThreadId());

    auto do_trace = [&](print_dialog_trace_stage stage, HRESULT hr = S_OK,
                        int apt_type = -1, int apt_qual = -1,
                        DWORD dw_action = 0) noexcept {
        print_dialog_trace_record rec{
            static_cast<uint32_t>(stage), attempt_id, thread_id, static_cast<uint32_t>(GetTickCount()),
            static_cast<int32_t>(hr), apt_type, apt_qual, dw_action
        };
        if (trace_cb) {
            trace_cb(rec, trace_user_data);
        }
#if defined(OZVIEWER_PRINT_DIAGNOSTICS)
        record_to_diag_buffer(rec);
#endif
    };

    do_trace(print_dialog_trace_stage::attempt_enter);

    print_dialog_co_init_fn fn_co_init = default_co_init;
    print_dialog_co_uninit_fn fn_co_uninit = default_co_uninit;
    print_dialog_print_dlg_ex_fn fn_print_dlg_ex = default_print_dlg_ex;
    print_dialog_global_free_fn fn_global_free = default_global_free;
    print_dialog_delete_dc_fn fn_delete_dc = default_delete_dc;
    print_dialog_co_get_apartment_fn fn_co_get_apartment = default_co_get_apartment;

    if (custom_seam) {
        if (custom_seam->co_init) fn_co_init = custom_seam->co_init;
        if (custom_seam->co_uninit) fn_co_uninit = custom_seam->co_uninit;
        if (custom_seam->print_dlg_ex) fn_print_dlg_ex = custom_seam->print_dlg_ex;
        if (custom_seam->global_free) fn_global_free = custom_seam->global_free;
        if (custom_seam->delete_dc) fn_delete_dc = custom_seam->delete_dc;
        if (custom_seam->co_get_apartment) fn_co_get_apartment = custom_seam->co_get_apartment;
    }

    // 1. Diagnostic apartment pre-query
    APTTYPE apt_type = APTTYPE_CURRENT;
    APTTYPEQUALIFIER apt_qual = APTTYPEQUALIFIER_NONE;
    HRESULT hr_apt = fn_co_get_apartment(&apt_type, &apt_qual);
    if (SUCCEEDED(hr_apt)) {
        do_trace(print_dialog_trace_stage::co_get_apartment_pre_query, hr_apt,
                 static_cast<int>(apt_type), static_cast<int>(apt_qual));
    } else {
        do_trace(print_dialog_trace_stage::co_get_apartment_pre_query, hr_apt);
    }

    // 2. CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)
    do_trace(print_dialog_trace_stage::co_init_enter);
    const HRESULT hr_init = fn_co_init(nullptr, COINIT_APARTMENTTHREADED);
    do_trace(print_dialog_trace_stage::co_init_return, hr_init);

    const bool com_initialized = (hr_init == S_OK || hr_init == S_FALSE);

    print_dialog_result result;
    result.deleter = fn_delete_dc;

    if (!com_initialized) {
        result.outcome = print_dialog_outcome::com_init_failed;
        result.hr = hr_init;
        result.detail = "CoInitializeEx failed";
        do_trace(print_dialog_trace_stage::attempt_return, hr_init);
        return result;
    }

    // 3. Post-init query immediately before dialog call
    apt_type = APTTYPE_CURRENT;
    apt_qual = APTTYPEQUALIFIER_NONE;
    hr_apt = fn_co_get_apartment(&apt_type, &apt_qual);
    if (SUCCEEDED(hr_apt)) {
        do_trace(print_dialog_trace_stage::co_get_apartment_post_query, hr_apt,
                 static_cast<int>(apt_type), static_cast<int>(apt_qual));
    } else {
        do_trace(print_dialog_trace_stage::co_get_apartment_post_query, hr_apt);
    }

    PRINTDLGEXW pdx{};
    pdx.lStructSize = sizeof(pdx);
    pdx.hwndOwner = hwnd_owner;
    pdx.Flags = PD_RETURNDC | PD_NOPAGENUMS | PD_NOSELECTION;
    pdx.nStartPage = START_PAGE_GENERAL;

    // 4. PrintDlgExW call
    do_trace(print_dialog_trace_stage::dialog_enter);
    const HRESULT dlg_hr = fn_print_dlg_ex(&pdx);
    do_trace(print_dialog_trace_stage::dialog_return, dlg_hr, -1, -1, pdx.dwResultAction);

    result.hr = dlg_hr;
    result.result_action = pdx.dwResultAction;

    if (SUCCEEDED(dlg_hr)) {
        if (pdx.dwResultAction == PD_RESULT_PRINT) {
            if (pdx.hDC != nullptr) {
                result.outcome = print_dialog_outcome::success_print;
                result.hdc = pdx.hDC;
                pdx.hDC = nullptr;  // Ownership transferred to result
            } else {
                result.outcome = print_dialog_outcome::dialog_failed;
                result.detail = "PrintDlgEx returned PD_RESULT_PRINT with null DC";
            }
        } else if (pdx.dwResultAction == PD_RESULT_APPLY) {
            result.outcome = print_dialog_outcome::apply_only;
        } else {
            result.outcome = print_dialog_outcome::cancelled;
        }
    } else {
        result.outcome = print_dialog_outcome::dialog_failed;
        result.detail = "PrintDlgExW returned failure HRESULT";
    }

    // 5. Cleanup handles before COM uninitialization
    do_trace(print_dialog_trace_stage::cleanup_enter);
    if (pdx.hDevMode) {
        fn_global_free(pdx.hDevMode);
        pdx.hDevMode = nullptr;
    }
    if (pdx.hDevNames) {
        fn_global_free(pdx.hDevNames);
        pdx.hDevNames = nullptr;
    }
    if (pdx.hDC) {
        fn_delete_dc(pdx.hDC);
        pdx.hDC = nullptr;
    }
    do_trace(print_dialog_trace_stage::cleanup_return);

    // 6. Uninitialize COM strictly AFTER all dialog-owned resources are freed
    do_trace(print_dialog_trace_stage::co_uninit_enter);
    fn_co_uninit();
    do_trace(print_dialog_trace_stage::co_uninit_return);

    do_trace(print_dialog_trace_stage::attempt_return, result.hr, -1, -1, result.result_action);

    return result;
}

}  // namespace oz::viewer

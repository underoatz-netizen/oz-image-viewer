// src/viewer/print_dialog.hpp - dedicated owning production helper for PrintDlgExW with
// strictly scoped apartment-threaded COM lifecycle, injected API seam, and private exported diagnostic buffer.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <commdlg.h>

#include <atomic>
#include <cstdint>
#include <string>

#if defined(OZVIEWER_PRINT_DIAGNOSTICS)
#define OZVIEWER_PRINT_DIAG_EXPORT __declspec(dllexport)
#else
#define OZVIEWER_PRINT_DIAG_EXPORT
#endif

namespace oz::viewer {

// Explicit outcome classification of a print dialog invocation.
enum class print_dialog_outcome {
    success_print,       // S_OK and dwResultAction == PD_RESULT_PRINT (HDC transferred)
    cancelled,           // User cancelled, closed dialog, or PD_RESULT_CANCEL (quiet, no job)
    apply_only,          // S_OK and dwResultAction == PD_RESULT_APPLY (no print requested, quiet)
    com_init_failed,     // CoInitializeEx returned an unrecoverable error (e.g. RPC_E_CHANGED_MODE)
    dialog_failed,       // PrintDlgExW failed (FAILED(hr), null HDC, or general dialog failure)
};

// Injected API function signatures for testing the dialog helper without OS/physical printer dependencies.
using print_dialog_co_init_fn = HRESULT (*)(void*, DWORD) noexcept;
using print_dialog_co_uninit_fn = void (*)() noexcept;
using print_dialog_print_dlg_ex_fn = HRESULT (*)(LPPRINTDLGEXW) noexcept;
using print_dialog_global_free_fn = HGLOBAL (*)(HGLOBAL) noexcept;
using print_dialog_delete_dc_fn = BOOL (*)(HDC) noexcept;
using print_dialog_co_get_apartment_fn = HRESULT (*)(APTTYPE*, APTTYPEQUALIFIER*) noexcept;

struct print_dialog_api_seam {
    print_dialog_co_init_fn co_init = nullptr;
    print_dialog_co_uninit_fn co_uninit = nullptr;
    print_dialog_print_dlg_ex_fn print_dlg_ex = nullptr;
    print_dialog_global_free_fn global_free = nullptr;
    print_dialog_delete_dc_fn delete_dc = nullptr;
    print_dialog_co_get_apartment_fn co_get_apartment = nullptr;
};

// Result object owning the transferred HDC with associated deleter.
struct print_dialog_result {
    print_dialog_outcome outcome = print_dialog_outcome::cancelled;
    HDC hdc = nullptr;  // Non-null ONLY on success_print
    HRESULT hr = S_OK;
    DWORD result_action = 0;
    const char* detail = nullptr; // Non-allocating static description
    print_dialog_delete_dc_fn deleter = nullptr;

    print_dialog_result() = default;
    ~print_dialog_result() {
        reset();
    }

    print_dialog_result(const print_dialog_result&) = delete;
    print_dialog_result& operator=(const print_dialog_result&) = delete;

    print_dialog_result(print_dialog_result&& other) noexcept
        : outcome(other.outcome),
          hdc(other.hdc),
          hr(other.hr),
          result_action(other.result_action),
          detail(other.detail),
          deleter(other.deleter) {
        other.hdc = nullptr;
        other.outcome = print_dialog_outcome::cancelled;
        other.detail = nullptr;
    }

    print_dialog_result& operator=(print_dialog_result&& other) noexcept {
        if (this != &other) {
            reset();
            outcome = other.outcome;
            hdc = other.hdc;
            hr = other.hr;
            result_action = other.result_action;
            detail = other.detail;
            deleter = other.deleter;
            other.hdc = nullptr;
            other.outcome = print_dialog_outcome::cancelled;
            other.detail = nullptr;
        }
        return *this;
    }

    void reset() noexcept {
        if (hdc) {
            if (deleter) {
                deleter(hdc);
            } else {
                DeleteDC(hdc);
            }
            hdc = nullptr;
        }
    }

    [[nodiscard]] HDC release_hdc() noexcept {
        HDC out = hdc;
        hdc = nullptr;
        return out;
    }
};

// Trace events for private bounded diagnostics and unit tests.
enum class print_dialog_trace_stage : uint32_t {
    attempt_enter = 0,
    co_get_apartment_pre_query = 1,
    co_init_enter = 2,
    co_init_return = 3,
    co_get_apartment_post_query = 4,
    dialog_enter = 5,
    dialog_return = 6,
    cleanup_enter = 7,
    cleanup_return = 8,
    co_uninit_enter = 9,
    co_uninit_return = 10,
    attempt_return = 11,
};

// Fixed ABI trace record layout: 32 bytes total.
// No raw pointer values (privacy/ASLR); no documents, passwords, or filepaths.
struct print_dialog_trace_record {
    uint32_t stage;             // print_dialog_trace_stage cast
    uint32_t attempt_id;
    uint32_t thread_id;
    uint32_t tick_count;
    int32_t hr;
    int32_t apt_type;
    int32_t apt_qualifier;
    uint32_t dw_result_action;
};
static_assert(sizeof(print_dialog_trace_record) == 32, "print_dialog_trace_record ABI size must be 32 bytes");

using print_dialog_trace_callback = void (*)(const print_dialog_trace_record&, void*) noexcept;

// Production entry point to present the print dialog on the given owner window.
// Strictly scopes STA CoInitializeEx/CoUninitialize, frees DevMode/DevNames/unused HDCs,
// and returns move-only print_dialog_result.
print_dialog_result show_print_dialog(
    HWND hwnd_owner,
    const print_dialog_api_seam* custom_seam = nullptr,
    print_dialog_trace_callback trace_cb = nullptr,
    void* trace_user_data = nullptr) noexcept;

// Fixed ABI in-memory diagnostic buffer descriptor.
constexpr size_t kMaxDiagTraceRecords = 128;

#pragma pack(push, 8)
struct print_dialog_diagnostic_buffer {
    uint32_t abi_version;        // 1
    uint32_t record_size;        // sizeof(print_dialog_trace_record) = 32
    uint32_t capacity;           // 128
    uint32_t reserved;           // 0
    std::atomic<uint32_t> reservations; // Total reservations attempted
    print_dialog_trace_record records[kMaxDiagTraceRecords];
    std::atomic<uint32_t> ready[kMaxDiagTraceRecords]; // Per-slot publish flag (1 = ready)
};
#pragma pack(pop)

#if defined(OZVIEWER_PRINT_DIAGNOSTICS)
// Private exported DATA symbol for external observability via ReadProcessMemory without function injection.
extern "C" OZVIEWER_PRINT_DIAG_EXPORT print_dialog_diagnostic_buffer g_ozviewer_print_diag_data;
#endif

// In-memory bounded diagnostic trace buffer access:
// Only reads slots whose per-slot ready flag is 1 with acquire ordering.
size_t copy_print_dialog_trace_buffer(print_dialog_trace_record* out_dest, size_t max_count) noexcept;

// Quiescent test-only buffer reset (for isolated unit tests only).
void quiescent_reset_print_dialog_trace_buffer() noexcept;

// RAII Print Attempt Guard for tracking in-progress dialog and gating reentrancy.
template <typename StateType, typename RefreshFn>
class basic_print_attempt_guard {
public:
    basic_print_attempt_guard(StateType& state, RefreshFn refresh)
        : state_(state), refresh_(refresh) {
        state_.print_dialog_in_progress = true;
        refresh_(state_);
    }

    ~basic_print_attempt_guard() {
        if (active_) {
            state_.print_dialog_in_progress = false;
            refresh_(state_);
        }
    }

    basic_print_attempt_guard(const basic_print_attempt_guard&) = delete;
    basic_print_attempt_guard& operator=(const basic_print_attempt_guard&) = delete;

    void disengage_and_preserve() noexcept {
        state_.print_dialog_in_progress = false;
        active_ = false;
    }

    [[nodiscard]] bool is_active() const noexcept {
        return active_;
    }

private:
    StateType& state_;
    RefreshFn refresh_;
    bool active_ = true;
};

}  // namespace oz::viewer

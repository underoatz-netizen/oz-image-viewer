// oz/viewer/shell_integration.hpp - On-demand shell integration boundary (Stage 9.1).
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <filesystem>
#include <functional>

namespace oz::viewer {

enum class shell_reveal_status {
    success,
    invalid_path,
    not_found,
    is_directory,
    com_init_failed,
    shell_failed
};

struct shell_reveal_result {
    shell_reveal_status status = shell_reveal_status::invalid_path;
    HRESULT hr = S_OK;

    [[nodiscard]] constexpr bool ok() const noexcept {
        return status == shell_reveal_status::success;
    }
};

// Seam function type for platform shell invocation:
// Given absolute wide path, reveal/select it in Explorer and return HRESULT.
using shell_reveal_invoker = std::function<HRESULT(const wchar_t* path)>;

// Production Win32 PIDL implementation:
// Calls SHParseDisplayName + SHOpenFolderAndSelectItems.
[[nodiscard]] HRESULT default_shell_reveal_invoker(const wchar_t* path) noexcept;

// Reveal file in Explorer with custom or default platform seam.
// Validates path: rejects empty, non-existent, non-regular-file (e.g. directories).
// Scopes COM init/uninit and handles RPC_E_CHANGED_MODE safely.
[[nodiscard]] shell_reveal_result reveal_file_in_explorer(
    const std::filesystem::path& file_path,
    shell_reveal_invoker invoker = nullptr);

// --- Stage 9.7 Recycle Bin boundary -------------------------------------------
// Narrow, testable file-recycling seam: moves exactly one validated regular
// file to the Shell Recycle Bin. Permanent deletion is never used: the
// production invoker always sets FOF_ALLOWUNDO.
//
// Contract (checked in order, invoker runs last):
//   1. empty path            -> invalid_path (invoker never runs)
//   2. missing / access fail -> not_found
//   3. directory             -> is_directory
//   4. source-identity recheck immediately before the shell operation
//      (verify_source_file_metadata against expected_size/expected_last_write;
//      zero expectations only assert existence/readability) ->
//      identity_mismatch when the file exists but no longer matches, not_found
//      when it vanished between validation and the recheck
//   5. shell operation fails or the seam throws -> recycle_failed
//   6. shell reports user cancellation (abort) -> cancelled (quiet path: the
//      caller preserves all state without an error prompt)
enum class recycle_status {
    success,
    invalid_path,
    not_found,
    is_directory,
    identity_mismatch,
    recycle_failed,
    cancelled
};

struct recycle_result {
    recycle_status status = recycle_status::invalid_path;
    HRESULT hr = S_OK;

    [[nodiscard]] constexpr bool ok() const noexcept {
        return status == recycle_status::success;
    }
};

// Seam function type for platform recycle invocation:
// Given absolute wide path, move it to the Recycle Bin and return HRESULT.
using recycle_invoker = std::function<HRESULT(const wchar_t* path)>;

// Production SHFileOperationW implementation (FO_DELETE with FOF_ALLOWUNDO,
// so the file is recycled, never permanently deleted).
[[nodiscard]] HRESULT default_recycle_invoker(const wchar_t* path) noexcept;

// Move one file to the Recycle Bin with custom or default platform seam.
[[nodiscard]] recycle_result recycle_file_to_bin(
    const std::filesystem::path& file_path,
    std::uint64_t expected_file_size = 0,
    std::uint64_t expected_last_write_time = 0,
    recycle_invoker invoker = nullptr);

}  // namespace oz::viewer

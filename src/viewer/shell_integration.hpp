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

}  // namespace oz::viewer

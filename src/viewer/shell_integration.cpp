// oz/viewer/shell_integration.cpp - On-demand shell integration boundary (Stage 9.1, 9.7).
#include "shell_integration.hpp"

#include "source_identity.hpp"

#include <shlobj.h>
#include <shellapi.h>
#include <system_error>
#include <vector>

namespace oz::viewer {

namespace {

class scoped_com_init {
public:
    scoped_com_init() noexcept {
        hr_ = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    }

    ~scoped_com_init() noexcept {
        if (SUCCEEDED(hr_)) {
            CoUninitialize();
        }
    }

    scoped_com_init(const scoped_com_init&) = delete;
    scoped_com_init& operator=(const scoped_com_init&) = delete;

    [[nodiscard]] bool ok() const noexcept {
        return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE;
    }

    [[nodiscard]] HRESULT hr() const noexcept {
        return hr_;
    }

private:
    HRESULT hr_ = E_FAIL;
};

struct co_task_mem_deleter {
    void operator()(void* ptr) const noexcept {
        if (ptr) {
            CoTaskMemFree(ptr);
        }
    }
};

using unique_pidl = std::unique_ptr<std::remove_pointer_t<PIDLIST_ABSOLUTE>, co_task_mem_deleter>;

}  // namespace

HRESULT default_shell_reveal_invoker(const wchar_t* path) noexcept {
    if (!path || *path == L'\0') {
        return E_INVALIDARG;
    }

    PIDLIST_ABSOLUTE pidl = nullptr;
    SFGAOF sfgao_out = 0;
    HRESULT hr = SHParseDisplayName(path, nullptr, &pidl, 0, &sfgao_out);
    if (FAILED(hr)) {
        return hr;
    }
    unique_pidl pidl_owner(pidl);

    hr = SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
    return hr;
}

shell_reveal_result reveal_file_in_explorer(
    const std::filesystem::path& file_path,
    shell_reveal_invoker invoker) {

    shell_reveal_result res{};

    if (file_path.empty()) {
        res.status = shell_reveal_status::invalid_path;
        res.hr = E_INVALIDARG;
        return res;
    }

    std::error_code ec;
    const auto status = std::filesystem::status(file_path, ec);
    if (ec || !std::filesystem::exists(status)) {
        res.status = shell_reveal_status::not_found;
        res.hr = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        return res;
    }

    if (std::filesystem::is_directory(status)) {
        res.status = shell_reveal_status::is_directory;
        res.hr = HRESULT_FROM_WIN32(ERROR_DIRECTORY_NOT_SUPPORTED);
        return res;
    }

    scoped_com_init com;
    if (!com.ok()) {
        res.status = shell_reveal_status::com_init_failed;
        res.hr = com.hr();
        return res;
    }

    const std::wstring wide_path = file_path.lexically_normal().wstring();

    HRESULT hr = S_OK;
    try {
        if (invoker) {
            hr = invoker(wide_path.c_str());
        } else {
            hr = default_shell_reveal_invoker(wide_path.c_str());
        }
    } catch (...) {
        res.status = shell_reveal_status::shell_failed;
        res.hr = E_FAIL;
        return res;
    }

    if (FAILED(hr)) {
        res.status = shell_reveal_status::shell_failed;
        res.hr = hr;
        return res;
    }

    res.status = shell_reveal_status::success;
    res.hr = S_OK;
    return res;
}

HRESULT default_recycle_invoker(const wchar_t* path) noexcept {
    if (!path || *path == L'\0') {
        return E_INVALIDARG;
    }

    // SHFileOperationW requires a double-NUL-terminated source list. The
    // vector carries path + two terminators; pFrom points at the first char.
    std::vector<wchar_t> from;
    try {
        const std::wstring src(path);
        from.reserve(src.size() + 2);
        from.assign(src.begin(), src.end());
        from.push_back(L'\0');
        from.push_back(L'\0');
    } catch (...) {
        return E_OUTOFMEMORY;
    }

    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.data();
    op.pTo = nullptr;
    // FOF_ALLOWUNDO is the load-bearing flag: the file goes to the Recycle
    // Bin instead of being permanently deleted. The remaining flags keep the
    // operation silent (the caller owns all user messaging).
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;

    int rc = 0;
    try {
        rc = SHFileOperationW(&op);
    } catch (...) {
        return E_FAIL;
    }
    if (op.fAnyOperationsAborted) {
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }
    if (rc == 0) {
        return S_OK;
    }
    if (rc > 0 && rc <= 0xFFFF) {
        return HRESULT_FROM_WIN32(static_cast<DWORD>(rc));
    }
    return E_FAIL;
}

recycle_result recycle_file_to_bin(
    const std::filesystem::path& file_path,
    std::uint64_t expected_file_size,
    std::uint64_t expected_last_write_time,
    recycle_invoker invoker) {

    recycle_result res{};

    if (file_path.empty()) {
        res.status = recycle_status::invalid_path;
        res.hr = E_INVALIDARG;
        return res;
    }

    std::error_code ec;
    const auto status = std::filesystem::status(file_path, ec);
    if (ec || !std::filesystem::exists(status)) {
        res.status = recycle_status::not_found;
        res.hr = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        return res;
    }

    if (std::filesystem::is_directory(status)) {
        res.status = recycle_status::is_directory;
        res.hr = HRESULT_FROM_WIN32(ERROR_DIRECTORY_NOT_SUPPORTED);
        return res;
    }

    // Source-identity recheck immediately before the shell operation: the
    // file must still be the same file the document was committed from.
    if (!verify_source_file_metadata(file_path, expected_file_size,
                                     expected_last_write_time)) {
        // Distinguish a file that vanished from one that changed.
        std::error_code re_ec;
        const auto re_status = std::filesystem::status(file_path, re_ec);
        if (re_ec || !std::filesystem::exists(re_status)) {
            res.status = recycle_status::not_found;
            res.hr = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        } else {
            res.status = recycle_status::identity_mismatch;
            res.hr = E_FAIL;
        }
        return res;
    }

    const std::wstring wide_path = file_path.lexically_normal().wstring();

    HRESULT hr = S_OK;
    try {
        if (invoker) {
            hr = invoker(wide_path.c_str());
        } else {
            hr = default_recycle_invoker(wide_path.c_str());
        }
    } catch (...) {
        res.status = recycle_status::recycle_failed;
        res.hr = E_FAIL;
        return res;
    }

    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        res.status = recycle_status::cancelled;
        res.hr = hr;
        return res;
    }

    if (FAILED(hr)) {
        res.status = recycle_status::recycle_failed;
        res.hr = hr;
        return res;
    }

    res.status = recycle_status::success;
    res.hr = S_OK;
    return res;
}

}  // namespace oz::viewer

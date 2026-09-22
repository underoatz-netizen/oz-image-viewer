// oz/viewer/shell_integration.cpp - On-demand shell integration boundary (Stage 9.1).
#include "shell_integration.hpp"

#include <shlobj.h>
#include <system_error>

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

}  // namespace oz::viewer

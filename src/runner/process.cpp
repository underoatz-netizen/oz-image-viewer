#include "oz/runner/process.hpp"

#include "oz/markers/markers.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace oz::runner {
namespace {

class handle_guard {
public:
    explicit handle_guard(HANDLE h = nullptr) : h_(h) {}
    ~handle_guard() {
        if (h_) {
            CloseHandle(h_);
        }
    }
    handle_guard(const handle_guard&) = delete;
    handle_guard& operator=(const handle_guard&) = delete;

HANDLE* ptr() noexcept { return &h_; }
    HANDLE get() const noexcept { return h_; }
    void reset(HANDLE h = nullptr) noexcept {
        if (h_ && h_ != h) {
            CloseHandle(h_);
        }
        h_ = h;
    }

private:
    HANDLE h_;
};

// CommandLineToArgvW-compatible quoting: wrap in double quotes, double
// backslashes before a quote, double trailing backslashes before the closing
// quote. If the argument is of the form /flag=val or -flag=val or --flag=val
// (where the prefix has no spaces or quotes), only the value portion is quoted
// so classic Windows tools (such as IrfanView) that scan for leading switch
// prefixes can recognize them while still passing a single valid argument to
// CommandLineToArgvW.
std::wstring quote_simple(const std::wstring& s) {
    if (s.empty()) {
        return L"\"\"";
    }
    const bool need_quotes = s.find_first_of(L" \t\"") != std::wstring::npos;
    if (!need_quotes) {
        return s;
    }
    std::wstring out = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t c : s) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out += L'"';
            backslashes = 0;
        } else {
            out.append(backslashes, L'\\');
            backslashes = 0;
            out += c;
        }
    }
    out.append(backslashes * 2, L'\\');
    out += L'"';
    return out;
}

std::wstring quote_arg(const std::wstring& arg) {
    if (arg.size() > 1 && (arg[0] == L'/' || arg[0] == L'-')) {
        const auto eq = arg.find(L'=');
        if (eq != std::wstring::npos && eq > 1) {
            const std::wstring prefix = arg.substr(0, eq + 1);
            if (prefix.find_first_of(L" \t\"") == std::wstring::npos) {
                std::wstring val = arg.substr(eq + 1);
                if (val.size() >= 2 && val.front() == L'"' && val.back() == L'"') {
                    val = val.substr(1, val.size() - 2);
                }
                return prefix + quote_simple(val);
            }
        }
    }
    return quote_simple(arg);
}

std::wstring build_command_line(const std::wstring& exe,
                                const std::vector<std::wstring>& args) {
    std::wstring cmd = quote_arg(exe);
    for (const auto& a : args) {
        cmd += L' ';
        cmd += quote_arg(a);
    }
    return cmd;
}

}  // namespace

process_result launch_process(const process_options& opts) {
    process_result result;
    const std::uint64_t freq = markers::qpc_frequency();
    const std::uint64_t t0 = markers::qpc_now();

    handle_guard read_pipe;
    handle_guard write_pipe;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    if (opts.capture_stdout) {
        if (!CreatePipe(read_pipe.ptr(), write_pipe.ptr(), &sa, 0)) {
            result.error = GetLastError();
            result.error_message = "CreatePipe failed";
            return result;
        }
        SetHandleInformation(read_pipe.get(), HANDLE_FLAG_INHERIT, 0);
    }

    const std::wstring cmdline = build_command_line(opts.executable, opts.args);
    const std::wstring workdir = opts.working_directory.value_or(L"");

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (opts.capture_stdout) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = write_pipe.get();
        si.hStdError = write_pipe.get();
    }

    const BOOL ok = CreateProcessW(
        nullptr, const_cast<wchar_t*>(cmdline.c_str()), nullptr, nullptr,
        TRUE /* inherit handles so the child keeps the pipe write end */,
        0, nullptr, workdir.empty() ? nullptr : workdir.c_str(), &si, &pi);
    if (!ok) {
        result.error = GetLastError();
        result.error_message = "CreateProcessW failed";
        return result;
    }
handle_guard process_guard(pi.hProcess);
    handle_guard thread_guard(pi.hThread);
    write_pipe.reset();  // parent closes its copy; the child keeps the handle
    result.launched = true;
    result.pid = pi.dwProcessId;

    std::string captured;
    std::atomic<bool> stop_reading{false};
    std::atomic<bool> reader_done{false};
    std::thread reader;
    if (opts.capture_stdout) {
        // Non-blocking reader: poll for available bytes instead of blocking
        // in ReadFile, so a grandchild that inherited the pipe write handle
        // cannot keep the runner blocked forever after the subject exits.
        reader = std::thread([&] {
            char buf[4096];
            for (;;) {
                if (stop_reading.load(std::memory_order_relaxed)) {
                    break;
                }
                DWORD avail = 0;
                if (!PeekNamedPipe(read_pipe.get(), nullptr, 0, nullptr, &avail, nullptr)) {
                    break;  // every writer closed the pipe
                }
                if (avail > 0) {
                    DWORD n = 0;
                    const DWORD want =
                        avail < sizeof(buf) ? avail : static_cast<DWORD>(sizeof(buf));
                    if (!ReadFile(read_pipe.get(), buf, want, &n, nullptr) || n == 0) {
                        break;
                    }
                    captured.append(buf, n);
                    continue;
                }
                Sleep(5);
            }
            reader_done.store(true, std::memory_order_relaxed);
        });
    }

    const DWORD wait = WaitForSingleObject(pi.hProcess, opts.timeout_ms);
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, INFINITE);
        result.timed_out = true;
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    result.exit_code = code;
    if (reader.joinable()) {
        // Bounded drain: give the reader a short grace period to consume
        // buffered output, then force it to stop. Without this, a timed-out
        // subject whose grandchildren inherited the pipe write handle would
        // leave the runner blocked in the reader join forever.
        const auto grace_end =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        while (!reader_done.load(std::memory_order_relaxed) &&
               std::chrono::steady_clock::now() < grace_end) {
            Sleep(5);
        }
        stop_reading.store(true, std::memory_order_relaxed);
        reader.join();
    }
    result.stdout_text = std::move(captured);
    result.duration_ns = markers::ticks_to_ns(markers::qpc_now() - t0, freq);
    return result;
}

}  // namespace oz::runner
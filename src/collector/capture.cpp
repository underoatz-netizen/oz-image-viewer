#include "oz/collector/capture.hpp"

#include "oz/markers/markers.hpp"
#include "oz/runner/utf.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <shellscalingapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace oz::collector {
namespace {

constexpr std::uint32_t kAcquireTimeoutMs = 250;

const char* kNoDesktopReason =
    "Desktop Duplication unavailable: no graphical desktop in this session "
    "(DXGI_ERROR_NOT_CURRENTLY_AVAILABLE); run from an interactive console "
    "session, not a service or a disconnected remote session";

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

template <typename T>
class com_ptr {
public:
    com_ptr() = default;
    ~com_ptr() {
        if (p_) {
            p_->Release();
        }
    }
    com_ptr(const com_ptr&) = delete;
    com_ptr& operator=(const com_ptr&) = delete;

    T** ptr() noexcept { return &p_; }
    T* get() const noexcept { return p_; }
    T* operator->() const noexcept { return p_; }
    void reset(T* p = nullptr) {
        if (p_) {
            p_->Release();
        }
        p_ = p;
    }

private:
    T* p_ = nullptr;
};

class com_guard {
public:
    com_guard() : hr_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~com_guard() {
        // CoUninitialize only balances a *successful* CoInitializeEx on this
        // thread. RPC_E_CHANGED_MODE means CoInitializeEx failed because COM
        // was already initialised in another apartment mode, so it did not
        // add a reference and must not be uninitialised here.
        if (SUCCEEDED(hr_)) {
            CoUninitialize();
        }
    }
    bool ok() const { return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE; }

private:
    HRESULT hr_;
};

std::string hr_hex(HRESULT hr) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned>(hr));
    return buf;
}

// CommandLineToArgvW-compatible quoting (same rule as the runner launcher).
// If the argument is of the form /flag=val or -flag=val or --flag=val
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

// True when a process with the exact executable path is already running
// (excluding this process). Used by preflight; such processes are never
// killed, the capture simply refuses to start. Path separators are
// normalised so forward-slash and backslash spellings compare equal.
bool process_running(const std::wstring& exe_path) {
    std::wstring target = exe_path;
    for (auto& c : target) {
        if (c == L'/') {
            c = L'\\';
        }
    }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return false;
    }
    handle_guard hg(snap);
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (!Process32FirstW(snap, &pe)) {
        return false;
    }
    do {
        if (pe.th32ProcessID == GetCurrentProcessId()) {
            continue;
        }
        HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                  pe.th32ProcessID);
        if (!proc) {
            continue;
        }
        handle_guard pg(proc);
        wchar_t buf[MAX_PATH];
        DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(proc, 0, buf, &n)) {
            for (wchar_t* p = buf; *p; ++p) {
                if (*p == L'/') {
                    *p = L'\\';
                }
            }
            if (_wcsicmp(buf, target.c_str()) == 0) {
                return true;
            }
        }
    } while (Process32NextW(snap, &pe));
    return false;
}

struct window_find {
    DWORD pid = 0;
    HWND hwnd = nullptr;
};

BOOL CALLBACK enum_window_proc(HWND hwnd, LPARAM lparam) {
    auto* wf = reinterpret_cast<window_find*>(lparam);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == wf->pid && IsWindowVisible(hwnd)) {
        wf->hwnd = hwnd;
        return FALSE;
    }
    return TRUE;
}

HWND find_window_for_pid(DWORD pid, std::uint32_t timeout_ms) {
    const std::uint64_t deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        window_find wf{pid, nullptr};
        EnumWindows(enum_window_proc, reinterpret_cast<LPARAM>(&wf));
        if (wf.hwnd) {
            return wf.hwnd;
        }
        if (GetTickCount64() >= deadline) {
            return nullptr;
        }
        Sleep(50);
    }
}

struct monitor_enum_ctx {
    std::vector<monitor_desc>* out = nullptr;
    int index = 0;
};

BOOL CALLBACK enum_monitor_proc(HMONITOR mon, HDC, LPRECT rect, LPARAM lparam) {
    auto* ctx = reinterpret_cast<monitor_enum_ctx*>(lparam);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) {
        return TRUE;
    }
    monitor_desc d;
    d.index = ctx->index++;
    d.name = oz::runner::wide_to_utf8(mi.szDevice);
    d.width = rect->right - rect->left;
    d.height = rect->bottom - rect->top;
    ctx->out->push_back(std::move(d));
    return TRUE;
}

int monitor_index_of(HMONITOR hmon) {
    const auto monitors = enumerate_monitors();
    for (const auto& m : monitors) {
        // Match by name: the HMONITOR handle cannot be compared directly
        // across the two enumeration APIs, but the device name can.
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(hmon, &mi) &&
            oz::runner::wide_to_utf8(mi.szDevice) == m.name) {
            return m.index;
        }
    }
    return -1;
}

// Returns true once the collector process has been made per-monitor DPI aware
// (v2 preferred). A per-monitor aware process receives physical-pixel window /
// client coordinates that map 1:1 onto the Desktop Duplication buffer, so no
// manual scaling is needed. Process-wide and idempotent; safe to call
// repeatedly. On failure (awareness already fixed by a manifest, or an
// unsupported OS) it returns false and the caller scales logical coordinates.
bool ensure_per_monitor_aware_dpi() {
    static const bool ok = [] {
        if (SetProcessDpiAwarenessContext(
                DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
            return true;
        }
        // Awareness already set elsewhere: per-monitor-v1 also returns
        // physical coordinates, so treat it as success too.
        const DPI_AWARENESS_CONTEXT ctx = GetThreadDpiAwarenessContext();
        return AreDpiAwarenessContextsEqual(
                   ctx, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE) ||
               AreDpiAwarenessContextsEqual(
                   ctx, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }();
    return ok;
}

// DPI scale (dpi / 96.0) that maps the collector's logical client coordinates
// to physical pixels for the window's monitor. 1.0 when the process is
// per-monitor aware (coordinates are already physical).
double dpi_scale_for_window(HWND hwnd) {
    if (ensure_per_monitor_aware_dpi()) {
        return 1.0;
    }
    // Fallback: the process could not be made per-monitor aware, so window /
    // client APIs return logical coordinates. Prefer the monitor's scale
    // factor (awareness-independent) so a high-DPI desktop is never assumed
    // to be 100%; degrade safely to the window DPI when it is unavailable.
    const HMONITOR hmon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (hmon) {
        DEVICE_SCALE_FACTOR sf = DEVICE_SCALE_FACTOR_INVALID;
        if (GetScaleFactorForMonitor(hmon, &sf) == S_OK &&
            sf > DEVICE_SCALE_FACTOR_INVALID) {
            return static_cast<double>(sf) / 100.0;
        }
    }
    const UINT dpi = GetDpiForWindow(hwnd);
    return dpi == 0 ? 1.0 : dpi / 96.0;
}

// Physical-pixel client rect of the window on the desktop. The collector is
// per-monitor-v2 aware (see ensure_per_monitor_aware_dpi), so GetClientRect /
// ClientToScreen already return physical pixels; the pure helper is applied
// with the (possibly 1.0) DPI scale so the fallback path stays correct.
RECT client_rect_physical(HWND hwnd) {
    RECT client{};
    GetClientRect(hwnd, &client);
    POINT origin{0, 0};
    ClientToScreen(hwnd, &origin);
    const dpi_rect phys = client_rect_to_physical(
        origin.x, origin.y, client.right - client.left, client.bottom - client.top,
        dpi_scale_for_window(hwnd));
    RECT out{};
    out.left = phys.left;
    out.top = phys.top;
    out.right = phys.right;
    out.bottom = phys.bottom;
    return out;
}

// Desktop Duplication capture for one output, with a reusable staging texture.
class desktop_duplicator {
public:
    // Returns an empty string on success; otherwise a human-readable reason.
    std::string init(HMONITOR hmon, const probe_config& probe_cfg);

    // Acquires the next frame (bounded wait) and evaluates the probe over the
    // given desktop region. Returns true when a frame was processed (out
    // holds the metrics); false when no frame arrived (hr is set: WAIT_TIMEOUT
    // or ACCESS_LOST).
    bool acquire_and_probe(const RECT& region, probe_metrics& out, HRESULT& hr);

    const monitor_info& monitor() const noexcept { return monitor_; }

private:
    com_ptr<ID3D11Device> device_;
    com_ptr<ID3D11DeviceContext> ctx_;
    com_ptr<IDXGIOutputDuplication> dup_;
    com_ptr<ID3D11Texture2D> staging_;
    D3D11_TEXTURE2D_DESC staging_desc_{};
    monitor_info monitor_;
    int origin_x_ = 0;
    int origin_y_ = 0;
    probe_config probe_cfg_;
};

std::string desktop_duplicator::init(HMONITOR hmon, const probe_config& probe_cfg) {
    probe_cfg_ = probe_cfg;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                   D3D11_SDK_VERSION, device_.ptr(), &fl, ctx_.ptr());
    if (FAILED(hr)) {
        device_.reset();
        ctx_.reset();
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                               D3D11_SDK_VERSION, device_.ptr(), &fl, ctx_.ptr());
    }
    if (FAILED(hr)) {
        return "D3D11 device creation failed (HRESULT " + hr_hex(hr) + ")";
    }

    com_ptr<IDXGIFactory1> factory;
    hr = CreateDXGIFactory1(IID_PPV_ARGS(factory.ptr()));
    if (FAILED(hr)) {
        return "CreateDXGIFactory1 failed (HRESULT " + hr_hex(hr) + ")";
    }

    com_ptr<IDXGIAdapter1> adapter;
    com_ptr<IDXGIOutput> output;
    bool found = false;
    for (UINT ai = 0; factory->EnumAdapters1(ai, adapter.ptr()) != DXGI_ERROR_NOT_FOUND;
         ++ai) {
        for (UINT oi = 0; adapter->EnumOutputs(oi, output.ptr()) != DXGI_ERROR_NOT_FOUND;
             ++oi) {
            DXGI_OUTPUT_DESC desc{};
            output->GetDesc(&desc);
            if (desc.Monitor == hmon) {
                found = true;
                break;
            }
        }
        if (found) {
            break;
        }
        adapter.reset();
        output.reset();
    }
    if (!found) {
        return "subject window monitor not found among DXGI outputs";
    }

    DXGI_OUTPUT_DESC od{};
    output->GetDesc(&od);
    monitor_.name = oz::runner::wide_to_utf8(od.DeviceName);
    monitor_.width =
        static_cast<int>(od.DesktopCoordinates.right - od.DesktopCoordinates.left);
    monitor_.height =
        static_cast<int>(od.DesktopCoordinates.bottom - od.DesktopCoordinates.top);
    origin_x_ = od.DesktopCoordinates.left;
    origin_y_ = od.DesktopCoordinates.top;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(od.DeviceName, ENUM_CURRENT_SETTINGS, &dm)) {
        monitor_.refresh_rate = dm.dmDisplayFrequency;
    }

    com_ptr<IDXGIOutput1> output1;
    hr = output->QueryInterface(IID_PPV_ARGS(output1.ptr()));
    if (FAILED(hr)) {
        return "IDXGIOutput1 unavailable (HRESULT " + hr_hex(hr) + ")";
    }
    hr = output1->DuplicateOutput(device_.get(), dup_.ptr());
    if (hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) {
        return kNoDesktopReason;
    }
    if (hr == E_ACCESSDENIED) {
        return "Desktop Duplication access denied (E_ACCESSDENIED): run in an "
               "interactive desktop session";
    }
    if (FAILED(hr)) {
        return "DuplicateOutput failed (HRESULT " + hr_hex(hr) + ")";
    }
    return {};
}

bool desktop_duplicator::acquire_and_probe(const RECT& region, probe_metrics& out,
                                           HRESULT& hr) {
    DXGI_OUTDUPL_FRAME_INFO fi{};
    com_ptr<IDXGIResource> resource;
    hr = dup_->AcquireNextFrame(kAcquireTimeoutMs, &fi, resource.ptr());
    if (hr == DXGI_ERROR_WAIT_TIMEOUT || hr == DXGI_ERROR_ACCESS_LOST) {
        return false;
    }
    if (FAILED(hr)) {
        return false;
    }
    struct frame_release {
        IDXGIOutputDuplication* d;
        ~frame_release() {
            if (d) {
                d->ReleaseFrame();
            }
        }
    } fr{dup_.get()};

    com_ptr<ID3D11Texture2D> tex;
    hr = resource->QueryInterface(IID_PPV_ARGS(tex.ptr()));
    if (FAILED(hr)) {
        return false;
    }
    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);
    if (!staging_.get() || staging_desc_.Width != td.Width ||
        staging_desc_.Height != td.Height || staging_desc_.Format != td.Format) {
        staging_.reset();
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        td.MiscFlags = 0;
        td.ArraySize = 1;
        td.MipLevels = 1;
        hr = device_->CreateTexture2D(&td, nullptr, staging_.ptr());
        if (FAILED(hr)) {
            return false;
        }
        staging_desc_ = td;
    }
    ctx_->CopyResource(staging_.get(), tex.get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = ctx_->Map(staging_.get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        return false;
    }
    struct unmap_guard {
        ID3D11DeviceContext* c;
        ID3D11Texture2D* t;
        ~unmap_guard() {
            if (c && t) {
                c->Unmap(t, 0);
            }
        }
    } um{ctx_.get(), staging_.get()};

    // Convert the desktop region into output-local coordinates.
    const int x0 = std::max(0, static_cast<int>(region.left) - origin_x_);
    const int y0 = std::max(0, static_cast<int>(region.top) - origin_y_);
    const int x1 = std::min(static_cast<int>(td.Width),
                            static_cast<int>(region.right) - origin_x_);
    const int y1 = std::min(static_cast<int>(td.Height),
                            static_cast<int>(region.bottom) - origin_y_);
    if (x1 <= x0 || y1 <= y0) {
        out = probe_metrics{};
        return true;  // window not visible on this output
    }
    const auto* base = static_cast<const std::uint8_t*>(mapped.pData);
    out = evaluate_probe(base + static_cast<std::size_t>(y0) * mapped.RowPitch +
                             static_cast<std::size_t>(x0) * 4,
                         x1 - x0, y1 - y0, static_cast<int>(mapped.RowPitch),
                         probe_cfg_);
    return true;
}

cleanup_info cleanup_subject(HANDLE job, HWND hwnd, HANDLE process,
                             const capture_options& opts) {
    cleanup_info ci;
    ci.wait_ms = opts.cleanup_wait_ms;
    Sleep(opts.cleanup_wait_ms);  // quiet interval before the close request
    if (hwnd) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }
    const DWORD wait = WaitForSingleObject(process, opts.close_wait_ms);
    if (wait == WAIT_OBJECT_0) {
        ci.outcome = "exited";
        return ci;
    }
    // Still running: Job cleanup kills the whole tree.
    if (TerminateJobObject(job, 1)) {
        WaitForSingleObject(process, 5000);
        ci.outcome = "forced";
        ci.forced = true;
    } else {
        ci.outcome = "failed";
    }
    return ci;
}

}  // namespace

const char* capture_status_name(capture_status s) noexcept {
    switch (s) {
        case capture_status::matched: return "matched";
        case capture_status::missing_window: return "missing_window";
        case capture_status::wrong_monitor: return "wrong_monitor";
        case capture_status::blank_or_nonmatching: return "blank_or_nonmatching";
        case capture_status::timeout: return "timeout";
        case capture_status::launch_failed: return "launch_failed";
        case capture_status::capture_failed: return "capture_failed";
        case capture_status::cleanup_failed: return "cleanup_failed";
        case capture_status::preflight_failed: return "preflight_failed";
    }
    return "unknown";
}

long scale_to_physical(long value, double dpi_scale) noexcept {
    return static_cast<long>(std::lround(value * dpi_scale));
}

dpi_rect client_rect_to_physical(long origin_x, long origin_y,
                                 long client_width, long client_height,
                                 double dpi_scale) noexcept {
    dpi_rect r;
    r.left = scale_to_physical(origin_x, dpi_scale);
    r.top = scale_to_physical(origin_y, dpi_scale);
    r.right = r.left + scale_to_physical(client_width, dpi_scale);
    r.bottom = r.top + scale_to_physical(client_height, dpi_scale);
    return r;
}

std::vector<monitor_desc> enumerate_monitors() {
    std::vector<monitor_desc> out;
    monitor_enum_ctx ctx{&out, 0};
    EnumDisplayMonitors(nullptr, nullptr, enum_monitor_proc,
                        reinterpret_cast<LPARAM>(&ctx));
    return out;
}

std::string desktop_duplication_availability() {
    ensure_per_monitor_aware_dpi();
    com_guard com;
    if (!com.ok()) {
        return "CoInitializeEx failed";
    }
    const POINT pt{0, 0};
    const HMONITOR hmon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    desktop_duplicator dup;
    return dup.init(hmon, probe_config{});
}

capture_result capture_external_visible(const capture_options& opts) {
    capture_result result;
    const std::uint64_t freq = markers::qpc_frequency();

    // Per-monitor-v2 DPI awareness before any window/client coordinate query
    // so GetClientRect / ClientToScreen return physical pixels that map 1:1
    // onto the Desktop Duplication buffer (no manual scaling/virtualization).
    ensure_per_monitor_aware_dpi();

    // Preflight: never kill unrelated processes; fail when the subject is
    // already running (a previous capture that leaked, or the user opened it).
    if (process_running(opts.executable)) {
        result.status = capture_status::preflight_failed;
        result.reason =
            "subject process already running; preflight refuses to launch or kill it";
        return result;
    }

    com_guard com;
    if (!com.ok()) {
        result.status = capture_status::capture_failed;
        result.reason = "CoInitializeEx failed";
        return result;
    }

    // Job Object with KILL_ON_JOB_CLOSE: closing the handle kills the tree.
    handle_guard job(CreateJobObjectW(nullptr, nullptr));
    if (!job.get()) {
        result.status = capture_status::launch_failed;
        result.error = GetLastError();
        result.reason = "CreateJobObjectW failed";
        return result;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
    jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &jeli,
                                 sizeof(jeli))) {
        result.status = capture_status::launch_failed;
        result.error = GetLastError();
        result.reason = "SetInformationJobObject failed";
        return result;
    }

    std::wstring cmdline = quote_arg(opts.executable);
    for (const auto& a : opts.args) {
        cmdline += L' ';
        cmdline += quote_arg(a);
    }
    const std::wstring workdir = opts.working_directory.value_or(L"");

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    result.launch_ticks = markers::qpc_now();  // immediately before CreateProcessW
    const BOOL ok = CreateProcessW(
        nullptr, const_cast<wchar_t*>(cmdline.c_str()), nullptr, nullptr, FALSE,
        CREATE_SUSPENDED, nullptr, workdir.empty() ? nullptr : workdir.c_str(), &si,
        &pi);
    if (!ok) {
        result.status = capture_status::launch_failed;
        result.error = GetLastError();
        result.reason = "CreateProcessW failed";
        return result;
    }
    handle_guard process_guard(pi.hProcess);
    handle_guard thread_guard(pi.hThread);
    result.pid = pi.dwProcessId;

    if (!AssignProcessToJobObject(job.get(), pi.hProcess)) {
        result.status = capture_status::launch_failed;
        result.error = GetLastError();
        result.reason = "AssignProcessToJobObject failed (KILL_ON_JOB_CLOSE guarantee lost)";
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
        return result;
    }
    ResumeThread(pi.hThread);

    // Associate the window (PID -> HWND).
    const HWND hwnd = find_window_for_pid(pi.dwProcessId, opts.window_wait_ms);
    if (!hwnd) {
        result.status = capture_status::missing_window;
        result.reason = "no visible window for the subject within window_wait_ms";
        result.cleanup = cleanup_subject(job.get(), nullptr, pi.hProcess, opts);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        result.exit_code = code;
        return result;
    }
    result.hwnd = reinterpret_cast<std::uint64_t>(hwnd);

    // Raise the subject window above other (non-topmost) windows so the
    // probe samples the subject's client area, not a covering window.
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);

    // Monitor association: duplicate the window's monitor; a configured
    // monitor_index that does not match is an explicit wrong_monitor failure.
    const HMONITOR hmon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    const int win_mon_index = monitor_index_of(hmon);
    result.monitor.index = win_mon_index;
    if (opts.monitor_index >= 0 && win_mon_index != opts.monitor_index) {
        result.status = capture_status::wrong_monitor;
        result.reason = "subject window is on monitor " + std::to_string(win_mon_index) +
                        ", plan requires monitor " + std::to_string(opts.monitor_index);
        result.cleanup = cleanup_subject(job.get(), hwnd, pi.hProcess, opts);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        result.exit_code = code;
        return result;
    }

    desktop_duplicator dup;
    std::string derr = dup.init(hmon, opts.probe);
    if (!derr.empty()) {
        result.status = capture_status::capture_failed;
        result.reason = derr;
        result.cleanup = cleanup_subject(job.get(), hwnd, pi.hProcess, opts);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        result.exit_code = code;
        return result;
    }
    result.monitor = dup.monitor();

    // Probe loop: `consecutive_frames` consecutive matching frames. The
    // ExternalFirstImageVisible endpoint is the *first* matching frame of the
    // run (detect_ticks); the run-completing frame only confirms it
    // (confirm_ticks) and never masks the endpoint.
    const RECT region = client_rect_physical(hwnd);
    consecutive_matcher matcher(opts.probe.consecutive_frames);
    bool saw_frame = false;
    const std::uint64_t deadline =
        markers::qpc_now() + freq * opts.frame_timeout_ms / 1000;
    for (;;) {
        if (markers::qpc_now() >= deadline) {
            break;
        }
        probe_metrics pm;
        HRESULT hr = S_OK;
        if (!dup.acquire_and_probe(region, pm, hr)) {
            if (hr == DXGI_ERROR_ACCESS_LOST) {
                // Desktop changed (mode switch / lock); re-initialize once.
                derr = dup.init(hmon, opts.probe);
                if (!derr.empty()) {
                    result.status = capture_status::capture_failed;
                    result.reason = derr;
                    break;
                }
                continue;
            }
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
                Sleep(5);
                continue;
            }
            if (FAILED(hr)) {
                result.status = capture_status::capture_failed;
                result.reason = "AcquireNextFrame failed (HRESULT " + hr_hex(hr) + ")";
                result.error = static_cast<std::uint32_t>(hr);
                break;
            }
            continue;
        }
        saw_frame = true;
        result.probe = pm;
        const bool confirmed = matcher.observe(pm.matched);
        if (matcher.started_new_run()) {
            // First matching frame of a run: this is the detection point.
            result.detect_ticks = markers::qpc_now();
        }
        if (confirmed) {
            result.confirm_ticks = markers::qpc_now();
            // relative_ns is based on detect_ticks (the first matching frame),
            // not confirm_ticks. For consecutive_frames == 1 the single
            // matching frame is both the detection and the confirmation.
            result.relative_ns =
                markers::ticks_to_ns(result.detect_ticks - result.launch_ticks, freq);
            result.status = capture_status::matched;
            break;
        }
    }
    if (result.status != capture_status::matched &&
        result.status != capture_status::capture_failed) {
        if (!saw_frame) {
            result.status = capture_status::timeout;
            result.timed_out = true;
            result.reason = "no desktop frame acquired within frame_timeout_ms";
        } else {
            result.status = capture_status::blank_or_nonmatching;
            result.reason =
                "client-area probe never matched " +
                std::to_string(opts.probe.consecutive_frames) +
                " consecutive frames within frame_timeout_ms";
        }
    }

    // Cleanup: quiet wait, request close, allow close_wait, then Job cleanup.
    result.cleanup = cleanup_subject(job.get(), hwnd, pi.hProcess, opts);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    result.exit_code = code;
    if (result.status == capture_status::matched &&
        result.cleanup.outcome == "failed") {
        result.status = capture_status::cleanup_failed;
        result.reason = "subject did not exit and Job cleanup failed";
    }
    return result;
}

}  // namespace oz::collector
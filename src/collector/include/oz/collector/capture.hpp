// oz/collector/capture.hpp - external_visible_frame capture engine.
//
// Measures ExternalFirstImageVisible: QPC elapsed from immediately before a
// direct CreateProcessW until DXGI Desktop Duplication detects the first of
// two consecutive desktop-composed frames where a fixed client-area image
// probe matches the expected selected-JPEG appearance. This is the reference
// endpoint for ozviewer, IrfanView and XnView; it is never labelled
// FirstPresent, window-ready, or native TTFI.
//
// The subject is launched inside a Job Object with KILL_ON_JOB_CLOSE so the
// whole process tree is cleaned up. Unrelated existing processes are never
// killed: preflight fails when the subject executable is already running.
#pragma once

#include "oz/collector/probe.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace oz::collector {

// Explicit non-success reasons recorded by the collector.
enum class capture_status : std::uint8_t {
    matched,             // two consecutive matching frames detected
    missing_window,      // no visible window for the subject within window_wait_ms
    wrong_monitor,       // subject window is not on the configured monitor
    blank_or_nonmatching,  // frames acquired but the probe never matched
    timeout,             // no desktop frame acquired within frame_timeout_ms
    launch_failed,       // CreateProcessW / Job Object setup failed
    capture_failed,      // Desktop Duplication / D3D11 / COM failure
    cleanup_failed,      // subject did not exit and Job cleanup failed
    preflight_failed,    // subject process already running (never killed)
};

const char* capture_status_name(capture_status s) noexcept;

struct monitor_info {
    int index = 0;
    std::string name;
    int width = 0;
    int height = 0;
    int refresh_rate = 0;
};

struct cleanup_info {
    std::string outcome;  // "exited" | "forced" | "failed" | "not_applicable"
    bool forced = false;
    std::uint32_t wait_ms = 0;
};

struct capture_options {
    std::wstring executable;
    std::vector<std::wstring> args;
    std::optional<std::wstring> working_directory;
    std::uint32_t window_wait_ms = 15000;   // wait for a visible window
    std::uint32_t frame_timeout_ms = 30000; // probe loop budget after the window appears
    int monitor_index = -1;                 // -1 = use the subject window's monitor
    probe_config probe;
    std::uint32_t cleanup_wait_ms = 500;    // quiet interval before the close request
    std::uint32_t close_wait_ms = 5000;     // allowed after the close request
};

struct capture_result {
    capture_status status = capture_status::launch_failed;
    std::string reason;
    std::uint64_t launch_ticks = 0;   // QPC immediately before CreateProcessW
    std::uint64_t detect_ticks = 0;   // QPC at the first matching frame (endpoint)
    std::uint64_t confirm_ticks = 0;  // QPC at the run-completing frame (provenance)
    std::int64_t relative_ns = 0;     // detect_ticks - launch_ticks (ExternalFirstImageVisible)
    std::uint32_t error = 0;          // GetLastError / HRESULT on failure
    std::uint32_t exit_code = 0;
    bool timed_out = false;
    std::uint32_t pid = 0;
    std::uint64_t hwnd = 0;
    monitor_info monitor;
    probe_metrics probe;
    cleanup_info cleanup;
};

// Launches the subject directly via CreateProcessW inside a Job Object with
// KILL_ON_JOB_CLOSE, waits for its window, duplicates the desktop output of
// the window's monitor, and probes the client area until `consecutive_frames`
// consecutive frames match (or a failure/timeout is recorded). The endpoint is
// timestamped at the first matching frame of the run; later matching frames
// only confirm it. Never kills unrelated processes: preflight fails when the
// subject executable is already running.
capture_result capture_external_visible(const capture_options& opts);

struct monitor_desc {
    int index = 0;
    std::string name;
    int width = 0;
    int height = 0;
};

// Enumerates the current desktop monitors (index, name, size). Used by the
// CLI and tests to resolve monitor_index.
std::vector<monitor_desc> enumerate_monitors();

// Returns an empty string when Desktop Duplication is available for the
// primary monitor; otherwise a human-readable reason (non-interactive
// session, no graphical desktop, access denied). Tests use this to skip
// Desktop Duplication cases cleanly instead of failing.
std::string desktop_duplication_availability();

// --- DPI-aware coordinate mapping (pure, testable) --------------------------
//
// The collector process is made per-monitor-v2 DPI aware before any window /
// client coordinate query (see capture_external_visible), so GetClientRect and
// ClientToScreen return physical pixels that map 1:1 onto the physical Desktop
// Duplication buffer and no manual scaling is needed (dpi_scale == 1.0). When
// that awareness cannot be set, those APIs return logical pixels and the
// region must be scaled by the monitor DPI scale (dpi / 96.0). These pure
// helpers centralise the single scaling rule so it can be unit-tested at
// 100/125/150/175/200% without a display.

struct dpi_rect {
    long left = 0;
    long top = 0;
    long right = 0;
    long bottom = 0;
};

// Rounds a logical coordinate to the nearest physical pixel.
long scale_to_physical(long value, double dpi_scale) noexcept;

// Maps a logical client rect (screen origin + client size) to physical desktop
// pixels. With dpi_scale == 1.0 (per-monitor-v2 aware) this is the identity;
// otherwise each coordinate is scaled by dpi_scale.
dpi_rect client_rect_to_physical(long origin_x, long origin_y,
                                 long client_width, long client_height,
                                 double dpi_scale) noexcept;

}  // namespace oz::collector
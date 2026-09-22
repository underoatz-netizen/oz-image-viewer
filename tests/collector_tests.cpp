// ozbench_collector_tests - focused tests for the external_visible_frame
// reference collector (no test framework).
//
// Pure tests (probe math, consecutive rule, manifest sealing, redaction,
// plan schema) always run. Desktop Duplication integration tests run only
// when a graphical desktop is available; otherwise they print SKIP and pass
// (environment absence is a clean explicit skip, never a false pass).
//
// Usage: ozbench_collector_tests <control_window_exe>
#include "oz/collector/capture.hpp"
#include "oz/collector/manifest.hpp"
#include "oz/collector/probe.hpp"
#include "oz/markers/markers.hpp"
#include "oz/runner/json.hpp"
#include "oz/runner/plan.hpp"
#include "oz/runner/process.hpp"
#include "oz/runner/record.hpp"
#include "oz/runner/utf.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace test {
int g_checks = 0;
int g_failures = 0;
int g_skips = 0;

void check(bool ok, const char* expr, const char* file, int line) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::cerr << "FAIL " << file << ":" << line << ": " << expr << "\n";
    }
}

void skip(const char* reason) {
    ++g_skips;
    std::cout << "SKIP: " << reason << "\n";
}
}  // namespace test

#define CHECK(expr) ::test::check(!!(expr), #expr, __FILE__, __LINE__)

namespace {

std::string read_file(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

bool write_file(const std::wstring& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        return false;
    }
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    return f.good();
}

std::wstring temp_dir() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    return std::wstring(tmp) + L"ozbench_collector_tests\\";
}

// --- probe math (pure CPU) --------------------------------------------------

std::vector<std::uint8_t> make_buffer(int w, int h, int fill) {
    return std::vector<std::uint8_t>(static_cast<std::size_t>(w) * h * 4,
                                     static_cast<std::uint8_t>(fill));
}

void fill_checkerboard(std::vector<std::uint8_t>& buf, int w, int h) {
    const int cell = 8;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int level = ((x / cell) + (y / cell)) % 4;
            const std::uint8_t v = static_cast<std::uint8_t>(40 + level * 55);
            std::uint8_t* p = buf.data() + (static_cast<std::size_t>(y) * w + x) * 4;
            p[0] = v;  // B
            p[1] = v;  // G
            p[2] = v;  // R
            p[3] = 255;
        }
    }
}

void test_probe_blank_rejected() {
    const int w = 640, h = 480;
    auto buf = make_buffer(w, h, 32);  // solid dark gray
    oz::collector::probe_config cfg;
    const auto m = oz::collector::evaluate_probe(buf.data(), w, h, w * 4, cfg);
    CHECK(!m.matched);
    CHECK(m.edge_density == 0.0);
    CHECK(m.luma_variance == 0.0);
    CHECK(m.mean_luma > 0.0 && m.mean_luma < 0.2);
}

void test_probe_pattern_matched() {
    const int w = 640, h = 480;
    auto buf = make_buffer(w, h, 0);
    fill_checkerboard(buf, w, h);
    oz::collector::probe_config cfg;
    const auto m = oz::collector::evaluate_probe(buf.data(), w, h, w * 4, cfg);
    CHECK(m.matched);
    CHECK(m.edge_density > 0.5);
    CHECK(m.luma_variance > 0.01);
    CHECK(m.mean_luma > 0.2 && m.mean_luma < 0.8);
}

void test_probe_extremes_rejected() {
    oz::collector::probe_config cfg;
    const int w = 320, h = 240;
    auto white = make_buffer(w, h, 255);
    const auto mw = oz::collector::evaluate_probe(white.data(), w, h, w * 4, cfg);
    CHECK(!mw.matched);  // mean luma 1.0 > max_mean_luma
    auto black = make_buffer(w, h, 0);
    const auto mb = oz::collector::evaluate_probe(black.data(), w, h, w * 4, cfg);
    CHECK(!mb.matched);  // no variance, no edges
    // null / bad args are safe
    const auto mn = oz::collector::evaluate_probe(nullptr, w, h, w * 4, cfg);
    CHECK(!mn.matched);
    const auto mz = oz::collector::evaluate_probe(white.data(), 0, 0, 0, cfg);
    CHECK(!mz.matched);
}

void test_consecutive_matcher() {
    oz::collector::consecutive_matcher m2(2);
    CHECK(!m2.observe(true));      // 1st match: not yet confirmed
    CHECK(m2.started_new_run());   // ... and it started the run (detection point)
    CHECK(m2.observe(true));       // 2nd consecutive match -> detected
    CHECK(!m2.started_new_run());  // ... but it is not the detection point
    CHECK(m2.matched_frames() == 2);

    oz::collector::consecutive_matcher m2b(2);
    CHECK(!m2b.observe(true));
    CHECK(m2b.started_new_run());
    CHECK(!m2b.observe(false));    // run broken
    CHECK(!m2b.started_new_run());
    CHECK(!m2b.observe(true));
    CHECK(m2b.started_new_run());  // a new run starts at the first match
    CHECK(m2b.observe(true));      // two consecutive matches after the break
    CHECK(!m2b.started_new_run());
    CHECK(m2b.matched_frames() == 2);

    oz::collector::consecutive_matcher m1(1);
    CHECK(m1.observe(true));       // single frame suffices when required == 1
    CHECK(m1.started_new_run());   // detection and confirmation on one frame
    CHECK(m1.matched_frames() == 1);

    oz::collector::consecutive_matcher m3(3);
    CHECK(!m3.observe(true));
    CHECK(m3.started_new_run());
    CHECK(!m3.observe(true));
    CHECK(!m3.started_new_run());
    CHECK(m3.observe(true));
    CHECK(!m3.started_new_run());
    CHECK(m3.matched_frames() == 3);
}

void test_detection_point_semantics() {
    // The endpoint must be timestamped at the first matching frame of a run;
    // the run-completing frame only confirms it. Simulate the capture loop's
    // tick assignment with a synthetic monotonic tick so relative_ns can be
    // proven to come from the detection frame, never the confirmation frame.
    oz::collector::consecutive_matcher m(2);
    std::uint64_t detect = 0;
    std::uint64_t confirm = 0;
    std::uint64_t tick = 1000;
    const bool frames[] = {false, false, true, true};
    for (const bool matched : frames) {
        if (m.observe(matched)) {
            confirm = tick;
        }
        if (m.started_new_run()) {
            detect = tick;
        }
        tick += 100;
    }
    CHECK(detect == 1200);   // first matching frame (third observation)
    CHECK(confirm == 1300);  // second consecutive match (fourth observation)
    CHECK(confirm > detect);
}

void test_dpi_scaling() {
    // scale_to_physical rounds a logical coordinate to the nearest pixel.
    CHECK(oz::collector::scale_to_physical(0, 1.0) == 0);
    CHECK(oz::collector::scale_to_physical(100, 1.0) == 100);
    CHECK(oz::collector::scale_to_physical(100, 1.25) == 125);
    CHECK(oz::collector::scale_to_physical(100, 1.5) == 150);
    CHECK(oz::collector::scale_to_physical(100, 1.75) == 175);
    CHECK(oz::collector::scale_to_physical(100, 2.0) == 200);
    CHECK(oz::collector::scale_to_physical(97, 1.5) == 146);   // 145.5 -> 146
    CHECK(oz::collector::scale_to_physical(97, 1.25) == 121);  // 121.25 -> 121

    // client_rect_to_physical maps a logical origin + client size to physical
    // pixels at 100/125/150/175/200% (origin 10,20; client 640x480).
    const auto r100 = oz::collector::client_rect_to_physical(10, 20, 640, 480, 1.0);
    CHECK(r100.left == 10 && r100.top == 20 && r100.right == 650 && r100.bottom == 500);

    const auto r125 = oz::collector::client_rect_to_physical(10, 20, 640, 480, 1.25);
    CHECK(r125.left == 13 && r125.top == 25 && r125.right == 813 && r125.bottom == 625);

    const auto r150 = oz::collector::client_rect_to_physical(10, 20, 640, 480, 1.5);
    CHECK(r150.left == 15 && r150.top == 30 && r150.right == 975 && r150.bottom == 750);

    const auto r175 = oz::collector::client_rect_to_physical(10, 20, 640, 480, 1.75);
    CHECK(r175.left == 18 && r175.top == 35 && r175.right == 1138 && r175.bottom == 875);

    const auto r200 = oz::collector::client_rect_to_physical(10, 20, 640, 480, 2.0);
    CHECK(r200.left == 20 && r200.top == 40 && r200.right == 1300 && r200.bottom == 1000);
}

// --- manifest sealing / tamper detection ------------------------------------

void test_manifest_seal() {
    const std::wstring dir = temp_dir();
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring path = dir + L"selection.private.json";
    const std::string body =
        "{\"schema\":\"oz.manifest.v1\",\"id\":\"selection-test\",\"images\":["
        "{\"alias\":\"jpeg-2mp\",\"path\":\"E:\\\\Pictures\\\\a.jpg\","
        "\"sha256\":\"abc\",\"width\":1920,\"height\":1080,\"size_bytes\":123,"
        "\"pixels\":2073600,\"target_mp\":2,\"deviation_mp\":0.1}]}";
    CHECK(write_file(path, body));
    const std::string seal = oz::collector::sha256_hex(body.data(), body.size());
    CHECK(seal.size() == 64);
    CHECK(write_file(path + L".sha256", seal + "\n"));

    std::string error;
    auto m = oz::collector::load_manifest(path, error);
    CHECK(m.has_value());
    if (m) {
        CHECK(m->schema == "oz.manifest.v1");
        CHECK(m->id == "selection-test");
        CHECK(m->images.size() == 1);
        if (!m->images.empty()) {
            CHECK(m->images[0].alias == "jpeg-2mp");
            CHECK(m->images[0].width == 1920);
            CHECK(m->images[0].height == 1080);
            CHECK(m->images[0].pixels == 2073600);
        }
    }

    // Tamper: modify the manifest body -> seal mismatch must be detected.
    std::string tampered = body;
    const auto pos = tampered.find("a.jpg");
    CHECK(pos != std::string::npos);
    tampered.replace(pos, 5, "b.jpg");
    CHECK(write_file(path, tampered));
    auto mt = oz::collector::load_manifest(path, error);
    CHECK(!mt.has_value());
    CHECK(error.find("seal mismatch") != std::string::npos);

    // Restore and verify the missing-seal case.
    CHECK(write_file(path, body));
    DeleteFileW((path + L".sha256").c_str());
    auto ms = oz::collector::load_manifest(path, error);
    CHECK(!ms.has_value());
    CHECK(error.find("missing seal") != std::string::npos);

    // Malformed JSON is rejected.
    CHECK(write_file(path, "{not json"));
    CHECK(write_file(path + L".sha256", seal + "\n"));
    auto mj = oz::collector::load_manifest(path, error);
    CHECK(!mj.has_value());

    DeleteFileW(path.c_str());
    RemoveDirectoryW(dir.c_str());
}

void test_manifest_unicode_path() {
    const std::wstring dir = temp_dir();
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring path = dir + L"unicode.private.json";
    // Unicode source path (escaped in JSON like the inventory script emits).
    const std::string body =
        "{\"schema\":\"oz.manifest.v1\",\"id\":\"unicode-test\",\"images\":["
        "{\"alias\":\"jpeg-12mp\",\"path\":\"E:\\\\Pictures\\\\\\u0e19\\u0e49\\u0e33\\u0e2d\\u0e34\\u0e07\\\\x.jpg\","
        "\"sha256\":\"def\",\"width\":4000,\"height\":3000,\"size_bytes\":456,"
        "\"pixels\":12000000,\"target_mp\":12,\"deviation_mp\":0}]}";
    CHECK(write_file(path, body));
    const std::string seal = oz::collector::sha256_hex(body.data(), body.size());
    CHECK(write_file(path + L".sha256", seal + "\n"));
    std::string error;
    auto m = oz::collector::load_manifest(path, error);
    CHECK(m.has_value());
    if (m && !m->images.empty()) {
        // The escaped path decodes to the Unicode folder name.
        CHECK(m->images[0].path.find("\xE0\xB8\x99\xE0\xB9\x89\xE0\xB8\xB3\xE0\xB8\xAD\xE0\xB8\xB4\xE0\xB8\x87") !=
              std::string::npos);
    }
    DeleteFileW(path.c_str());
    DeleteFileW((path + L".sha256").c_str());
    RemoveDirectoryW(dir.c_str());
}

// --- redaction --------------------------------------------------------------

void test_redaction() {
    oz::runner::record r;
    r.kind = oz::runner::record_kind::external_visible;
    r.scenario = "ozviewer|cold|jpeg-2mp";
    r.subject = "ozviewer";
    r.subject_path = "C:\\private\\ozviewer.exe";
    r.redacted = true;
    r.image_alias = "jpeg-2mp";
    r.endpoint = "ExternalFirstImageVisible";
    r.status = "matched";
    r.relative_ns = 12345;
    oz::runner::image_info ii;
    ii.path = "E:\\Pictures\\secret\\photo.jpg";
    ii.alias = "jpeg-2mp";
    ii.sha256 = "abc";
    r.image = ii;
    const std::string line = oz::runner::to_jsonl(r);
    CHECK(line.find("C:\\private\\ozviewer.exe") == std::string::npos);
    CHECK(line.find("E:\\Pictures\\secret\\photo.jpg") == std::string::npos);
    CHECK(line.find("jpeg-2mp") != std::string::npos);
    CHECK(line.find("\"subject_path\":null") != std::string::npos);
    CHECK(line.find("\"path\":null") != std::string::npos);
    CHECK(line.find("ExternalFirstImageVisible") != std::string::npos);

    // Non-redacted records still carry the path (backwards compatible).
    oz::runner::record r2;
    r2.kind = oz::runner::record_kind::run_start;
    r2.scenario = "fixture|cold|sample.jpg";
    r2.subject = "fixture";
    r2.subject_path = "C:\\fixture.exe";
    const std::string line2 = oz::runner::to_jsonl(r2);
    // JSON escapes backslashes, so the serialized form is C:\\fixture.exe.
    CHECK(line2.find("C:\\\\fixture.exe") != std::string::npos);
}

// --- plan schema extension --------------------------------------------------

void test_plan_schema_extension() {
    const std::string text =
        "{\"schema\":\"oz.plan.v1\",\"name\":\"ref\","
        "\"subject\":{\"name\":\"ozviewer\",\"executable\":\"{{viewer_exe}}\","
        "\"version_hint\":\"1.0\"},"
        "\"collector\":\"external_visible_frame\",\"adapter\":\"oz_desktop_duplication\","
        "\"endpoint\":\"ExternalFirstImageVisible\",\"cache\":\"cold\","
        "\"manifest\":\"benchmark/manifests/selection.private.json\","
        "\"collector_config\":{\"monitor_index\":0,\"window_wait_ms\":20000,"
        "\"frame_timeout_ms\":40000,"
        "\"probe\":{\"min_edge_density\":0.03,\"min_luma_variance\":0.002,"
        "\"min_mean_luma\":0.05,\"max_mean_luma\":0.95,\"consecutive_frames\":2}},"
        "\"args\":[\"{{image_path}}\"],"
        "\"images\":[{\"path\":\"x.jpg\",\"alias\":\"jpeg-2mp\",\"sha256\":\"abc\","
        "\"format\":\"jpg\"}]}";
    auto pr = oz::runner::parse_plan(text);
    CHECK(pr.errors.empty());
    if (pr.value) {
        const auto& p = *pr.value;
        CHECK(p.collector == "external_visible_frame");
        CHECK(p.endpoint == "ExternalFirstImageVisible");
        CHECK(p.manifest.has_value());
        CHECK(p.collector_config.has_value());
        if (p.collector_config) {
            CHECK(p.collector_config->monitor_index == 0);
            CHECK(p.collector_config->window_wait_ms == 20000);
            CHECK(p.collector_config->frame_timeout_ms == 40000);
            CHECK(p.collector_config->probe.min_edge_density == 0.03);
            CHECK(p.collector_config->probe.consecutive_frames == 2);
        }
        CHECK(!p.images.empty());
        if (!p.images.empty()) {
            CHECK(p.images[0].alias == "jpeg-2mp");
            CHECK(p.images[0].sha256 == "abc");
        }
    }

    // Wrong endpoint label is rejected (never FirstPresent for this collector).
    auto bad = oz::runner::parse_plan(
        "{\"schema\":\"oz.plan.v1\",\"name\":\"bad\","
        "\"subject\":{\"name\":\"s\",\"executable\":\"x.exe\"},"
        "\"collector\":\"external_visible_frame\",\"endpoint\":\"FirstPresent\","
        "\"images\":[{\"path\":\"a.jpg\"}]}");
    CHECK(!bad.errors.empty());
    bool found = false;
    for (const auto& e : bad.errors) {
        if (e.find("ExternalFirstImageVisible") != std::string::npos) {
            found = true;
        }
    }
    CHECK(found);

    // Old plans still parse identically (backwards compatibility).
    auto old = oz::runner::parse_plan(
        "{\"schema\":\"oz.plan.v1\",\"name\":\"old\","
        "\"subject\":{\"name\":\"fixture\",\"executable\":\"{{fixture_exe}}\"},"
        "\"collector\":\"markers\",\"images\":[{\"path\":\"a.jpg\"}]}");
    CHECK(old.errors.empty());
    if (old.value) {
        CHECK(old.value->collector == "markers");
        CHECK(!old.value->collector_config.has_value());
        CHECK(old.value->endpoint.empty());
    }
}

// --- capture engine (Desktop Duplication; skippable) ------------------------

oz::collector::capture_options make_capture_options(const std::wstring& control_exe,
                                                    const std::vector<std::wstring>& extra) {
    oz::collector::capture_options co;
    co.executable = control_exe;
    co.args = extra;
    co.window_wait_ms = 8000;
    co.frame_timeout_ms = 8000;
    co.monitor_index = -1;
    // Pilot-sealed thresholds: strict enough to reject the DWM window-appearance
    // animation (dimmed/scaled pattern) and only match the settled pattern.
    co.probe.min_edge_density = 0.3;
    co.probe.min_luma_variance = 0.04;
    co.probe.min_mean_luma = 0.02;
    co.probe.max_mean_luma = 0.98;
    co.probe.consecutive_frames = 2;
    co.cleanup_wait_ms = 100;   // keep the tests fast
    co.close_wait_ms = 3000;
    return co;
}

void test_capture_blank_then_pattern(const std::wstring& control_exe) {
    // Blank frame must be rejected until the matching pattern appears.
    auto co = make_capture_options(control_exe,
                                   {L"--blank-ms", L"400", L"--animate-ms", L"50", L"--topmost"});
    const auto cr = oz::collector::capture_external_visible(co);
    CHECK(cr.status == oz::collector::capture_status::matched);
    if (cr.status == oz::collector::capture_status::matched) {
        CHECK(cr.relative_ns > 0);
        CHECK(cr.detect_ticks >= cr.launch_ticks);
        CHECK(cr.confirm_ticks > cr.detect_ticks);  // confirmation on a later frame
        CHECK(cr.pid != 0);
        CHECK(cr.hwnd != 0);
        CHECK(cr.probe.matched);
        CHECK(cr.probe.edge_density > 0.2);  // clearly photo-like vs blank's 0.0
        // relative_ns is derived from the first matching frame (detect_ticks),
        // never the confirmation frame (confirm_ticks).
        const auto freq = oz::markers::qpc_frequency();
        CHECK(cr.relative_ns ==
              oz::markers::ticks_to_ns(cr.detect_ticks - cr.launch_ticks, freq));
        CHECK(cr.cleanup.outcome == "exited" || cr.cleanup.outcome == "forced");
    } else {
        std::cerr << "  reason: " << cr.reason << "\n";
    }
}

void test_capture_blank_never_matches(const std::wstring& control_exe) {
    // The window shows a blank forever; the probe must time out with an
    // explicit blank_or_nonmatching reason (never a false match).
    auto co = make_capture_options(control_exe,
                                   {L"--blank-ms", L"60000", L"--animate-ms", L"0", L"--topmost"});
    co.frame_timeout_ms = 1500;
    const auto cr = oz::collector::capture_external_visible(co);
    CHECK(cr.status == oz::collector::capture_status::blank_or_nonmatching);
    CHECK(!cr.reason.empty());
    CHECK(cr.relative_ns == 0);
}

void test_capture_missing_window(const std::wstring& control_exe) {
    // A subject that never creates a window -> missing_window.
    auto co = make_capture_options(control_exe, {L"--no-window"});
    co.window_wait_ms = 800;
    const auto cr = oz::collector::capture_external_visible(co);
    CHECK(cr.status == oz::collector::capture_status::missing_window);
    CHECK(!cr.reason.empty());
}

void test_capture_wrong_monitor(const std::wstring& control_exe) {
    // The window appears on its own monitor; a mismatched configured monitor
    // index must be an explicit wrong_monitor failure.
    const auto monitors = oz::collector::enumerate_monitors();
    if (monitors.empty()) {
        test::skip("no monitors enumerated");
        return;
    }
    auto co = make_capture_options(control_exe, {L"--pattern", L"--animate-ms", L"50", L"--topmost"});
    co.monitor_index = monitors[0].index + 1;  // guaranteed different from the window's
    const auto cr = oz::collector::capture_external_visible(co);
    CHECK(cr.status == oz::collector::capture_status::wrong_monitor);
    CHECK(!cr.reason.empty());
}

void test_capture_preflight_process_reuse(const std::wstring& control_exe) {
    // A subject already running must fail preflight; it is never killed.
    // launch_process waits for exit, so launch directly and keep the handle.
    std::wstring cmdline = L"\"" + control_exe +
                           L"\" --pattern --animate-ms 50 --exit-after-ms 20000";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, const_cast<wchar_t*>(cmdline.c_str()), nullptr, nullptr,
                        FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CHECK(false);
        return;
    }
    Sleep(600);  // let the window appear
    auto co = make_capture_options(control_exe, {L"--pattern", L"--animate-ms", L"50", L"--topmost"});
    const auto cr = oz::collector::capture_external_visible(co);
    CHECK(cr.status == oz::collector::capture_status::preflight_failed);
    CHECK(!cr.reason.empty());
    // The pre-existing process must still be alive afterwards (never killed).
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CHECK(code == STILL_ACTIVE);
    TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

void test_capture_forced_cleanup(const std::wstring& control_exe) {
    // A subject that ignores WM_CLOSE must be cleaned up by the Job Object
    // and the forced cleanup recorded.
    auto co = make_capture_options(control_exe,
                                   {L"--pattern", L"--animate-ms", L"50", L"--topmost", L"--ignore-close"});
    co.close_wait_ms = 800;
    const auto cr = oz::collector::capture_external_visible(co);
    CHECK(cr.status == oz::collector::capture_status::matched);
    CHECK(cr.cleanup.outcome == "forced");
    CHECK(cr.cleanup.forced);
}

void test_capture_unicode_title(const std::wstring& control_exe) {
    // Unicode window title: the window is associated by PID, so the title
    // must not break window discovery or the probe.
    auto co = make_capture_options(
        control_exe, {L"--pattern", L"--animate-ms", L"50", L"--topmost",
                      L"--title", L"\u00fc\u4e2d\u6587 \u0e19\u0e49\u0e33\u0e2d\u0e34\u0e07"});
    const auto cr = oz::collector::capture_external_visible(co);
    CHECK(cr.status == oz::collector::capture_status::matched);
}

void test_capture_launch_failure() {
    // A non-executable file must produce launch_failed, not a crash.
    const std::wstring dir = temp_dir();
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring path = dir + L"not_an_exe.txt";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(h, "not an executable", 17, &written, nullptr);
        CloseHandle(h);
    }
    auto co = make_capture_options(path, {});
    const auto cr = oz::collector::capture_external_visible(co);
    CHECK(cr.status == oz::collector::capture_status::launch_failed);
    CHECK(cr.error != 0);
    DeleteFileW(path.c_str());
    RemoveDirectoryW(dir.c_str());
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::cerr << "usage: ozbench_collector_tests <control_window_exe>\n";
        return 2;
    }
    const std::wstring control_exe = argv[1];

    // Pure tests always run.
    test_probe_blank_rejected();
    test_probe_pattern_matched();
    test_probe_extremes_rejected();
    test_consecutive_matcher();
    test_detection_point_semantics();
    test_dpi_scaling();
    test_manifest_seal();
    test_manifest_unicode_path();
    test_redaction();
    test_plan_schema_extension();
    test_capture_launch_failure();

    // Desktop Duplication integration tests: clean explicit skip when no
    // graphical desktop is available (never a false pass).
    const std::string dd_reason = oz::collector::desktop_duplication_availability();
    if (!dd_reason.empty()) {
        test::skip(("Desktop Duplication unavailable: " + dd_reason).c_str());
        std::cout << "ozbench_collector_tests: " << test::g_checks << " checks, "
                  << test::g_failures << " failures, " << test::g_skips << " skips\n";
        return test::g_failures == 0 ? 0 : 1;
    }

    test_capture_blank_then_pattern(control_exe);
    test_capture_blank_never_matches(control_exe);
    test_capture_missing_window(control_exe);
    test_capture_wrong_monitor(control_exe);
    test_capture_preflight_process_reuse(control_exe);
    test_capture_forced_cleanup(control_exe);
    test_capture_unicode_title(control_exe);

    std::cout << "ozbench_collector_tests: " << test::g_checks << " checks, "
              << test::g_failures << " failures, " << test::g_skips << " skips\n";
    return test::g_failures == 0 ? 0 : 1;
}
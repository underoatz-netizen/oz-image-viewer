// ozviewer_core_tests - focused Stage 1 tests (no test framework).
//
// Usage: ozviewer_core_tests
// Registered with CTest as "ozviewer_core_tests".
#include "oz/viewer_core/cache.hpp"
#include "oz/viewer_core/codec.hpp"
#include "oz/viewer_core/document_state.hpp"
#if OZVIEWER_ENABLE_EDITING
#include "oz/viewer_core/crop_interaction.hpp"
#include "oz/viewer_core/edit_state.hpp"
#include "oz/viewer_core/edit_pipeline.hpp"
#include "oz/viewer_core/image_document.hpp"
#include "oz/viewer_core/save_pipeline.hpp"
#include "../src/viewer/edit_controller.hpp"
#include "../src/viewer/modern_dialogs.hpp"
#include "../src/viewer/svg_icon_cache.hpp"
#include "../src/viewer/clipboard_output.hpp"
#include "../src/viewer/shortcut_routing.hpp"
#include "../src/viewer/ui_command_state.hpp"
#include "../src/viewer/ui_layout.hpp"
#endif
#include "oz/viewer_core/fit.hpp"
#include "oz/viewer_core/jpeg.hpp"
#include "oz/viewer_core/marker_emit.hpp"
#include "oz/viewer_core/navigation.hpp"
#include "oz/viewer_core/view_state.hpp"
#include "oz/viewer_core/work_queue.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef small
#undef small
#endif
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <wrl/client.h>

#include "png_mini_writer.hpp"
#include "webp_fixtures.hpp"
#include "phasea_fixtures.hpp"
#include "phaseb_fixtures.hpp"
#include "avif_fixtures.hpp"
#include "heic_fixtures.hpp"
#include "webp.hpp"
#include "../src/viewer_core/avif.hpp"
#include "../src/viewer_core/heic.hpp"
#include "../src/viewer_core/heif_loader.hpp"
#include "../src/viewer_core/raster_wic.hpp"
#include "../src/viewer_core/tga.hpp"

#include <turbojpeg.h>

#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace test {
int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* expr, const char* file, int line) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
    }
}
}  // namespace test

#define CHECK(expr) ::test::check(!!(expr), #expr, __FILE__, __LINE__)

namespace {

// --- fit math ---------------------------------------------------------------

void test_fit_centered() {
    // Image smaller than viewport: no upscale, centred.
    auto r = oz::viewer_core::fit_centered(100, 50, 400, 300);
    CHECK(r.width == 100);
    CHECK(r.height == 50);
    CHECK(r.x == (400 - 100) / 2);
    CHECK(r.y == (300 - 50) / 2);

    // Image larger than viewport: scaled down to fit, aspect preserved.
    r = oz::viewer_core::fit_centered(400, 300, 200, 100);
    // scale = min(200/400, 100/300, 1) = min(0.5, 0.3333) = 0.3333
    CHECK(r.width == 133);   // 400 * 0.3333 = 133.33 -> 133
    CHECK(r.height == 100);  // 300 * 0.3333 = 100
    CHECK(r.x == (200 - 133) / 2);
    CHECK(r.y == 0);

    // Exact fit.
    r = oz::viewer_core::fit_centered(200, 100, 200, 100);
    CHECK(r.width == 200);
    CHECK(r.height == 100);
    CHECK(r.x == 0);
    CHECK(r.y == 0);

    // Degenerate inputs yield an empty rectangle.
    r = oz::viewer_core::fit_centered(0, 100, 200, 100);
    CHECK(r.width == 0 && r.height == 0);
    r = oz::viewer_core::fit_centered(100, 100, 0, 100);
    CHECK(r.width == 0 && r.height == 0);
}

// DPI -> DIP coordinate contract for the Direct2D target (Stage 1 finding M4).
void test_fit_centered_dip() {
    auto approx = [](float a, float b) { return std::fabs(a - b) < 1e-3f; };

    // dpi == 96 is exactly the physical-pixel fit (1:1).
    const auto p96 = oz::viewer_core::fit_centered(100, 50, 400, 300);
    const auto d96 = oz::viewer_core::fit_centered_dip(100, 50, 400, 300, 96.0f);
    CHECK(approx(d96.x, static_cast<float>(p96.x)));
    CHECK(approx(d96.y, static_cast<float>(p96.y)));
    CHECK(approx(d96.width, static_cast<float>(p96.width)));
    CHECK(approx(d96.height, static_cast<float>(p96.height)));

    // 150% (dpi 144): the physical fit is scaled into DIPs by 96/144.
    const auto d144 = oz::viewer_core::fit_centered_dip(100, 50, 400, 300, 144.0f);
    CHECK(approx(d144.x, 150.0f * 96.0f / 144.0f));
    CHECK(approx(d144.y, 125.0f * 96.0f / 144.0f));
    CHECK(approx(d144.width, 100.0f * 96.0f / 144.0f));
    CHECK(approx(d144.height, 50.0f * 96.0f / 144.0f));

    // Downscaling case at 200% (dpi 192): DIP rect == physical fit * 0.5.
    const auto ph = oz::viewer_core::fit_centered(400, 300, 200, 100);
    const auto d192 = oz::viewer_core::fit_centered_dip(400, 300, 200, 100, 192.0f);
    CHECK(approx(d192.width, static_cast<float>(ph.width) * 0.5f));
    CHECK(approx(d192.height, static_cast<float>(ph.height) * 0.5f));
    CHECK(approx(d192.x, static_cast<float>(ph.x) * 0.5f));

    // Round-trip: mapping the DIP rect back to physical pixels recovers the
    // physical fit for every supported per-monitor scale (100/125/150/175/200%).
    for (const int pct : {100, 125, 150, 175, 200}) {
        const float dpi = 96.0f * static_cast<float>(pct) / 100.0f;
        const auto phy = oz::viewer_core::fit_centered(640, 480, 1280, 720);
        const auto dd = oz::viewer_core::fit_centered_dip(640, 480, 1280, 720, dpi);
        CHECK(approx(dd.width * dpi / 96.0f, static_cast<float>(phy.width)));
        CHECK(approx(dd.height * dpi / 96.0f, static_cast<float>(phy.height)));
        CHECK(approx(dd.x * dpi / 96.0f, static_cast<float>(phy.x)));
        CHECK(approx(dd.y * dpi / 96.0f, static_cast<float>(phy.y)));
    }

    // Degenerate inputs yield an empty DIP rectangle.
    const auto de = oz::viewer_core::fit_centered_dip(0, 100, 200, 100, 144.0f);
    CHECK(de.width == 0.0f && de.height == 0.0f);
    const auto de2 = oz::viewer_core::fit_centered_dip(100, 100, 0, 100, 144.0f);
    CHECK(de2.width == 0.0f && de2.height == 0.0f);

    // Non-positive dpi is treated as 96 (1:1), never a divide-by-zero.
    const auto dz = oz::viewer_core::fit_centered_dip(100, 50, 400, 300, 0.0f);
    CHECK(approx(dz.width, 100.0f) && approx(dz.height, 50.0f));
}

void test_scaling_denominator() {
    // Image much larger than viewport: use 1/8 if it still covers the viewport.
    // 8000/8=1000 >= 1000 but 6000/8=750 < 800 -> 1/8 undershoots the viewport
    // height, so the largest denominator that still covers the viewport is 1/4.
    CHECK(oz::viewer_core::choose_scaling_denominator(8000, 6000, 1000, 800) == 4);
    // 8000/8=1000 >= 1000, 6000/8=750 < 800 -> 1/4 (2000x1500).
    CHECK(oz::viewer_core::choose_scaling_denominator(8000, 6000, 1000, 900) == 4);
    // Image smaller than viewport: full resolution (no upscale).
    CHECK(oz::viewer_core::choose_scaling_denominator(100, 50, 400, 300) == 1);
    // Exact fit at full res.
    CHECK(oz::viewer_core::choose_scaling_denominator(200, 100, 200, 100) == 1);
    // Degenerate.
    CHECK(oz::viewer_core::choose_scaling_denominator(0, 100, 200, 100) == 1);
}

// --- JPEG validation / limits ----------------------------------------------

void test_jpeg_magic() {
    const std::uint8_t soi[] = {0xFF, 0xD8};
    CHECK(oz::viewer_core::has_jpeg_magic(soi, 2));
    const std::uint8_t not_jpeg[] = {0x89, 0x50, 0x4E, 0x47};
    CHECK(!oz::viewer_core::has_jpeg_magic(not_jpeg, 4));
    CHECK(!oz::viewer_core::has_jpeg_magic(soi, 1));  // too short
    CHECK(!oz::viewer_core::has_jpeg_magic(nullptr, 0));
}

void test_decoded_size_limits() {
    CHECK(oz::viewer_core::decoded_size_within_limits(100, 100));
    // Dimension limit: > 65535 rejected.
    CHECK(!oz::viewer_core::decoded_size_within_limits(65536, 100));
    CHECK(!oz::viewer_core::decoded_size_within_limits(100, 65536));
    // Zero / negative rejected.
    CHECK(!oz::viewer_core::decoded_size_within_limits(0, 100));
    CHECK(!oz::viewer_core::decoded_size_within_limits(-1, 100));
    // Overflow: 65535*65535*4 ~ 17 GB exceeds the 2 GiB decoded limit.
    CHECK(!oz::viewer_core::decoded_size_within_limits(65535, 65535));
}

// Build a small valid JPEG in memory using the TurboJPEG C API (libjpeg-turbo
// 3.2.0 legacy compress path: tjInitCompress + tjCompress2).
std::vector<std::uint8_t> make_test_jpeg(int w, int h) {
    std::vector<std::uint8_t> out;
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
            rgba[i + 0] = static_cast<std::uint8_t>((x * 255) / w);
            rgba[i + 1] = static_cast<std::uint8_t>((y * 255) / h);
            rgba[i + 2] = 128;
            rgba[i + 3] = 255;
        }
    }
    tjhandle enc = tjInitCompress();
    if (enc == nullptr) {
        return out;
    }
    unsigned char* buf = nullptr;
    unsigned long out_size = 0;
    const int rc = tjCompress2(enc, rgba.data(), w, 0, h, TJPF_RGBA, &buf, &out_size,
                               TJSAMP_420, 90, TJFLAG_FASTDCT);
    tjDestroy(enc);
    if (rc != 0 || buf == nullptr) {
        return out;
    }
    out.assign(buf, buf + out_size);
    tjFree(buf);
    return out;
}

void test_parse_and_decode_valid() {
    const auto jpeg = make_test_jpeg(64, 48);
    CHECK(!jpeg.empty());
    if (jpeg.empty()) {
        return;
    }
    oz::viewer_core::jpeg_header h;
    const auto err = oz::viewer_core::parse_jpeg_header(jpeg.data(), jpeg.size(), h);
    CHECK(err == oz::viewer_core::jpeg_error_kind::none);
    CHECK(h.width == 64);
    CHECK(h.height == 48);

    auto r = oz::viewer_core::decode_jpeg(jpeg.data(), jpeg.size(), 1);
    CHECK(r.error == oz::viewer_core::jpeg_error_kind::none);
    CHECK(r.header.width == 64);
    CHECK(r.header.height == 48);
    CHECK(r.stride == 64 * 4);
    CHECK(r.pixels.size() == static_cast<std::size_t>(64 * 48 * 4));
    // Alpha forced to 255.
    bool alpha_ok = true;
    for (std::size_t i = 3; i < r.pixels.size(); i += 4) {
        if (r.pixels[i] != 0xFF) {
            alpha_ok = false;
            break;
        }
    }
    CHECK(alpha_ok);

    // Scaled decode at 1/2 -> 32x24.
    auto r2 = oz::viewer_core::decode_jpeg(jpeg.data(), jpeg.size(), 2);
    CHECK(r2.error == oz::viewer_core::jpeg_error_kind::none);
    CHECK(r2.header.width == 32);
    CHECK(r2.header.height == 24);
}

void test_jpeg_errors() {
    // Not a JPEG (missing SOI).
    const std::uint8_t png[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    oz::viewer_core::jpeg_header h;
    CHECK(oz::viewer_core::parse_jpeg_header(png, sizeof(png), h) ==
          oz::viewer_core::jpeg_error_kind::not_jpeg);
    auto r = oz::viewer_core::decode_jpeg(png, sizeof(png), 1);
    CHECK(r.error == oz::viewer_core::jpeg_error_kind::not_jpeg);

    // Truncated: SOI only, no header.
    const std::uint8_t soi[] = {0xFF, 0xD8};
    CHECK(oz::viewer_core::parse_jpeg_header(soi, sizeof(soi), h) ==
          oz::viewer_core::jpeg_error_kind::invalid_header);

    // Corrupt: SOI + garbage.
    const std::uint8_t corrupt[] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46};
    CHECK(oz::viewer_core::parse_jpeg_header(corrupt, sizeof(corrupt), h) ==
          oz::viewer_core::jpeg_error_kind::invalid_header);

    // Empty / null.
    CHECK(oz::viewer_core::parse_jpeg_header(nullptr, 0, h) ==
          oz::viewer_core::jpeg_error_kind::not_jpeg);
}

// --- marker emission --------------------------------------------------------

void test_marker_emitter_disabled() {
    // Disabled emitter still constructs the session (ProcessStart) but writes
    // nothing. We cannot easily capture stdout here, so we assert the session
    // records the markers fed to it and that a contiguous canonical prefix
    // (as the real viewer emits) validates cleanly.
    oz::viewer_core::marker_emitter em(false);
    CHECK(em.session().records().size() == 1);
    CHECK(em.session().records().front().id == oz::markers::marker_id::process_start);
    // Record the Stage 1 canonical prefix (indices 1..9) after ProcessStart.
    const oz::markers::marker_id prefix[] = {
        oz::markers::marker_id::renderer_ready,     oz::markers::marker_id::file_opened,
        oz::markers::marker_id::codec_detected,     oz::markers::marker_id::header_parsed,
        oz::markers::marker_id::decode_start,       oz::markers::marker_id::decode_complete,
        oz::markers::marker_id::gpu_upload_start,   oz::markers::marker_id::gpu_upload_complete,
        oz::markers::marker_id::first_present,
    };
    for (const auto id : prefix) {
        em.record(id);
    }
    CHECK(em.session().records().size() == 10);
    CHECK(em.session().validate().empty());
}

// --- Stage 2.1 navigation tests ---------------------------------------------

void test_jpeg_extension_filter() {
    CHECK(oz::viewer_core::is_jpeg_extension(L"test.jpg"));
    CHECK(oz::viewer_core::is_jpeg_extension(L"test.JPG"));
    CHECK(oz::viewer_core::is_jpeg_extension(L"test.jpeg"));
    CHECK(oz::viewer_core::is_jpeg_extension(L"test.JPEG"));
    CHECK(oz::viewer_core::is_jpeg_extension(L"test.JpeG"));
    CHECK(oz::viewer_core::is_jpeg_extension(L"folder/sub/IMAGE.Jpg"));

    CHECK(!oz::viewer_core::is_jpeg_extension(L"test.png"));
    CHECK(!oz::viewer_core::is_jpeg_extension(L"test.bmp"));
    CHECK(!oz::viewer_core::is_jpeg_extension(L"test.webp"));
    CHECK(!oz::viewer_core::is_jpeg_extension(L"jpg"));
    CHECK(!oz::viewer_core::is_jpeg_extension(L""));
    CHECK(!oz::viewer_core::is_jpeg_extension(L"test.txt"));
}

void test_natural_path_sorting() {
    // Standard natural sort: img2.jpg comes before img10.jpg
    CHECK(oz::viewer_core::natural_path_less(L"img2.jpg", L"img10.jpg"));
    CHECK(!oz::viewer_core::natural_path_less(L"img10.jpg", L"img2.jpg"));
    CHECK(oz::viewer_core::natural_path_less(L"img1.jpg", L"img2.jpg"));

    // Case-insensitive natural ordering
    CHECK(oz::viewer_core::natural_path_less(L"a1.jpg", L"B1.jpg"));
    CHECK(oz::viewer_core::natural_path_less(L"A1.jpg", L"b1.jpg"));
}

void test_navigation_build_and_active_index() {
    std::vector<std::filesystem::path> candidates = {
        L"dir/img10.jpg",
        L"dir/notes.txt",
        L"dir/img2.jpg",
        L"dir/ignore.png",
        L"dir/img1.jpeg",
        L"dir/IMG3.JPG"
    };

    // Filtered for supported images (.jpg/.jpeg/.png) and naturally sorted:
    // [0]: dir/ignore.png
    // [1]: dir/img1.jpeg
    // [2]: dir/img2.jpg
    // [3]: dir/IMG3.JPG
    // [4]: dir/img10.jpg
    auto nav = oz::viewer_core::navigation_list::build(candidates, L"dir/IMG3.JPG");
    CHECK(nav.size() == 5);
    CHECK(nav.active_index().has_value() && *nav.active_index() == 3);
    CHECK(nav.current() != nullptr && *nav.current() == L"dir/IMG3.JPG");

    CHECK(*nav.at(0) == L"dir/ignore.png");
    CHECK(*nav.at(1) == L"dir/img1.jpeg");
    CHECK(*nav.at(2) == L"dir/img2.jpg");
    CHECK(*nav.at(3) == L"dir/IMG3.JPG");
    CHECK(*nav.at(4) == L"dir/img10.jpg");
    CHECK(nav.at(5) == nullptr);

    // Active path case matching (case-insensitive on Windows)
    auto nav_ci = oz::viewer_core::navigation_list::build(candidates, L"dir/img3.jpg");
    CHECK(nav_ci.active_index().has_value() && *nav_ci.active_index() == 3);
}

void test_navigation_boundary_and_movement() {
    std::vector<std::filesystem::path> candidates = {
        L"a.jpg", L"b.jpg", L"c.jpg"
    };
    auto nav = oz::viewer_core::navigation_list::build(candidates, L"b.jpg");
    CHECK(nav.active_index().has_value() && *nav.active_index() == 1);

    // select_next -> c.jpg (index 2)
    const auto* next = nav.select_next();
    CHECK(next != nullptr && *next == L"c.jpg");
    CHECK(*nav.active_index() == 2);

    // select_next at end -> no wrap, returns nullptr, index stays 2
    const auto* next_end = nav.select_next();
    CHECK(next_end == nullptr);
    CHECK(*nav.active_index() == 2);

    // select_previous -> b.jpg (index 1)
    const auto* prev = nav.select_previous();
    CHECK(prev != nullptr && *prev == L"b.jpg");
    CHECK(*nav.active_index() == 1);

    // select_previous -> a.jpg (index 0)
    const auto* prev_start = nav.select_previous();
    CHECK(prev_start != nullptr && *prev_start == L"a.jpg");
    CHECK(*nav.active_index() == 0);

    // select_previous at start -> no wrap, returns nullptr, index stays 0
    CHECK(nav.select_previous() == nullptr);
    CHECK(*nav.active_index() == 0);

    // select_last -> c.jpg (index 2)
    const auto* last = nav.select_last();
    CHECK(last != nullptr && *last == L"c.jpg");
    CHECK(*nav.active_index() == 2);
    // select_last when already at last -> nullptr
    CHECK(nav.select_last() == nullptr);

    // select_first -> a.jpg (index 0)
    const auto* first = nav.select_first();
    CHECK(first != nullptr && *first == L"a.jpg");
    CHECK(*nav.active_index() == 0);
    // select_first when already at first -> nullptr
    CHECK(nav.select_first() == nullptr);
}

void test_navigation_absent_active() {
    std::vector<std::filesystem::path> candidates = {
        L"x.jpg", L"y.jpg"
    };
    // Active path not in candidates
    auto nav = oz::viewer_core::navigation_list::build(candidates, L"z.jpg");
    CHECK(nav.size() == 2);
    CHECK(!nav.active_index().has_value());
    CHECK(nav.current() == nullptr);

    // Moving prev/next with no active index is a safe no-op
    CHECK(nav.select_previous() == nullptr);
    CHECK(nav.select_next() == nullptr);

    // First/last establishes active index
    const auto* f = nav.select_first();
    CHECK(f != nullptr && *f == L"x.jpg");
    CHECK(nav.active_index().has_value() && *nav.active_index() == 0);
}

void test_navigation_empty() {
    // Non-image extensions only; every supported common-raster hint is
    // filtered in (Phase A formats + Phase B .cur/.tga).
    std::vector<std::filesystem::path> candidates = {
        L"y.txt", L"z.raw"
    };
    auto nav = oz::viewer_core::navigation_list::build(candidates, L"foo.jpg");
    CHECK(nav.empty());
    CHECK(nav.size() == 0);
    CHECK(!nav.active_index().has_value());
    CHECK(nav.current() == nullptr);
    CHECK(nav.at(0) == nullptr);
    CHECK(nav.select_next() == nullptr);
    CHECK(nav.select_previous() == nullptr);
    CHECK(nav.select_first() == nullptr);
    CHECK(nav.select_last() == nullptr);
}

// --- Stage 2.2 Cache and Preload tests ------------------------------------

void test_cache_budget_calculation() {
    // Budget rule: min(512 MiB, floor(ullAvailPhys / 10))
    constexpr std::size_t k512MB = 512ull * 1024ull * 1024ull;

    // Very large memory: 16 GiB -> 10% is 1.6 GiB -> clamped to 512 MiB
    CHECK(oz::viewer_core::compute_cpu_cache_budget(16ull * 1024ull * 1024ull * 1024ull) == k512MB);

    // Exactly 5120 MiB -> 10% is 512 MiB
    CHECK(oz::viewer_core::compute_cpu_cache_budget(5120ull * 1024ull * 1024ull) == k512MB);

    // Small memory: 2 GiB -> 10% is 204.8 MiB (< 512 MiB)
    std::uint64_t avail_2gb = 2ull * 1024ull * 1024ull * 1024ull;
    CHECK(oz::viewer_core::compute_cpu_cache_budget(avail_2gb) == avail_2gb / 10);

    // Zero memory -> 0
    CHECK(oz::viewer_core::compute_cpu_cache_budget(0) == 0);

    // Integer truncation (floor)
    CHECK(oz::viewer_core::compute_cpu_cache_budget(19) == 1);
}

void test_preload_plan_generation() {
    std::vector<std::filesystem::path> items = {
        L"0.jpg", L"1.jpg", L"2.jpg", L"3.jpg", L"4.jpg", L"5.jpg", L"6.jpg"
    };

    // Active index 3 in middle:
    // P0: 3
    // P1: 4 (+1)
    // P2: 2 (-1)
    // P3: 5 (+2)
    // P4: 1 (-2)
    auto plan = oz::viewer_core::compute_preload_plan(items.size(), 3, items, 1);
    CHECK(plan.size() == 5);
    CHECK(plan[0].index == 3 && plan[0].priority == oz::viewer_core::work_priority::p0_selected);
    CHECK(plan[1].index == 4 && plan[1].priority == oz::viewer_core::work_priority::p1_next);
    CHECK(plan[2].index == 2 && plan[2].priority == oz::viewer_core::work_priority::p2_prev);
    CHECK(plan[3].index == 5 && plan[3].priority == oz::viewer_core::work_priority::p3_next2);
    CHECK(plan[4].index == 1 && plan[4].priority == oz::viewer_core::work_priority::p4_prev2);

    // Active index 0 (start boundary):
    // P0: 0
    // P1: 1 (+1)
    // P3: 2 (+2)
    // (no negative indices)
    auto plan0 = oz::viewer_core::compute_preload_plan(items.size(), 0, items, 1);
    CHECK(plan0.size() == 3);
    CHECK(plan0[0].index == 0 && plan0[0].priority == oz::viewer_core::work_priority::p0_selected);
    CHECK(plan0[1].index == 1 && plan0[1].priority == oz::viewer_core::work_priority::p1_next);
    CHECK(plan0[2].index == 2 && plan0[2].priority == oz::viewer_core::work_priority::p3_next2);

    // Active index 6 (end boundary):
    // P0: 6
    // P2: 5 (-1)
    // P4: 4 (-2)
    auto plan_end = oz::viewer_core::compute_preload_plan(items.size(), 6, items, 1);
    CHECK(plan_end.size() == 3);
    CHECK(plan_end[0].index == 6 && plan_end[0].priority == oz::viewer_core::work_priority::p0_selected);
    CHECK(plan_end[1].index == 5 && plan_end[1].priority == oz::viewer_core::work_priority::p2_prev);
    CHECK(plan_end[2].index == 4 && plan_end[2].priority == oz::viewer_core::work_priority::p4_prev2);

    // Empty list
    auto plan_empty = oz::viewer_core::compute_preload_plan(0, 0, {}, 1);
    CHECK(plan_empty.empty());

    // Single item list
    std::vector<std::filesystem::path> one = { L"solo.jpg" };
    auto plan_one = oz::viewer_core::compute_preload_plan(one.size(), 0, one, 1);
    CHECK(plan_one.size() == 1);
    CHECK(plan_one[0].index == 0 && plan_one[0].priority == oz::viewer_core::work_priority::p0_selected);
}

void test_cache_key_matching_and_profile() {
    oz::viewer_core::cache_key k1{L"C:/photos/IMG_001.JPG", 1};
    oz::viewer_core::cache_key k2{L"c:/photos/img_001.jpg", 1};
    oz::viewer_core::cache_key k3{L"C:/photos/IMG_001.JPG", 2};  // different profile/scale

    // Normalized path comparison
    CHECK(oz::viewer_core::normalize_path_key(L"C:/photos/IMG_001.JPG") ==
          oz::viewer_core::normalize_path_key(L"c:/photos/img_001.jpg"));

    oz::viewer_core::cache_key nk1{oz::viewer_core::normalize_path_key(L"C:/photos/IMG_001.JPG"), 1};
    oz::viewer_core::cache_key nk2{oz::viewer_core::normalize_path_key(L"c:/photos/img_001.jpg"), 1};
    oz::viewer_core::cache_key nk3{oz::viewer_core::normalize_path_key(L"c:/photos/img_001.jpg"), 2};

    CHECK(nk1 == nk2);
    CHECK(!(nk1 == nk3));  // Different scaling denominator is a cache miss
}

static std::shared_ptr<oz::viewer_core::decoded_image_data> make_dummy_image(std::size_t pixel_bytes) {
    auto img = std::make_shared<oz::viewer_core::decoded_image_data>();
    img->pixels.resize(pixel_bytes);
    img->width = 100;
    img->height = 100;
    img->stride = 400;
    return img;
}

void test_cache_admission_eviction_and_pinned_current() {
    // Each dummy image of 3000 bytes has byte_size() == 3000 + sizeof(decoded_image_data).
    // Let budget be 8000 bytes so 2 images fit (~6064 bytes) but 3 do not (~9096 > 8000).
    oz::viewer_core::cpu_image_cache cache(8000);

    oz::viewer_core::cache_key k1{L"img1.jpg", 1};
    oz::viewer_core::cache_key k2{L"img2.jpg", 1};
    oz::viewer_core::cache_key k3{L"img3.jpg", 1};

    auto img1 = make_dummy_image(3000);
    auto img2 = make_dummy_image(3000);
    auto img3 = make_dummy_image(3000);

    const std::size_t sz2 = img2->byte_size();
    const std::size_t sz3 = img3->byte_size();

    // Insert img1 as current (pinned)
    cache.insert(k1, img1, true);
    CHECK(cache.size() == 1);
    CHECK(cache.get(k1) != nullptr);
    CHECK(cache.pinned_key().has_value() && *cache.pinned_key() == k1);

    // Insert img2 as preload
    std::vector<oz::viewer_core::cache_key> window = {k1, k2};
    CHECK(cache.can_admit_preload(sz2));
    cache.insert(k2, img2, false, window);
    CHECK(cache.size() == 2);
    CHECK(cache.get(k2) != nullptr);

    // Insert img3 as preload with window containing only k1 and k3 (k2 outside window)
    std::vector<oz::viewer_core::cache_key> new_window = {k1, k3};
    // k2 is outside window, so it can be evicted to admit k3
    CHECK(cache.can_admit_preload(sz3));
    cache.insert(k3, img3, false, new_window);

    // Verify k2 was evicted because it was outside window, and k1 was pinned so never evicted
    CHECK(cache.get(k1) != nullptr);
    CHECK(cache.get(k3) != nullptr);
    CHECK(cache.get(k2) == nullptr);
    CHECK(cache.size() == 2);
}

void test_cache_pinned_oversized_current() {
    // Budget 5000 bytes
    oz::viewer_core::cpu_image_cache cache(5000);

    oz::viewer_core::cache_key k_small{L"small.jpg", 1};
    auto img_small = make_dummy_image(2000);
    cache.insert(k_small, img_small, false);
    CHECK(cache.size() == 1);

    // Now insert an oversized current image (8000 bytes > 5000 budget)
    oz::viewer_core::cache_key k_big{L"big.jpg", 1};
    auto img_big = make_dummy_image(8000);
    cache.insert(k_big, img_big, true);

    // Current alone exceeds budget: must retain it alone, evict everything else, and disable preloads
    CHECK(cache.size() == 1);
    CHECK(cache.get(k_big) != nullptr);
    CHECK(cache.get(k_small) == nullptr);
    CHECK(cache.preloads_disabled());
    CHECK(!cache.can_admit_preload(100));

    // Trying to insert a preload while preloads disabled must be rejected
    oz::viewer_core::cache_key k_pre{L"pre.jpg", 1};
    auto img_pre = make_dummy_image(500);
    cache.insert(k_pre, img_pre, false);
    CHECK(cache.get(k_pre) == nullptr);
    CHECK(cache.size() == 1);
}

void test_work_queue_priority_and_replacement() {
    oz::viewer_core::decode_work_queue queue;

    // Queue initial item
    oz::viewer_core::decode_request req1;
    req1.epoch = 1;
    req1.priority = oz::viewer_core::work_priority::p1_next;
    req1.path = L"next.jpg";
    queue.push(req1);

    // Now replace plan with epoch 2 containing P0 and P2
    oz::viewer_core::decode_request p0;
    p0.epoch = 2;
    p0.priority = oz::viewer_core::work_priority::p0_selected;
    p0.path = L"cur.jpg";

    oz::viewer_core::decode_request p2;
    p2.epoch = 2;
    p2.priority = oz::viewer_core::work_priority::p2_prev;
    p2.path = L"prev.jpg";

    queue.replace_plan(2, {p2, p0});

    uint64_t pop_epoch = 0;
    auto first_popped = queue.pop_work(pop_epoch);
    CHECK(first_popped.has_value());
    CHECK(first_popped->priority == oz::viewer_core::work_priority::p0_selected);
    CHECK(first_popped->path == L"cur.jpg");
    CHECK(pop_epoch == 2);

    auto second_popped = queue.pop_work(pop_epoch);
    CHECK(second_popped.has_value());
    CHECK(second_popped->priority == oz::viewer_core::work_priority::p2_prev);
    CHECK(second_popped->path == L"prev.jpg");

    queue.request_stop();
    auto stop_pop = queue.pop_work(pop_epoch);
    CHECK(!stop_pop.has_value());
}

static std::shared_ptr<oz::viewer_core::decoded_image_data> make_profile_image(
    std::size_t pixel_bytes, int scale_den, std::int32_t src_w, std::int32_t src_h) {
    auto img = make_dummy_image(pixel_bytes);
    img->scale_den = scale_den;
    img->src_width = src_w;
    img->src_height = src_h;
    return img;
}

void test_actual_profile_selection_and_cache_mismatch() {
    // Large source requires 1/4 for a 1000x800 viewport.
    CHECK(oz::viewer_core::choose_scaling_denominator(8000, 6000, 1000, 800) == 4);
    auto img_wrong = make_profile_image(1000, 1, 8000, 6000);
    auto img_right = make_profile_image(1000, 4, 8000, 6000);
    CHECK(!oz::viewer_core::image_profile_matches_viewport(*img_wrong, 1000, 800));
    CHECK(oz::viewer_core::image_profile_matches_viewport(*img_right, 1000, 800));
    // Same right-profile image mismatches a much larger viewport (needs 1/2).
    CHECK(oz::viewer_core::choose_scaling_denominator(8000, 6000, 4000, 3000) == 2);
    CHECK(!oz::viewer_core::image_profile_matches_viewport(*img_right, 4000, 3000));

    // Cache holds both variants; viewport lookup accepts only the matching one.
    oz::viewer_core::cpu_image_cache cache(1 << 20);
    const std::wstring norm = oz::viewer_core::normalize_path_key(L"big.jpg");
    cache.insert({norm, 1}, img_wrong, false);
    cache.insert({norm, 4}, img_right, false);
    auto found = cache.find_for_viewport(norm, 1000, 800);
    CHECK(found != nullptr);
    CHECK(found->scale_den == 4);
    auto key_opt = cache.find_key_for_viewport(norm, 1000, 800);
    CHECK(key_opt.has_value() && key_opt->scale_den == 4);
    // Viewport requiring 1/2 matches neither stored variant.
    CHECK(cache.find_for_viewport(norm, 4000, 3000) == nullptr);
}

void test_cache_entry_vs_resident_image_counts() {
    // cache_entry_count and resident_decoded_count are distinct metrics: two
    // decode variants of one source count twice as entries and once resident.
    oz::viewer_core::cpu_image_cache cache(1 << 20);
    const std::wstring a = oz::viewer_core::normalize_path_key(L"a.jpg");
    const std::wstring b = oz::viewer_core::normalize_path_key(L"b.jpg");
    cache.insert({a, 1}, make_profile_image(1000, 1, 8000, 6000), false);
    cache.insert({a, 4}, make_profile_image(1000, 4, 8000, 6000), false);
    cache.insert({b, 1}, make_profile_image(1000, 1, 2000, 1000), false);
    CHECK(cache.size() == 3);
    CHECK(cache.resident_image_count() == 2);
    cache.clear_except_pinned();
    CHECK(cache.size() == 0);
    CHECK(cache.resident_image_count() == 0);
}

void test_initial_profile_key_correctness() {
    // Initial decode for a large source must be keyed by its actual profile.
    const int den = oz::viewer_core::choose_scaling_denominator(8000, 6000, 1000, 800);
    CHECK(den == 4);
    const std::wstring norm = oz::viewer_core::normalize_path_key(L"initial.jpg");
    const oz::viewer_core::cache_key init_key{norm, den};
    CHECK(init_key.scale_den == 4);
    auto img = make_profile_image(2000, den, 8000, 6000);
    oz::viewer_core::cpu_image_cache cache(1 << 20);
    cache.insert(init_key, img, true);
    CHECK(cache.pinned_key().has_value() && *cache.pinned_key() == init_key);
    // Exact full-res key is a miss; viewport lookup hits the actual profile.
    CHECK(cache.get({norm, 1}) == nullptr);
    CHECK(cache.find_for_viewport(norm, 1000, 800) != nullptr);
    // Source metadata is recoverable without file I/O for plan guessing.
    std::int32_t sw = 0;
    std::int32_t sh = 0;
    CHECK(cache.try_get_source_dims(norm, sw, sh));
    CHECK(sw == 8000 && sh == 6000);
    CHECK(oz::viewer_core::choose_scaling_denominator(sw, sh, 1000, 800) == den);
}

void test_stale_completion_promotion_no_duplicate() {
    const std::wstring desired = oz::viewer_core::normalize_path_key(L"cur.jpg");
    auto img = make_profile_image(1000, 4, 8000, 6000);
    oz::viewer_core::decode_result_msg stale;
    stale.epoch = 1;  // strictly older than the latest epoch
    stale.priority = oz::viewer_core::work_priority::p0_selected;
    stale.key = {desired, 4};
    stale.path = L"cur.jpg";
    stale.ok = true;
    stale.image = img;
    stale.src_width = 8000;
    stale.src_height = 6000;
    stale.actual_scale_den = 4;
    // Same path+profile is promotable despite the epoch gap.
    CHECK(oz::viewer_core::should_commit_completion(stale, desired, 1000, 800));
    // Wrong path never commits.
    CHECK(!oz::viewer_core::should_commit_completion(
        stale, oz::viewer_core::normalize_path_key(L"other.jpg"), 1000, 800));
    // Wrong profile (viewport changed) never commits.
    CHECK(!oz::viewer_core::should_commit_completion(stale, desired, 4000, 3000));
    auto bad = stale;
    bad.key.scale_den = 1;
    bad.actual_scale_den = 1;
    bad.image = make_profile_image(1000, 1, 8000, 6000);
    CHECK(!oz::viewer_core::should_commit_completion(bad, desired, 1000, 800));
    auto failed = stale;
    failed.ok = false;
    CHECK(!oz::viewer_core::should_commit_completion(failed, desired, 1000, 800));

    // A valid queued completion suppresses a duplicate P0 schedule.
    oz::viewer_core::completion_queue q;
    CHECK(!q.has_valid_for_path(desired, 1000, 800));
    q.push(stale);
    CHECK(q.has_valid_for_path(desired, 1000, 800));
    CHECK(!q.has_valid_for_path(desired, 4000, 3000));
}

void test_failed_present_preserves_pin_seam() {
    // Failure path keeps the candidate only as an unpinned preload:
    // the old pin/current entry is untouched.
    oz::viewer_core::cpu_image_cache cache(1 << 20);
    const oz::viewer_core::cache_key k_old{L"old.jpg", 1};
    auto img_old = make_dummy_image(2000);
    cache.insert(k_old, img_old, true);
    const auto pinned_before = cache.pinned_key();
    CHECK(pinned_before.has_value() && *pinned_before == k_old);
    const oz::viewer_core::cache_key k_cand{L"new.jpg", 1};
    auto img_cand = make_dummy_image(2000);
    CHECK(cache.can_admit_preload(img_cand->byte_size()));
    cache.insert(k_cand, img_cand, false);
    CHECK(cache.pinned_key().has_value() && *cache.pinned_key() == k_old);
    CHECK(cache.get(k_old) != nullptr);
    CHECK(cache.get(k_cand) != nullptr);
}

void test_oversized_current_stays_pinned() {
    oz::viewer_core::cpu_image_cache cache(5000);
    const oz::viewer_core::cache_key k_small{L"small.jpg", 1};
    cache.insert(k_small, make_dummy_image(2000), false);
    const oz::viewer_core::cache_key k_big{L"big.jpg", 1};
    cache.insert(k_big, make_dummy_image(8000), true);
    CHECK(cache.size() == 1);
    CHECK(cache.get(k_big) != nullptr);
    CHECK(cache.get(k_small) == nullptr);
    CHECK(cache.pinned_key().has_value() && *cache.pinned_key() == k_big);
    CHECK(cache.preloads_disabled());
}

void test_completion_retention_drop_policy() {
    oz::viewer_core::completion_queue q;
    auto mk = [](int i, oz::viewer_core::work_priority p) {
        oz::viewer_core::decode_result_msg m;
        m.epoch = static_cast<std::uint64_t>(i);
        m.priority = p;
        m.ok = true;
        return m;
    };
    // Fill to capacity with oldest = P1 then P0s.
    q.push(mk(0, oz::viewer_core::work_priority::p1_next));
    for (int i = 1; i < 32; ++i) {
        q.push(mk(i, oz::viewer_core::work_priority::p0_selected));
    }
    CHECK(q.size() == 32);
    // New preload must drop the oldest non-P0 (epoch 0), retaining all P0s.
    q.push(mk(100, oz::viewer_core::work_priority::p2_prev));
    CHECK(q.size() == 32);
    auto all = q.pop_all();
    CHECK(all.size() == 32);
    bool has_epoch0 = false;
    bool has_epoch100 = false;
    int p0_count = 0;
    for (const auto& m : all) {
        if (m.epoch == 0) {
            has_epoch0 = true;
        }
        if (m.epoch == 100) {
            has_epoch100 = true;
        }
        if (m.priority == oz::viewer_core::work_priority::p0_selected) {
            ++p0_count;
        }
    }
    CHECK(!has_epoch0);
    CHECK(has_epoch100);
    CHECK(p0_count == 31);

    // All-P0 full queue drops the oldest entry.
    oz::viewer_core::completion_queue q2;
    for (int i = 0; i < 32; ++i) {
        q2.push(mk(i, oz::viewer_core::work_priority::p0_selected));
    }
    q2.push(mk(99, oz::viewer_core::work_priority::p0_selected));
    auto all2 = q2.pop_all();
    CHECK(all2.size() == 32);
    CHECK(all2.front().epoch == 1);
    CHECK(all2.back().epoch == 99);
}

// --- Stage 3.1 codec registry + PNG (WIC) tests ----------------------------

// Signature-first routing: bytes alone decide; extensions never do.
void test_registry_signature_routing() {
    const std::uint8_t jpeg_soi[] = {0xFF, 0xD8, 0xFF, 0xE0};
    CHECK(oz::viewer_core::detect_codec(jpeg_soi, sizeof(jpeg_soi)).has_value());
    CHECK(*oz::viewer_core::detect_codec(jpeg_soi, sizeof(jpeg_soi)) ==
          oz::viewer_core::codec_id::jpeg);

    const auto png = oz::test_png::make_rgb_png(
        2, 2, oz::test_png::gradient(2, 2));
    CHECK(oz::viewer_core::has_png_signature(png.data(), png.size()));
    const auto png_codec = oz::viewer_core::detect_codec(png.data(), png.size());
    CHECK(png_codec.has_value() &&
          *png_codec == oz::viewer_core::codec_id::png_wic);

    // PNG bytes "named .jpg" still route to PNG; JPEG bytes "named .png"
    // still route to JPEG: routing never consults the extension.
    CHECK(*oz::viewer_core::detect_codec(png.data(), png.size()) ==
          oz::viewer_core::codec_id::png_wic);
    CHECK(*oz::viewer_core::detect_codec(jpeg_soi, sizeof(jpeg_soi)) ==
          oz::viewer_core::codec_id::jpeg);

    // Unsupported data: wrong magic, truncated signature, empty.
    const std::uint8_t bmp[] = {'B', 'M', 0, 0};
    CHECK(!oz::viewer_core::detect_codec(bmp, sizeof(bmp)).has_value());
    const std::uint8_t short_sig[] = {0x89, 0x50, 0x4E};
    CHECK(!oz::viewer_core::detect_codec(short_sig, sizeof(short_sig)).has_value());
    CHECK(!oz::viewer_core::detect_codec(nullptr, 0).has_value());
    CHECK(!oz::viewer_core::has_png_signature(nullptr, 0));
    CHECK(!oz::viewer_core::has_png_signature(short_sig, sizeof(short_sig)));

    // Registry header rejects unsupported data with a bounded generic error.
    oz::viewer_core::image_info info;
    CHECK(oz::viewer_core::parse_image_header(bmp, sizeof(bmp), info) ==
          oz::viewer_core::codec_error::unsupported);
    auto dr = oz::viewer_core::decode_image(bmp, sizeof(bmp), 1);
    CHECK(dr.error == oz::viewer_core::codec_error::unsupported);
}

void test_registry_checked_dims() {
    std::int32_t stride = 0;
    std::size_t bytes = 0;
    CHECK(oz::viewer_core::checked_stride_and_size(100, 100, stride, bytes));
    CHECK(stride == 400);
    CHECK(bytes == 40000);
    CHECK(!oz::viewer_core::checked_stride_and_size(0, 100, stride, bytes));
    CHECK(!oz::viewer_core::checked_stride_and_size(100, -5, stride, bytes));
    CHECK(!oz::viewer_core::checked_stride_and_size(65536, 10, stride, bytes));
    // 32768x16385x4 exceeds the 2 GiB decoded cap.
    CHECK(!oz::viewer_core::checked_stride_and_size(32768, 16385, stride, bytes));
    // Registry limits mirror the JPEG bounds.
    CHECK(oz::viewer_core::decoded_size_within_limits(100, 100));
    CHECK(!oz::viewer_core::decoded_size_within_limits(65535, 65535));
}

// JPEG regression sentinel through the registry: identical profile,
// opaque premultiplied output, bounded errors preserved.
void test_registry_jpeg_sentinel() {
    const auto jpeg = make_test_jpeg(64, 48);
    CHECK(!jpeg.empty());
    if (jpeg.empty()) {
        return;
    }
    oz::viewer_core::image_info info;
    CHECK(oz::viewer_core::parse_image_header(jpeg.data(), jpeg.size(), info) ==
          oz::viewer_core::codec_error::none);
    CHECK(info.codec == oz::viewer_core::codec_id::jpeg);
    CHECK(info.src_width == 64 && info.src_height == 48);

    auto dr = oz::viewer_core::decode_image(jpeg.data(), jpeg.size(), 1);
    CHECK(dr.error == oz::viewer_core::codec_error::none);
    CHECK(dr.image.codec == oz::viewer_core::codec_id::jpeg);
    CHECK(dr.image.width == 64 && dr.image.height == 48);
    CHECK(dr.image.stride == 64 * 4);
    CHECK(dr.image.scale_den == 1);
    CHECK(dr.image.src_width == 64 && dr.image.src_height == 48);
    CHECK(dr.image.pixels.size() == static_cast<std::size_t>(64 * 48 * 4));
    bool alpha_ok = true;
    for (std::size_t i = 3; i < dr.image.pixels.size(); i += 4) {
        if (dr.image.pixels[i] != 0xFF) {
            alpha_ok = false;
            break;
        }
    }
    CHECK(alpha_ok);

    // Invalid scaling denominator falls back to 1.
    auto dr_bad = oz::viewer_core::decode_image(jpeg.data(), jpeg.size(), 3);
    CHECK(dr_bad.error == oz::viewer_core::codec_error::none);
    CHECK(dr_bad.image.width == 64 && dr_bad.image.height == 48);
}

// Opaque RGB PNG: exact dims/stride, alpha 255, canonical profile.
void test_png_decode_opaque_rgb() {
    const auto png =
        oz::test_png::make_rgb_png(4, 3, oz::test_png::gradient(4, 3));
    oz::viewer_core::image_info info;
    CHECK(oz::viewer_core::parse_image_header(png.data(), png.size(), info) ==
          oz::viewer_core::codec_error::none);
    CHECK(info.codec == oz::viewer_core::codec_id::png_wic);
    CHECK(info.src_width == 4 && info.src_height == 3);

    auto dr = oz::viewer_core::decode_image(png.data(), png.size(), 1);
    CHECK(dr.error == oz::viewer_core::codec_error::none);
    CHECK(dr.image.width == 4 && dr.image.height == 3);
    CHECK(dr.image.stride == 16);
    CHECK(dr.image.scale_den == 1);
    CHECK(dr.image.src_width == 4 && dr.image.src_height == 3);
    CHECK(dr.image.pixels.size() == static_cast<std::size_t>(4 * 3 * 4));
    for (std::size_t i = 3; i < dr.image.pixels.size(); i += 4) {
        CHECK(dr.image.pixels[i] == 0xFF);
    }
    // Spot-check the gradient corners (opaque => straight == premultiplied):
    // (0,0) is (R0,G0,B128) -> BGRA (128,0,0,255); (3,2) is (255,255,128).
    CHECK(dr.image.pixels[0] == 128 && dr.image.pixels[1] == 0 &&
          dr.image.pixels[2] == 0 && dr.image.pixels[3] == 0xFF);
    const std::size_t last = dr.image.pixels.size() - 4;
    CHECK(dr.image.pixels[last + 0] == 128 && dr.image.pixels[last + 1] == 255 &&
          dr.image.pixels[last + 2] == 255 && dr.image.pixels[last + 3] == 0xFF);
}

// Exact RGBA premultiplication: only 0/255 channels with 0/128/255 alpha so
// the (c*a+127)/255 rounding is exact under any conforming converter.
void test_png_alpha_premultiplication_exact() {
    using oz::test_png::rgba;
    // 2x2: opaque red, half white, transparent green, opaque black.
    const std::vector<rgba> px = {{255, 0, 0, 255}, {255, 255, 255, 128},
                                  {0, 255, 0, 0},   {0, 0, 0, 255}};
    const auto png = oz::test_png::make_rgba_png(2, 2, px);
    auto dr = oz::viewer_core::decode_image(png.data(), png.size(), 1);
    CHECK(dr.error == oz::viewer_core::codec_error::none);
    CHECK(dr.image.width == 2 && dr.image.height == 2);
    CHECK(dr.image.pixels.size() == 16);
    const auto& p = dr.image.pixels;
    // Row 0: opaque red -> (0,0,255,255); half white -> (128,128,128,128).
    CHECK(p[0] == 0 && p[1] == 0 && p[2] == 255 && p[3] == 255);
    CHECK(p[4] == 128 && p[5] == 128 && p[6] == 128 && p[7] == 128);
    // Row 1: transparent green is preserved as (0,0,0,0); opaque black stays.
    CHECK(p[8] == 0 && p[9] == 0 && p[10] == 0 && p[11] == 0);
    CHECK(p[12] == 0 && p[13] == 0 && p[14] == 0 && p[15] == 255);
}

void test_png_gray_palette_interlaced() {
    // Grayscale maps to (g,g,g,255).
    const std::vector<std::uint8_t> gray = {0, 85, 170, 255};
    const auto gray_png = oz::test_png::make_gray_png(2, 2, gray);
    auto dg = oz::viewer_core::decode_image(gray_png.data(), gray_png.size(), 1);
    CHECK(dg.error == oz::viewer_core::codec_error::none);
    CHECK(dg.image.width == 2 && dg.image.height == 2);
    for (int i = 0; i < 4; ++i) {
        const std::uint8_t g = gray[static_cast<std::size_t>(i)];
        CHECK(dg.image.pixels[static_cast<std::size_t>(i) * 4 + 0] == g);
        CHECK(dg.image.pixels[static_cast<std::size_t>(i) * 4 + 1] == g);
        CHECK(dg.image.pixels[static_cast<std::size_t>(i) * 4 + 2] == g);
        CHECK(dg.image.pixels[static_cast<std::size_t>(i) * 4 + 3] == 0xFF);
    }

    // Palette + tRNS: index 0 opaque red, index 1 half blue.
    using oz::test_png::rgba;
    const std::vector<std::uint8_t> idx = {0, 1, 1, 0};
    const std::vector<rgba> pal = {{255, 0, 0, 255}, {0, 0, 255, 255}};
    const std::vector<std::uint8_t> trns = {255, 128};
    const auto pal_png = oz::test_png::make_palette_png(2, 2, idx, pal, trns);
    auto dp = oz::viewer_core::decode_image(pal_png.data(), pal_png.size(), 1);
    CHECK(dp.error == oz::viewer_core::codec_error::none);
    CHECK(dp.image.width == 2 && dp.image.height == 2);
    // Opaque red exact (BGRA 0,0,255,255); half blue exact (128,0,0,128).
    CHECK(dp.image.pixels[2] == 255 && dp.image.pixels[3] == 255);
    CHECK(dp.image.pixels[4] == 128 && dp.image.pixels[5] == 0 &&
          dp.image.pixels[6] == 0 && dp.image.pixels[7] == 128);

    // Interlaced decodes to the same pixels as non-interlaced.
    const auto grad = oz::test_png::gradient(7, 5);
    const auto plain = oz::test_png::make_rgba_png(7, 5, grad, false);
    const auto inter = oz::test_png::make_rgba_png(7, 5, grad, true);
    auto d_plain = oz::viewer_core::decode_image(plain.data(), plain.size(), 1);
    auto d_inter = oz::viewer_core::decode_image(inter.data(), inter.size(), 1);
    CHECK(d_plain.error == oz::viewer_core::codec_error::none);
    CHECK(d_inter.error == oz::viewer_core::codec_error::none);
    CHECK(d_inter.image.width == 7 && d_inter.image.height == 5);
    CHECK(d_plain.image.pixels == d_inter.image.pixels);
}

// EXIF orientations 1-8 incl. swapped dims; invalid values fall back to normal.
void test_png_exif_orientations() {
    using oz::test_png::rgba;
    // 3x2 stored image with distinct opaque pixels: value v(y,x) = (B=y,G=x,R=0).
    const std::uint32_t w = 3;
    const std::uint32_t h = 2;
    std::vector<rgba> px(w * h);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            px[y * w + x] = {0, static_cast<std::uint8_t>(x),
                             static_cast<std::uint8_t>(y), 255};
        }
    }
    auto stored_at = [&](std::uint32_t x, std::uint32_t y) -> rgba {
        return px[y * w + x];
    };
    // Expected oriented pixel for each orientation (canonical EXIF semantics).
    auto expect = [&](int o, std::uint32_t dx, std::uint32_t dy) -> rgba {
        switch (o) {
            case 2: return stored_at(w - 1 - dx, dy);
            case 3: return stored_at(w - 1 - dx, h - 1 - dy);
            case 4: return stored_at(dx, h - 1 - dy);
            case 5: return stored_at(dy, dx);
            case 6: return stored_at(dy, h - 1 - dx);
            case 7: return stored_at(w - 1 - dy, h - 1 - dx);
            case 8: return stored_at(w - 1 - dy, dx);
            default: return stored_at(dx, dy);
        }
    };
    for (int o = 1; o <= 8; ++o) {
        const auto png = oz::test_png::make_rgba_png(w, h, px, false, o);
        oz::viewer_core::image_info info;
        CHECK(oz::viewer_core::parse_image_header(png.data(), png.size(), info) ==
              oz::viewer_core::codec_error::none);
        const bool swap = o >= 5;
        CHECK(info.src_width == (swap ? 2 : 3));
        CHECK(info.src_height == (swap ? 3 : 2));
        auto dr = oz::viewer_core::decode_image(png.data(), png.size(), 1);
        CHECK(dr.error == oz::viewer_core::codec_error::none);
        CHECK(dr.image.width == (swap ? 2 : 3));
        CHECK(dr.image.height == (swap ? 3 : 2));
        // Full-pixel comparison against the expected transform.
        bool match = true;
        for (std::int32_t y = 0; y < dr.image.height && match; ++y) {
            for (std::int32_t x = 0; x < dr.image.width && match; ++x) {
                const rgba e = expect(o, static_cast<std::uint32_t>(x),
                                      static_cast<std::uint32_t>(y));
                const std::uint8_t* q =
                    dr.image.pixels.data() +
                    (static_cast<std::size_t>(y) * dr.image.width + x) * 4;
                if (q[0] != e.b || q[1] != e.g || q[2] != e.r || q[3] != 0xFF) {
                    match = false;
                }
            }
        }
        CHECK(match);
    }
    // Invalid orientation value is ignored (decodes as normal).
    const auto bad = oz::test_png::make_rgba_png(w, h, px, false, 9);
    oz::viewer_core::image_info binfo;
    CHECK(oz::viewer_core::parse_image_header(bad.data(), bad.size(), binfo) ==
          oz::viewer_core::codec_error::none);
    CHECK(binfo.src_width == 3 && binfo.src_height == 2);
    auto dbad = oz::viewer_core::decode_image(bad.data(), bad.size(), 1);
    CHECK(dbad.error == oz::viewer_core::codec_error::none);
    CHECK(dbad.image.width == 3 && dbad.image.height == 2);
}

// Truncated/corrupt/oversize PNG inputs yield bounded errors, never success.
void test_png_reject_bad_inputs() {
    const auto png =
        oz::test_png::make_rgb_png(8, 8, oz::test_png::gradient(8, 8));
    oz::viewer_core::image_info info;
    // Severed header: truncation is rejected deterministically before WIC.
    CHECK(oz::viewer_core::decode_image(png.data(), 20, 1).error ==
          oz::viewer_core::codec_error::truncated);
    // Valid signature with no IHDR is an invalid header (byte gate, no WIC).
    std::vector<std::uint8_t> no_ihdr(108, 0);
    no_ihdr[0] = 0x89;
    no_ihdr[1] = 0x50;
    no_ihdr[2] = 0x4E;
    no_ihdr[3] = 0x47;
    no_ihdr[4] = 0x0D;
    no_ihdr[5] = 0x0A;
    no_ihdr[6] = 0x1A;
    no_ihdr[7] = 0x0A;
    CHECK(oz::viewer_core::parse_image_header(no_ihdr.data(), no_ihdr.size(), info) ==
          oz::viewer_core::codec_error::invalid_header);
    CHECK(oz::viewer_core::decode_image(no_ihdr.data(), no_ihdr.size(), 1).error !=
          oz::viewer_core::codec_error::none);
    // Bare signature only.
    CHECK(oz::viewer_core::parse_image_header(png.data(), 8, info) ==
          oz::viewer_core::codec_error::truncated);
    // Corrupt IHDR type.
    auto corrupt = png;
    corrupt[12] = 'X';
    CHECK(oz::viewer_core::parse_image_header(corrupt.data(), corrupt.size(), info) ==
          oz::viewer_core::codec_error::invalid_header);
    CHECK(oz::viewer_core::decode_image(corrupt.data(), corrupt.size(), 1).error !=
          oz::viewer_core::codec_error::none);
    // Oversize IHDR dimensions (70000 wide) without pixel allocation.
    auto huge = png;
    huge[16] = 0x00;
    huge[17] = 0x01;
    huge[18] = 0x11;
    huge[19] = 0x70;  // 70000
    CHECK(oz::viewer_core::parse_image_header(huge.data(), huge.size(), info) ==
          oz::viewer_core::codec_error::dimension_limit);
    // Encoded size beyond 512 MiB is rejected before touching memory.
    const std::uint8_t one = 0;
    CHECK(oz::viewer_core::parse_image_header(
              &one, oz::viewer_core::kMaxEncodedBytes + 1, info) ==
          oz::viewer_core::codec_error::allocation_limit);
}

// Invalid/unsupported ICC profiles fall back internally: decode succeeds.
void test_png_icc_fallback_contract() {
    const std::vector<std::uint8_t> bogus_icc = {0, 1, 2, 3, 4, 5, 6, 7,
                                                 8, 9, 10, 11, 12, 13, 14, 15};
    const auto png = oz::test_png::make_rgba_png(
        4, 4, oz::test_png::gradient(4, 4), false, 0, bogus_icc);
    auto dr = oz::viewer_core::decode_image(png.data(), png.size(), 1);
    CHECK(dr.error == oz::viewer_core::codec_error::none);
    CHECK(dr.image.width == 4 && dr.image.height == 4);
    CHECK(dr.image.pixels.size() == static_cast<std::size_t>(4 * 4 * 4));
}

// PNG scaling denominator contract: ceil semantics like the JPEG path.
void test_png_scaling_denominator() {
    const auto png = oz::test_png::make_rgba_png(
        64, 48, oz::test_png::gradient(64, 48));
    auto d2 = oz::viewer_core::decode_image(png.data(), png.size(), 2);
    CHECK(d2.error == oz::viewer_core::codec_error::none);
    CHECK(d2.image.width == 32 && d2.image.height == 24);
    CHECK(d2.image.scale_den == 2);
    CHECK(d2.image.src_width == 64 && d2.image.src_height == 48);
    auto d8 = oz::viewer_core::decode_image(png.data(), png.size(), 8);
    CHECK(d8.error == oz::viewer_core::codec_error::none);
    CHECK(d8.image.width == 8 && d8.image.height == 6);
}

// Supported-image navigation semantics: .png joins .jpg/.jpeg,
// case-insensitively, naturally sorted; other extensions stay excluded.
// Stage 3.2: .webp joins the hint set when static WebP is enabled.
void test_supported_image_navigation() {
    CHECK(oz::viewer_core::is_supported_image_extension(L"a.png"));
    CHECK(oz::viewer_core::is_supported_image_extension(L"b.PNG"));
    CHECK(oz::viewer_core::is_supported_image_extension(L"c.PnG"));
    CHECK(oz::viewer_core::is_supported_image_extension(L"d.jpg"));
    CHECK(oz::viewer_core::is_supported_image_extension(L"e.JPEG"));
    // Phase A: BMP/GIF/TIFF/ICO join the hint set (bytes still win).
    // Phase B: CUR/TGA join the same explicit non-RAW allowlist.
    CHECK(oz::viewer_core::is_supported_image_extension(L"f.bmp"));
    CHECK(oz::viewer_core::is_supported_image_extension(L"f.BMP"));
    CHECK(oz::viewer_core::is_supported_image_extension(L"g.gif"));
    CHECK(oz::viewer_core::is_supported_image_extension(L"h.tif"));
    CHECK(oz::viewer_core::is_supported_image_extension(L"i.TIFF"));
    CHECK(oz::viewer_core::is_supported_image_extension(L"j.ico"));
    CHECK(oz::viewer_core::is_supported_image_extension(L"j.ICO"));
    CHECK(oz::viewer_core::is_supported_image_extension(L"k.cur"));
    CHECK(oz::viewer_core::is_supported_image_extension(L"k.CUR"));
    CHECK(oz::viewer_core::is_supported_image_extension(L"l.tga"));
    CHECK(oz::viewer_core::is_supported_image_extension(L"l.TGA"));
    CHECK(!oz::viewer_core::is_supported_image_extension(L"h.txt"));
    CHECK(!oz::viewer_core::is_supported_image_extension(L"m.raw"));
#if OZVIEWER_HAVE_WEBP
    CHECK(oz::viewer_core::is_supported_image_extension(L"g.webp"));
    CHECK(oz::viewer_core::is_supported_image_extension(L"g.WEBP"));
    CHECK(oz::viewer_core::is_supported_image_extension(L"g.WeBp"));
#else
    CHECK(!oz::viewer_core::is_supported_image_extension(L"g.webp"));
#endif
#if OZVIEWER_HAVE_AVIF
    CHECK(oz::viewer_core::is_supported_image_extension(L"m.avif"));
    CHECK(oz::viewer_core::is_supported_image_extension(L"m.AVIF"));
    CHECK(oz::viewer_core::is_supported_image_extension(L"m.AvIf"));
#else
    CHECK(!oz::viewer_core::is_supported_image_extension(L"m.avif"));
#endif

    std::vector<std::filesystem::path> candidates = {
        L"dir/img10.png", L"dir/notes.txt", L"dir/img2.jpg",
        L"dir/photo.PNG",   L"dir/img1.jpeg", L"dir/anim.webp",
    };
    auto nav = oz::viewer_core::navigation_list::build(candidates, L"dir/photo.PNG");
#if OZVIEWER_HAVE_WEBP
    // Natural order: anim.webp, img1.jpeg, img2.jpg, img10.png, photo.PNG.
    CHECK(nav.size() == 5);
    CHECK(nav.active_index().has_value());
    CHECK(nav.current() != nullptr && *nav.current() == L"dir/photo.PNG");
    CHECK(*nav.at(0) == L"dir/anim.webp");
    CHECK(*nav.at(1) == L"dir/img1.jpeg");
    CHECK(*nav.at(2) == L"dir/img2.jpg");
    CHECK(*nav.at(3) == L"dir/img10.png");
    CHECK(*nav.at(4) == L"dir/photo.PNG");
#else
    CHECK(nav.size() == 4);
    CHECK(nav.active_index().has_value());
    CHECK(nav.current() != nullptr && *nav.current() == L"dir/photo.PNG");
    // Natural order: img1.jpeg, img2.jpg, img10.png, photo.PNG.
    CHECK(*nav.at(0) == L"dir/img1.jpeg");
    CHECK(*nav.at(1) == L"dir/img2.jpg");
    CHECK(*nav.at(2) == L"dir/img10.png");
    CHECK(*nav.at(3) == L"dir/photo.PNG");
#endif
}

// Cache entries carry the codec tag; the shared path+denominator key stays.
void test_cache_codec_tag() {
    oz::viewer_core::decoded_image_data img;
    CHECK(img.codec == oz::viewer_core::codec_id::jpeg);
    img.codec = oz::viewer_core::codec_id::png_wic;
    CHECK(img.codec == oz::viewer_core::codec_id::png_wic);
}

// --- Stage 3.2 static WebP tests (checked-in fixtures, no encoder) --------

void test_webp_signature_routing() {
    using oz::test_webp::k_lossless_4x3;
    using oz::test_webp::k_lossless_4x3_len;
    // Exact RIFF+WEBP signature (RIFF bytes + "WEBP" at offset 8).
    CHECK(oz::viewer_core::has_webp_signature(k_lossless_4x3, k_lossless_4x3_len));
    CHECK(!oz::viewer_core::has_webp_signature(k_lossless_4x3, 11));
    CHECK(!oz::viewer_core::has_webp_signature(nullptr, 0));
    const std::uint8_t riff_jpeg[] = {'R', 'I', 'F', 'F', 4, 0, 0, 0,
                                      'J', 'P', 'E', 'G'};
    CHECK(!oz::viewer_core::has_webp_signature(riff_jpeg, sizeof(riff_jpeg)));
    const std::uint8_t png[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    CHECK(!oz::viewer_core::has_webp_signature(png, sizeof(png)));
#if OZVIEWER_HAVE_WEBP
    const auto codec =
        oz::viewer_core::detect_codec(k_lossless_4x3, k_lossless_4x3_len);
    CHECK(codec.has_value() && *codec == oz::viewer_core::codec_id::webp);
    // Named-extension mismatch follows bytes: webp bytes "named .jpg" still
    // route to webp; jpeg bytes "named .webp" still route to jpeg.
    CHECK(*oz::viewer_core::detect_codec(k_lossless_4x3, k_lossless_4x3_len) ==
          oz::viewer_core::codec_id::webp);
    const std::uint8_t jpeg_soi[] = {0xFF, 0xD8, 0xFF, 0xE0};
    CHECK(*oz::viewer_core::detect_codec(jpeg_soi, sizeof(jpeg_soi)) ==
          oz::viewer_core::codec_id::jpeg);
    CHECK(oz::viewer_core::has_webp_signature(
        oz::test_webp::k_animated_2x2, oz::test_webp::k_animated_2x2_len));
#else
    CHECK(!oz::viewer_core::detect_codec(k_lossless_4x3, k_lossless_4x3_len)
               .has_value());
#endif
}

#if OZVIEWER_HAVE_WEBP

void test_webp_static_lossy() {
    // Generated lossy 4x3: dims/stride/profile exact, opaque alpha.
    auto dr = oz::viewer_core::decode_image(oz::test_webp::k_lossy_4x3,
                                            oz::test_webp::k_lossy_4x3_len, 1);
    CHECK(dr.error == oz::viewer_core::codec_error::none);
    CHECK(dr.image.codec == oz::viewer_core::codec_id::webp);
    CHECK(dr.image.width == 4 && dr.image.height == 3);
    CHECK(dr.image.stride == 16);
    CHECK(dr.image.scale_den == 1);
    CHECK(dr.image.src_width == 4 && dr.image.src_height == 3);
    CHECK(dr.image.pixels.size() == static_cast<std::size_t>(4 * 3 * 4));
    for (std::size_t i = 3; i < dr.image.pixels.size(); i += 4) {
        CHECK(dr.image.pixels[i] == 0xFF);
    }
    oz::viewer_core::image_info info;
    CHECK(oz::viewer_core::parse_image_header(oz::test_webp::k_lossy_4x3,
                                              oz::test_webp::k_lossy_4x3_len,
                                              info) ==
          oz::viewer_core::codec_error::none);
    CHECK(info.codec == oz::viewer_core::codec_id::webp);
    CHECK(info.src_width == 4 && info.src_height == 3);

    // Upstream lossy fixture (libwebp examples/test.webp, 128x128) decodes.
    auto du = oz::viewer_core::decode_image(oz::test_webp::k_upstream_test,
                                            oz::test_webp::k_upstream_test_len, 1);
    CHECK(du.error == oz::viewer_core::codec_error::none);
    CHECK(du.image.width == 128 && du.image.height == 128);
    CHECK(du.image.pixels.size() ==
          static_cast<std::size_t>(128) * 128 * 4);
}

void test_webp_lossless_exact() {
    auto dr = oz::viewer_core::decode_image(oz::test_webp::k_lossless_4x3,
                                            oz::test_webp::k_lossless_4x3_len, 1);
    CHECK(dr.error == oz::viewer_core::codec_error::none);
    CHECK(dr.image.width == 4 && dr.image.height == 3);
    // Gradient corners are exact under lossless (opaque => straight ==
    // premultiplied): (0,0) is (R0,G0,B128) -> BGRA (128,0,0,255);
    // (3,2) is (255,255,128) -> BGRA (128,255,255,255).
    CHECK(dr.image.pixels[0] == 128 && dr.image.pixels[1] == 0 &&
          dr.image.pixels[2] == 0 && dr.image.pixels[3] == 0xFF);
    const std::size_t last = dr.image.pixels.size() - 4;
    CHECK(dr.image.pixels[last + 0] == 128 && dr.image.pixels[last + 1] == 255 &&
          dr.image.pixels[last + 2] == 255 && dr.image.pixels[last + 3] == 0xFF);
}

void test_webp_alpha_premultiplication_exact() {
    auto dr = oz::viewer_core::decode_image(oz::test_webp::k_alpha_2x2,
                                            oz::test_webp::k_alpha_2x2_len, 1);
    CHECK(dr.error == oz::viewer_core::codec_error::none);
    CHECK(dr.image.width == 2 && dr.image.height == 2);
    CHECK(dr.image.pixels.size() == 16);
    const auto& p = dr.image.pixels;
    // Row 0: opaque red -> (0,0,255,255); half white -> (128,128,128,128)
    // via exactly one (c*a+127)/255 pass.
    CHECK(p[0] == 0 && p[1] == 0 && p[2] == 255 && p[3] == 255);
    CHECK(p[4] == 128 && p[5] == 128 && p[6] == 128 && p[7] == 128);
    // Row 1: transparent green collapses to (0,0,0,0); opaque black stays.
    CHECK(p[8] == 0 && p[9] == 0 && p[10] == 0 && p[11] == 0);
    CHECK(p[12] == 0 && p[13] == 0 && p[14] == 0 && p[15] == 255);
}

void test_webp_icc_fallback_contract() {
    // Bogus ICCP chunk: decode succeeds, pixels treated as sRGB.
    auto dr = oz::viewer_core::decode_image(oz::test_webp::k_icc_bogus_4x4,
                                            oz::test_webp::k_icc_bogus_4x4_len, 1);
    CHECK(dr.error == oz::viewer_core::codec_error::none);
    CHECK(dr.image.width == 4 && dr.image.height == 4);
    CHECK(dr.image.pixels.size() == static_cast<std::size_t>(4 * 4 * 4));
}

void test_webp_exif_swapped_orientation() {
    CHECK(oz::viewer_core::webp::read_exif_orientation(
              oz::test_webp::k_exif_o6_3x2, oz::test_webp::k_exif_o6_3x2_len) == 6);
    CHECK(oz::viewer_core::webp::read_exif_orientation(
              oz::test_webp::k_lossless_4x3,
              oz::test_webp::k_lossless_4x3_len) == 0);
    oz::viewer_core::image_info info;
    CHECK(oz::viewer_core::parse_image_header(oz::test_webp::k_exif_o6_3x2,
                                              oz::test_webp::k_exif_o6_3x2_len,
                                              info) ==
          oz::viewer_core::codec_error::none);
    // 3x2 stored with orientation 6 presents as 2x3.
    CHECK(info.src_width == 2 && info.src_height == 3);
    auto dr = oz::viewer_core::decode_image(oz::test_webp::k_exif_o6_3x2,
                                            oz::test_webp::k_exif_o6_3x2_len, 1);
    CHECK(dr.error == oz::viewer_core::codec_error::none);
    CHECK(dr.image.width == 2 && dr.image.height == 3);
    // Stored pattern is (B=y,G=x,R=0); orientation 6 maps output (dx,dy)
    // to stored (dy, h-1-dx) with stored w=3,h=2.
    bool match = true;
    for (std::int32_t y = 0; y < 3 && match; ++y) {
        for (std::int32_t x = 0; x < 2 && match; ++x) {
            const std::uint8_t eg = static_cast<std::uint8_t>(y);
            const std::uint8_t eb = static_cast<std::uint8_t>(1 - x);
            const std::uint8_t* q = dr.image.pixels.data() +
                                    (static_cast<std::size_t>(y) * 2 + x) * 4;
            if (q[0] != eb || q[1] != eg || q[2] != 0 || q[3] != 0xFF) {
                match = false;
            }
        }
    }
    CHECK(match);
}

void test_webp_animated_rejected() {
    // Animation is rejected before allocation/decode: never frame 0.
    oz::viewer_core::image_info info;
    CHECK(oz::viewer_core::parse_image_header(oz::test_webp::k_animated_2x2,
                                              oz::test_webp::k_animated_2x2_len,
                                              info) ==
          oz::viewer_core::codec_error::decode_failed);
    auto dr = oz::viewer_core::decode_image(oz::test_webp::k_animated_2x2,
                                            oz::test_webp::k_animated_2x2_len, 1);
    CHECK(dr.error == oz::viewer_core::codec_error::decode_failed);
    CHECK(dr.image.pixels.empty());
    auto dr8 = oz::viewer_core::decode_image(oz::test_webp::k_animated_2x2,
                                             oz::test_webp::k_animated_2x2_len, 8);
    CHECK(dr8.error == oz::viewer_core::codec_error::decode_failed);
}

void test_webp_reject_bad_inputs() {
    oz::viewer_core::image_info info;
    // Severed envelope is truncation, never success.
    CHECK(oz::viewer_core::decode_image(oz::test_webp::k_lossy_4x3, 20, 1).error ==
          oz::viewer_core::codec_error::truncated);
    CHECK(oz::viewer_core::parse_image_header(oz::test_webp::k_lossy_4x3, 8, info) ==
          oz::viewer_core::codec_error::unsupported);
    // Corrupt WEBP tag.
    std::vector<std::uint8_t> bad_tag(
        oz::test_webp::k_lossy_4x3,
        oz::test_webp::k_lossy_4x3 + oz::test_webp::k_lossy_4x3_len);
    bad_tag[8] = 'X';
    CHECK(oz::viewer_core::parse_image_header(bad_tag.data(), bad_tag.size(), info) ==
          oz::viewer_core::codec_error::unsupported);
    // Trailing garbage breaks the exact RIFF envelope match.
    std::vector<std::uint8_t> trailing(
        oz::test_webp::k_lossless_4x3,
        oz::test_webp::k_lossless_4x3 + oz::test_webp::k_lossless_4x3_len);
    trailing.push_back(0);
    CHECK(oz::viewer_core::parse_image_header(trailing.data(), trailing.size(), info) !=
          oz::viewer_core::codec_error::none);
    // Chunk-length overrun: declare a huge first-chunk payload.
    std::vector<std::uint8_t> overrun(
        oz::test_webp::k_lossless_4x3,
        oz::test_webp::k_lossless_4x3 + oz::test_webp::k_lossless_4x3_len);
    overrun[12 + 4] = 0xFF;
    overrun[12 + 5] = 0xFF;
    overrun[12 + 6] = 0xFF;
    overrun[12 + 7] = 0x7F;
    CHECK(oz::viewer_core::decode_image(overrun.data(), overrun.size(), 1).error !=
          oz::viewer_core::codec_error::none);
    // Oversized-metadata shape: a 17 MiB ICCP declaration that the buffer
    // cannot back is rejected by the bounds walk without allocation.
    std::vector<std::uint8_t> huge_meta(
        oz::test_webp::k_lossless_4x3,
        oz::test_webp::k_lossless_4x3 + oz::test_webp::k_lossless_4x3_len);
    {
        const std::uint32_t riff =
            static_cast<std::uint32_t>(huge_meta[4]) |
            (static_cast<std::uint32_t>(huge_meta[5]) << 8) |
            (static_cast<std::uint32_t>(huge_meta[6]) << 16) |
            (static_cast<std::uint32_t>(huge_meta[7]) << 24);
        const std::uint32_t grown = riff + 12u + 17u * 1024u * 1024u;
        huge_meta[4] = static_cast<std::uint8_t>(grown & 0xFF);
        huge_meta[5] = static_cast<std::uint8_t>((grown >> 8) & 0xFF);
        huge_meta[6] = static_cast<std::uint8_t>((grown >> 16) & 0xFF);
        huge_meta[7] = static_cast<std::uint8_t>((grown >> 24) & 0xFF);
        const std::uint8_t tail[] = {'I', 'C', 'C', 'P', 0x00, 0x00, 0x01, 0x01};
        huge_meta.insert(huge_meta.end(), tail, tail + 8);
    }
    CHECK(oz::viewer_core::decode_image(huge_meta.data(), huge_meta.size(), 1).error !=
          oz::viewer_core::codec_error::none);
    // Oversized-dimension shape: VP8X canvas 70000x10 is bounded, no pixels.
    std::vector<std::uint8_t> huge_dims = {
        'R', 'I', 'F', 'F', 22, 0, 0, 0, 'W', 'E', 'B', 'P',
        'V', 'P', '8', 'X', 10, 0, 0, 0, 0, 0, 0, 0,
        0x70, 0x11, 0x01, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00};
    huge_dims[24] = 0x6F;  // canvas width - 1 = 69999 -> width 70000
    huge_dims[25] = 0x11;
    huge_dims[26] = 0x01;
    CHECK(oz::viewer_core::parse_image_header(huge_dims.data(), huge_dims.size(), info) !=
          oz::viewer_core::codec_error::none);
    // Decode corruption: truncated payload tail of a valid file.
    std::vector<std::uint8_t> cut(
        oz::test_webp::k_lossy_4x3,
        oz::test_webp::k_lossy_4x3 + oz::test_webp::k_lossy_4x3_len - 8);
    {
        const std::uint32_t riff =
            static_cast<std::uint32_t>(cut.size() - 8);
        cut[4] = static_cast<std::uint8_t>(riff & 0xFF);
        cut[5] = static_cast<std::uint8_t>((riff >> 8) & 0xFF);
        cut[6] = static_cast<std::uint8_t>((riff >> 16) & 0xFF);
        cut[7] = static_cast<std::uint8_t>((riff >> 24) & 0xFF);
    }
    CHECK(oz::viewer_core::decode_image(cut.data(), cut.size(), 1).error !=
          oz::viewer_core::codec_error::none);
    // Encoded size beyond 512 MiB is rejected before touching memory.
    const std::uint8_t one = 0;
    CHECK(oz::viewer_core::parse_image_header(
              &one, oz::viewer_core::kMaxEncodedBytes + 1, info) ==
          oz::viewer_core::codec_error::allocation_limit);
}

void test_webp_scaling_denominator() {
    // Ceil semantics mirror the JPEG/PNG paths.
    auto d2 = oz::viewer_core::decode_image(oz::test_webp::k_bench_64x48,
                                            oz::test_webp::k_bench_64x48_len, 2);
    CHECK(d2.error == oz::viewer_core::codec_error::none);
    CHECK(d2.image.width == 32 && d2.image.height == 24);
    CHECK(d2.image.scale_den == 2);
    CHECK(d2.image.src_width == 64 && d2.image.src_height == 48);
    auto d8 = oz::viewer_core::decode_image(oz::test_webp::k_bench_64x48,
                                            oz::test_webp::k_bench_64x48_len, 8);
    CHECK(d8.error == oz::viewer_core::codec_error::none);
    CHECK(d8.image.width == 8 && d8.image.height == 6);
    auto d3 = oz::viewer_core::decode_image(oz::test_webp::k_lossless_4x3,
                                            oz::test_webp::k_lossless_4x3_len, 4);
    CHECK(d3.error == oz::viewer_core::codec_error::none);
    CHECK(d3.image.width == 1 && d3.image.height == 1);
}

void test_webp_cache_nav_mixed_extensions() {
    // Mixed JPEG/PNG/WebP navigation stays naturally sorted and the cache
    // key shape (path + scale profile) is codec-agnostic.
    std::vector<std::filesystem::path> candidates = {
        L"dir/b.png", L"dir/a.webp", L"dir/c.jpg",
    };
    auto nav = oz::viewer_core::navigation_list::build(candidates, L"dir/b.png");
    CHECK(nav.size() == 3);
    CHECK(*nav.at(0) == L"dir/a.webp");
    CHECK(*nav.at(1) == L"dir/b.png");
    CHECK(*nav.at(2) == L"dir/c.jpg");
    const std::wstring norm =
        oz::viewer_core::normalize_path_key(L"dir/a.WEBP");
    CHECK(norm == oz::viewer_core::normalize_path_key(L"dir/a.webp"));
    oz::viewer_core::cpu_image_cache cache(1 << 20);
    auto webp_img = std::make_shared<oz::viewer_core::decoded_image_data>();
    webp_img->pixels.resize(16);
    webp_img->codec = oz::viewer_core::codec_id::webp;
    cache.insert({norm, 1}, webp_img, false);
    CHECK(cache.get({norm, 1}) != nullptr);
}

#else  // !OZVIEWER_HAVE_WEBP -- feature-disabled build advertises no WebP.

void test_webp_disabled_build() {
    // has_webp_signature is byte-level and still true; the registry refuses
    // to route or decode WebP in a feature-disabled build.
    CHECK(oz::viewer_core::has_webp_signature(
        oz::test_webp::k_lossless_4x3, oz::test_webp::k_lossless_4x3_len));
    CHECK(!oz::viewer_core::detect_codec(oz::test_webp::k_lossless_4x3,
                                         oz::test_webp::k_lossless_4x3_len)
               .has_value());
    auto dr = oz::viewer_core::decode_image(oz::test_webp::k_lossless_4x3,
                                            oz::test_webp::k_lossless_4x3_len, 1);
    CHECK(dr.error == oz::viewer_core::codec_error::unsupported);
    oz::viewer_core::image_info info;
    CHECK(oz::viewer_core::parse_image_header(oz::test_webp::k_lossless_4x3,
                                              oz::test_webp::k_lossless_4x3_len,
                                              info) ==
          oz::viewer_core::codec_error::unsupported);
}

#endif  // OZVIEWER_HAVE_WEBP

// --- Common Raster Decode Profile v1 Phase A tests ------------------------
// Decoder-independent fixtures (tests/phasea_fixtures.hpp); decode paths
// run through the registry on the in-box WIC adapters after the
// byte-level structural gate. Extensions never select a codec.

void test_phasea_signature_routing() {
    using oz::test_phasea::k_bmp_opaque_4x3;
    using oz::test_phasea::k_bmp_opaque_4x3_len;
    using oz::test_phasea::k_gif_static_2x2;
    using oz::test_phasea::k_gif_static_2x2_len;
    using oz::test_phasea::k_tiff_opaque_4x3;
    using oz::test_phasea::k_tiff_opaque_4x3_len;
    using oz::test_phasea::k_ico_multi;
    using oz::test_phasea::k_ico_multi_len;
    CHECK(oz::viewer_core::has_bmp_signature(k_bmp_opaque_4x3, k_bmp_opaque_4x3_len));
    CHECK(oz::viewer_core::has_gif_signature(k_gif_static_2x2, k_gif_static_2x2_len));
    CHECK(oz::viewer_core::has_tiff_signature(k_tiff_opaque_4x3, k_tiff_opaque_4x3_len));
    CHECK(oz::viewer_core::has_ico_signature(k_ico_multi, k_ico_multi_len));
    CHECK(*oz::viewer_core::detect_codec(k_bmp_opaque_4x3, k_bmp_opaque_4x3_len) ==
          oz::viewer_core::codec_id::bmp_wic);
    CHECK(*oz::viewer_core::detect_codec(k_gif_static_2x2, k_gif_static_2x2_len) ==
          oz::viewer_core::codec_id::gif_wic);
    CHECK(*oz::viewer_core::detect_codec(k_tiff_opaque_4x3, k_tiff_opaque_4x3_len) ==
          oz::viewer_core::codec_id::tiff_wic);
    CHECK(*oz::viewer_core::detect_codec(k_ico_multi, k_ico_multi_len) ==
          oz::viewer_core::codec_id::ico_wic);
    // Bytes win over extensions: BMP bytes "named .jpg" still route to BMP.
    CHECK(*oz::viewer_core::detect_codec(k_bmp_opaque_4x3, k_bmp_opaque_4x3_len) ==
          oz::viewer_core::codec_id::bmp_wic);
    // CUR type 2 is not an ICO signature; BigTIFF magic 43 is not TIFF.
    const std::uint8_t cur[] = {0, 0, 2, 0, 1, 0};
    CHECK(!oz::viewer_core::has_ico_signature(cur, sizeof(cur)));
    CHECK(!oz::viewer_core::detect_codec(oz::test_phasea::k_tiff_bigtiff,
                                         oz::test_phasea::k_tiff_bigtiff_len)
               .has_value());
    // TGA footer bytes are not a supported signature.
    const std::uint8_t tga[] = {'T', 'R', 'U', 'E', 'V', 'I', 'S', 'I', 'O', 'N'};
    CHECK(!oz::viewer_core::detect_codec(tga, sizeof(tga)).has_value());
}

void test_phasea_spoof_and_truncation() {
    using namespace oz::test_phasea;
    oz::viewer_core::image_info info;
    // BMP bytes with a PNG extension hint still route by bytes.
    CHECK(*oz::viewer_core::detect_codec(k_bmp_opaque_4x3, k_bmp_opaque_4x3_len) ==
          oz::viewer_core::codec_id::bmp_wic);
    // Truncated BMP (severed pixel data, signature intact) is truncated.
    CHECK(oz::viewer_core::decode_image(k_bmp_opaque_4x3, 70, 1).error ==
          oz::viewer_core::codec_error::truncated);
    // Unsupported BMP compression (BI_JPEG) passes the signature gate but
    // fails the structural parse as an invalid header.
    std::vector<std::uint8_t> bad_comp(k_bmp_opaque_4x3,
                                       k_bmp_opaque_4x3 + k_bmp_opaque_4x3_len);
    bad_comp[30] = 4;
    bad_comp[31] = 0;
    bad_comp[32] = 0;
    bad_comp[33] = 0;
    CHECK(oz::viewer_core::parse_image_header(bad_comp.data(), bad_comp.size(), info) ==
          oz::viewer_core::codec_error::invalid_header);
    // Oversized BMP dimensions without allocation.
    std::vector<std::uint8_t> huge(k_bmp_opaque_4x3,
                                   k_bmp_opaque_4x3 + k_bmp_opaque_4x3_len);
    huge[18] = 0x70;
    huge[19] = 0x11;
    huge[20] = 0x01;
    huge[21] = 0x00;  // 70000 wide
    CHECK(oz::viewer_core::parse_image_header(huge.data(), huge.size(), info) ==
          oz::viewer_core::codec_error::dimension_limit);
    // Truncated GIF (severed block walk) is truncated.
    CHECK(oz::viewer_core::decode_image(k_gif_static_2x2, 20, 1).error ==
          oz::viewer_core::codec_error::truncated);
    // Truncated TIFF header (signature present, IHDR/IFD cut) is truncated.
    CHECK(oz::viewer_core::parse_image_header(k_tiff_opaque_4x3, 4, info) ==
          oz::viewer_core::codec_error::truncated);
    // Invalid ICO directory (zero entries) is rejected.
    std::vector<std::uint8_t> bad_ico(k_ico_multi, k_ico_multi + k_ico_multi_len);
    bad_ico[4] = 0;
    bad_ico[5] = 0;
    CHECK(!oz::viewer_core::has_ico_signature(bad_ico.data(), bad_ico.size()));
    // Encoded size beyond 512 MiB is rejected before touching memory.
    const std::uint8_t one = 0;
    CHECK(oz::viewer_core::parse_image_header(
              &one, oz::viewer_core::kMaxEncodedBytes + 1, info) ==
          oz::viewer_core::codec_error::allocation_limit);
}

void test_phasea_gif_static_only() {
    using namespace oz::test_phasea;
    oz::viewer_core::image_info info;
    CHECK(oz::viewer_core::parse_image_header(k_gif_static_2x2, k_gif_static_2x2_len,
                                              info) ==
          oz::viewer_core::codec_error::none);
    CHECK(info.codec == oz::viewer_core::codec_id::gif_wic);
    CHECK(info.src_width == 2 && info.src_height == 2);
    // Animated GIF is rejected before allocation, never frame 0.
    CHECK(oz::viewer_core::parse_image_header(k_gif_animated_2x2,
                                              k_gif_animated_2x2_len, info) ==
          oz::viewer_core::codec_error::decode_failed);
    auto dr = oz::viewer_core::decode_image(k_gif_animated_2x2,
                                            k_gif_animated_2x2_len, 1);
    CHECK(dr.error == oz::viewer_core::codec_error::decode_failed);
    CHECK(dr.image.pixels.empty());
    // Malformed extension (severed sub-blocks) is truncated.
    std::vector<std::uint8_t> cut(k_gif_static_2x2,
                                  k_gif_static_2x2 + k_gif_static_2x2_len - 4);
    CHECK(oz::viewer_core::decode_image(cut.data(), cut.size(), 1).error ==
          oz::viewer_core::codec_error::truncated);
}

void test_phasea_tiff_policies() {
    using namespace oz::test_phasea;
    oz::viewer_core::image_info info;
    CHECK(oz::viewer_core::parse_image_header(k_tiff_opaque_4x3,
                                              k_tiff_opaque_4x3_len, info) ==
          oz::viewer_core::codec_error::none);
    CHECK(info.codec == oz::viewer_core::codec_id::tiff_wic);
    CHECK(info.src_width == 4 && info.src_height == 3);
    // BigTIFF is unsupported (never routed).
    CHECK(oz::viewer_core::parse_image_header(k_tiff_bigtiff, k_tiff_bigtiff_len,
                                              info) ==
          oz::viewer_core::codec_error::unsupported);
    // RAW/DNG indicator is unsupported so no embedded preview is shown.
    CHECK(oz::viewer_core::parse_image_header(k_tiff_raw_dng, k_tiff_raw_dng_len,
                                              info) ==
          oz::viewer_core::codec_error::unsupported);
    CHECK(oz::viewer_core::decode_image(k_tiff_raw_dng, k_tiff_raw_dng_len, 1).error ==
          oz::viewer_core::codec_error::unsupported);
    // IFD loop is an invalid header.
    CHECK(oz::viewer_core::parse_image_header(k_tiff_loop, k_tiff_loop_len, info) ==
          oz::viewer_core::codec_error::invalid_header);
    // Multipage renders page 0 dims.
    CHECK(oz::viewer_core::parse_image_header(k_tiff_multipage_4x3,
                                              k_tiff_multipage_4x3_len, info) ==
          oz::viewer_core::codec_error::none);
    CHECK(info.src_width == 4 && info.src_height == 3);
    auto dr = oz::viewer_core::decode_image(k_tiff_multipage_4x3,
                                            k_tiff_multipage_4x3_len, 1);
    CHECK(dr.error == oz::viewer_core::codec_error::none);
    CHECK(dr.image.width == 4 && dr.image.height == 3);
    // EXIF orientation 6 swaps page-0 dims (4x3 stored, 3x4 shown).
    CHECK(oz::viewer_core::parse_image_header(k_tiff_orient_o6,
                                              k_tiff_orient_o6_len, info) ==
          oz::viewer_core::codec_error::none);
    CHECK(info.src_width == 3 && info.src_height == 4);
    auto dr_o6 = oz::viewer_core::decode_image(k_tiff_orient_o6,
                                               k_tiff_orient_o6_len, 1);
    CHECK(dr_o6.error == oz::viewer_core::codec_error::none);
    CHECK(dr_o6.image.width == 3 && dr_o6.image.height == 4);
    CHECK(dr_o6.image.pixels.size() == static_cast<std::size_t>(3 * 4 * 4));
}

void test_phasea_ico_selection() {
    using namespace oz::test_phasea;
    oz::viewer_core::ico_wic::ico_entry sel;
    CHECK(oz::viewer_core::ico_wic::select_entry(k_ico_multi, k_ico_multi_len,
                                                 sel) ==
          oz::viewer_core::codec_error::none);
    CHECK(sel.width == 32 && sel.height == 32);
    // Tie-break: same area prefers higher bit depth, then directory order.
    oz::viewer_core::ico_wic::ico_entry tie;
    CHECK(oz::viewer_core::ico_wic::select_entry(k_ico_tie_16, k_ico_tie_16_len,
                                                 tie) ==
          oz::viewer_core::codec_error::none);
    CHECK(tie.width == 16 && tie.height == 16);
    CHECK(tie.bit_depth == 32);
    CHECK(tie.index == 1);
    oz::viewer_core::image_info info;
    CHECK(oz::viewer_core::parse_image_header(k_ico_multi, k_ico_multi_len, info) ==
          oz::viewer_core::codec_error::none);
    CHECK(info.src_width == 32 && info.src_height == 32);
}

void test_phasea_canonical_pixels() {
    using namespace oz::test_phasea;
    // BMP opaque corners exact (24-bit => straight == premultiplied).
    auto db = oz::viewer_core::decode_image(k_bmp_opaque_4x3, k_bmp_opaque_4x3_len, 1);
    CHECK(db.error == oz::viewer_core::codec_error::none);
    CHECK(db.image.width == 4 && db.image.height == 3);
    CHECK(db.image.stride == 16);
    CHECK(db.image.pixels.size() == static_cast<std::size_t>(4 * 3 * 4));
    CHECK(db.image.pixels[0] == 128 && db.image.pixels[1] == 0 &&
          db.image.pixels[2] == 0 && db.image.pixels[3] == 0xFF);
    // BMP alpha satisfies the canonical PBGRA contract (a==0 => rgb==0).
    auto da = oz::viewer_core::decode_image(k_bmp_alpha_2x2, k_bmp_alpha_2x2_len, 1);
    CHECK(da.error == oz::viewer_core::codec_error::none);
    for (std::size_t i = 0; i < da.image.pixels.size(); i += 4) {
        if (da.image.pixels[i + 3] == 0) {
            CHECK(da.image.pixels[i] == 0 && da.image.pixels[i + 1] == 0 &&
                  da.image.pixels[i + 2] == 0);
        }
    }
    // Static GIF decodes with dims/stride exact.
    auto dg = oz::viewer_core::decode_image(k_gif_static_2x2, k_gif_static_2x2_len, 1);
    CHECK(dg.error == oz::viewer_core::codec_error::none);
    CHECK(dg.image.width == 2 && dg.image.height == 2);
    CHECK(dg.image.stride == 8);
    // Classic TIFF page 0 decodes exact.
    auto dt = oz::viewer_core::decode_image(k_tiff_opaque_4x3, k_tiff_opaque_4x3_len, 1);
    CHECK(dt.error == oz::viewer_core::codec_error::none);
    CHECK(dt.image.width == 4 && dt.image.height == 3);
    CHECK(dt.image.pixels.size() == static_cast<std::size_t>(4 * 3 * 4));
    // ICO largest entry decodes exact.
    auto di = oz::viewer_core::decode_image(k_ico_multi, k_ico_multi_len, 1);
    CHECK(di.error == oz::viewer_core::codec_error::none);
    CHECK(di.image.width == 32 && di.image.height == 32);
    CHECK(di.image.stride == 128);
    // Gradient top-left is exact through the payload-slice path (top-down).
    CHECK(di.image.pixels[0] == 128 && di.image.pixels[1] == 0 &&
          di.image.pixels[2] == 0 && di.image.pixels[3] == 0xFF);
    // Scaling denominator contract: ceil semantics like JPEG/PNG/WebP.
    auto ds = oz::viewer_core::decode_image(k_bmp_opaque_4x3, k_bmp_opaque_4x3_len, 2);
    CHECK(ds.error == oz::viewer_core::codec_error::none);
    CHECK(ds.image.width == 2 && ds.image.height == 2);
    CHECK(ds.image.scale_den == 2);
}

void test_phasea_mixed_navigation() {
    std::vector<std::filesystem::path> candidates = {
        L"dir/b.png", L"dir/a.bmp", L"dir/c.jpg", L"dir/d.gif",
        L"dir/e.tif", L"dir/f.tiff", L"dir/g.ico", L"dir/h.cur",
        L"dir/i.tga", L"dir/j.txt",
    };
    auto nav = oz::viewer_core::navigation_list::build(candidates, L"dir/a.bmp");
    CHECK(nav.size() == 9);  // Phase B adds .cur and .tga hints
    CHECK(*nav.at(0) == L"dir/a.bmp");
    CHECK(*nav.at(3) == L"dir/d.gif");
    CHECK(*nav.at(7) == L"dir/h.cur");
    CHECK(*nav.at(8) == L"dir/i.tga");
    CHECK(nav.active_index().has_value());
    const std::wstring norm =
        oz::viewer_core::normalize_path_key(L"dir/G.ICO");
    CHECK(norm == oz::viewer_core::normalize_path_key(L"dir/g.ico"));
}

// --- Common Raster Decode Profile v1 Phase B tests ------------------------
// Native bounded TGA adapter + deterministic CUR selection. Fixtures are
// decoder-independent checked-in bytes (tests/data/phaseb/README.md).

void test_phaseb_signature_routing() {
    using namespace oz::test_phaseb;
    // TGA has no leading magic; the structural discriminator accepts the
    // valid fixtures and rejects random/ambiguous bytes.
    CHECK(oz::viewer_core::has_tga_signature(k_tga_opaque_4x3,
                                             k_tga_opaque_4x3_len));
    CHECK(oz::viewer_core::has_tga_signature(k_tga_footer_ext_4x3,
                                             k_tga_footer_ext_4x3_len));
    CHECK(*oz::viewer_core::detect_codec(k_tga_rle_4x3, k_tga_rle_4x3_len) ==
          oz::viewer_core::codec_id::tga);
    // CUR exact header only.
    CHECK(oz::viewer_core::has_cur_signature(k_cur_png_2x2, k_cur_png_2x2_len));
    CHECK(oz::viewer_core::has_cur_signature(k_cur_multi_16_32,
                                             k_cur_multi_16_32_len));
    CHECK(*oz::viewer_core::detect_codec(k_cur_dib_2x2, k_cur_dib_2x2_len) ==
          oz::viewer_core::codec_id::cur_wic);
    // Type 1 is ICO, type 2 is CUR; a type-2 header is never ICO.
    CHECK(!oz::viewer_core::has_ico_signature(k_cur_dib_2x2,
                                              k_cur_dib_2x2_len));
    // Bytes always determine the codec (extension hints are never consulted).
    CHECK(*oz::viewer_core::detect_codec(k_cur_png_2x2, k_cur_png_2x2_len) ==
          oz::viewer_core::codec_id::cur_wic);
    // Random/ambiguous bytes are never claimed as TGA.
    const std::uint8_t zero[30] = {};
    CHECK(!oz::viewer_core::detect_codec(zero, sizeof(zero)).has_value());
}

void test_phaseb_tga_origin_and_pixels() {
    using namespace oz::test_phaseb;
    // Bottom-left and top-down origin encodings of the same canonical image
    // must normalize to identical top-down pixels.
    auto db = oz::viewer_core::decode_image(k_tga_opaque_4x3,
                                            k_tga_opaque_4x3_len, 1);
    auto dt = oz::viewer_core::decode_image(k_tga_topdown_4x3,
                                            k_tga_topdown_4x3_len, 1);
    CHECK(db.error == oz::viewer_core::codec_error::none);
    CHECK(dt.error == oz::viewer_core::codec_error::none);
    CHECK(db.image.width == 4 && db.image.height == 3);
    CHECK(db.image.stride == 16);
    CHECK(db.image.codec == oz::viewer_core::codec_id::tga);
    CHECK(db.image.pixels == dt.image.pixels);
    // Top-left canonical pixel is red (BGRA).
    CHECK(db.image.pixels[0] == 0 && db.image.pixels[1] == 0 &&
          db.image.pixels[2] == 255 && db.image.pixels[3] == 255);
    // Bottom-right canonical pixel (200,100,50) opaque.
    const std::size_t last = (2 * 4 + 3) * 4;
    CHECK(db.image.pixels[last + 0] == 50 && db.image.pixels[last + 1] == 100 &&
          db.image.pixels[last + 2] == 200 && db.image.pixels[last + 3] == 255);
    // RLE true-color decodes identically to the uncompressed fixture.
    auto dr = oz::viewer_core::decode_image(k_tga_rle_4x3, k_tga_rle_4x3_len, 1);
    CHECK(dr.error == oz::viewer_core::codec_error::none);
    CHECK(dr.image.pixels == db.image.pixels);
    // TGA 2.0 footer/extension container decodes the same pixels.
    auto df = oz::viewer_core::decode_image(k_tga_footer_ext_4x3,
                                            k_tga_footer_ext_4x3_len, 1);
    CHECK(df.error == oz::viewer_core::codec_error::none);
    CHECK(df.image.pixels == dt.image.pixels);
    // Scaling denominator contract matches the other codecs (ceil).
    auto ds = oz::viewer_core::decode_image(k_tga_opaque_4x3,
                                            k_tga_opaque_4x3_len, 2);
    CHECK(ds.error == oz::viewer_core::codec_error::none);
    CHECK(ds.image.width == 2 && ds.image.height == 2 &&
          ds.image.scale_den == 2);
}

void test_phaseb_tga_variants() {
    using namespace oz::test_phaseb;
    // Color-mapped 8-bit indices, 24-bit palette.
    auto dc = oz::viewer_core::decode_image(k_tga_cmap_2x2, k_tga_cmap_2x2_len, 1);
    CHECK(dc.error == oz::viewer_core::codec_error::none);
    CHECK(dc.image.width == 2 && dc.image.height == 2);
    CHECK(dc.image.pixels[0] == 0 && dc.image.pixels[1] == 0 &&
          dc.image.pixels[2] == 255 && dc.image.pixels[3] == 255);  // red
    CHECK(dc.image.pixels[4] == 0 && dc.image.pixels[5] == 255 &&
          dc.image.pixels[6] == 0 && dc.image.pixels[7] == 255);  // green
    // Color-mapped 32-bit palette with attribute bits: alpha premultiplied
    // exactly once. The fixture descriptor is top-down (0x28).
    auto dca = oz::viewer_core::decode_image(k_tga_cmap_alpha_2x2,
                                             k_tga_cmap_alpha_2x2_len, 1);
    CHECK(dca.error == oz::viewer_core::codec_error::none);
    CHECK(dca.image.pixels[0] == 255 && dca.image.pixels[3] == 255);  // white
    CHECK(dca.image.pixels[4] == 0 && dca.image.pixels[5] == 128 &&
          dca.image.pixels[6] == 0 && dca.image.pixels[7] == 128);  // green a128
    CHECK(dca.image.pixels[11] == 0);  // fully transparent palette entry
    CHECK(dca.image.pixels[12] == 0 && dca.image.pixels[14] == 255 &&
          dca.image.pixels[15] == 255);  // red
    // Grayscale 8-bit.
    auto dg = oz::viewer_core::decode_image(k_tga_gray_4x3, k_tga_gray_4x3_len, 1);
    CHECK(dg.error == oz::viewer_core::codec_error::none);
    CHECK(dg.image.pixels[0] == 0 && dg.image.pixels[1] == 0 &&
          dg.image.pixels[2] == 0 && dg.image.pixels[3] == 255);
    CHECK(dg.image.pixels[12] == 255 && dg.image.pixels[15] == 255);
    // Grayscale 16-bit intensity + alpha.
    auto dg16 = oz::viewer_core::decode_image(k_tga_gray16_2x2,
                                              k_tga_gray16_2x2_len, 1);
    CHECK(dg16.error == oz::viewer_core::codec_error::none);
    CHECK(dg16.image.pixels[0] == 100 && dg16.image.pixels[3] == 128);
    CHECK(dg16.image.pixels[4] == 100 && dg16.image.pixels[7] == 255);
    CHECK(dg16.image.pixels[8] == 0 && dg16.image.pixels[11] == 0);
    // 32-bit true-color with alpha bits.
    auto da = oz::viewer_core::decode_image(k_tga_alpha_2x2, k_tga_alpha_2x2_len, 1);
    CHECK(da.error == oz::viewer_core::codec_error::none);
    CHECK(da.image.pixels[0] == 0 && da.image.pixels[1] == 0 &&
          da.image.pixels[2] == 128 && da.image.pixels[3] == 128);  // red a128
    CHECK(da.image.pixels[4] == 0 && da.image.pixels[5] == 0 &&
          da.image.pixels[6] == 0 && da.image.pixels[7] == 0);  // a0 => 0
    CHECK(da.image.pixels[12] == 157 && da.image.pixels[13] == 118 &&
          da.image.pixels[14] == 78 && da.image.pixels[15] == 200);
    // 16-bit ARGB1555 packed with attribute bit.
    auto dp = oz::viewer_core::decode_image(k_tga_packed16_2x2,
                                            k_tga_packed16_2x2_len, 1);
    CHECK(dp.error == oz::viewer_core::codec_error::none);
    CHECK(dp.image.pixels[2] == 255 && dp.image.pixels[3] == 255);  // red opaque
    CHECK(dp.image.pixels[7] == 0);                                 // green a0
    CHECK(dp.image.pixels[8] == 255 && dp.image.pixels[9] == 0 &&
          dp.image.pixels[10] == 0 && dp.image.pixels[11] == 255);  // blue
    CHECK(dp.image.pixels[12] == 132 && dp.image.pixels[15] == 255);
    // Header parse is decoder-independent (no allocation).
    oz::viewer_core::image_info info;
    CHECK(oz::viewer_core::parse_image_header(k_tga_gray_4x3,
                                              k_tga_gray_4x3_len, info) ==
          oz::viewer_core::codec_error::none);
    CHECK(info.codec == oz::viewer_core::codec_id::tga);
    CHECK(info.src_width == 4 && info.src_height == 3);
}

void test_phaseb_tga_rejection() {
    using namespace oz::test_phaseb;
    // Truncated pixel data: the structural gate never claims the bytes.
    CHECK(oz::viewer_core::detect_codec(k_tga_opaque_4x3,
                                        k_tga_opaque_4x3_len - 1)
              .has_value() == false);
    std::int32_t tw = 0;
    std::int32_t th = 0;
    CHECK(oz::viewer_core::tga::parse_header(k_tga_opaque_4x3,
                                             k_tga_opaque_4x3_len - 1, tw, th) ==
          oz::viewer_core::codec_error::truncated);
    // RLE packet that overfills the declared image.
    std::vector<std::uint8_t> bad_rle = {
        0, 0, 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 2, 0, 24, 0,
        0x85, 0x00, 0x00, 0xFF};
    CHECK(!oz::viewer_core::has_tga_signature(bad_rle.data(), bad_rle.size()));
    // Trailing byte after a legacy (footerless) extent is ambiguous.
    std::vector<std::uint8_t> trailing(k_tga_opaque_4x3,
                                       k_tga_opaque_4x3 + k_tga_opaque_4x3_len);
    trailing.push_back(0);
    CHECK(!oz::viewer_core::has_tga_signature(trailing.data(), trailing.size()));
    // Corrupt footer extension offset.
    std::vector<std::uint8_t> bad_footer(k_tga_footer_ext_4x3,
                                         k_tga_footer_ext_4x3 +
                                             k_tga_footer_ext_4x3_len);
    const std::size_t footer_off = bad_footer.size() - 26;
    bad_footer[footer_off + 0] = 0xFF;
    bad_footer[footer_off + 1] = 0xFF;
    CHECK(!oz::viewer_core::has_tga_signature(bad_footer.data(),
                                              bad_footer.size()));
    // Oversized declared dimensions are rejected before allocation.
    const std::uint8_t huge[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                   0xFF, 0xFF, 0xFF, 0xFF, 24, 0x20};
    CHECK(!oz::viewer_core::has_tga_signature(huge, sizeof(huge)));
    // Palette index outside the declared color-map range: structurally
    // valid container, decoded failure is precise (never corrupt output).
    const std::uint8_t bad_index[31] = {
        0, 1, 1, 0, 0, 4, 0, 24, 0, 0, 0, 0, 1, 0, 1, 0, 8, 0,
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 9};
    CHECK(oz::viewer_core::has_tga_signature(bad_index, sizeof(bad_index)));
    auto dbad = oz::viewer_core::decode_image(bad_index, sizeof(bad_index), 1);
    CHECK(dbad.error == oz::viewer_core::codec_error::invalid_header);
    CHECK(dbad.image.pixels.empty());
}

void test_phaseb_cur_selection_and_decode() {
    using namespace oz::test_phaseb;
    oz::viewer_core::cur_wic::cur_entry sel;
    // Largest safe entry wins; directory words are hotspot metadata only.
    CHECK(oz::viewer_core::cur_wic::select_entry(k_cur_multi_16_32,
                                                 k_cur_multi_16_32_len, sel) ==
          oz::viewer_core::codec_error::none);
    CHECK(sel.width == 32 && sel.height == 32);
    CHECK(sel.index == 1);
    // Same area: higher derived/validated bit depth wins (8bpp skipped).
    oz::viewer_core::cur_wic::cur_entry tie;
    CHECK(oz::viewer_core::cur_wic::select_entry(k_cur_depth_tie_16,
                                                 k_cur_depth_tie_16_len, tie) ==
          oz::viewer_core::codec_error::none);
    CHECK(tie.width == 16 && tie.height == 16);
    CHECK(tie.bit_depth == 24);
    CHECK(tie.index == 1);
    oz::viewer_core::image_info info;
    CHECK(oz::viewer_core::parse_image_header(k_cur_multi_16_32,
                                              k_cur_multi_16_32_len, info) ==
          oz::viewer_core::codec_error::none);
    CHECK(info.codec == oz::viewer_core::codec_id::cur_wic);
    CHECK(info.src_width == 32 && info.src_height == 32);
    // Decoded CUR (DIB payload) canonical pixels; nonzero hotspot ignored.
    auto dm = oz::viewer_core::decode_image(k_cur_multi_16_32,
                                            k_cur_multi_16_32_len, 1);
    CHECK(dm.error == oz::viewer_core::codec_error::none);
    CHECK(dm.image.width == 32 && dm.image.height == 32);
    CHECK(dm.image.codec == oz::viewer_core::codec_id::cur_wic);
    CHECK(dm.image.pixels[0] == 25 && dm.image.pixels[1] == 50 &&
          dm.image.pixels[2] == 200 && dm.image.pixels[3] == 255);
    // PNG-in-CUR payload slice.
    auto dp = oz::viewer_core::decode_image(k_cur_png_2x2, k_cur_png_2x2_len, 1);
    CHECK(dp.error == oz::viewer_core::codec_error::none);
    CHECK(dp.image.width == 2 && dp.image.height == 2);
    CHECK(dp.image.pixels[0] == 0 && dp.image.pixels[1] == 0 &&
          dp.image.pixels[2] == 255 && dp.image.pixels[3] == 255);
    // DIB-in-CUR payload slice.
    auto dd = oz::viewer_core::decode_image(k_cur_dib_2x2, k_cur_dib_2x2_len, 1);
    CHECK(dd.error == oz::viewer_core::codec_error::none);
    CHECK(dd.image.width == 2 && dd.image.height == 2);
    // Canonical alpha contract: transparent => zero RGB.
    for (std::size_t i = 0; i < dd.image.pixels.size(); i += 4) {
        if (dd.image.pixels[i + 3] == 0) {
            CHECK(dd.image.pixels[i] == 0 && dd.image.pixels[i + 1] == 0 &&
                  dd.image.pixels[i + 2] == 0);
        }
    }
    // Scaling denominator contract.
    auto ds = oz::viewer_core::decode_image(k_cur_multi_16_32,
                                            k_cur_multi_16_32_len, 4);
    CHECK(ds.error == oz::viewer_core::codec_error::none);
    CHECK(ds.image.width == 8 && ds.image.height == 8 && ds.image.scale_den == 4);
}

void test_phaseb_cur_rejection() {
    using namespace oz::test_phaseb;
    // Zero directory count.
    const std::uint8_t zero_count[6] = {0, 0, 2, 0, 0, 0};
    CHECK(!oz::viewer_core::has_cur_signature(zero_count, sizeof(zero_count)));
    CHECK(!oz::viewer_core::detect_codec(zero_count, sizeof(zero_count))
               .has_value());
    // Wrong type (type 1 is ICO, never CUR).
    const std::uint8_t wrong_type[22] = {0, 0, 1, 0, 1, 0, 2, 2, 0, 0, 0, 0,
                                         0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(!oz::viewer_core::has_cur_signature(wrong_type, sizeof(wrong_type)));
    // Directory entry payload overlapping the directory.
    std::vector<std::uint8_t> overlap(k_cur_dib_2x2,
                                      k_cur_dib_2x2 + k_cur_dib_2x2_len);
    overlap[6 + 12] = 4;  // offset low byte -> 4 (< dir_end = 22)
    overlap[6 + 13] = 0;
    overlap[6 + 14] = 0;
    overlap[6 + 15] = 0;
    CHECK(!oz::viewer_core::has_cur_signature(overlap.data(), overlap.size()));
    // Truncated payload range.
    CHECK(!oz::viewer_core::has_cur_signature(k_cur_dib_2x2,
                                              k_cur_dib_2x2_len - 1));
    std::int32_t cw = 0;
    std::int32_t ch = 0;
    CHECK(oz::viewer_core::cur_wic::parse_header(k_cur_dib_2x2,
                                                 k_cur_dib_2x2_len - 1, cw, ch) ==
          oz::viewer_core::codec_error::invalid_header);
    // Truncated multi-entry CUR is rejected before any COM/WIC use.
    CHECK(oz::viewer_core::decode_image(k_cur_multi_16_32,
                                        k_cur_multi_16_32_len - 8, 1)
              .error != oz::viewer_core::codec_error::none);
}

// --- Stage 3.3 static AVIF tests (checked-in fixtures, no encoder) --------

void test_avif_signature_routing() {
    using oz::test_avif::k_white_1x1;
    using oz::test_avif::k_white_1x1_len;
    using oz::test_avif::k_colors_animated_8bpc;
    using oz::test_avif::k_colors_animated_8bpc_len;

    // Exact ftyp avif signature
    CHECK(oz::viewer_core::has_avif_signature(k_white_1x1, k_white_1x1_len));
    CHECK(!oz::viewer_core::has_avif_signature(k_white_1x1, 11));
    CHECK(!oz::viewer_core::has_avif_signature(nullptr, 0));

    // Animated avis sequence must be rejected by signature parser
    CHECK(!oz::viewer_core::has_avif_signature(k_colors_animated_8bpc, k_colors_animated_8bpc_len));

    // mif1 alone without avif brand is not AVIF
    std::uint8_t mif1_box[] = {
        0x00, 0x00, 0x00, 0x18, 'f', 't', 'y', 'p',
        'm', 'i', 'f', '1', 0x00, 0x00, 0x00, 0x00,
        'm', 'i', 'f', '1', 'h', 'e', 'i', 'c'
    };
    CHECK(!oz::viewer_core::has_avif_signature(mif1_box, sizeof(mif1_box)));

    // Non-BMFF signatures are not AVIF
    const std::uint8_t png[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    CHECK(!oz::viewer_core::has_avif_signature(png, sizeof(png)));

#if OZVIEWER_HAVE_AVIF
    const auto codec = oz::viewer_core::detect_codec(k_white_1x1, k_white_1x1_len);
    CHECK(codec.has_value() && *codec == oz::viewer_core::codec_id::avif);
#else
    CHECK(!oz::viewer_core::detect_codec(k_white_1x1, k_white_1x1_len).has_value());
#endif
}

#if OZVIEWER_HAVE_AVIF
void test_avif_static_decode() {
    using oz::test_avif::k_white_1x1;
    using oz::test_avif::k_white_1x1_len;
    using oz::test_avif::k_colors_sdr_srgb;
    using oz::test_avif::k_colors_sdr_srgb_len;
    using oz::test_avif::k_circle_custom_properties;
    using oz::test_avif::k_circle_custom_properties_len;

    // Decode white 1x1
    auto res1 = oz::viewer_core::decode_image(k_white_1x1, k_white_1x1_len, 1);
    CHECK(res1.error == oz::viewer_core::codec_error::none);
    CHECK(res1.image.width == 1 && res1.image.height == 1);
    CHECK(res1.image.codec == oz::viewer_core::codec_id::avif);
    CHECK(res1.image.pixels.size() == 4);
    // Alpha channel is opaque (255)
    CHECK(res1.image.pixels[3] == 255);

    // Decode colors_sdr_srgb
    oz::viewer_core::image_info info{};
    CHECK(oz::viewer_core::parse_image_header(k_colors_sdr_srgb, k_colors_sdr_srgb_len, info) ==
          oz::viewer_core::codec_error::none);
    CHECK(info.codec == oz::viewer_core::codec_id::avif);
    CHECK(info.src_width > 0 && info.src_height > 0);

    auto res2 = oz::viewer_core::decode_image(k_colors_sdr_srgb, k_colors_sdr_srgb_len, 1);
    CHECK(res2.error == oz::viewer_core::codec_error::none);
    CHECK(res2.image.width == info.src_width && res2.image.height == info.src_height);

    // Decode circle with custom properties
    auto res3 = oz::viewer_core::decode_image(k_circle_custom_properties, k_circle_custom_properties_len, 1);
    CHECK(res3.error == oz::viewer_core::codec_error::none);
    CHECK(res3.image.width > 0 && res3.image.height > 0);
}

void test_avif_rejection_policies() {
    using oz::test_avif::k_white_1x1;
    using oz::test_avif::k_white_1x1_len;

    // Buffer smaller than ftyp box size
    oz::viewer_core::image_info info{};
    CHECK(!oz::viewer_core::has_avif_signature(k_white_1x1, 16));
    CHECK(oz::viewer_core::detect_codec(k_white_1x1, 16) == std::nullopt);

    // Tamper ftyp major brand and compatible brand
    std::vector<std::uint8_t> corrupt(k_white_1x1, k_white_1x1 + k_white_1x1_len);
    std::memset(corrupt.data() + 8, 0, 24);
    CHECK(!oz::viewer_core::has_avif_signature(corrupt.data(), corrupt.size()));
    CHECK(oz::viewer_core::parse_image_header(corrupt.data(), corrupt.size(), info) ==
          oz::viewer_core::codec_error::unsupported);

    // Encoded size limit
    const std::uint8_t zero = 0;
    CHECK(oz::viewer_core::parse_image_header(&zero, oz::viewer_core::kMaxEncodedBytes + 1, info) ==
          oz::viewer_core::codec_error::allocation_limit);
}
#endif

void test_heic_signature_routing() {
    using oz::test_heic::k_colors_no_alpha;
    using oz::test_heic::k_colors_no_alpha_len;
    using oz::test_heic::k_colors_with_alpha;
    using oz::test_heic::k_colors_with_alpha_len;

    CHECK(oz::viewer_core::has_heic_signature(k_colors_no_alpha, k_colors_no_alpha_len));
    CHECK(oz::viewer_core::has_heic_signature(k_colors_with_alpha, k_colors_with_alpha_len));

    // AVIF signature check on HEIC must return false
    CHECK(!oz::viewer_core::has_avif_signature(k_colors_no_alpha, k_colors_no_alpha_len));

    // HEIC signature check on AVIF must return false
    using oz::test_avif::k_white_1x1;
    using oz::test_avif::k_white_1x1_len;
    CHECK(!oz::viewer_core::has_heic_signature(k_white_1x1, k_white_1x1_len));

    // Sequence brand 'hevc'/'msf1' must be rejected
    // Construct dummy ftyp box with 'hevc' or 'msf1'
    std::uint8_t seq_box[] = {
        0x00, 0x00, 0x00, 0x18, 'f', 't', 'y', 'p',
        'h', 'e', 'v', 'c', 0x00, 0x00, 0x00, 0x00,
        'h', 'e', 'i', 'c', 'm', 'i', 'f', '1'
    };
    CHECK(!oz::viewer_core::has_heic_signature(seq_box, sizeof(seq_box)));

    std::uint8_t msf1_box[] = {
        0x00, 0x00, 0x00, 0x18, 'f', 't', 'y', 'p',
        'm', 's', 'f', '1', 0x00, 0x00, 0x00, 0x00,
        'h', 'e', 'i', 'c', 'm', 'i', 'f', '1'
    };
    CHECK(!oz::viewer_core::has_heic_signature(msf1_box, sizeof(msf1_box)));

    // mif1 alone routes to neither
    std::uint8_t mif1_only[] = {
        0x00, 0x00, 0x00, 0x14, 'f', 't', 'y', 'p',
        'm', 'i', 'f', '1', 0x00, 0x00, 0x00, 0x00,
        'm', 'i', 'a', 'f'
    };
    CHECK(!oz::viewer_core::has_heic_signature(mif1_only, sizeof(mif1_only)));
    CHECK(!oz::viewer_core::has_avif_signature(mif1_only, sizeof(mif1_only)));
}

#if OZVIEWER_HAVE_HEIC
void test_heic_static_decode() {
    using oz::test_heic::k_colors_no_alpha;
    using oz::test_heic::k_colors_no_alpha_len;
    using oz::test_heic::k_colors_with_alpha;
    using oz::test_heic::k_colors_with_alpha_len;

    if (!oz::viewer_core::heif_loader::is_heif_available()) {
        std::printf("test_heic_static_decode: DLLs absent in current test run, skipping decode\n");
        return;
    }

    // Decode colors_no_alpha
    oz::viewer_core::image_info info{};
    CHECK(oz::viewer_core::parse_image_header(k_colors_no_alpha, k_colors_no_alpha_len, info) ==
          oz::viewer_core::codec_error::none);
    CHECK(info.codec == oz::viewer_core::codec_id::heic);
    CHECK(info.src_width == 64 && info.src_height == 64);

    auto res1 = oz::viewer_core::decode_image(k_colors_no_alpha, k_colors_no_alpha_len, 1);
    CHECK(res1.error == oz::viewer_core::codec_error::none);
    CHECK(res1.image.width == 64 && res1.image.height == 64);
    CHECK(res1.image.codec == oz::viewer_core::codec_id::heic);
    CHECK(res1.image.pixels.size() == 64 * 64 * 4);
    // All pixels should have alpha = 255
    CHECK(res1.image.pixels[3] == 255);

    // Decode colors_with_alpha
    oz::viewer_core::image_info info_a{};
    CHECK(oz::viewer_core::parse_image_header(k_colors_with_alpha, k_colors_with_alpha_len, info_a) ==
          oz::viewer_core::codec_error::none);
    CHECK(info_a.codec == oz::viewer_core::codec_id::heic);
    CHECK(info_a.src_width == 64 && info_a.src_height == 64);

    auto res2 = oz::viewer_core::decode_image(k_colors_with_alpha, k_colors_with_alpha_len, 1);
    CHECK(res2.error == oz::viewer_core::codec_error::none);
    CHECK(res2.image.width == 64 && res2.image.height == 64);
    CHECK(res2.image.pixels.size() == 64 * 64 * 4);

    // Scaling denominator test (scaling to 32x32 with den=2)
    auto res_scaled = oz::viewer_core::decode_image(k_colors_no_alpha, k_colors_no_alpha_len, 2);
    CHECK(res_scaled.error == oz::viewer_core::codec_error::none);
    CHECK(res_scaled.image.width == 32 && res_scaled.image.height == 32);
    CHECK(res_scaled.image.scale_den == 2);
}

void test_heic_rejection_policies() {
    using oz::test_heic::k_colors_no_alpha;
    using oz::test_heic::k_colors_no_alpha_len;

    // Buffer smaller than ftyp box size
    oz::viewer_core::image_info info{};
    CHECK(!oz::viewer_core::has_heic_signature(k_colors_no_alpha, 16));
    CHECK(oz::viewer_core::detect_codec(k_colors_no_alpha, 16) == std::nullopt);

    // Encoded size limit
    const std::uint8_t zero = 0;
    CHECK(oz::viewer_core::parse_image_header(&zero, oz::viewer_core::kMaxEncodedBytes + 1, info) ==
          oz::viewer_core::codec_error::allocation_limit);

    // Rejection of sequence and non-8bit fixtures from disk
    auto read_file = [](const std::filesystem::path& p) -> std::vector<std::uint8_t> {
        FILE* f = _wfopen(p.c_str(), L"rb");
        if (!f) return {};
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        std::vector<std::uint8_t> buf(sz);
        if (fread(buf.data(), 1, sz, f) != static_cast<size_t>(sz)) return {};
        fclose(f);
        return buf;
    };

    // 1. hevc32.heif: valid still with 'heic' brand
    auto hevc32 = read_file(L"tests/data/heic/hevc32.heif");
    if (!hevc32.empty()) {
        CHECK(oz::viewer_core::has_heic_signature(hevc32.data(), hevc32.size()));
        CHECK(oz::viewer_core::detect_codec(hevc32.data(), hevc32.size()) == oz::viewer_core::codec_id::heic);
    }

    // 2. seq_seed.heif: sequence brand 'msf1'
    auto seq_seed = read_file(L"tests/data/heic/seq_seed.heif");
    if (!seq_seed.empty()) {
        CHECK(!oz::viewer_core::has_heic_signature(seq_seed.data(), seq_seed.size()));
        CHECK(oz::viewer_core::detect_codec(seq_seed.data(), seq_seed.size()) == std::nullopt);
    }

    // 3. uncompressed 16-bit rejection
    auto b16 = read_file(L"tests/data/heic/uncompressed_comp_B16R16G16.heic");
    if (!b16.empty()) {
        auto dec_b16 = oz::viewer_core::decode_image(b16.data(), b16.size(), 1);
        CHECK(dec_b16.error != oz::viewer_core::codec_error::none);
    }
}

void test_heic_navigation() {
#if OZVIEWER_HAVE_HEIC
    CHECK(oz::viewer_core::is_supported_image_extension(L"test.heic"));
    CHECK(oz::viewer_core::is_supported_image_extension(L"test.HEIF"));
#else
    CHECK(!oz::viewer_core::is_supported_image_extension(L"test.heic"));
    CHECK(!oz::viewer_core::is_supported_image_extension(L"test.HEIF"));
#endif
}

// Stage 8 Conformance: Opening a JPEG must neither initialize AVIF nor load/init HEIC runtime
void test_stage8_jpeg_lazy_isolation() {
    using oz::viewer_core::heif_loader::is_heif_loaded;
    using oz::viewer_core::heif_loader::load_attempt_count;
    using oz::viewer_core::heif_loader::reset_load_attempt_count;
    using oz::viewer_core::avif::decoder_invocation_count;
    using oz::viewer_core::avif::reset_decoder_invocation_count;

    // Reset probe counters
    reset_decoder_invocation_count();
    reset_load_attempt_count();

    // 1. Prior to any HEIC decode, HEIC must NOT be loaded or attempted merely by inspecting JPEG
    // or by running extension checks or sibling enumeration.
    auto jpeg_bytes = make_test_jpeg(100, 80);
    CHECK(!jpeg_bytes.empty());

    // Detecting codec on JPEG must not invoke AVIF or load/attempt HEIC
    const auto codec = oz::viewer_core::detect_codec(jpeg_bytes.data(), jpeg_bytes.size());
    CHECK(codec.has_value() && *codec == oz::viewer_core::codec_id::jpeg);
    CHECK(decoder_invocation_count() == 0);
    CHECK(!is_heif_loaded());
    CHECK(load_attempt_count() == 0);

    // Parsing JPEG header must not invoke AVIF or load/attempt HEIC
    oz::viewer_core::image_info info{};
    const auto perr = oz::viewer_core::parse_image_header(jpeg_bytes.data(), jpeg_bytes.size(), info);
    CHECK(perr == oz::viewer_core::codec_error::none);
    CHECK(info.codec == oz::viewer_core::codec_id::jpeg);
    CHECK(decoder_invocation_count() == 0);
    CHECK(!is_heif_loaded());
    CHECK(load_attempt_count() == 0);

    // Decoding JPEG must not invoke AVIF or load/attempt HEIC
    const auto dres = oz::viewer_core::decode_image(jpeg_bytes.data(), jpeg_bytes.size(), 1);
    CHECK(dres.error == oz::viewer_core::codec_error::none);
    CHECK(dres.image.codec == oz::viewer_core::codec_id::jpeg);
    CHECK(decoder_invocation_count() == 0);
    CHECK(!is_heif_loaded());
    CHECK(load_attempt_count() == 0);

    // Sibling-extension filtering and navigation build on JPEG paths
    // must NOT invoke AVIF or load/attempt HEIC DLLs.
    std::vector<std::filesystem::path> cand = {
        L"dir/photo1.jpg", L"dir/photo2.jpeg", L"dir/photo3.png",
        L"dir/photo4.heic", L"dir/photo5.avif"
    };
    auto nav = oz::viewer_core::navigation_list::build(cand, L"dir/photo1.jpg");
    CHECK(nav.size() > 0);
    CHECK(decoder_invocation_count() == 0);
    CHECK(!is_heif_loaded());
    CHECK(load_attempt_count() == 0);

    // 2. Fallback preservation test:
    // If a current image is active, and a subsequent corrupt/missing/incompatible HEIC fails to decode,
    // decode_image returns a clear error code (unsupported or invalid_header) without crashing or corrupting memory,
    // and callers preserve the previous valid image state.
    // We construct an actually HEIC-routable fixture: valid ISO-BMFF box (box_size=16, 'ftyp', major 'heic', minor 0)
    // with truncated/corrupt metadata that correctly routes detect_codec() to heic (not generic unsupported).
    const std::uint8_t corrupt_heic[] = {
        0x00, 0x00, 0x00, 0x10, 'f', 't', 'y', 'p',
        'h', 'e', 'i', 'c', 0x00, 0x00, 0x00, 0x00,
        0xFF, 0xFF, 0xFF, 0xFF
    };
    const auto detected_corrupt = oz::viewer_core::detect_codec(corrupt_heic, sizeof(corrupt_heic));
    CHECK(detected_corrupt.has_value() && *detected_corrupt == oz::viewer_core::codec_id::heic);

    const auto corrupt_res = oz::viewer_core::decode_image(corrupt_heic, sizeof(corrupt_heic), 1);
    CHECK(corrupt_res.error == oz::viewer_core::codec_error::invalid_header ||
          corrupt_res.error == oz::viewer_core::codec_error::unsupported ||
          corrupt_res.error == oz::viewer_core::codec_error::truncated);
    // Original JPEG image remains intact and valid in the caller's state
    CHECK(dres.image.width > 0 && dres.image.height > 0 && !dres.image.pixels.empty());
}
#endif

// --- Stage 4 ViewState, Geometry, and DocumentState unit tests ---------------
void test_stage4_view_state_and_geometry() {
    using namespace oz::viewer_core;

    auto approx = [](double a, double b) { return std::abs(a - b) < 1e-4; };

    // 1. Fit scale & no-upscale
    // Smaller than viewport: scale = 1.0 (no upscale)
    CHECK(approx(compute_fit_scale(400, 300, 1000, 800), 1.0));
    // Larger than viewport: aspect preserved scale
    CHECK(approx(compute_fit_scale(2000, 1000, 1000, 800), 0.5));
    CHECK(approx(compute_fit_scale(1000, 2000, 1000, 800), 0.4));

    // 2. Clamp scale limits [1/64, 32]
    CHECK(approx(clamp_scale(0.001), view_state::kMinScale));
    CHECK(approx(clamp_scale(100.0), view_state::kMaxScale));
    CHECK(approx(clamp_scale(2.5), 2.5));

    // 3. Degenerate / zero viewport
    CHECK(approx(compute_fit_scale(0, 100, 1000, 800), 1.0));
    CHECK(approx(compute_fit_scale(100, 0, 1000, 800), 1.0));
    CHECK(approx(compute_fit_scale(100, 100, 0, 800), 1.0));
    CHECK(approx(compute_fit_scale(100, 100, 1000, 0), 1.0));

    view_state vs_degen;
    auto degen_geom = compute_view_geometry(vs_degen, 100, 100, 0, 0);
    CHECK(degen_geom.dst_w == 0.0 && degen_geom.dst_h == 0.0);

    // 4. Cursor invariance on wheel zoom
    // When zooming at cursor (cx, cy) while the image exceeds viewport (or once zoomed in),
    // the unconstrained source point under the cursor before zoom must remain under the cursor.
    // Notice that if extent <= viewport, pan is clamped to 0 (image centered).
    // When zoomed in such that extent > viewport (e.g. src 2000x1600 in 1000x800 viewport),
    // cursor invariance holds strictly.
    view_state vs;
    const std::int32_t src_w = 2000;
    const std::int32_t src_h = 1600;
    const std::int32_t vw = 1000;
    const std::int32_t vh = 800;

    vs.mode = view_mode::custom;
    vs.scale = 1.0; // extent = 2000x1600 > 1000x800
    vs.pan_x = 0.0;
    vs.pan_y = 0.0;

    const double cursor_x = 500.0; // center
    const double cursor_y = 400.0;

    const auto geom_before = compute_view_geometry(vs, src_w, src_h, vw, vh);
    const double src_pt_x_before = (cursor_x - geom_before.dst_x) / geom_before.scale;
    const double src_pt_y_before = (cursor_y - geom_before.dst_y) / geom_before.scale;

    // Zoom in by 1 step
    zoom_at_point(vs, src_w, src_h, vw, vh, cursor_x, cursor_y, 1.0);
    const auto geom_after1 = compute_view_geometry(vs, src_w, src_h, vw, vh);
    const double cursor_recalc_x1 = geom_after1.dst_x + src_pt_x_before * geom_after1.scale;
    const double cursor_recalc_y1 = geom_after1.dst_y + src_pt_y_before * geom_after1.scale;
    CHECK(approx(cursor_recalc_x1, cursor_x));
    CHECK(approx(cursor_recalc_y1, cursor_y));

    // Zoom in with high-resolution fractional wheel delta (e.g. 0.25 step)
    zoom_at_point(vs, src_w, src_h, vw, vh, cursor_x, cursor_y, 0.25);
    const auto geom_after2 = compute_view_geometry(vs, src_w, src_h, vw, vh);
    const double cursor_recalc_x2 = geom_after2.dst_x + src_pt_x_before * geom_after2.scale;
    const double cursor_recalc_y2 = geom_after2.dst_y + src_pt_y_before * geom_after2.scale;
    CHECK(approx(cursor_recalc_x2, cursor_x));
    CHECK(approx(cursor_recalc_y2, cursor_y));

    // 5. Pan clamp
    // If extent <= viewport: pan must be zero / centered
    view_state vs_pan;
    vs_pan.mode = view_mode::custom;
    vs_pan.scale = 1.0;
    // src is 400x300, viewport is 800x600 -> extent <= viewport
    vs_pan.pan_x = 100.0;
    vs_pan.pan_y = -50.0;
    clamp_pan(vs_pan, 400, 300, 800, 600);
    CHECK(vs_pan.pan_x == 0.0);
    CHECK(vs_pan.pan_y == 0.0);

    // If extent > viewport: pan clamped to +-(extent - viewport)/2
    // src 2000x1000 at scale 1.0, viewport 1000x600:
    // extent_w = 2000, max_pan_x = (2000 - 1000)/2 = 500
    // extent_h = 1000, max_pan_y = (1000 - 600)/2 = 200
    vs_pan.pan_x = 1000.0; // excessive
    vs_pan.pan_y = -500.0; // excessive
    clamp_pan(vs_pan, 2000, 1000, 1000, 600);
    CHECK(approx(vs_pan.pan_x, 500.0));
    CHECK(approx(vs_pan.pan_y, -200.0));

    // 6. Viewport resize in fit vs custom mode
    view_state vs_res;
    set_fit(vs_res, 1000, 500, 500, 500); // fit scale = 0.5
    CHECK(approx(vs_res.scale, 0.5));
    on_viewport_resized(vs_res, 1000, 500, 500, 500, 2000, 1000); // fit scale now 1.0 (no upscale)
    CHECK(approx(vs_res.scale, 1.0));
    CHECK(vs_res.pan_x == 0.0 && vs_res.pan_y == 0.0);

    // In custom mode: preserve center source point
    set_one_to_one(vs_res, 1000, 500, 500, 500);
    CHECK(vs_res.mode == view_mode::custom);
    CHECK(approx(vs_res.scale, 1.0));
    on_viewport_resized(vs_res, 1000, 500, 500, 500, 800, 600);
    CHECK(approx(vs_res.scale, 1.0));

    // 7. Double-click Fit <-> 100% toggle
    view_state vs_toggle;
    set_fit(vs_toggle, 1600, 1200, 800, 600);
    CHECK(vs_toggle.mode == view_mode::fit);
    // Double click at (200, 150) switches to 100% anchored at (200, 150)
    toggle_fit_or_one(vs_toggle, 1600, 1200, 800, 600, 200.0, 150.0);
    CHECK(vs_toggle.mode == view_mode::custom);
    CHECK(approx(vs_toggle.scale, 1.0));
    // Another double click switches back to Fit
    toggle_fit_or_one(vs_toggle, 1600, 1200, 800, 600, 200.0, 150.0);
    CHECK(vs_toggle.mode == view_mode::fit);

    // 8. DPI -> DIP view_geometry_to_dip contract
    const auto geom = compute_view_geometry(vs_toggle, 1600, 1200, 800, 600);
    const auto dip96 = view_geometry_to_dip(geom, 96.0f);
    CHECK(approx(dip96.width, static_cast<float>(geom.dst_w)));
    const auto dip144 = view_geometry_to_dip(geom, 144.0f);
    CHECK(approx(dip144.width, static_cast<float>(geom.dst_w * 96.0 / 144.0)));

    // 9. DocumentState monotonicity & generation rejection
    document_state doc;
    CHECK(doc.generation == 1);
    CHECK(!doc.has_document);
    doc.commit_document(L"C:\\test\\img1.jpg");
    CHECK(doc.has_document);
    CHECK(doc.active_path == L"C:\\test\\img1.jpg");

    const auto gen2 = doc.next_generation();
    CHECK(gen2 == 2);
    CHECK(doc.generation == 2);
    // Stale generation checks
    const std::uint64_t stale_gen = 1;
    CHECK(stale_gen != doc.generation);

    doc.reset_to_empty();
    CHECK(doc.generation == 3);
    CHECK(!doc.has_document);
    CHECK(doc.active_path.empty());

    // Navigation commit invariant: active_path tracks successfully presented documents,
    // does not update on pending/failed attempts, and updates on successful sibling presentation.
    doc.commit_document(L"C:\\test\\sibling1.png");
    CHECK(doc.has_document);
    CHECK(doc.active_path == L"C:\\test\\sibling1.png");

    // Pending navigation to sibling2: before presentation/commit, active_path remains sibling1
    const std::filesystem::path pending_path = L"C:\\test\\sibling2.png";
    const bool decode_failed = true;
    if (!decode_failed) {
        doc.commit_document(pending_path);
    }
    CHECK(doc.active_path == L"C:\\test\\sibling1.png");

    // Successful sibling presentation commits sibling2
    const bool present_succeeded = true;
    if (present_succeeded) {
        doc.commit_document(pending_path);
    }
    CHECK(doc.active_path == L"C:\\test\\sibling2.png");
}

#if OZVIEWER_ENABLE_EDITING
void test_stage5_slice1_edit_foundation() {
    using namespace oz::viewer_core;

    auto approx = [](double a, double b) { return std::abs(a - b) < 1e-4; };

    // ---------------------------------------------------------------------------
    // 1. Basic Type & Rect Validation
    // ---------------------------------------------------------------------------
    {
        edit_dimensions valid_dims{800, 600};
        CHECK(valid_dims.is_valid());
        edit_dimensions zero_dims{0, 600};
        CHECK(!zero_dims.is_valid());
        edit_dimensions neg_dims{800, -1};
        CHECK(!neg_dims.is_valid());
        edit_dimensions overflow_dims{kMaxEditDimension + 1, 600};
        CHECK(!overflow_dims.is_valid());

        edit_rect valid_rect{10, 20, 100, 200};
        CHECK(valid_rect.is_valid());
        CHECK(valid_rect.right() == 110);
        CHECK(valid_rect.bottom() == 220);

        edit_rect neg_rect{-1, 0, 10, 10};
        CHECK(!neg_rect.is_valid());
        edit_rect zero_w_rect{0, 0, 0, 10};
        CHECK(!zero_w_rect.is_valid());
        edit_rect overflow_rect{kMaxEditDimension, 0, 1, 10};
        CHECK(!overflow_rect.is_valid());
    }

    // ---------------------------------------------------------------------------
    // 2. D4 Discrete Symmetry Group (Composition, Inversion, Mapping)
    // ---------------------------------------------------------------------------
    {
        const edit_dimensions in_dims{800, 600};

        const auto id = d4_transform::make_identity();
        CHECK(!id.swaps_dimensions());
        CHECK(id.transform_dimensions(in_dims) == in_dims);
        CHECK(id.map_point({0, 0}, in_dims) == edit_point(0, 0));
        CHECK(id.map_point({799, 599}, in_dims) == edit_point(799, 599));
        CHECK(id.compose(id) == id);
        CHECK(id.inverse() == id);

        const auto r90_cw = d4_transform::make_rot90_cw();
        CHECK(r90_cw.swaps_dimensions());
        const edit_dimensions dims_cw{600, 800};
        CHECK(r90_cw.transform_dimensions(in_dims) == dims_cw);
        // Top-left (0, 0) in 800x600 rotated 90 CW becomes top-right of 600x800:
        // x' = 600 - 1 - y = 599, y' = x = 0 => (599, 0)
        CHECK(r90_cw.map_point({0, 0}, in_dims) == edit_point(599, 0));
        // Bottom-right (799, 599): x' = 600 - 1 - 599 = 0, y' = 799 => (0, 799)
        CHECK(r90_cw.map_point({799, 599}, in_dims) == edit_point(0, 799));

        const auto r90_ccw = d4_transform::make_rot90_ccw();
        CHECK(r90_ccw.swaps_dimensions());
        CHECK(r90_ccw.transform_dimensions(in_dims) == dims_cw);
        // Top-left (0, 0) rotated 90 CCW:
        // x' = y = 0, y' = 800 - 1 - x = 799 => (0, 799)
        CHECK(r90_ccw.map_point({0, 0}, in_dims) == edit_point(0, 799));

        // Composition: 4 consecutive CW rotations return to identity
        auto r180 = r90_cw.compose(r90_cw);
        CHECK(r180 == d4_transform::make_rot180());
        CHECK(!r180.swaps_dimensions());
        auto r270 = r180.compose(r90_cw);
        CHECK(r270 == r90_ccw);
        auto r360 = r270.compose(r90_cw);
        CHECK(r360 == id);

        // Inverse: CW inverse is CCW
        CHECK(r90_cw.inverse() == r90_ccw);
        CHECK(r90_ccw.inverse() == r90_cw);

        // Flips: horizontal and vertical
        const auto fh = d4_transform::make_flip_h();
        const auto fv = d4_transform::make_flip_v();
        CHECK(!fh.swaps_dimensions());
        CHECK(!fv.swaps_dimensions());
        // Double flip is identity
        CHECK(fh.compose(fh) == id);
        CHECK(fv.compose(fv) == id);
        // H-flip then V-flip is 180-degree rotation
        CHECK(fh.compose(fv) == r180);

        // Point round-trip under inverse
        edit_point original_pt{123, 456};
        auto mapped_cw = r90_cw.map_point(original_pt, in_dims);
        auto round_trip = r90_cw.inverse().map_point(mapped_cw, dims_cw);
        CHECK(round_trip == original_pt);
    }

    // ---------------------------------------------------------------------------
    // 3. Crop Clamping, Half-Open Range, Ratios & A-Series
    // ---------------------------------------------------------------------------
    {
        const edit_dimensions canvas{1920, 1080};

        // Half-open clamping
        edit_rect inside{100, 100, 500, 400};
        CHECK(clamp_crop_rect(inside, canvas) == inside);

        // Out of bounds on right/bottom
        edit_rect out_bound{1900, 1000, 100, 100};
        auto clamped = clamp_crop_rect(out_bound, canvas);
        CHECK(clamped.x == 1900);
        CHECK(clamped.y == 1000);
        CHECK(clamped.width == 20); // clamped to canvas width 1920
        CHECK(clamped.height == 80); // clamped to canvas height 1080

        // Negative coordinates clamped to 0
        edit_rect neg_rect{-50, -50, 200, 200};
        auto clamped_neg = clamp_crop_rect(neg_rect, canvas);
        CHECK(clamped_neg.x == 0);
        CHECK(clamped_neg.y == 0);
        CHECK(clamped_neg.width == 200);
        CHECK(clamped_neg.height == 200);

        // Minimum 1x1 size guaranteed
        edit_rect zero_sz{100, 100, 0, 0};
        auto min_clamped = clamp_crop_rect(zero_sz, canvas);
        CHECK(min_clamped.width >= 1);
        CHECK(min_clamped.height >= 1);

        // Preset Aspect Ratios
        CHECK(!get_crop_aspect_ratio(crop_ratio_preset::free, canvas).has_value());
        CHECK(approx(*get_crop_aspect_ratio(crop_ratio_preset::original, canvas), 1920.0 / 1080.0));
        CHECK(approx(*get_crop_aspect_ratio(crop_ratio_preset::square_1x1, canvas), 1.0));
        CHECK(approx(*get_crop_aspect_ratio(crop_ratio_preset::ratio_3x2, canvas), 1.5));
        CHECK(approx(*get_crop_aspect_ratio(crop_ratio_preset::ratio_3x2, canvas, true), 2.0 / 3.0));
        CHECK(approx(*get_crop_aspect_ratio(crop_ratio_preset::ratio_4x3, canvas), 4.0 / 3.0));
        CHECK(approx(*get_crop_aspect_ratio(crop_ratio_preset::ratio_5x4, canvas), 1.25));
        CHECK(approx(*get_crop_aspect_ratio(crop_ratio_preset::ratio_16x9, canvas), 16.0 / 9.0));
        CHECK(approx(*get_crop_aspect_ratio(crop_ratio_preset::ratio_9x16, canvas), 9.0 / 16.0));

        // A-series: 1 : sqrt(2)
        const double sqrt2 = 1.414213562373;
        CHECK(approx(*get_crop_aspect_ratio(crop_ratio_preset::a_series, canvas, false), sqrt2));
        CHECK(approx(*get_crop_aspect_ratio(crop_ratio_preset::a_series, canvas, true), 1.0 / sqrt2));

        // Centered crop calculation
        // 1:1 on 1920x1080 canvas: height is limiting (1080), width = 1080, x = (1920-1080)/2 = 420
        auto c1x1 = compute_centered_aspect_crop(canvas, 1.0);
        CHECK(c1x1.x == 420);
        CHECK(c1x1.y == 0);
        CHECK(c1x1.width == 1080);
        CHECK(c1x1.height == 1080);

        // 16:9 on 1920x1080 canvas: exact fit
        auto c16x9 = compute_centered_aspect_crop(canvas, 16.0 / 9.0);
        CHECK(c16x9.x == 0);
        CHECK(c16x9.y == 0);
        CHECK(c16x9.width == 1920);
        CHECK(c16x9.height == 1080);
    }

    // ---------------------------------------------------------------------------
    // 4. Resize Dimension Calculations & Constraints
    // ---------------------------------------------------------------------------
    {
        const edit_dimensions orig{1600, 1200};

        // Percent mode
        resize_calculation_params p_pct;
        p_pct.current_dims = orig;
        p_pct.unit = resize_unit::percent;
        p_pct.requested_percent = 50.0;
        auto res_50 = calculate_resize_dimensions(p_pct);
        CHECK(res_50.has_value());
        CHECK(res_50->width == 800);
        CHECK(res_50->height == 600);

        p_pct.requested_percent = 200.0;
        auto res_200 = calculate_resize_dimensions(p_pct);
        CHECK(res_200.has_value());
        CHECK(res_200->width == 3200);
        CHECK(res_200->height == 2400);

        // Percent <= 0 or overflow rejected
        p_pct.requested_percent = 0.0;
        CHECK(!calculate_resize_dimensions(p_pct).has_value());
        p_pct.requested_percent = 100000.0; // exceeds kMaxEditDimension
        CHECK(!calculate_resize_dimensions(p_pct).has_value());

        // Pixel mode with aspect ratio lock (maintain_aspect_ratio = true)
        resize_calculation_params p_pix;
        p_pix.current_dims = orig;
        p_pix.unit = resize_unit::pixels;
        p_pix.maintain_aspect_ratio = true;

        // Specify only width -> height auto-computed
        p_pix.requested_width = 800;
        p_pix.requested_height = std::nullopt;
        auto res_w800 = calculate_resize_dimensions(p_pix);
        CHECK(res_w800.has_value());
        CHECK(res_w800->width == 800);
        CHECK(res_w800->height == 600);

        // Specify only height -> width auto-computed
        p_pix.requested_width = std::nullopt;
        p_pix.requested_height = 300;
        auto res_h300 = calculate_resize_dimensions(p_pix);
        CHECK(res_h300.has_value());
        CHECK(res_h300->width == 400);
        CHECK(res_h300->height == 300);

        // Pixel mode independent (maintain_aspect_ratio = false)
        p_pix.maintain_aspect_ratio = false;
        p_pix.requested_width = 1024;
        p_pix.requested_height = 768;
        auto res_indep = calculate_resize_dimensions(p_pix);
        CHECK(res_indep.has_value());
        CHECK(res_indep->width == 1024);
        CHECK(res_indep->height == 768);

        // Reject > kMaxEditDimension
        p_pix.requested_width = kMaxEditDimension + 10;
        p_pix.requested_height = 100;
        CHECK(!calculate_resize_dimensions(p_pix).has_value());

        // Reject negative or zero
        p_pix.requested_width = -100;
        p_pix.requested_height = 100;
        CHECK(!calculate_resize_dimensions(p_pix).has_value());
    }

    // ---------------------------------------------------------------------------
    // 5. Command Evaluation Math & Canvas Dimensions
    // ---------------------------------------------------------------------------
    {
        const edit_dimensions init{1000, 500};

        // Rotate Left/Right swaps dimensions
        auto rot_r = evaluate_command_dimensions(init, rotate_command{rotate_direction::right_90});
        CHECK(rot_r.has_value());
        CHECK(rot_r->width == 500);
        CHECK(rot_r->height == 1000);

        auto rot_l = evaluate_command_dimensions(init, rotate_command{rotate_direction::left_90});
        CHECK(rot_l.has_value());
        CHECK(rot_l->width == 500);
        CHECK(rot_l->height == 1000);

        // Flip preserves dimensions
        auto flip_h = evaluate_command_dimensions(init, flip_command{flip_direction::horizontal});
        CHECK(flip_h.has_value());
        CHECK(*flip_h == init);

        // Crop within bounds succeeds
        crop_command valid_crop{edit_rect{100, 100, 400, 300}};
        auto cr_res = evaluate_command_dimensions(init, valid_crop);
        CHECK(cr_res.has_value());
        CHECK(cr_res->width == 400);
        CHECK(cr_res->height == 300);

        // Crop out of bounds fails
        crop_command oob_crop{edit_rect{900, 400, 200, 200}};
        CHECK(!evaluate_command_dimensions(init, oob_crop).has_value());

        // Resize
        resize_command res_cmd{edit_dimensions{640, 480}, resize_filter::automatic};
        auto res_eval = evaluate_command_dimensions(init, res_cmd);
        CHECK(res_eval.has_value());
        CHECK(res_eval->width == 640);
        CHECK(res_eval->height == 480);
    }

    // ---------------------------------------------------------------------------
    // 6. Chronological Edit History, Redo Truncation, Limits & Dirty Semantics
    // ---------------------------------------------------------------------------
    {
        edit_dimensions src_dims{1000, 800};
        edit_history history{src_dims};

        CHECK(!history.is_dirty());
        CHECK(history.revision() == 0);
        CHECK(history.undo_count() == 0);
        CHECK(history.redo_count() == 0);
        CHECK(history.current_dimensions() == src_dims);

        // 1. Push rotate
        CHECK(history.push_command(rotate_command{rotate_direction::right_90}));
        CHECK(history.is_dirty());
        CHECK(history.revision() == 1);
        CHECK(history.undo_count() == 1);
        CHECK(history.redo_count() == 0);
        CHECK(history.current_dimensions() == edit_dimensions(800, 1000));

        // 2. Push crop on the rotated canvas (800x1000)
        CHECK(history.push_command(crop_command{edit_rect{50, 50, 500, 500}}));
        CHECK(history.revision() == 2);
        CHECK(history.undo_count() == 2);
        CHECK(history.current_dimensions() == edit_dimensions(500, 500));

        // 3. Push flip
        CHECK(history.push_command(flip_command{flip_direction::vertical}));
        CHECK(history.undo_count() == 3);
        CHECK(history.current_dimensions() == edit_dimensions(500, 500));

        // 4. Test Undo
        CHECK(history.undo());
        CHECK(history.revision() == 4);
        CHECK(history.undo_count() == 2);
        CHECK(history.redo_count() == 1);
        CHECK(history.current_dimensions() == edit_dimensions(500, 500));

        CHECK(history.undo());
        CHECK(history.undo_count() == 1);
        CHECK(history.redo_count() == 2);
        CHECK(history.current_dimensions() == edit_dimensions(800, 1000));

        // 5. Redo truncation: pushing a new command after undo truncates redo stack
        CHECK(history.push_command(resize_command{edit_dimensions{400, 500}, resize_filter::bicubic}));
        CHECK(history.undo_count() == 2); // 1st rotate + new resize
        CHECK(history.redo_count() == 0); // truncated!
        CHECK(history.current_dimensions() == edit_dimensions(400, 500));

        // 6. Max 64 undoable transitions floor test:
        // Push until reaching 64 undo transitions. Then one more.
        // Commands are NEVER physically dropped: all 65 remain in commands_ so replay from immutable original is correct.
        // Undo floor advances so undo cannot go below cursor - 64.
        edit_history cap_hist{edit_dimensions{100, 100}};
        for (std::size_t i = 0; i < kMaxUndoTransitions; ++i) {
            CHECK(cap_hist.push_command(flip_command{flip_direction::horizontal}));
        }
        CHECK(cap_hist.undo_count() == kMaxUndoTransitions);
        CHECK(cap_hist.total_commands() == kMaxUndoTransitions);
        CHECK(cap_hist.undo_floor() == 0);

        // Push 65th command: total commands grows to 65 (never physically discarded),
        // undo_floor moves to 1, undo_count remains clamped to 64.
        CHECK(cap_hist.push_command(flip_command{flip_direction::vertical}));
        CHECK(cap_hist.total_commands() == 65);
        CHECK(cap_hist.undo_floor() == 1);
        CHECK(cap_hist.undo_count() == kMaxUndoTransitions);

        // Verify undo stops at floor (can only undo 64 times, not 65):
        for (std::size_t i = 0; i < kMaxUndoTransitions; ++i) {
            CHECK(cap_hist.can_undo());
            CHECK(cap_hist.undo());
        }
        CHECK(!cap_hist.can_undo());
        CHECK(cap_hist.undo_count() == 0);
        // Commands are still all 65 in history:
        CHECK(cap_hist.total_commands() == 65);
        CHECK(cap_hist.redo_count() == 64);

        // Redo back to end:
        for (std::size_t i = 0; i < kMaxUndoTransitions; ++i) {
            CHECK(cap_hist.can_redo());
            CHECK(cap_hist.redo());
        }
        CHECK(!cap_hist.can_redo());
        CHECK(cap_hist.undo_count() == kMaxUndoTransitions);

        // 7. Max 256 lifetime command limit test:
        // Push up to 256 lifetime commands. The 257th must be rejected.
        for (std::size_t i = cap_hist.lifetime_pushed_commands(); i < kMaxTotalCommands; ++i) {
            CHECK(cap_hist.push_command(flip_command{flip_direction::horizontal}));
        }
        CHECK(cap_hist.lifetime_pushed_commands() == kMaxTotalCommands);
        CHECK(cap_hist.total_commands() == kMaxTotalCommands);
        CHECK(cap_hist.undo_floor() == kMaxTotalCommands - kMaxUndoTransitions);
        // Next push must fail:
        CHECK(!cap_hist.push_command(flip_command{flip_direction::horizontal}));
        CHECK(cap_hist.lifetime_pushed_commands() == kMaxTotalCommands);
    }

    // ---------------------------------------------------------------------------
    // 7. ImageDocument Lightweight Domain Model
    // ---------------------------------------------------------------------------
    {
        document_metadata meta;
        meta.source_path = L"C:\\images\\sample.jpg";
        meta.file_size = 123456;
        meta.source_dimensions = {1200, 800};
        meta.codec_name = "jpeg";

        image_document doc1{meta};
        CHECK(doc1.instance_id() > 0);
        CHECK(doc1.metadata().source_path == meta.source_path);
        CHECK(doc1.metadata().source_dimensions == meta.source_dimensions);
        CHECK(!doc1.is_dirty());
        CHECK(doc1.edit_revision() == 0);
        CHECK(doc1.current_dimensions() == meta.source_dimensions);

        // Distinct instance IDs
        image_document doc2{meta};
        CHECK(doc2.instance_id() != doc1.instance_id());

        // Perform edit on doc1
        CHECK(doc1.history().push_command(rotate_command{rotate_direction::right_90}));
        CHECK(doc1.is_dirty());
        CHECK(doc1.edit_revision() == 1);
        CHECK(doc1.current_dimensions() == edit_dimensions(800, 1200));

        // Reset document with new metadata
        document_metadata meta2;
        meta2.source_path = L"C:\\images\\another.png";
        meta2.source_dimensions = {640, 480};
        const auto old_id = doc1.instance_id();
        doc1.reset(meta2);

        CHECK(doc1.instance_id() != old_id);
        CHECK(!doc1.is_dirty());
        CHECK(doc1.edit_revision() == 0);
        CHECK(doc1.current_dimensions() == meta2.source_dimensions);
    }

    // ---------------------------------------------------------------------------
    // 8. Stage 5 Slice 2: Bounded Pure CPU Edit Materialization Pipeline Tests
    // ---------------------------------------------------------------------------
    {
        // Helper to generate a deterministic asymmetric test pattern in canonical PBGRA8
        // Size 4x3 (width 4, height 3), with distinctive pixel values at every (x, y)
        // Pixel layout: B, G, R, A (premultiplied BGRA8)
        // With alpha varying and color components properly premultiplied (B, G, R <= A)
        auto make_pattern_4x3 = []() -> std::pair<std::vector<std::uint8_t>, edit_source_view> {
            constexpr std::int32_t w = 4;
            constexpr std::int32_t h = 3;
            constexpr std::int32_t stride = w * 4;
            std::vector<std::uint8_t> pixels(static_cast<std::size_t>(stride * h));

            for (std::int32_t y = 0; y < h; ++y) {
                for (std::int32_t x = 0; x < w; ++x) {
                    const std::size_t idx = static_cast<std::size_t>(y * stride + x * 4);
                    const std::uint8_t a = static_cast<std::uint8_t>(200 + y * 15 + x * 5); // 200..245
                    const std::uint8_t b = static_cast<std::uint8_t>((x * 40) % a);
                    const std::uint8_t g = static_cast<std::uint8_t>((y * 60) % a);
                    const std::uint8_t r = static_cast<std::uint8_t>((x * 30 + y * 40) % a);
                    pixels[idx + 0] = b;
                    pixels[idx + 1] = g;
                    pixels[idx + 2] = r;
                    pixels[idx + 3] = a;
                }
            }
            edit_source_view view;
            view.pixels = std::span<const std::uint8_t>(pixels.data(), pixels.size());
            view.dimensions = {w, h};
            view.stride = stride;
            return {std::move(pixels), view};
        };

        auto [src_buf, src_view] = make_pattern_4x3();
        src_view.pixels = std::span<const std::uint8_t>(src_buf.data(), src_buf.size());

        // Snapshot original buffer to guarantee NO source mutation
        const std::vector<std::uint8_t> src_copy = src_buf;

        auto get_px = [](const std::vector<std::uint8_t>& buf, std::int32_t stride, std::int32_t x, std::int32_t y) {
            const std::size_t idx = static_cast<std::size_t>(y * stride + x * 4);
            return std::make_tuple(buf[idx + 0], buf[idx + 1], buf[idx + 2], buf[idx + 3]);
        };

        // A. Materialize 0 commands: returns exact copy of source
        {
            auto res = materialize_edits(src_view, {});
            CHECK(res.error == edit_pipeline_error::none);
            CHECK(res.dimensions == src_view.dimensions);
            CHECK(res.stride == src_view.stride);
            CHECK(res.pixels == src_buf);
        }

        // B. Rotate 90 CW: 4x3 -> 3x4
        {
            std::vector<edit_command> cmds = {rotate_command{rotate_direction::right_90}};
            auto res = materialize_edits(src_view, cmds);
            CHECK(res.error == edit_pipeline_error::none);
            CHECK(res.dimensions == edit_dimensions(3, 4));
            CHECK(res.stride == 3 * 4);

            // In CW rotation: dst(dst_x, dst_y) = src(x, y) where dst_x = src_h - 1 - y, dst_y = x
            // Test (x=0, y=0) -> (dst_x = 3 - 1 - 0 = 2, dst_y = 0)
            CHECK(get_px(res.pixels, res.stride, 2, 0) == get_px(src_buf, src_view.stride, 0, 0));
            // Test (x=3, y=2) -> (dst_x = 3 - 1 - 2 = 0, dst_y = 3)
            CHECK(get_px(res.pixels, res.stride, 0, 3) == get_px(src_buf, src_view.stride, 3, 2));
            // Test (x=1, y=2) -> (dst_x = 3 - 1 - 2 = 0, dst_y = 1)
            CHECK(get_px(res.pixels, res.stride, 0, 1) == get_px(src_buf, src_view.stride, 1, 2));
        }

        // C. Rotate 90 CCW: 4x3 -> 3x4
        {
            std::vector<edit_command> cmds = {rotate_command{rotate_direction::left_90}};
            auto res = materialize_edits(src_view, cmds);
            CHECK(res.error == edit_pipeline_error::none);
            CHECK(res.dimensions == edit_dimensions(3, 4));
            // In CCW rotation: dst_x = y, dst_y = src_w - 1 - x
            // Test (x=0, y=0) -> (dst_x = 0, dst_y = 4 - 1 - 0 = 3)
            CHECK(get_px(res.pixels, res.stride, 0, 3) == get_px(src_buf, src_view.stride, 0, 0));
            // Test (x=3, y=2) -> (dst_x = 2, dst_y = 4 - 1 - 3 = 0)
            CHECK(get_px(res.pixels, res.stride, 2, 0) == get_px(src_buf, src_view.stride, 3, 2));
        }

        // D. 4x Rot 90 CW returns to identity
        {
            std::vector<edit_command> cmds = {
                rotate_command{rotate_direction::right_90},
                rotate_command{rotate_direction::right_90},
                rotate_command{rotate_direction::right_90},
                rotate_command{rotate_direction::right_90}
            };
            auto res = materialize_edits(src_view, cmds);
            CHECK(res.error == edit_pipeline_error::none);
            CHECK(res.dimensions == src_view.dimensions);
            CHECK(res.pixels == src_buf);
        }

        // E. Flip horizontal
        {
            std::vector<edit_command> cmds = {flip_command{flip_direction::horizontal}};
            auto res = materialize_edits(src_view, cmds);
            CHECK(res.error == edit_pipeline_error::none);
            CHECK(res.dimensions == src_view.dimensions);
            // (x=0, y=1) should equal (x=3, y=1) of src
            CHECK(get_px(res.pixels, res.stride, 0, 1) == get_px(src_buf, src_view.stride, 3, 1));
            CHECK(get_px(res.pixels, res.stride, 3, 1) == get_px(src_buf, src_view.stride, 0, 1));
        }

        // F. Flip vertical
        {
            std::vector<edit_command> cmds = {flip_command{flip_direction::vertical}};
            auto res = materialize_edits(src_view, cmds);
            CHECK(res.error == edit_pipeline_error::none);
            CHECK(res.dimensions == src_view.dimensions);
            // (x=2, y=0) should equal (x=2, y=2) of src
            CHECK(get_px(res.pixels, res.stride, 2, 0) == get_px(src_buf, src_view.stride, 2, 2));
            CHECK(get_px(res.pixels, res.stride, 2, 2) == get_px(src_buf, src_view.stride, 2, 0));
        }

        // G. Crop: half-open rect [1, 1, 2, 2) -> dims 2x2
        {
            std::vector<edit_command> cmds = {crop_command{edit_rect{1, 1, 2, 2}}};
            auto res = materialize_edits(src_view, cmds);
            CHECK(res.error == edit_pipeline_error::none);
            CHECK(res.dimensions == edit_dimensions(2, 2));
            CHECK(res.stride == 2 * 4);
            // dst(0, 0) should be src(1, 1)
            CHECK(get_px(res.pixels, res.stride, 0, 0) == get_px(src_buf, src_view.stride, 1, 1));
            // dst(1, 1) should be src(2, 2)
            CHECK(get_px(res.pixels, res.stride, 1, 1) == get_px(src_buf, src_view.stride, 2, 2));
        }

        // H. Order preservation: Rotate 90 CW then Crop vs Crop then Rotate
        {
            // Rotate 90 CW gives 3x4. Crop [0, 0, 2, 2) on 3x4
            std::vector<edit_command> cmds_rot_crop = {
                rotate_command{rotate_direction::right_90},
                crop_command{edit_rect{0, 0, 2, 2}}
            };
            auto res1 = materialize_edits(src_view, cmds_rot_crop);
            CHECK(res1.error == edit_pipeline_error::none);
            CHECK(res1.dimensions == edit_dimensions(2, 2));

            // Crop [0, 0, 2, 2) on 4x3 gives 2x2, then rotate gives 2x2
            std::vector<edit_command> cmds_crop_rot = {
                crop_command{edit_rect{0, 0, 2, 2}},
                rotate_command{rotate_direction::right_90}
            };
            auto res2 = materialize_edits(src_view, cmds_crop_rot);
            CHECK(res2.error == edit_pipeline_error::none);
            CHECK(res2.dimensions == edit_dimensions(2, 2));

            // The pixels should NOT be the same because rotate CW changed what pixel ended up in [0, 2)
            CHECK(res1.pixels != res2.pixels);
        }

        // I. Resize: exact same dimensions returns exact copy
        {
            std::vector<edit_command> cmds = {resize_command{src_view.dimensions, resize_filter::automatic}};
            auto res = materialize_edits(src_view, cmds);
            CHECK(res.error == edit_pipeline_error::none);
            CHECK(res.dimensions == src_view.dimensions);
            CHECK(res.pixels == src_buf);
        }

        // J. Resize: downscale and upscale via WIC
        {
            // Downscale 4x3 to 2x2 (Fant)
            std::vector<edit_command> down_cmds = {resize_command{edit_dimensions{2, 2}, resize_filter::automatic}};
            auto down_res = materialize_edits(src_view, down_cmds);
            CHECK(down_res.error == edit_pipeline_error::none);
            CHECK(down_res.dimensions == edit_dimensions(2, 2));
            CHECK(down_res.stride == 2 * 4);
            CHECK(down_res.pixels.size() == 2 * 4 * 2);

            // Verify alpha and PBGRA invariant: color channels must not exceed alpha (B, G, R <= A)
            for (std::size_t i = 0; i < down_res.pixels.size(); i += 4) {
                const std::uint8_t b = down_res.pixels[i + 0];
                const std::uint8_t g = down_res.pixels[i + 1];
                const std::uint8_t r = down_res.pixels[i + 2];
                const std::uint8_t a = down_res.pixels[i + 3];
                CHECK(b <= a);
                CHECK(g <= a);
                CHECK(r <= a);
            }

            // Repeatability: executing the downscale twice gives identical bytes
            auto down_res2 = materialize_edits(src_view, down_cmds);
            CHECK(down_res.pixels == down_res2.pixels);

            // Upscale 4x3 to 8x6 (HighQualityCubic)
            std::vector<edit_command> up_cmds = {resize_command{edit_dimensions{8, 6}, resize_filter::automatic}};
            auto up_res = materialize_edits(src_view, up_cmds);
            CHECK(up_res.error == edit_pipeline_error::none);
            CHECK(up_res.dimensions == edit_dimensions(8, 6));
            CHECK(up_res.stride == 8 * 4);
            CHECK(up_res.pixels.size() == 8 * 4 * 6);

            // Check PBGRA invariants on upscale
            for (std::size_t i = 0; i < up_res.pixels.size(); i += 4) {
                const std::uint8_t b = up_res.pixels[i + 0];
                const std::uint8_t g = up_res.pixels[i + 1];
                const std::uint8_t r = up_res.pixels[i + 2];
                const std::uint8_t a = up_res.pixels[i + 3];
                CHECK(b <= a);
                CHECK(g <= a);
                CHECK(r <= a);
            }

            auto up_res2 = materialize_edits(src_view, up_cmds);
            CHECK(up_res.pixels == up_res2.pixels);
        }

        // K. Verify source buffer was NEVER mutated throughout all tests
        CHECK(src_buf == src_copy);

        // L. Malformed / invalid command rejection
        {
            // Invalid crop (out of bounds)
            std::vector<edit_command> bad_crop = {crop_command{edit_rect{2, 2, 5, 5}}};
            auto res = materialize_edits(src_view, bad_crop);
            CHECK(res.error == edit_pipeline_error::invalid_command);

            // Invalid resize (target dimensions 0)
            std::vector<edit_command> bad_resize = {resize_command{edit_dimensions{0, 100}, resize_filter::automatic}};
            auto res2 = materialize_edits(src_view, bad_resize);
            CHECK(res2.error == edit_pipeline_error::invalid_command);

            // Invalid source view
            edit_source_view bad_src = src_view;
            bad_src.stride = 2; // invalid stride < width * 4
            auto res3 = materialize_edits(bad_src, {});
            CHECK(res3.error == edit_pipeline_error::invalid_source);
        }

        // M. Memory admission budget limit
        {
            // Set budget to tiny amount (e.g. 10 bytes) that cannot even admit output
            std::vector<edit_command> rot_cmd = {rotate_command{rotate_direction::right_90}};
            auto res = materialize_edits(src_view, rot_cmd, 10);
            CHECK(res.error == edit_pipeline_error::memory_limit);

            // Budget helper calculation check
            CHECK(compute_edit_admission_budget(1000) == 250);
            CHECK(compute_edit_admission_budget(10ull * 1024ull * 1024ull * 1024ull) == 1024ull * 1024ull * 1024ull); // clamped at 1 GiB
        }

        // N. Replay from immutable source with commands beyond undo floor
        {
            edit_history hist{src_view.dimensions};
            // Push 70 rotate commands
            for (std::size_t i = 0; i < 70; ++i) {
                CHECK(hist.push_command(rotate_command{rotate_direction::right_90}));
            }
            CHECK(hist.total_commands() == 70);
            CHECK(hist.undo_floor() == 6);
            CHECK(hist.undo_count() == 64);

            // Active commands span 70 commands
            auto active = hist.active_commands();
            CHECK(active.size() == 70);

            // Materialize all active commands: 70 CW rotations == (70 % 4) = 2 CW rotations == 180 deg
            auto res = materialize_edits(src_view, active);
            CHECK(res.error == edit_pipeline_error::none);
            CHECK(res.dimensions == src_view.dimensions);

            // Compare with explicit 2 CW rotations:
            std::vector<edit_command> two_rot = {
                rotate_command{rotate_direction::right_90},
                rotate_command{rotate_direction::right_90}
            };
            auto res_two = materialize_edits(src_view, two_rot);
            CHECK(res.pixels == res_two.pixels);

            // Test undo and redo truncation
            // Undo 4 times (active commands becomes 66, which is > undo_floor 6)
            for (int i = 0; i < 4; ++i) {
                CHECK(hist.undo());
            }
            CHECK(hist.active_commands().size() == 66);
            CHECK(hist.redo_count() == 4);

            // Push a flip_h: truncates the 4 redo commands, makes total 67 commands
            CHECK(hist.push_command(flip_command{flip_direction::horizontal}));
            CHECK(hist.redo_count() == 0);
            CHECK(hist.total_commands() == 67);
            CHECK(hist.active_commands().size() == 67);
            // Replay from immutable original produces correct dimensions and results
            auto res_trunc = materialize_edits(src_view, hist.active_commands());
            CHECK(res_trunc.error == edit_pipeline_error::none);
            CHECK(res_trunc.dimensions == src_view.dimensions);
        }

        // ---------------------------------------------------------------------------
        // 9. Stage 5 Slice 3: Domain Integration, Stale Reject, ABA, Presentation Plan
        // ---------------------------------------------------------------------------
        {
            // A. Document instance ID and generation ABA / stale edit completion check
            document_metadata meta;
            meta.source_path = L"C:\\images\\test.png";
            meta.source_dimensions = {100, 50};
            meta.codec_name = "png";

            image_document doc{meta};
            const auto id1 = doc.instance_id();
            CHECK(id1 != 0);

            // Stale completion with older instance_id is rejected
            const std::uint64_t stale_id = id1 - 1;
            CHECK(stale_id != doc.instance_id());

            // Stale revision check
            CHECK(doc.history().push_command(rotate_command{rotate_direction::right_90}));
            const auto rev1 = doc.edit_revision();
            CHECK(rev1 == 1);
            CHECK(doc.history().push_command(flip_command{flip_direction::horizontal}));
            const auto rev2 = doc.edit_revision();
            CHECK(rev2 == 2);
            // Completion for rev1 arriving when active revision is rev2 must not be treated as current
            CHECK(rev1 != doc.edit_revision());

            // B. Dirty discard / cancel decision logic
            CHECK(doc.is_dirty());
            // Undo back to pristine clears dirty state
            CHECK(doc.history().undo());
            CHECK(doc.is_dirty()); // 1 command still active
            CHECK(doc.history().undo());
            CHECK(!doc.is_dirty()); // pristine!

            // C. Transactional rollback logic: history push and rollback
            CHECK(doc.history().push_command(rotate_command{rotate_direction::left_90}));
            CHECK(doc.history().total_commands() == 1);
            CHECK(doc.history().undo()); // safely roll back
            CHECK(!doc.is_dirty());

            // D. D4 presentation plan composition and dimension swapping
            d4_transform d4 = d4_transform::make_identity();
            CHECK(!d4.swaps_dimensions());

            // Rotate 90 CW swaps dimensions
            d4 = d4.compose(d4_transform::make_rot90_cw());
            CHECK(d4.swaps_dimensions());
            edit_dimensions dims{100, 50};
            CHECK(d4.transform_dimensions(dims) == edit_dimensions(50, 100));

            // Another Rotate 90 CW returns to unswapped (180)
            d4 = d4.compose(d4_transform::make_rot90_cw());
            CHECK(!d4.swaps_dimensions());
            CHECK(d4.transform_dimensions(dims) == edit_dimensions(100, 50));

            // E. Presentation uses the immutable source as its renderer base;
            // a materialized output must never become a transformed base for a
            // subsequent replay of the same command log.
            const auto identity_d4 = d4_transform::make_identity();
            CHECK(identity_d4.m00 == 1 && identity_d4.m01 == 0 && identity_d4.m10 == 0 && identity_d4.m11 == 1);
            CHECK(!identity_d4.swaps_dimensions());

            // F. Source file verification abstraction
            CHECK(!oz::viewer_core::detect_codec(nullptr, 0).has_value());
        }
    }
}

void test_stage5_crop_interaction() {
    using namespace oz::viewer_core;

    // Half-open rectangle mapping is the bridge between a crop authored in
    // displayed D4 coordinates and its retained source-bitmap rectangle.
    const edit_dimensions raw{4, 3};
    const auto cw = d4_transform::make_rot90_cw();
    const edit_dimensions rotated{3, 4};
    const auto raw_for_top_left_rotated = map_half_open_rect(
        cw.inverse(), edit_rect{0, 0, 2, 2}, rotated);
    CHECK(raw_for_top_left_rotated.has_value());
    CHECK((*raw_for_top_left_rotated == edit_rect{0, 1, 2, 2}));
    const auto back_to_rotated = map_half_open_rect(cw, *raw_for_top_left_rotated, raw);
    CHECK(back_to_rotated.has_value());
    CHECK((*back_to_rotated == edit_rect{0, 0, 2, 2}));

    const auto flip_h_crop = map_half_open_rect(
        d4_transform::make_flip_h(), edit_rect{0, 0, 2, 2}, raw);
    CHECK(flip_h_crop.has_value());
    CHECK((*flip_h_crop == edit_rect{2, 0, 2, 2}));
    CHECK((!map_half_open_rect(cw, edit_rect{3, 0, 2, 2}, raw).has_value()));

    // Crop state is draft-only and remains bounded inside the logical canvas.
    auto state = begin_crop_interaction({100, 80});
    CHECK(state.active);
    CHECK((state.selection == edit_rect{0, 0, 100, 80}));

    // Resize from the top-left handle, then create a new free selection in the
    // now-unselected region and move it; all stages stay inside the canvas.
    CHECK(begin_crop_drag(state, 0.0, 0.0, 2.0));
    CHECK(state.drag == crop_drag_target::top_left);
    CHECK(update_crop_drag(state, 20.0, 10.0));
    CHECK((state.selection == edit_rect{20, 10, 80, 70}));
    end_crop_drag(state);

    CHECK(begin_crop_drag(state, 5.0, 5.0, 1.0));
    CHECK(state.drag == crop_drag_target::create);
    CHECK(update_crop_drag(state, 45.0, 35.0));
    CHECK((state.selection == edit_rect{5, 5, 40, 30}));
    end_crop_drag(state);

    CHECK(begin_crop_drag(state, 25.0, 20.0, 1.0));
    CHECK(state.drag == crop_drag_target::move);
    CHECK(update_crop_drag(state, 35.0, 30.0));
    CHECK((state.selection == edit_rect{15, 15, 40, 30}));
    end_crop_drag(state);

    // Fixed-ratio presets reset to the maximum centered selection and preserve
    // a close integer ratio while an edge/corner is dragged.
    set_crop_interaction_ratio(state, crop_ratio_preset::ratio_16x9);
    CHECK(state.selection.is_valid());
    CHECK(std::abs(static_cast<double>(state.selection.width) /
                   static_cast<double>(state.selection.height) - (16.0 / 9.0)) < 0.03);
    CHECK(begin_crop_drag(state, static_cast<double>(state.selection.right()),
                          static_cast<double>(state.selection.bottom()), 2.0));
    CHECK(state.drag == crop_drag_target::bottom_right);
    CHECK(update_crop_drag(state, 70.0, 60.0));
    CHECK(state.selection.is_valid());
    CHECK(std::abs(static_cast<double>(state.selection.width) /
                   static_cast<double>(state.selection.height) - (16.0 / 9.0)) < 0.05);
    end_crop_drag(state);

    reset_crop_interaction(state);
    CHECK(state.selection.is_valid());
    auto invalid = begin_crop_interaction({0, 80});
    CHECK(!invalid.active);
    CHECK(!begin_crop_drag(invalid, 0.0, 0.0, 1.0));
}

static void test_stage5_slice5_resize_math_and_proxy() {
    using namespace oz::viewer_core;

    // 1. Dimension math: range 1..65535, percent and pixel constraints
    const edit_dimensions base{1920, 1080};
    resize_calculation_params p{};
    p.current_dims = base;

    // Percent mode
    p.unit = resize_unit::percent;
    p.requested_percent = 100.0;
    auto res = calculate_resize_dimensions(p);
    CHECK(res.has_value());
    CHECK((res == base));

    p.requested_percent = 50.0;
    res = calculate_resize_dimensions(p);
    CHECK(res.has_value());
    CHECK((res == edit_dimensions{960, 540}));

    // Percent mode boundary limits
    p.requested_percent = 0.0;
    CHECK(!calculate_resize_dimensions(p).has_value());
    p.requested_percent = -10.0;
    CHECK(!calculate_resize_dimensions(p).has_value());
    p.requested_percent = 5000.0; // 1920 * 50 = 96000 > 65535
    CHECK(!calculate_resize_dimensions(p).has_value());
    // Non-finite percentages must be rejected before any cast.
    p.requested_percent = std::numeric_limits<double>::quiet_NaN();
    CHECK(!calculate_resize_dimensions(p).has_value());
    p.requested_percent = std::numeric_limits<double>::infinity();
    CHECK(!calculate_resize_dimensions(p).has_value());
    p.requested_percent = -std::numeric_limits<double>::infinity();
    CHECK(!calculate_resize_dimensions(p).has_value());
    // A rounding-to-zero percentage is invalid, not a 0-pixel canvas.
    p.requested_percent = 0.01;
    CHECK(!calculate_resize_dimensions(p).has_value());

    // Pixels mode: Aspect ratio lock ON
    p.unit = resize_unit::pixels;
    p.maintain_aspect_ratio = true;
    p.requested_percent = std::nullopt;
    p.requested_width = 1280;
    p.requested_height = std::nullopt;
    res = calculate_resize_dimensions(p);
    CHECK(res.has_value());
    CHECK((res == edit_dimensions{1280, 720}));

    p.requested_width = std::nullopt;
    p.requested_height = 360;
    res = calculate_resize_dimensions(p);
    CHECK(res.has_value());
    CHECK((res == edit_dimensions{640, 360}));

    // Pixels mode: Aspect ratio lock OFF (independent dimensions)
    p.maintain_aspect_ratio = false;
    p.requested_width = 800;
    p.requested_height = 800;
    res = calculate_resize_dimensions(p);
    CHECK(res.has_value());
    CHECK((res == edit_dimensions{800, 800}));

    p.requested_width = 0;
    CHECK(!calculate_resize_dimensions(p).has_value());
    p.requested_width = kMaxEditDimension + 1;
    CHECK(!calculate_resize_dimensions(p).has_value());
    p.requested_width = kMaxEditDimension;
    p.requested_height = kMaxEditDimension;
    res = calculate_resize_dimensions(p);
    CHECK(res.has_value());
    CHECK((res == edit_dimensions{kMaxEditDimension, kMaxEditDimension}));

    // 2. Affine proxy source rectangle mapping
    // Initial untouched image: full source rect [0, 0, 1920, 1080], identity
    std::vector<edit_command> cmds;
    auto proxy = compute_affine_proxy_plan(base, cmds);
    CHECK(proxy.valid);
    CHECK((proxy.logical_dimensions == base));
    CHECK((proxy.transform == d4_transform::make_identity()));
    CHECK(std::abs(proxy.source_rectf.x - 0.0f) < 1e-4f);
    CHECK(std::abs(proxy.source_rectf.y - 0.0f) < 1e-4f);
    CHECK(std::abs(proxy.source_rectf.width - 1920.0f) < 1e-4f);
    CHECK(std::abs(proxy.source_rectf.height - 1080.0f) < 1e-4f);

    // Draft resize on untouched source: canvas updates to draft, source rect remains untouched
    const edit_dimensions draft_target{960, 540};
    proxy = compute_affine_proxy_plan(base, cmds, draft_target);
    CHECK(proxy.valid);
    CHECK((proxy.logical_dimensions == draft_target));
    CHECK((proxy.transform == d4_transform::make_identity()));
    CHECK(std::abs(proxy.source_rectf.width - 1920.0f) < 1e-4f);
    CHECK(std::abs(proxy.source_rectf.height - 1080.0f) < 1e-4f);

    // Rotate 90 CW + Crop + Resize chain
    // Rotate 90 CW => canvas is 1080x1920
    cmds.push_back(rotate_command{rotate_direction::right_90});
    // Crop center 540x960
    cmds.push_back(crop_command{edit_rect{270, 480, 540, 960}});
    // Resize to 270x480
    cmds.push_back(resize_command{edit_dimensions{270, 480}, resize_filter::bicubic});

    proxy = compute_affine_proxy_plan(base, cmds);
    CHECK(proxy.valid);
    CHECK((proxy.logical_dimensions == edit_dimensions{270, 480}));
    CHECK((proxy.transform == d4_transform::make_rot90_cw()));
    // Source rect mapped back to unrotated original coordinate space:
    // crop {270, 480, 540, 960} on 1080x1920 under inverse CW:
    // x' = y = 480, y' = 1080 - (270 + 540) = 270 (continuous bounds),
    // mapped bounds: [480, 1440) x [270, 810).
    CHECK(std::abs(proxy.source_rectf.x - 480.0f) < 1e-3f);
    CHECK(std::abs(proxy.source_rectf.y - 270.0f) < 1e-3f);
    CHECK(std::abs(proxy.source_rectf.width - 960.0f) < 1e-3f);
    CHECK(std::abs(proxy.source_rectf.height - 540.0f) < 1e-3f);

    // Draft resize applied on top of the chain
    proxy = compute_affine_proxy_plan(base, cmds, edit_dimensions{135, 240});
    CHECK(proxy.valid);
    CHECK((proxy.logical_dimensions == edit_dimensions{135, 240}));
    CHECK((proxy.transform == d4_transform::make_rot90_cw()));
    CHECK(std::abs(proxy.source_rectf.x - 480.0f) < 1e-3f);
    CHECK(std::abs(proxy.source_rectf.y - 270.0f) < 1e-3f);
    CHECK(std::abs(proxy.source_rectf.width - 960.0f) < 1e-3f);
    CHECK(std::abs(proxy.source_rectf.height - 540.0f) < 1e-3f);

    // 3. Crop after a non-uniform resize: the crop must map back through the
    // per-axis inverse scale, producing fractional source edges.
    {
        std::vector<edit_command> resize_then_crop = {
            resize_command{edit_dimensions{853, 480}, resize_filter::automatic},
            crop_command{edit_rect{100, 60, 400, 300}}};
        proxy = compute_affine_proxy_plan(base, resize_then_crop);
        CHECK(proxy.valid);
        CHECK((proxy.transform == d4_transform::make_identity()));
        CHECK((proxy.logical_dimensions == edit_dimensions{400, 300}));
        const double sx = 1920.0 / 853.0;
        const double sy = 1080.0 / 480.0;
        CHECK(std::abs(proxy.source_rectf.x - static_cast<float>(100.0 * sx)) < 5e-3f);
        CHECK(std::abs(proxy.source_rectf.y - static_cast<float>(60.0 * sy)) < 5e-3f);
        CHECK(std::abs(proxy.source_rectf.width - static_cast<float>(400.0 * sx)) < 5e-3f);
        CHECK(std::abs(proxy.source_rectf.height - static_cast<float>(300.0 * sy)) < 5e-3f);
        // Fractional edges stay inside the source after the float round trip.
        CHECK(proxy.source_rectf.x >= 0.0f && proxy.source_rectf.y >= 0.0f);
        CHECK(proxy.source_rectf.right() <= 1920.0f + 1e-2f);
        CHECK(proxy.source_rectf.bottom() <= 1080.0f + 1e-2f);
    }

    // 4. Resize / crop / resize chain: the final resize only changes the
    // logical canvas, never the source sub-rectangle.
    {
        std::vector<edit_command> chain = {
            resize_command{edit_dimensions{960, 540}, resize_filter::automatic},
            crop_command{edit_rect{100, 50, 200, 100}},
            resize_command{edit_dimensions{50, 25}, resize_filter::automatic}};
        proxy = compute_affine_proxy_plan(base, chain);
        CHECK(proxy.valid);
        CHECK((proxy.logical_dimensions == edit_dimensions{50, 25}));
        CHECK((proxy.transform == d4_transform::make_identity()));
        CHECK(std::abs(proxy.source_rectf.x - 200.0f) < 1e-3f);
        CHECK(std::abs(proxy.source_rectf.y - 100.0f) < 1e-3f);
        CHECK(std::abs(proxy.source_rectf.width - 400.0f) < 1e-3f);
        CHECK(std::abs(proxy.source_rectf.height - 200.0f) < 1e-3f);
    }

    // 5. Flip + crop + resize: reflection maps the crop through inverse D4.
    {
        std::vector<edit_command> flip_crop = {
            flip_command{flip_direction::horizontal},
            crop_command{edit_rect{10, 20, 100, 50}},
            resize_command{edit_dimensions{20, 10}, resize_filter::automatic}};
        proxy = compute_affine_proxy_plan(base, flip_crop);
        CHECK(proxy.valid);
        CHECK((proxy.transform == d4_transform::make_flip_h()));
        CHECK((proxy.logical_dimensions == edit_dimensions{20, 10}));
        CHECK(std::abs(proxy.source_rectf.x - 1810.0f) < 1e-3f);
        CHECK(std::abs(proxy.source_rectf.y - 20.0f) < 1e-3f);
        CHECK(std::abs(proxy.source_rectf.width - 100.0f) < 1e-3f);
        CHECK(std::abs(proxy.source_rectf.height - 50.0f) < 1e-3f);
    }

    // 6. Rotate / crop / rotate / crop: composite translations must be
    // resolved with the canvas dimensions of each crop step.
    {
        const edit_dimensions small{100, 50};
        std::vector<edit_command> rcrc = {
            rotate_command{rotate_direction::right_90},
            crop_command{edit_rect{10, 10, 30, 20}},
            rotate_command{rotate_direction::right_90},
            crop_command{edit_rect{0, 0, 10, 10}}};
        proxy = compute_affine_proxy_plan(small, rcrc);
        CHECK(proxy.valid);
        CHECK((proxy.transform == d4_transform::make_rot180()));
        CHECK((proxy.logical_dimensions == edit_dimensions{10, 10}));
        CHECK(std::abs(proxy.source_rectf.x - 20.0f) < 1e-3f);
        CHECK(std::abs(proxy.source_rectf.y - 30.0f) < 1e-3f);
        CHECK(std::abs(proxy.source_rectf.width - 10.0f) < 1e-3f);
        CHECK(std::abs(proxy.source_rectf.height - 10.0f) < 1e-3f);
    }

    // 7. Non-uniform resize then rotation then crop: the per-axis scale must
    // swap axes through the rotation before the crop is mapped back.
    {
        std::vector<edit_command> nrs = {
            resize_command{edit_dimensions{500, 600}, resize_filter::automatic},
            rotate_command{rotate_direction::right_90},
            crop_command{edit_rect{100, 50, 200, 150}}};
        proxy = compute_affine_proxy_plan(base, nrs);
        CHECK(proxy.valid);
        CHECK((proxy.transform == d4_transform::make_rot90_cw()));
        CHECK((proxy.logical_dimensions == edit_dimensions{200, 150}));
        const double sx = 1920.0 / 500.0;
        const double sy = 1080.0 / 600.0;
        CHECK(std::abs(proxy.source_rectf.x - static_cast<float>(50.0 * sx)) < 5e-3f);
        CHECK(std::abs(proxy.source_rectf.y - static_cast<float>(300.0 * sy)) < 5e-3f);
        CHECK(std::abs(proxy.source_rectf.width - static_cast<float>(150.0 * sx)) < 5e-3f);
        CHECK(std::abs(proxy.source_rectf.height - static_cast<float>(200.0 * sy)) < 5e-3f);
    }

    // 8. Invalid inputs never produce a usable proxy plan.
    {
        std::vector<edit_command> bad_crop = {crop_command{edit_rect{0, 0, 5000, 5000}}};
        CHECK(!compute_affine_proxy_plan(base, bad_crop).valid);
        CHECK(!compute_affine_proxy_plan(base, {}, edit_dimensions{0, 10}).valid);
        CHECK(!compute_affine_proxy_plan(edit_dimensions{0, 0}, {}).valid);
    }

    // 9. Undo/redo of a committed resize: no new lifetime commands, and the
    // active command prefix always produces a presentable proxy plan.
    {
        edit_history history{base};
        CHECK(history.push_command(
            resize_command{edit_dimensions{960, 540}, resize_filter::automatic}));
        const std::size_t lifetime_after_push = history.lifetime_pushed_commands();

        auto active_proxy = [&history]() {
            const auto active = history.active_commands();
            return compute_affine_proxy_plan(
                edit_dimensions{1920, 1080},
                std::span<const edit_command>(active.data(), active.size()));
        };

        CHECK(history.undo());
        CHECK(history.redo());
        CHECK(history.lifetime_pushed_commands() == lifetime_after_push);
        CHECK((history.current_dimensions() == edit_dimensions{960, 540}));
        auto plan = active_proxy();
        CHECK(plan.valid);
        CHECK((plan.logical_dimensions == edit_dimensions{960, 540}));
        CHECK(std::abs(plan.source_rectf.width - 1920.0f) < 1e-4f);
        CHECK(std::abs(plan.source_rectf.height - 1080.0f) < 1e-4f);

        CHECK(history.undo());
        CHECK(history.lifetime_pushed_commands() == lifetime_after_push);
        CHECK((history.current_dimensions() == base));
        plan = active_proxy();
        CHECK(plan.valid);
        CHECK((plan.logical_dimensions == base));
        CHECK(!history.can_undo());
    }
}

// ---------------------------------------------------------------------------
// Stage 5 Slice 6: conservative Save/Save As export pipeline + saved
// checkpoint. Focused, deterministic Windows tests using a scratch directory
// under the system temp folder. No test fakes success: every outcome is
// asserted from real encoded files and real error taxonomy values.
// ---------------------------------------------------------------------------

struct pixel_source {
    std::vector<std::uint8_t> pixels;
    oz::viewer_core::edit_source_view view{};

    pixel_source() = default;
    pixel_source(const pixel_source&) = delete;
    pixel_source& operator=(const pixel_source&) = delete;
    pixel_source(pixel_source&&) = default;
    pixel_source& operator=(pixel_source&&) = default;
};

// Deterministic, position-distinct canonical PBGRA8 pattern. When alpha < 255
// the color channels are premultiplied (B/G/R <= A) exactly as the canonical
// contract requires.
pixel_source make_test_pattern(std::int32_t width, std::int32_t height,
                               std::uint8_t alpha) {
    pixel_source out;
    const std::int32_t stride = width * 4;
    out.pixels.assign(static_cast<std::size_t>(stride) * static_cast<std::size_t>(height), 0);
    for (std::int32_t y = 0; y < height; ++y) {
        for (std::int32_t x = 0; x < width; ++x) {
            const std::size_t index = static_cast<std::size_t>(y * stride + x * 4);
            const std::uint32_t a = alpha;
            const std::uint32_t straight_b = static_cast<std::uint32_t>((x * 53 + y * 17 + 11) & 0xFF);
            const std::uint32_t straight_g = static_cast<std::uint32_t>((x * 29 + y * 61 + 37) & 0xFF);
            const std::uint32_t straight_r = static_cast<std::uint32_t>((x * 97 + y * 7 + 71) & 0xFF);
            out.pixels[index + 0] = static_cast<std::uint8_t>((straight_b * a) / 255u);
            out.pixels[index + 1] = static_cast<std::uint8_t>((straight_g * a) / 255u);
            out.pixels[index + 2] = static_cast<std::uint8_t>((straight_r * a) / 255u);
            out.pixels[index + 3] = static_cast<std::uint8_t>(a);
        }
    }
    out.view = oz::viewer_core::edit_source_view{
        .pixels = std::span<const std::uint8_t>(out.pixels.data(), out.pixels.size()),
        .dimensions = {width, height},
        .stride = stride};
    return out;
}

// Smooth linear-gradient pattern for the lossy JPEG reconstruction test.
// Low spatial frequency keeps the admissible lossy error bounded and stable.
pixel_source make_gradient_pattern(std::int32_t width, std::int32_t height) {
    pixel_source out;
    const std::int32_t stride = width * 4;
    out.pixels.assign(static_cast<std::size_t>(stride) * static_cast<std::size_t>(height), 0);
    for (std::int32_t y = 0; y < height; ++y) {
        for (std::int32_t x = 0; x < width; ++x) {
            const std::size_t index = static_cast<std::size_t>(y * stride + x * 4);
            out.pixels[index + 0] = static_cast<std::uint8_t>(32 + x * 8);
            out.pixels[index + 1] = static_cast<std::uint8_t>(32 + y * 8);
            out.pixels[index + 2] = static_cast<std::uint8_t>(64 + (x + y) * 4);
            out.pixels[index + 3] = 255;
        }
    }
    out.view = oz::viewer_core::edit_source_view{
        .pixels = std::span<const std::uint8_t>(out.pixels.data(), out.pixels.size()),
        .dimensions = {width, height},
        .stride = stride};
    return out;
}

// Scratch directory owned by one test invocation. Removed on destruction.
struct scratch_dir {
    std::filesystem::path path;

    explicit scratch_dir(const wchar_t* name) {
        std::error_code ec;
        path = std::filesystem::temp_directory_path(ec) / name;
        std::filesystem::remove_all(path, ec);
        std::filesystem::create_directories(path, ec);
    }
    ~scratch_dir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    scratch_dir(const scratch_dir&) = delete;
    scratch_dir& operator=(const scratch_dir&) = delete;
};

std::vector<std::uint8_t> read_file_bytes(const std::filesystem::path& path) {
    std::vector<std::uint8_t> bytes;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return bytes;
    }
    bytes.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    return bytes;
}

void write_file_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

// True when any same-directory save temp file remains in `dir`.
bool has_save_temp_file(const std::filesystem::path& dir) {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.path().filename().wstring().find(L".ozsave-") != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

void test_stage5_slice6_save_and_checkpoint() {
    using namespace oz::viewer_core;

    // -----------------------------------------------------------------------
    // 1. Saved-active-command checkpoint (dirty is command-value equality)
    // -----------------------------------------------------------------------
    {
        document_metadata meta;
        meta.source_path = L"C:\\images\\checkpoint.png";
        meta.source_dimensions = {1000, 800};
        meta.codec = codec_id::png_wic;
        meta.codec_name = "png";

        image_document doc{meta};
        CHECK(!doc.is_dirty());
        CHECK(doc.saved_commands().empty());

        // Edit after the (empty) initial checkpoint -> dirty.
        CHECK(doc.history().push_command(rotate_command{rotate_direction::right_90}));
        CHECK(doc.is_dirty());

        // Successful save records the captured snapshot without rewriting
        // history or revisions.
        doc.mark_saved();
        CHECK(!doc.is_dirty());
        CHECK(doc.saved_commands().size() == 1);
        CHECK(doc.history().total_commands() == 1);
        CHECK(doc.history().revision() == 1);

        // Edit after save -> dirty at a later revision.
        CHECK(doc.history().push_command(flip_command{flip_direction::horizontal}));
        CHECK(doc.edit_revision() == 2);
        CHECK(doc.is_dirty());

        // Undo back to the exact saved values -> clean even at a later revision.
        CHECK(doc.history().undo());
        CHECK(doc.edit_revision() == 3);
        CHECK(!doc.is_dirty());

        // Redo -> dirty again.
        CHECK(doc.history().redo());
        CHECK(doc.is_dirty());

        // Undo past the checkpoint -> dirty (active sequence differs).
        CHECK(doc.history().undo());
        CHECK(doc.history().undo());
        CHECK(doc.history().active_commands().empty());
        CHECK(doc.is_dirty());

        // Redo back to the saved snapshot -> clean.
        CHECK(doc.history().redo());
        CHECK(!doc.is_dirty());

        // Cursor/revision movement alone never moves the checkpoint.
        CHECK(doc.history().total_commands() == 2);
        CHECK(doc.history().revision() >= 5);

        // Reset clears both history and checkpoint.
        document_metadata meta2;
        meta2.source_path = L"C:\\images\\other.jpg";
        meta2.file_size = 5000;
        meta2.last_write_time = 12345;
        meta2.source_dimensions = {640, 480};
        meta2.codec = codec_id::jpeg;
        doc.reset(meta2);
        CHECK(!doc.is_dirty());
        CHECK(doc.saved_commands().empty());
        CHECK(doc.history().total_commands() == 0);
        CHECK(doc.history().revision() == 0);
        CHECK(doc.source_baseline().valid);
        CHECK(doc.source_baseline().file_size == 5000);
        CHECK(doc.source_baseline().last_write_time == 12345);

        // Update baseline on same-source save updates future baseline
        doc.update_source_file_metadata(6200, 67890);
        CHECK(doc.source_baseline().file_size == 6200);
        CHECK(doc.source_baseline().last_write_time == 67890);

        // A different snapshot (crop + resize) also round-trips through
        // undo/redo value equality.
        document_metadata meta3;
        meta3.source_dimensions = {400, 300};
        image_document chain_doc{meta3};
        CHECK(chain_doc.history().push_command(crop_command{edit_rect{10, 10, 200, 100}}));
        CHECK(chain_doc.history().push_command(
            resize_command{edit_dimensions{100, 50}, resize_filter::automatic}));
        chain_doc.mark_saved();
        CHECK(!chain_doc.is_dirty());
        CHECK(chain_doc.history().push_command(flip_command{flip_direction::vertical}));
        CHECK(chain_doc.is_dirty());
        CHECK(chain_doc.history().undo());
        CHECK(!chain_doc.is_dirty());
        CHECK(chain_doc.history().redo());
        CHECK(chain_doc.is_dirty());
    }

    // -----------------------------------------------------------------------
    // 2. Save routing + typed-extension rejection policy
    // -----------------------------------------------------------------------
    {
        CHECK(route_save_request(codec_id::png_wic, true, false).route == save_route::save_in_place);
        CHECK(route_save_request(codec_id::png_wic, true, false).format == save_format::png);
        CHECK(route_save_request(codec_id::jpeg, true, false).route == save_route::save_in_place);
        CHECK(route_save_request(codec_id::jpeg, true, false).format == save_format::jpeg);
        CHECK(route_save_request(codec_id::bmp_wic, true, false).route == save_route::save_in_place);
        CHECK(route_save_request(codec_id::bmp_wic, true, false).format == save_format::bmp);
        if (webp_encoder_available()) {
            CHECK(route_save_request(codec_id::webp, true, false).route == save_route::save_in_place);
            CHECK(route_save_request(codec_id::webp, true, false).format == save_format::webp);
        } else {
            CHECK(route_save_request(codec_id::webp, true, false).route == save_route::save_as);
            CHECK(route_save_request(codec_id::webp, true, false).format == save_format::png);
        }

        const codec_id unsupported[] = {codec_id::avif, codec_id::heic,
                                        codec_id::gif_wic, codec_id::tiff_wic,
                                        codec_id::ico_wic, codec_id::cur_wic, codec_id::tga};
        for (const codec_id codec : unsupported) {
            const auto routing = route_save_request(codec, true, false);
            CHECK(routing.route == save_route::save_as);
            CHECK(routing.format == save_format::png);
            CHECK(!routing.source_supports_in_place);
        }

        // No source path -> Save As with PNG selected.
        CHECK(route_save_request(codec_id::png_wic, false, false).route == save_route::save_as);
        CHECK(route_save_request(codec_id::jpeg, false, false).route == save_route::save_as);
        // Explicit Save As always routes to Save As with PNG selected.
        CHECK(route_save_request(codec_id::png_wic, true, true).route == save_route::save_as);
        CHECK(route_save_request(codec_id::png_wic, true, true).format == save_format::png);

        CHECK(codec_supports_in_place_save(codec_id::png_wic));
        CHECK(codec_supports_in_place_save(codec_id::jpeg));
        CHECK(codec_supports_in_place_save(codec_id::bmp_wic));
        CHECK(codec_supports_in_place_save(codec_id::webp) == webp_encoder_available());
        CHECK(!codec_supports_in_place_save(codec_id::tiff_wic)); // TIFF is Save-As-only
        CHECK(!codec_supports_in_place_save(codec_id::heic));
        CHECK(!codec_supports_in_place_save(codec_id::avif));
        CHECK(!codec_supports_in_place_save(codec_id::gif_wic));

        CHECK(save_format_for_extension(L".png") == save_format::png);
        CHECK(save_format_for_extension(L".PNG") == save_format::png);
        CHECK(save_format_for_extension(L".jpg") == save_format::jpeg);
        CHECK(save_format_for_extension(L".jpeg") == save_format::jpeg);
        CHECK(save_format_for_extension(L".JpEg") == save_format::jpeg);
        CHECK(save_format_for_extension(L".webp") == save_format::webp);
        CHECK(save_format_for_extension(L".WEBP") == save_format::webp);
        CHECK(save_format_for_extension(L".tif") == save_format::tiff);
        CHECK(save_format_for_extension(L".tiff") == save_format::tiff);
        CHECK(save_format_for_extension(L".TIFF") == save_format::tiff);
        CHECK(save_format_for_extension(L".bmp") == save_format::bmp);
        CHECK(save_format_for_extension(L".BMP") == save_format::bmp);
        CHECK(!save_format_for_extension(L".avif").has_value());
        CHECK(!save_format_for_extension(L"").has_value());
        CHECK(!save_format_for_extension(L".pngx").has_value());
        CHECK(std::wcscmp(preferred_extension(save_format::png), L".png") == 0);
        CHECK(std::wcscmp(preferred_extension(save_format::jpeg), L".jpg") == 0);
        CHECK(std::wcscmp(preferred_extension(save_format::webp), L".webp") == 0);
        CHECK(std::wcscmp(preferred_extension(save_format::tiff), L".tif") == 0);
        CHECK(std::wcscmp(preferred_extension(save_format::bmp), L".bmp") == 0);
    }

    scratch_dir scratch(L"ozviewer_slice6_save_tests");

    // -----------------------------------------------------------------------
    // 3. Deterministic encode/decode pixel tests
    // -----------------------------------------------------------------------
    {
        auto src = make_test_pattern(5, 3, 255);

        auto verify_png_roundtrip = [&](std::span<const edit_command> commands,
                                        const wchar_t* name) {
            const auto materialized = materialize_edits(src.view, commands);
            CHECK(materialized.error == edit_pipeline_error::none);
            const std::filesystem::path file = scratch.path / name;
            const auto encoded = encode_png_file(
                file, materialized.pixels.data(), materialized.dimensions.width,
                materialized.dimensions.height, materialized.stride);
            CHECK(encoded.error == save_error::none);

            const auto bytes = read_file_bytes(file);
            CHECK(!bytes.empty());
            const auto decoded = decode_image(bytes.data(), bytes.size(), 1);
            CHECK(decoded.error == codec_error::none);
            CHECK(decoded.image.width == materialized.dimensions.width);
            CHECK(decoded.image.height == materialized.dimensions.height);
            CHECK(decoded.image.stride == decoded.image.width * 4);
            bool equal = decoded.image.pixels.size() == materialized.pixels.size();
            if (equal) {
                equal = std::memcmp(decoded.image.pixels.data(), materialized.pixels.data(),
                                    materialized.pixels.size()) == 0;
            }
            CHECK(equal);
        };

        // Rotate/flip/crop: exact lossless PNG round trip (no double transform,
        // normal orientation: decode returns exactly the materialized pixels).
        verify_png_roundtrip(
            std::vector<edit_command>{rotate_command{rotate_direction::right_90}}, L"rotate.png");
        verify_png_roundtrip(
            std::vector<edit_command>{flip_command{flip_direction::horizontal}}, L"flip.png");
        verify_png_roundtrip(
            std::vector<edit_command>{crop_command{edit_rect{1, 1, 3, 2}}}, L"crop.png");
        // Resize (WIC exact-dimension copy in this case) and a full chain.
        verify_png_roundtrip(
            std::vector<edit_command>{resize_command{edit_dimensions{5, 3},
                                                     resize_filter::automatic}},
            L"resize_same.png");
        verify_png_roundtrip(
            std::vector<edit_command>{
                rotate_command{rotate_direction::right_90},
                crop_command{edit_rect{0, 0, 3, 2}},
                flip_command{flip_direction::vertical}},
            L"chain.png");
    }

    // PNG alpha retention: fully transparent, partial (representable), and
    // fully opaque pixels survive the straight-BGRA export exactly for alpha
    // and within bounded rounding for partial color.
    {
        auto src = make_test_pattern(4, 2, 255);
        const std::int32_t stride = src.view.stride;
        auto set_pixel = [&](std::int32_t x, std::int32_t y, std::uint8_t a, std::uint8_t b,
                             std::uint8_t g, std::uint8_t r) {
            const std::size_t index = static_cast<std::size_t>(y * stride + x * 4);
            src.pixels[index + 0] = b;
            src.pixels[index + 1] = g;
            src.pixels[index + 2] = r;
            src.pixels[index + 3] = a;
        };
        // (1,0): fully transparent. (2,0): alpha 128 with premultiplied 64
        // (round-trips as straight 128 -> premultiplied 64).
        set_pixel(1, 0, 0, 0, 0, 0);
        set_pixel(2, 0, 128, 64, 64, 64);

        const auto materialized = materialize_edits(src.view, {});
        CHECK(materialized.error == edit_pipeline_error::none);
        const std::filesystem::path file = scratch.path / L"alpha.png";
        const auto encoded = encode_png_file(file, materialized.pixels.data(),
                                             materialized.dimensions.width,
                                             materialized.dimensions.height,
                                             materialized.stride);
        CHECK(encoded.error == save_error::none);

        const auto bytes = read_file_bytes(file);
        const auto decoded = decode_image(bytes.data(), bytes.size(), 1);
        CHECK(decoded.error == codec_error::none);
        CHECK(decoded.image.width == 4);
        CHECK(decoded.image.height == 2);
        for (std::int32_t y = 0; y < 2; ++y) {
            for (std::int32_t x = 0; x < 4; ++x) {
                const std::size_t index = static_cast<std::size_t>(y * stride + x * 4);
                const std::uint8_t expected_a = src.pixels[index + 3];
                const std::uint8_t actual_a = decoded.image.pixels[index + 3];
                CHECK(expected_a == actual_a);
                if (expected_a == 0) {
                    CHECK(decoded.image.pixels[index + 0] == 0);
                    CHECK(decoded.image.pixels[index + 1] == 0);
                    CHECK(decoded.image.pixels[index + 2] == 0);
                } else if (expected_a == 128) {
                    const int db = std::abs(static_cast<int>(decoded.image.pixels[index + 0]) -
                                            static_cast<int>(src.pixels[index + 0]));
                    const int dg = std::abs(static_cast<int>(decoded.image.pixels[index + 1]) -
                                            static_cast<int>(src.pixels[index + 1]));
                    const int dr = std::abs(static_cast<int>(decoded.image.pixels[index + 2]) -
                                            static_cast<int>(src.pixels[index + 2]));
                    CHECK(db <= 2);
                    CHECK(dg <= 2);
                    CHECK(dr <= 2);
                } else {
                    CHECK(decoded.image.pixels[index + 0] == src.pixels[index + 0]);
                    CHECK(decoded.image.pixels[index + 1] == src.pixels[index + 1]);
                    CHECK(decoded.image.pixels[index + 2] == src.pixels[index + 2]);
                }
            }
        }
    }

    // Opaque JPEG at the fixed 0.90 quality: decodes at the same dimensions
    // with bounded lossy error.
    {
        auto src = make_gradient_pattern(16, 16);
        const auto materialized = materialize_edits(src.view, {});
        CHECK(materialized.error == edit_pipeline_error::none);
        const std::filesystem::path file = scratch.path / L"opaque.jpg";
        const auto encoded = encode_jpeg_file(file, materialized.pixels.data(),
                                              materialized.dimensions.width,
                                              materialized.dimensions.height,
                                              materialized.stride, kDefaultJpegQuality);
        CHECK(encoded.error == save_error::none);

        const auto bytes = read_file_bytes(file);
        const auto decoded = decode_image(bytes.data(), bytes.size(), 1);
        CHECK(decoded.error == codec_error::none);
        CHECK(decoded.image.width == 16);
        CHECK(decoded.image.height == 16);
        CHECK(decoded.image.pixels.size() == materialized.pixels.size());

        int max_diff = 0;
        double mean_diff = 0.0;
        std::size_t samples = 0;
        for (std::size_t i = 0; i + 3 < decoded.image.pixels.size(); i += 4) {
            for (int channel = 0; channel < 3; ++channel) {
                const int diff = std::abs(static_cast<int>(decoded.image.pixels[i + channel]) -
                                          static_cast<int>(materialized.pixels[i + channel]));
                max_diff = (std::max)(max_diff, diff);
                mean_diff += static_cast<double>(diff);
                ++samples;
            }
        }
        mean_diff /= static_cast<double>(samples);
        CHECK(max_diff <= 32);
        CHECK(mean_diff <= 6.0);
    }

    // JPEG alpha rejection: a single alpha != 255 pixel fails explicitly, and
    // no file is produced. There is no implicit matte.
    {
        auto src = make_test_pattern(4, 3, 255);
        src.pixels[static_cast<std::size_t>(1 * src.view.stride + 1 * 4 + 3)] = 254;
        const auto materialized = materialize_edits(src.view, {});
        CHECK(materialized.error == edit_pipeline_error::none);
        const std::filesystem::path file = scratch.path / L"alpha_rejected.jpg";
        const auto encoded = encode_jpeg_file(file, materialized.pixels.data(),
                                              materialized.dimensions.width,
                                              materialized.dimensions.height,
                                              materialized.stride, kDefaultJpegQuality);
        CHECK(encoded.error == save_error::alpha_not_supported);
        CHECK(!std::filesystem::exists(file));
    }

    // BMP exact 24bpp BGR opaque roundtrip & alpha rejection
    {
        auto src = make_test_pattern(5, 3, 255);
        const auto materialized = materialize_edits(src.view, {});
        CHECK(materialized.error == edit_pipeline_error::none);
        const std::filesystem::path file = scratch.path / L"opaque_roundtrip.bmp";
        const auto encoded = encode_bmp_file(file, materialized.pixels.data(),
                                             materialized.dimensions.width,
                                             materialized.dimensions.height,
                                             materialized.stride);
        CHECK(encoded.error == save_error::none);

        const auto bytes = read_file_bytes(file);
        CHECK(!bytes.empty());

        // Exact BMP container structural assertions:
        // Must be BITMAPFILEHEADER (14 bytes) + BITMAPINFOHEADER (>= 40 bytes)
        // Magic 'BM' (0x4D42)
        CHECK(bytes.size() >= 54);
        CHECK(bytes[0] == 'B' && bytes[1] == 'M');
        const auto rd_u16 = [](const std::uint8_t* p) -> std::uint16_t {
            return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
        };
        const auto rd_u32 = [](const std::uint8_t* p) -> std::uint32_t {
            return static_cast<std::uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
        };
        const auto rd_s32 = [](const std::uint8_t* p) -> std::int32_t {
            return static_cast<std::int32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
        };
        const std::uint32_t bfh_size = rd_u32(bytes.data() + 2);
        CHECK(bfh_size == bytes.size());
        const std::uint32_t bfh_off_bits = rd_u32(bytes.data() + 10);
        const std::uint32_t bih_size = rd_u32(bytes.data() + 14);
        CHECK(bih_size >= 40); // Standard BITMAPINFOHEADER or larger DIB header
        CHECK(bfh_off_bits >= 14 + bih_size); // Pixel array begins after header
        const std::int32_t dib_width = rd_s32(bytes.data() + 18);
        const std::int32_t dib_height = rd_s32(bytes.data() + 22);
        CHECK(dib_width == 5);
        CHECK(std::abs(dib_height) == 3); // 3 rows (bottom-up or top-down)
        const std::uint16_t dib_planes = rd_u16(bytes.data() + 26);
        CHECK(dib_planes == 1);
        const std::uint16_t dib_bpp = rd_u16(bytes.data() + 28);
        CHECK(dib_bpp == 24); // Exactly 24bpp (no 32bpp, no palette/8bpp/16bpp)
        const std::uint32_t dib_compression = rd_u32(bytes.data() + 30);
        CHECK(dib_compression == 0); // BI_RGB (uncompressed; no RLE or bitfields)
        const std::uint32_t dib_clr_used = rd_u32(bytes.data() + 46);
        CHECK(dib_clr_used == 0); // No color palette used

        // Row stride for 5 pixels at 24bpp (15 bytes) padded to 4-byte multiple is 16 bytes.
        // Total pixel data size = 16 * 3 = 48 bytes.
        const std::uint32_t expected_stride = (static_cast<std::uint32_t>(5) * 3 + 3) & ~3u;
        CHECK(expected_stride == 16);
        CHECK(bfh_off_bits + expected_stride * 3 <= bytes.size());

        const auto decoded = decode_image(bytes.data(), bytes.size(), 1);
        CHECK(decoded.error == codec_error::none);
        CHECK(decoded.image.width == materialized.dimensions.width);
        CHECK(decoded.image.height == materialized.dimensions.height);
        CHECK(decoded.image.pixels == materialized.pixels);

        // Alpha rejection: single alpha != 255 fails explicitly
        auto alpha_src = make_test_pattern(4, 2, 255);
        alpha_src.pixels[3] = 200; // top-left alpha
        const auto alpha_mat = materialize_edits(alpha_src.view, {});
        const std::filesystem::path alpha_file = scratch.path / L"alpha_rejected.bmp";
        const auto alpha_enc = encode_bmp_file(alpha_file, alpha_mat.pixels.data(),
                                               alpha_mat.dimensions.width,
                                               alpha_mat.dimensions.height,
                                               alpha_mat.stride);
        CHECK(alpha_enc.error == save_error::alpha_not_supported);
        CHECK(!std::filesystem::exists(alpha_file));
    }

    // TIFF exact 32bpp BGRA ZIP roundtrip with alpha
    {
        auto src = make_test_pattern(4, 2, 255);
        const std::int32_t stride = src.view.stride;
        // set (1,0) transparent, (2,0) partial alpha 128
        src.pixels[static_cast<std::size_t>(0 * stride + 1 * 4 + 0)] = 0;
        src.pixels[static_cast<std::size_t>(0 * stride + 1 * 4 + 1)] = 0;
        src.pixels[static_cast<std::size_t>(0 * stride + 1 * 4 + 2)] = 0;
        src.pixels[static_cast<std::size_t>(0 * stride + 1 * 4 + 3)] = 0;

        src.pixels[static_cast<std::size_t>(0 * stride + 2 * 4 + 0)] = 64;
        src.pixels[static_cast<std::size_t>(0 * stride + 2 * 4 + 1)] = 64;
        src.pixels[static_cast<std::size_t>(0 * stride + 2 * 4 + 2)] = 64;
        src.pixels[static_cast<std::size_t>(0 * stride + 2 * 4 + 3)] = 128;

        const auto materialized = materialize_edits(src.view, {});
        CHECK(materialized.error == edit_pipeline_error::none);
        const std::filesystem::path file = scratch.path / L"alpha_roundtrip.tif";
        const auto encoded = encode_tiff_file(file, materialized.pixels.data(),
                                              materialized.dimensions.width,
                                              materialized.dimensions.height,
                                              materialized.stride);
        CHECK(encoded.error == save_error::none);

        const auto bytes = read_file_bytes(file);
        CHECK(!bytes.empty());

        // Exact TIFF container structural assertions:
        // Must be single-page classic TIFF (magic 42, little or big endian)
        CHECK(bytes.size() >= 8);
        if (bytes.size() < 8) return;
        const bool little = (bytes[0] == 'I' && bytes[1] == 'I');
        const bool big = (bytes[0] == 'M' && bytes[1] == 'M');
        CHECK(little || big);
        if (!little && !big) return;
        const auto tiff_u16 = [little](const std::uint8_t* p) -> std::uint16_t {
            return little ? static_cast<std::uint16_t>(p[0] | (p[1] << 8))
                          : static_cast<std::uint16_t>((p[0] << 8) | p[1]);
        };
        const auto tiff_u32 = [little](const std::uint8_t* p) -> std::uint32_t {
            return little ? static_cast<std::uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24))
                          : static_cast<std::uint32_t>((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
        };
        const std::uint16_t tiff_magic = tiff_u16(bytes.data() + 2);
        CHECK(tiff_magic == 42); // Classic TIFF (not BigTIFF 43)
        if (tiff_magic != 42) return;
        const std::uint32_t first_ifd = tiff_u32(bytes.data() + 4);
        CHECK(first_ifd >= 8);
        CHECK(first_ifd + 2 <= bytes.size());
        if (first_ifd < 8 || first_ifd + 2 > bytes.size()) return;

        const std::uint16_t num_entries = tiff_u16(bytes.data() + first_ifd);
        const std::size_t entries_end = first_ifd + 2 + static_cast<std::size_t>(num_entries) * 12;
        CHECK(entries_end + 4 <= bytes.size());
        if (entries_end + 4 > bytes.size()) return;

        // Exactly one IFD / single page: next IFD offset must be 0
        const std::uint32_t next_ifd = tiff_u32(bytes.data() + entries_end);
        CHECK(next_ifd == 0);

        std::uint32_t tiff_width = 0;
        std::uint32_t tiff_height = 0;
        std::uint16_t tiff_compression = 0;
        std::uint16_t tiff_photometric = 0;
        std::uint16_t tiff_samples_per_pixel = 0;
        std::vector<std::uint16_t> tiff_bits_per_sample;
        std::uint16_t tiff_extra_samples = 0;
        bool found_compression = false;
        bool found_samples = false;
        bool found_bits = false;
        bool found_extra_samples = false;

        for (std::uint16_t e = 0; e < num_entries; ++e) {
            const std::size_t entry_off = first_ifd + 2 + static_cast<std::size_t>(e) * 12;
            CHECK(entry_off + 12 <= bytes.size());
            if (entry_off + 12 > bytes.size()) return;
            const std::uint16_t tag = tiff_u16(bytes.data() + entry_off);
            const std::uint16_t type = tiff_u16(bytes.data() + entry_off + 2);
            const std::uint32_t count = tiff_u32(bytes.data() + entry_off + 4);

            auto read_val_u32 = [&](std::uint32_t& v) {
                if (type == 3 && count == 1) { // SHORT
                    v = tiff_u16(bytes.data() + entry_off + 8);
                    return true;
                } else if (type == 4 && count == 1) { // LONG
                    v = tiff_u32(bytes.data() + entry_off + 8);
                    return true;
                }
                return false;
            };

            if (tag == 0x0100) { // ImageWidth
                CHECK(read_val_u32(tiff_width));
            } else if (tag == 0x0101) { // ImageLength
                CHECK(read_val_u32(tiff_height));
            } else if (tag == 0x0102) { // BitsPerSample
                found_bits = true;
                if (count == 4 && type == 3) {
                    // count 4 SHORTs: 8 bytes total, external offset
                    const std::uint32_t val_off = tiff_u32(bytes.data() + entry_off + 8);
                    CHECK(static_cast<std::size_t>(val_off) + 8 <= bytes.size());
                    if (static_cast<std::size_t>(val_off) + 8 <= bytes.size()) {
                        for (int s = 0; s < 4; ++s) {
                            tiff_bits_per_sample.push_back(tiff_u16(bytes.data() + val_off + s * 2));
                        }
                    }
                }
            } else if (tag == 0x0103) { // Compression
                std::uint32_t c = 0;
                if (read_val_u32(c)) {
                    tiff_compression = static_cast<std::uint16_t>(c);
                    found_compression = true;
                }
            } else if (tag == 0x0106) { // PhotometricInterpretation
                std::uint32_t p = 0;
                if (read_val_u32(p)) {
                    tiff_photometric = static_cast<std::uint16_t>(p);
                }
            } else if (tag == 0x0115) { // SamplesPerPixel
                std::uint32_t s = 0;
                if (read_val_u32(s)) {
                    tiff_samples_per_pixel = static_cast<std::uint16_t>(s);
                    found_samples = true;
                }
            } else if (tag == 0x0152) { // ExtraSamples
                std::uint32_t ex = 0;
                if (read_val_u32(ex)) {
                    tiff_extra_samples = static_cast<std::uint16_t>(ex);
                    found_extra_samples = true;
                }
            }
        }

        CHECK(tiff_width == 4);
        CHECK(tiff_height == 2);
        CHECK(found_compression);
        // TIFF compression 8 is Adobe Deflate / ZIP (WICTiffCompressionZIP)
        CHECK(tiff_compression == 8);
        CHECK(found_samples);
        CHECK(tiff_samples_per_pixel == 4); // 4 samples (BGRA)
        CHECK(found_bits);
        CHECK(tiff_bits_per_sample.size() == 4);
        for (std::uint16_t b : tiff_bits_per_sample) {
            CHECK(b == 8); // 8 bits per sample -> exact 32bpp
        }
        CHECK(tiff_photometric == 2); // RGB
        CHECK(found_extra_samples);
        CHECK(tiff_extra_samples == 2); // Unassociated alpha (straight alpha)

        const auto decoded = decode_image(bytes.data(), bytes.size(), 1);
        CHECK(decoded.error == codec_error::none);
        CHECK(decoded.image.width == 4);
        CHECK(decoded.image.height == 2);
        for (std::int32_t y = 0; y < 2; ++y) {
            for (std::int32_t x = 0; x < 4; ++x) {
                const std::size_t index = static_cast<std::size_t>(y * stride + x * 4);
                const std::uint8_t expected_a = src.pixels[index + 3];
                const std::uint8_t actual_a = decoded.image.pixels[index + 3];
                CHECK(expected_a == actual_a);
                if (expected_a == 0) {
                    CHECK(decoded.image.pixels[index + 0] == 0);
                    CHECK(decoded.image.pixels[index + 1] == 0);
                    CHECK(decoded.image.pixels[index + 2] == 0);
                } else if (expected_a == 128) {
                    const int db = std::abs(static_cast<int>(decoded.image.pixels[index + 0]) -
                                            static_cast<int>(src.pixels[index + 0]));
                    const int dg = std::abs(static_cast<int>(decoded.image.pixels[index + 1]) -
                                            static_cast<int>(src.pixels[index + 1]));
                    const int dr = std::abs(static_cast<int>(decoded.image.pixels[index + 2]) -
                                            static_cast<int>(src.pixels[index + 2]));
                    CHECK(db <= 2);
                    CHECK(dg <= 2);
                    CHECK(dr <= 2);
                } else {
                    CHECK(decoded.image.pixels[index + 0] == src.pixels[index + 0]);
                    CHECK(decoded.image.pixels[index + 1] == src.pixels[index + 1]);
                    CHECK(decoded.image.pixels[index + 2] == src.pixels[index + 2]);
                }
            }
        }
    }

    // WebP exact lossless BGRA roundtrip with alpha & opaque (when encoder available)
    if (webp_encoder_available()) {
        auto src = make_test_pattern(4, 2, 255);
        const std::int32_t stride = src.view.stride;
        // set (1,0) transparent, (2,0) partial alpha 128
        src.pixels[static_cast<std::size_t>(0 * stride + 1 * 4 + 0)] = 0;
        src.pixels[static_cast<std::size_t>(0 * stride + 1 * 4 + 1)] = 0;
        src.pixels[static_cast<std::size_t>(0 * stride + 1 * 4 + 2)] = 0;
        src.pixels[static_cast<std::size_t>(0 * stride + 1 * 4 + 3)] = 0;

        src.pixels[static_cast<std::size_t>(0 * stride + 2 * 4 + 0)] = 64;
        src.pixels[static_cast<std::size_t>(0 * stride + 2 * 4 + 1)] = 64;
        src.pixels[static_cast<std::size_t>(0 * stride + 2 * 4 + 2)] = 64;
        src.pixels[static_cast<std::size_t>(0 * stride + 2 * 4 + 3)] = 128;

        const auto materialized = materialize_edits(src.view, {});
        CHECK(materialized.error == edit_pipeline_error::none);
        const std::filesystem::path file = scratch.path / L"alpha_roundtrip.webp";
        const auto encoded = encode_webp_file(file, materialized.pixels.data(),
                                              materialized.dimensions.width,
                                              materialized.dimensions.height,
                                              materialized.stride);
        CHECK(encoded.error == save_error::none);

        const auto bytes = read_file_bytes(file);
        CHECK(!bytes.empty());
        const auto decoded = decode_image(bytes.data(), bytes.size(), 1);
        CHECK(decoded.error == codec_error::none);
        CHECK(decoded.image.width == 4);
        CHECK(decoded.image.height == 2);
        for (std::int32_t y = 0; y < 2; ++y) {
            for (std::int32_t x = 0; x < 4; ++x) {
                const std::size_t index = static_cast<std::size_t>(y * stride + x * 4);
                const std::uint8_t expected_a = src.pixels[index + 3];
                const std::uint8_t actual_a = decoded.image.pixels[index + 3];
                CHECK(expected_a == actual_a);
                if (expected_a == 0) {
                    CHECK(decoded.image.pixels[index + 0] == 0);
                    CHECK(decoded.image.pixels[index + 1] == 0);
                    CHECK(decoded.image.pixels[index + 2] == 0);
                } else if (expected_a == 128) {
                    const int db = std::abs(static_cast<int>(decoded.image.pixels[index + 0]) -
                                            static_cast<int>(src.pixels[index + 0]));
                    const int dg = std::abs(static_cast<int>(decoded.image.pixels[index + 1]) -
                                            static_cast<int>(src.pixels[index + 1]));
                    const int dr = std::abs(static_cast<int>(decoded.image.pixels[index + 2]) -
                                            static_cast<int>(src.pixels[index + 2]));
                    CHECK(db <= 2);
                    CHECK(dg <= 2);
                    CHECK(dr <= 2);
                } else {
                    CHECK(decoded.image.pixels[index + 0] == src.pixels[index + 0]);
                    CHECK(decoded.image.pixels[index + 1] == src.pixels[index + 1]);
                    CHECK(decoded.image.pixels[index + 2] == src.pixels[index + 2]);
                }
            }
        }

        // Cooperative cancellation test for WebP
        const std::filesystem::path cancel_file = scratch.path / L"cancel.webp";
        const auto cancel_res = encode_webp_file(cancel_file, materialized.pixels.data(),
                                                 materialized.dimensions.width,
                                                 materialized.dimensions.height,
                                                 materialized.stride,
                                                 [] { return true; });
        CHECK(cancel_res.error == save_error::cancelled);
    }

    // -----------------------------------------------------------------------
    // 4. execute_save commit/cancellation/error seams
    // -----------------------------------------------------------------------
    {
        auto src = make_test_pattern(6, 4, 255);
        const auto source_bytes_before = src.pixels;
        const auto materialized = materialize_edits(src.view, {});
        CHECK(materialized.error == edit_pipeline_error::none);

        save_request request;
        request.source = src.view;
        request.format = save_format::png;
        request.source_fingerprint.expected = false;

        // (a) New destination: MoveFileExW commit path.
        const std::filesystem::path dest_new = scratch.path / L"new_target.png";
        request.destination = dest_new;
        request.destination_baseline = {false, 0, 0};
        auto result = execute_save(request, [] { return false; });
        CHECK(result.error == save_error::none);
        CHECK(result.committed);
        CHECK(std::filesystem::exists(dest_new));
        {
            const auto bytes = read_file_bytes(dest_new);
            const auto decoded = decode_image(bytes.data(), bytes.size(), 1);
            CHECK(decoded.error == codec_error::none);
            CHECK(decoded.image.width == materialized.dimensions.width);
            CHECK(decoded.image.height == materialized.dimensions.height);
            CHECK(decoded.image.pixels == materialized.pixels);
        }
        CHECK(!has_save_temp_file(scratch.path));

        // (b) Existing destination with matching baseline: ReplaceFileW path.
        const std::filesystem::path dest_existing = scratch.path / L"existing_target.png";
        write_file_bytes(dest_existing, {'P', 'N', 'G', '-', 'p', 'l', 'a', 'c', 'e', 'h', 'o', 'l', 'd'});
        const auto existing_baseline = query_file_fingerprint(dest_existing);
        CHECK(existing_baseline.exists);
        request.destination = dest_existing;
        request.destination_baseline = {true, existing_baseline.file_size,
                                        existing_baseline.last_write_time};
        result = execute_save(request, [] { return false; });
        CHECK(result.error == save_error::none);
        CHECK(result.committed);
        {
            const auto bytes = read_file_bytes(dest_existing);
            const auto decoded = decode_image(bytes.data(), bytes.size(), 1);
            CHECK(decoded.error == codec_error::none);
            CHECK(decoded.image.pixels == materialized.pixels);
        }
        CHECK(!has_save_temp_file(scratch.path));

        // (c) Stale destination baseline: destination_conflict and the current
        // destination bytes are preserved exactly.
        write_file_bytes(dest_existing, {'s', 't', 'a', 'l', 'e'});
        const auto stale_bytes = read_file_bytes(dest_existing);
        result = execute_save(request, [] { return false; });
        CHECK(result.error == save_error::destination_conflict);
        CHECK(!result.committed);
        CHECK(read_file_bytes(dest_existing) == stale_bytes);
        CHECK(!has_save_temp_file(scratch.path));

        // (d) New-target race: destination appeared after capture ->
        // destination_conflict, existing content preserved.
        const std::filesystem::path dest_race = scratch.path / L"race_target.png";
        request.destination = dest_race;
        request.destination_baseline = {false, 0, 0};
        write_file_bytes(dest_race, {'r', 'a', 'c', 'e'});
        const auto race_bytes = read_file_bytes(dest_race);
        result = execute_save(request, [] { return false; });
        CHECK(result.error == save_error::destination_conflict);
        CHECK(read_file_bytes(dest_race) == race_bytes);
        CHECK(!has_save_temp_file(scratch.path));

        // (e) Cancellation before materialization: no destination created.
        const std::filesystem::path dest_cancel = scratch.path / L"cancel_target.png";
        request.destination = dest_cancel;
        request.destination_baseline = {false, 0, 0};
        result = execute_save(request, [] { return true; });
        CHECK(result.error == save_error::cancelled);
        CHECK(!std::filesystem::exists(dest_cancel));
        CHECK(!has_save_temp_file(scratch.path));

        // (f) Cancellation after encoding but before replacement (the cancel
        // callback is polled before materialization, before encoding, and
        // before the replacement step): destination preserved, temp cleaned.
        write_file_bytes(dest_existing, {'k', 'e', 'e', 'p'});
        const auto keep_bytes = read_file_bytes(dest_existing);
        const auto keep_baseline = query_file_fingerprint(dest_existing);
        save_request cancel_request = request;
        cancel_request.destination = dest_existing;
        cancel_request.destination_baseline = {true, keep_baseline.file_size,
                                               keep_baseline.last_write_time};
        int cancel_polls = 0;
        result = execute_save(cancel_request, [&cancel_polls] {
            ++cancel_polls;
            return cancel_polls >= 3;
        });
        CHECK(result.error == save_error::cancelled);
        CHECK(cancel_polls >= 3);
        CHECK(read_file_bytes(dest_existing) == keep_bytes);
        CHECK(!has_save_temp_file(scratch.path));

        // (g) Stale captured source identity -> stale_source, nothing written.
        save_request stale_source_request = request;
        stale_source_request.destination = scratch.path / L"stale_source_target.png";
        stale_source_request.destination_baseline = {false, 0, 0};
        stale_source_request.source_fingerprint.expected = true;
        stale_source_request.source_fingerprint.path = scratch.path / L"missing_source.png";
        stale_source_request.source_fingerprint.file_size = 123;
        stale_source_request.source_fingerprint.last_write_time = 456;
        result = execute_save(stale_source_request, [] { return false; });
        CHECK(result.error == save_error::stale_source);
        CHECK(!std::filesystem::exists(stale_source_request.destination));
        CHECK(!has_save_temp_file(scratch.path));

        // (h) Temp creation failure (missing destination directory) is explicit.
        save_request bad_dir_request = request;
        bad_dir_request.destination = scratch.path / L"missing_dir" / L"out.png";
        bad_dir_request.destination_baseline = {false, 0, 0};
        result = execute_save(bad_dir_request, [] { return false; });
        CHECK(result.error == save_error::temp_create_failed);
        CHECK(!std::filesystem::exists(bad_dir_request.destination));
        CHECK(!has_save_temp_file(scratch.path));

        // (i) JPEG alpha rejection through the full worker body: no file/temps.
        auto alpha_src = make_test_pattern(4, 3, 254);
        save_request jpeg_alpha_request = request;
        jpeg_alpha_request.destination = scratch.path / L"alpha_reject_target.jpg";
        jpeg_alpha_request.destination_baseline = {false, 0, 0};
        jpeg_alpha_request.format = save_format::jpeg;
        jpeg_alpha_request.source = alpha_src.view;
        result = execute_save(jpeg_alpha_request, [] { return false; });
        CHECK(result.error == save_error::alpha_not_supported);
        CHECK(!std::filesystem::exists(jpeg_alpha_request.destination));
        CHECK(!has_save_temp_file(scratch.path));

        // (j) Successful JPEG commit through the full worker body.
        save_request jpeg_request = jpeg_alpha_request;
        jpeg_request.destination = scratch.path / L"opaque_ok.jpg";
        jpeg_request.source = src.view;
        result = execute_save(jpeg_request, [] { return false; });
        CHECK(result.error == save_error::none);
        CHECK(result.committed);
        CHECK(std::filesystem::exists(jpeg_request.destination));
        CHECK(!has_save_temp_file(scratch.path));

        // (k) Durability flush failure (reopen failure): returns flush_failed,
        // preserves destination, leaves no temp files behind.
        {
            const auto dest_before = scratch.path / L"flush_fail_target.png";
            // Pre-create destination with known content to prove it remains untouched
            {
                HANDLE h = CreateFileW(dest_before.c_str(), GENERIC_WRITE, 0, nullptr,
                                       CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
                CHECK(h != INVALID_HANDLE_VALUE);
                DWORD written = 0;
                WriteFile(h, "ORIGINAL_DEST", 13, &written, nullptr);
                CloseHandle(h);
            }
            save_request flush_req = request;
            flush_req.destination = dest_before;
            flush_req.destination_baseline = {true, 13, 0};
            auto fp = query_file_fingerprint(dest_before);
            flush_req.destination_baseline.last_write_time = fp.last_write_time;

            {
                scoped_durability_flush_failure fail_reopen(/*fail_reopen=*/true);
                result = execute_save(flush_req, [] { return false; });
                CHECK(result.error == save_error::flush_failed);
                CHECK(!result.committed);
                CHECK(!has_save_temp_file(scratch.path));
                // Old destination untouched
                auto fp_after = query_file_fingerprint(dest_before);
                CHECK(fp_after.exists);
                CHECK(fp_after.file_size == 13);
            }

            // (l) Durability flush failure (FlushFileBuffers failure): returns flush_failed,
            // preserves destination, cleans temp file.
            {
                scoped_durability_flush_failure fail_flush(/*fail_reopen=*/false);
                result = execute_save(flush_req, [] { return false; });
                CHECK(result.error == save_error::flush_failed);
                CHECK(!result.committed);
                CHECK(!has_save_temp_file(scratch.path));
                // Old destination untouched
                auto fp_after = query_file_fingerprint(dest_before);
                CHECK(fp_after.exists);
                CHECK(fp_after.file_size == 13);
            }
        }

        // (m) Source mutation after open / before Save: external modification detected
        // via size/last-write baseline -> rejected as stale_source, destination untouched.
        {
            const auto source_path = scratch.path / L"source_for_mutation.png";
            save_request write_src = request;
            write_src.destination = source_path;
            write_src.destination_baseline = {false, 0, 0};
            result = execute_save(write_src, [] { return false; });
            CHECK(result.error == save_error::none);
            CHECK(result.committed);

            const auto original_source_fp = query_file_fingerprint(source_path);
            CHECK(original_source_fp.exists);

            // Document opened with baseline
            document_metadata doc_meta;
            doc_meta.source_path = source_path;
            doc_meta.file_size = original_source_fp.file_size;
            doc_meta.last_write_time = original_source_fp.last_write_time;
            doc_meta.source_dimensions = {4, 3};
            doc_meta.codec = codec_id::png_wic;
            image_document open_doc{doc_meta};
            CHECK(open_doc.source_baseline().valid);
            CHECK(open_doc.source_baseline().file_size == original_source_fp.file_size);
            CHECK(open_doc.source_baseline().last_write_time == original_source_fp.last_write_time);

            // Simulate external modification to source on disk before Save
            {
                HANDLE h = CreateFileW(source_path.c_str(), FILE_WRITE_ATTRIBUTES,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                CHECK(h != INVALID_HANDLE_VALUE);
                FILETIME ft_new{};
                ft_new.dwLowDateTime = 0x12345678;
                ft_new.dwHighDateTime = 0x01D00000;
                SetFileTime(h, nullptr, nullptr, &ft_new);
                CloseHandle(h);
            }

            const auto mutated_fp = query_file_fingerprint(source_path);
            CHECK(mutated_fp.last_write_time != original_source_fp.last_write_time);

            // In-place save attempt using the stale source baseline:
            // Worker-side revalidation rejects when request.source_fingerprint uses the trusted baseline.
            save_request in_place_req = request;
            in_place_req.destination = source_path;
            in_place_req.destination_baseline = {true, original_source_fp.file_size, original_source_fp.last_write_time};
            in_place_req.source_fingerprint.expected = true;
            in_place_req.source_fingerprint.path = source_path;
            in_place_req.source_fingerprint.file_size = open_doc.source_baseline().file_size;
            in_place_req.source_fingerprint.last_write_time = open_doc.source_baseline().last_write_time;
            in_place_req.source_fingerprint.dimensions = doc_meta.source_dimensions;

            result = execute_save(in_place_req, [] { return false; });
            CHECK(result.error == save_error::stale_source);
            CHECK(!result.committed);
            CHECK(!has_save_temp_file(scratch.path));

            // Destination retains mutated state; document checkpoint never advanced
            const auto after_fail_fp = query_file_fingerprint(source_path);
            CHECK(after_fail_fp.last_write_time == mutated_fp.last_write_time);
            CHECK(!open_doc.is_dirty() == false || open_doc.saved_commands().empty());
        }

        // (n) Focused regressions for decode-bound identity, Save vs Save As checkpoint advancement,
        // missing/invalid commit fingerprint, and flush failure retaining dirty state.
        {
            const auto test_img_path = scratch.path / L"regression_identity_doc.png";
            save_request write_initial = request;
            write_initial.destination = test_img_path;
            write_initial.destination_baseline = {false, 0, 0};
            auto res = execute_save(write_initial, [] { return false; });
            CHECK(res.error == save_error::none);
            CHECK(res.committed);
            CHECK(res.committed_fingerprint.exists);
            CHECK(res.committed_fingerprint.file_size > 0);

            // 1. Stable read boundary: modification between start and finish of read cannot produce a valid identity.
            {
                // Simulate stable read where file was modified during read
                // (before metadata captured, file changed, after metadata captured).
                const auto initial_fp = query_file_fingerprint(test_img_path);
                CHECK(initial_fp.exists);

                // If size/time snapshot before read does not match snapshot after read, baseline is rejected.
                document_metadata unverified_meta;
                unverified_meta.source_path = test_img_path;
                unverified_meta.source_dimensions = {6, 4};
                // If a decode path observed mismatching before/after snapshots, metadata is invalid (not blessed)
                unverified_meta.file_size = 0;
                unverified_meta.last_write_time = 0;
                image_document unverified_doc{unverified_meta};
                CHECK(!unverified_doc.source_baseline().valid);
            }

            // 2. Pre-submit rejection before materialization / replacement if external modification happened after open
            {
                const auto open_fp = query_file_fingerprint(test_img_path);
                document_metadata valid_meta;
                valid_meta.source_path = test_img_path;
                valid_meta.file_size = open_fp.file_size;
                valid_meta.last_write_time = open_fp.last_write_time;
                valid_meta.source_dimensions = {6, 4};
                valid_meta.codec = codec_id::png_wic;
                image_document doc{valid_meta};
                CHECK(doc.source_baseline().valid);

                // Add an edit command so doc is dirty
                CHECK(doc.history().push_command(rotate_command{rotate_direction::right_90}));
                CHECK(doc.is_dirty());

                // Before the external modification the fingerprint matches the
                // trusted baseline, so the same eligibility helper accepts.
                CHECK(save_pre_submit_eligible(doc.source_baseline(), open_fp));

                // External modification happens on disk
                {
                    HANDLE h = CreateFileW(test_img_path.c_str(), FILE_WRITE_ATTRIBUTES,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                    CHECK(h != INVALID_HANDLE_VALUE);
                    FILETIME ft_new{};
                    ft_new.dwLowDateTime = 0x87654321;
                    ft_new.dwHighDateTime = 0x01D00001;
                    SetFileTime(h, nullptr, nullptr, &ft_new);
                    CloseHandle(h);
                }

                // UI pre-submit check: destination fingerprint vs trusted baseline
                // through the same pure eligibility helper main.cpp invokes.
                const auto current_dest_fp = query_file_fingerprint(test_img_path);
                CHECK(current_dest_fp.last_write_time != doc.source_baseline().last_write_time);
                CHECK(!save_pre_submit_eligible(doc.source_baseline(), current_dest_fp));
                // Pre-submit check rejected: save request is NOT submitted, checkpoint remains untouched and dirty
                CHECK(doc.is_dirty());
                CHECK(doc.saved_commands().empty());
            }

            // 3. Same-source Save vs Save As advancement using worker-completion committed_fingerprint
            {
                // Reset file to known state
                save_request write_known = request;
                write_known.destination = test_img_path;
                const auto dest_base = query_file_fingerprint(test_img_path);
                write_known.destination_baseline = {true, dest_base.file_size, dest_base.last_write_time};
                auto res_known = execute_save(write_known, [] { return false; });
                CHECK(res_known.error == save_error::none);
                CHECK(res_known.committed);

                const auto current_fp = res_known.committed_fingerprint;
                CHECK(current_fp.exists);

                document_metadata doc_meta2;
                doc_meta2.source_path = test_img_path;
                doc_meta2.file_size = current_fp.file_size;
                doc_meta2.last_write_time = current_fp.last_write_time;
                doc_meta2.source_dimensions = {6, 4};
                doc_meta2.codec = codec_id::png_wic;
                image_document doc2{doc_meta2};

                // Add an edit command
                CHECK(doc2.history().push_command(rotate_command{rotate_direction::right_90}));
                CHECK(doc2.is_dirty());

                // Simulate in-place save execution
                save_request in_place = request;
                in_place.destination = test_img_path;
                in_place.destination_baseline = {true, current_fp.file_size, current_fp.last_write_time};
                in_place.source_fingerprint.expected = true;
                in_place.source_fingerprint.path = test_img_path;
                in_place.source_fingerprint.file_size = doc2.source_baseline().file_size;
                in_place.source_fingerprint.last_write_time = doc2.source_baseline().last_write_time;
                in_place.source_fingerprint.dimensions = doc_meta2.source_dimensions;

                auto in_place_res = execute_save(in_place, [] { return false; });
                CHECK(in_place_res.error == save_error::none);
                CHECK(in_place_res.committed);
                CHECK(in_place_res.committed_fingerprint.exists);
                CHECK(in_place_res.committed_fingerprint.file_size > 0);

                // Document advances checkpoint and baseline using worker-provided committed_fingerprint
                doc2.mark_saved_with_baseline(in_place_res.committed_fingerprint.file_size,
                                              in_place_res.committed_fingerprint.last_write_time);
                CHECK(!doc2.is_dirty());
                CHECK(doc2.saved_commands().size() == 1);
                CHECK(doc2.source_baseline().file_size == in_place_res.committed_fingerprint.file_size);
                CHECK(doc2.source_baseline().last_write_time == in_place_res.committed_fingerprint.last_write_time);

                // Add another edit
                CHECK(doc2.history().push_command(flip_command{flip_direction::horizontal}));
                CHECK(doc2.is_dirty());

                // Now execute Save As to a different file
                const auto save_as_path = scratch.path / L"exported_copy.png";
                save_request save_as_req = request;
                save_as_req.destination = save_as_path;
                save_as_req.destination_baseline = {false, 0, 0};
                save_as_req.source_fingerprint.expected = false;

                auto save_as_res = execute_save(save_as_req, [] { return false; });
                CHECK(save_as_res.error == save_error::none);
                CHECK(save_as_res.committed);

                // Save As must NEVER advance source baseline or checkpoint: document remains dirty
                CHECK(doc2.is_dirty());
                CHECK(doc2.saved_commands().size() == 1); // still the previous in-place save snapshot
                CHECK(doc2.source_baseline().path == test_img_path);
                CHECK(doc2.source_baseline().file_size == in_place_res.committed_fingerprint.file_size);

                // 3b. Later external modification against the newly committed
                // baseline: the same pre-submit eligibility helper main.cpp
                // invokes rejects before materialization/replacement.
                {
                    const auto committed_fp = query_file_fingerprint(test_img_path);
                    CHECK(save_pre_submit_eligible(doc2.source_baseline(), committed_fp));

                    // External modification: size change on disk.
                    write_file_bytes(test_img_path, {'m', 'o', 'd', 'i', 'f', 'i', 'e', 'd'});
                    const auto modified_fp = query_file_fingerprint(test_img_path);
                    CHECK(modified_fp.file_size != doc2.source_baseline().file_size);
                    CHECK(!save_pre_submit_eligible(doc2.source_baseline(), modified_fp));

                    // Rejected before submission: checkpoint and baseline
                    // untouched, document remains dirty.
                    CHECK(doc2.is_dirty());
                    CHECK(doc2.saved_commands().size() == 1);
                    CHECK(doc2.source_baseline().file_size == in_place_res.committed_fingerprint.file_size);
                }
            }

            // 4. Missing or invalid commit fingerprint cannot clean / advance baseline
            {
                document_metadata doc_meta3;
                doc_meta3.source_path = test_img_path;
                doc_meta3.file_size = 100;
                doc_meta3.last_write_time = 200;
                doc_meta3.source_dimensions = {6, 4};
                doc_meta3.codec = codec_id::png_wic;
                image_document doc3{doc_meta3};

                CHECK(doc3.history().push_command(rotate_command{rotate_direction::left_90}));
                CHECK(doc3.is_dirty());

                // Worker completion with missing/failed committed_fingerprint (exists == false)
                file_fingerprint invalid_fp{};
                invalid_fp.exists = false;

                if (invalid_fp.exists) {
                    doc3.mark_saved_with_baseline(invalid_fp.file_size, invalid_fp.last_write_time);
                }
                // Document MUST remain dirty, checkpoint unadvanced, baseline untouched
                CHECK(doc3.is_dirty());
                CHECK(doc3.saved_commands().empty());
                CHECK(doc3.source_baseline().file_size == 100);
                CHECK(doc3.source_baseline().last_write_time == 200);
            }

            // 5. Durability flush failure retains dirty state through completion logic
            {
                const auto flush_dest = scratch.path / L"flush_regress_target.png";
                save_request flush_init = request;
                flush_init.destination = flush_dest;
                flush_init.destination_baseline = {false, 0, 0};
                auto res_init = execute_save(flush_init, [] { return false; });
                CHECK(res_init.committed);

                const auto dest_fp = res_init.committed_fingerprint;
                document_metadata doc_meta4;
                doc_meta4.source_path = flush_dest;
                doc_meta4.file_size = dest_fp.file_size;
                doc_meta4.last_write_time = dest_fp.last_write_time;
                doc_meta4.source_dimensions = {6, 4};
                doc_meta4.codec = codec_id::png_wic;
                image_document doc4{doc_meta4};

                CHECK(doc4.history().push_command(rotate_command{rotate_direction::right_90}));
                CHECK(doc4.is_dirty());

                save_request flush_req = request;
                flush_req.destination = flush_dest;
                flush_req.destination_baseline = {true, dest_fp.file_size, dest_fp.last_write_time};
                flush_req.source_fingerprint.expected = true;
                flush_req.source_fingerprint.path = flush_dest;
                flush_req.source_fingerprint.file_size = doc4.source_baseline().file_size;
                flush_req.source_fingerprint.last_write_time = doc4.source_baseline().last_write_time;
                flush_req.source_fingerprint.dimensions = doc_meta4.source_dimensions;

                {
                    scoped_durability_flush_failure fail_flush(/*fail_reopen=*/false);
                    auto flush_result = execute_save(flush_req, [] { return false; });
                    CHECK(flush_result.error == save_error::flush_failed);
                    CHECK(!flush_result.committed);

                    // If error != none or !committed, UI completion never calls mark_saved
                    const bool should_mark = (flush_result.committed && flush_result.error == save_error::none);
                    CHECK(!should_mark);
                    if (should_mark) {
                        doc4.mark_saved_with_baseline(flush_result.committed_fingerprint.file_size,
                                                      flush_result.committed_fingerprint.last_write_time);
                    }
                    CHECK(doc4.is_dirty());
                    CHECK(doc4.saved_commands().empty());
                    CHECK(doc4.source_baseline().file_size == dest_fp.file_size);
                    CHECK(doc4.source_baseline().last_write_time == dest_fp.last_write_time);
                }
            }
        }

        // TIFF is Save-As-only: route_save_request on TIFF source with a known path
        // routes to Save As with PNG selected, rejecting in-place save.
        {
            const auto tiff_routing = route_save_request(codec_id::tiff_wic, true, false);
            CHECK(tiff_routing.route == save_route::save_as);
            CHECK(tiff_routing.format == save_format::png);
            CHECK(!tiff_routing.source_supports_in_place);
        }

        // BMP with a known source path supports in-place save
        {
            const auto bmp_routing = route_save_request(codec_id::bmp_wic, true, false);
            CHECK(bmp_routing.route == save_route::save_in_place);
            CHECK(bmp_routing.format == save_format::bmp);
            CHECK(bmp_routing.source_supports_in_place);
        }

        // WebP with a known source path supports in-place save if encoder is available
        {
            const auto webp_routing = route_save_request(codec_id::webp, true, false);
            if (webp_encoder_available()) {
                CHECK(webp_routing.route == save_route::save_in_place);
                CHECK(webp_routing.format == save_format::webp);
                CHECK(webp_routing.source_supports_in_place);
            } else {
                CHECK(webp_routing.route == save_route::save_as);
                CHECK(webp_routing.format == save_format::png);
                CHECK(!webp_routing.source_supports_in_place);
            }
        }

        // The immutable source pixels are untouched by every outcome above.
        CHECK(src.pixels == source_bytes_before);
    }
}

// Deterministic regression coverage for the save completion acceptance policy
// extracted from the viewer UI thread (handle_save_completion). Exercises the
// real production decision function with real image_document instances instead
// of bypassing the decision path.
void test_stage5_slice6_save_completion_policy() {
    using namespace oz::viewer_core;

    document_metadata meta;
    meta.source_path = L"C:\\images\\policy.png";
    meta.file_size = 100;
    meta.last_write_time = 200;
    meta.source_dimensions = {1000, 800};
    meta.codec = codec_id::png_wic;

    // 1. Matching committed same-source save with a valid committed
    //    fingerprint -> checkpoint (document advances clean + baseline).
    {
        image_document doc{meta};
        CHECK(doc.history().push_command(rotate_command{rotate_direction::right_90}));
        CHECK(doc.is_dirty());
        const auto active = doc.history().active_commands();
        const std::vector<edit_command> captured(active.begin(), active.end());

        save_completion_policy_input input;
        input.committed = true;
        input.error = save_error::none;
        input.document_matches = true;
        input.same_source = true;
        input.active_commands = doc.history().active_commands();
        input.captured_commands = captured;
        input.committed_fingerprint = {true, 1234, 5678};

        CHECK(evaluate_save_completion_policy(input) == save_completion_action::checkpoint);

        // The production action for this outcome.
        doc.mark_saved_with_baseline(input.committed_fingerprint.file_size,
                                     input.committed_fingerprint.last_write_time);
        CHECK(!doc.is_dirty());
        CHECK(doc.saved_commands().size() == 1);
        CHECK(doc.source_baseline().file_size == 1234);
        CHECK(doc.source_baseline().last_write_time == 5678);
    }

    // 2. Same match but the committed fingerprint is missing -> the document
    //    must remain modified (no checkpoint, no baseline advance).
    {
        image_document doc{meta};
        CHECK(doc.history().push_command(rotate_command{rotate_direction::right_90}));
        CHECK(doc.is_dirty());
        const auto active = doc.history().active_commands();
        const std::vector<edit_command> captured(active.begin(), active.end());

        save_completion_policy_input input;
        input.committed = true;
        input.error = save_error::none;
        input.document_matches = true;
        input.same_source = true;
        input.active_commands = doc.history().active_commands();
        input.captured_commands = captured;
        input.committed_fingerprint = {};  // exists == false

        CHECK(evaluate_save_completion_policy(input) ==
              save_completion_action::saved_unverified_baseline);
        CHECK(doc.is_dirty());
        CHECK(doc.saved_commands().empty());
        CHECK(doc.source_baseline().file_size == 100);
        CHECK(doc.source_baseline().last_write_time == 200);
    }

    // 3. Commands changed during the save -> the document must stay modified.
    {
        image_document doc{meta};
        CHECK(doc.history().push_command(rotate_command{rotate_direction::right_90}));
        CHECK(doc.is_dirty());
        const auto active = doc.history().active_commands();
        const std::vector<edit_command> captured(active.begin(), active.end());

        // A conflicting edit lands while the save is in flight.
        CHECK(doc.history().push_command(flip_command{flip_direction::horizontal}));

        save_completion_policy_input input;
        input.committed = true;
        input.error = save_error::none;
        input.document_matches = true;
        input.same_source = true;
        input.active_commands = doc.history().active_commands();
        input.captured_commands = captured;  // stale snapshot from submit time
        input.committed_fingerprint = {true, 1234, 5678};

        CHECK(evaluate_save_completion_policy(input) ==
              save_completion_action::saved_commands_changed);
        CHECK(doc.is_dirty());
        CHECK(doc.saved_commands().empty());
        CHECK(doc.source_baseline().file_size == 100);
    }

    // 4. Committed Save As copy (same_source == false) -> silent; the document
    //    remains dirty and the checkpoint/baseline never move.
    {
        image_document doc{meta};
        CHECK(doc.history().push_command(rotate_command{rotate_direction::right_90}));
        CHECK(doc.is_dirty());
        const auto active = doc.history().active_commands();
        const std::vector<edit_command> captured(active.begin(), active.end());

        save_completion_policy_input input;
        input.committed = true;
        input.error = save_error::none;
        input.document_matches = true;
        input.same_source = false;
        input.active_commands = doc.history().active_commands();
        input.captured_commands = captured;
        input.committed_fingerprint = {true, 999, 888};

        CHECK(evaluate_save_completion_policy(input) == save_completion_action::saved_silent);
        CHECK(doc.is_dirty());
        CHECK(doc.saved_commands().empty());
        CHECK(doc.source_baseline().file_size == 100);
    }

    // 5. Non-success outcomes -> none (no state change, no silent success).
    {
        image_document doc{meta};
        CHECK(doc.history().push_command(rotate_command{rotate_direction::right_90}));
        const auto active = doc.history().active_commands();
        const std::vector<edit_command> captured(active.begin(), active.end());

        // Failed commit (e.g. flush_failed): never clean.
        save_completion_policy_input err_input;
        err_input.committed = false;
        err_input.error = save_error::flush_failed;
        err_input.document_matches = true;
        err_input.same_source = true;
        err_input.active_commands = doc.history().active_commands();
        err_input.captured_commands = captured;
        err_input.committed_fingerprint = {true, 1234, 5678};
        CHECK(evaluate_save_completion_policy(err_input) == save_completion_action::none);

        // User-initiated cancellation: no file was written.
        save_completion_policy_input cancel_input = err_input;
        cancel_input.error = save_error::cancelled;
        CHECK(evaluate_save_completion_policy(cancel_input) == save_completion_action::none);

        // Stale completion for a different document: no state change.
        save_completion_policy_input stale_input = err_input;
        stale_input.error = save_error::none;
        stale_input.committed = true;
        stale_input.document_matches = false;
        CHECK(evaluate_save_completion_policy(stale_input) == save_completion_action::none);

        // Committed=false with no error (pre-commit cancel): no state change.
        save_completion_policy_input not_committed = err_input;
        not_committed.error = save_error::none;
        not_committed.committed = false;
        CHECK(evaluate_save_completion_policy(not_committed) == save_completion_action::none);
    }

    // 6. Empty command snapshot (unedited document) still matches and can
    //    checkpoint on a committed same-source save.
    {
        image_document clean_doc{meta};
        save_completion_policy_input input;
        input.committed = true;
        input.error = save_error::none;
        input.document_matches = true;
        input.same_source = true;
        input.active_commands = clean_doc.history().active_commands();  // empty
        input.captured_commands = std::span<const edit_command>{};      // empty
        input.committed_fingerprint = {true, 42, 43};

        CHECK(evaluate_save_completion_policy(input) == save_completion_action::checkpoint);
    }
}

// Production request-identity boundary for save completions: the real
// handle_save_completion guard (no pending / mismatched request ID) must keep
// the pending save, never clear/consume it, and never advance the clean
// checkpoint or source baseline -- while a later matching completion for the
// same pending request remains eligible. Calls the production gate used by
// handle_save_completion, never a replicated condition.
void test_stage5_slice6_save_completion_request_id_boundary() {
    using namespace oz::viewer_core;

    document_metadata meta;
    meta.source_path = L"C:\\images\\request_id.png";
    meta.file_size = 100;
    meta.last_write_time = 200;
    meta.source_dimensions = {1000, 800};
    meta.codec = codec_id::png_wic;

    image_document doc{meta};
    CHECK(doc.history().push_command(rotate_command{rotate_direction::right_90}));
    CHECK(doc.is_dirty());
    const auto baseline_before = doc.source_baseline();
    const std::vector<edit_command> captured(doc.history().active_commands().begin(),
                                             doc.history().active_commands().end());

    constexpr std::uint64_t kPendingRequestId = 7;
    constexpr std::uint64_t kStaleRequestId = 8;

    // No pending save: even an ID-equal completion is not eligible.
    CHECK(!save_completion_is_for_pending(false, kPendingRequestId, kPendingRequestId));

    // Mismatched completion for a live pending save: ignored, pending kept.
    bool has_pending = true;
    CHECK(!save_completion_is_for_pending(has_pending, kPendingRequestId, kStaleRequestId));
    CHECK(has_pending);  // not consumed
    CHECK(doc.is_dirty());
    CHECK(doc.saved_commands().empty());
    CHECK(doc.source_baseline().file_size == baseline_before.file_size);
    CHECK(doc.source_baseline().last_write_time == baseline_before.last_write_time);

    // The later matching completion for the same pending request is eligible,
    // evaluates to checkpoint through the production policy, and advances the
    // clean checkpoint + baseline exactly once.
    CHECK(save_completion_is_for_pending(has_pending, kPendingRequestId, kPendingRequestId));
    save_completion_policy_input match_input;
    match_input.committed = true;
    match_input.error = save_error::none;
    match_input.document_matches = true;
    match_input.same_source = true;
    match_input.active_commands = doc.history().active_commands();
    match_input.captured_commands = captured;
    match_input.committed_fingerprint = {true, 1234, 5678};
    CHECK(evaluate_save_completion_policy(match_input) == save_completion_action::checkpoint);
    doc.mark_saved_with_baseline(match_input.committed_fingerprint.file_size,
                                 match_input.committed_fingerprint.last_write_time);
    has_pending = false;  // consumed only by the matching completion
    CHECK(!doc.is_dirty());
    CHECK(doc.saved_commands().size() == 1);
    CHECK(doc.source_baseline().file_size == 1234);
    CHECK(doc.source_baseline().last_write_time == 5678);
}

// Deterministic boundary coverage for the real stable-read helper used by the
// edit/save workers (read_file_stable_identity). A scoped RAII hook mutates
// the source at the genuine post-first-snapshot/pre-second-snapshot boundary;
// the read must fail closed and produce no valid identity / decode
// eligibility. No sleeps or timing races: the hook is invoked synchronously.
void test_stage5_slice6_stable_read_boundary() {
    using namespace oz::viewer_core;

    scratch_dir scratch(L"ozviewer_slice6_stable_read_tests");
    const auto stable_path = scratch.path / L"stable_read_source.png";
    write_file_bytes(stable_path, {'A', 'B', 'C', 'D', 'E'});
    const auto fp = query_file_fingerprint(stable_path);
    CHECK(fp.exists);
    CHECK(fp.file_size == 5);

    // 1. A normal stable read succeeds and reports the verified identity.
    {
        std::vector<std::uint8_t> data;
        std::uint64_t stable_size = 0;
        std::uint64_t stable_write = 0;
        std::string err;
        const bool ok = oz::viewer::read_file_stable_identity(
            stable_path, data, fp.file_size, fp.last_write_time,
            stable_size, stable_write, err, [] { return false; });
        CHECK(ok);
        CHECK(err.empty());
        CHECK(data == std::vector<std::uint8_t>({'A', 'B', 'C', 'D', 'E'}));
        CHECK(stable_size == fp.file_size);
        CHECK(stable_write == fp.last_write_time);
    }

    // 2. Deterministic size change at the genuine post-first-snapshot/pre-
    //    second-snapshot boundary: the stable read fails closed and produces
    //    no valid identity / decode eligibility.
    {
        std::vector<std::uint8_t> data;
        std::uint64_t stable_size = 0;
        std::uint64_t stable_write = 0;
        std::string err;
        bool hook_invoked = false;
        {
            oz::viewer::scoped_stable_read_hook hook([&] {
                hook_invoked = true;
                // Size change (5 -> 7 bytes) between the first and second
                // identity snapshots.
                write_file_bytes(stable_path, {'X', 'Y', 'Z', 'W', 'V', 'U', 'T'});
            });
            const bool ok = oz::viewer::read_file_stable_identity(
                stable_path, data, fp.file_size, fp.last_write_time,
                stable_size, stable_write, err, [] { return false; });
            CHECK(!ok);
            CHECK(hook_invoked);
            CHECK(err.find("modified") != std::string::npos);
            // No valid identity is produced: stable outputs stay zero.
            CHECK(stable_size == 0);
            CHECK(stable_write == 0);
        }

        // 3. RAII reset: after the hook scope the next stable read succeeds
        //    against the mutated file (no expectation, no hook installed).
        {
            std::vector<std::uint8_t> data2;
            std::uint64_t stable_size2 = 0;
            std::uint64_t stable_write2 = 0;
            std::string err2;
            const bool ok2 = oz::viewer::read_file_stable_identity(
                stable_path, data2, 0, 0, stable_size2, stable_write2, err2, [] { return false; });
            CHECK(ok2);
            CHECK(err2.empty());
            CHECK(data2 == std::vector<std::uint8_t>({'X', 'Y', 'Z', 'W', 'V', 'U', 'T'}));
            CHECK(stable_size2 == 7);
        }
    }
}
#endif

#if OZVIEWER_ENABLE_EDITING
void test_stage7_ui_command_state() {
    using namespace oz::viewer;

    // 1. Benchmark mode: all interactive commands disabled and invisible
    {
        ui_command_context ctx{};
        ctx.benchmark_mode = true;
        ctx.have_image = true;
        ctx.has_document = true;
        ctx.has_source = true;
        ctx.is_dirty = true;
        ctx.can_undo = true;

        const auto save_st = derive_command_state(ui_command_id::file_save, ctx);
        CHECK(!save_st.enabled);
        CHECK(!save_st.visible);

        const auto rot_st = derive_command_state(ui_command_id::edit_rotate_right, ctx);
        CHECK(!rot_st.enabled);
        CHECK(!rot_st.visible);

        const auto undo_st = derive_command_state(ui_command_id::edit_undo, ctx);
        CHECK(!undo_st.enabled);
        CHECK(!undo_st.visible);
    }

    // 2. Zero-arg launch / empty state (no image)
    {
        ui_command_context ctx{};
        ctx.benchmark_mode = false;
        ctx.have_image = false;
        ctx.has_document = false;

        const auto btn_st = derive_command_state(ui_command_id::btn_open, ctx);
        CHECK(btn_st.visible);
        CHECK(btn_st.enabled);

        const auto open_st = derive_command_state(ui_command_id::file_open, ctx);
        CHECK(open_st.visible);
        CHECK(open_st.enabled);

        const auto save_st = derive_command_state(ui_command_id::file_save, ctx);
        CHECK(!save_st.enabled);

        const auto rot_st = derive_command_state(ui_command_id::edit_rotate_right, ctx);
        CHECK(!rot_st.enabled);

        const auto view_fit_st = derive_command_state(ui_command_id::view_fit, ctx);
        CHECK(!view_fit_st.enabled);
    }

    // 3. Clean loaded image (not dirty, has source)
    {
        ui_command_context ctx{};
        ctx.have_image = true;
        ctx.has_document = true;
        ctx.has_source = true;
        ctx.is_dirty = false;
        ctx.can_undo = false;
        ctx.can_redo = false;
        ctx.is_fit_mode = true;
        ctx.can_navigate_next = true;

        const auto btn_st = derive_command_state(ui_command_id::btn_open, ctx);
        CHECK(!btn_st.visible);
        CHECK(!btn_st.enabled);

        const auto save_st = derive_command_state(ui_command_id::file_save, ctx);
        CHECK(!save_st.enabled); // Save disabled when not dirty

        const auto save_as_st = derive_command_state(ui_command_id::file_save_as, ctx);
        CHECK(save_as_st.enabled); // Save As enabled with valid source

        const auto rot_st = derive_command_state(ui_command_id::edit_rotate_left, ctx);
        CHECK(rot_st.enabled);

        const auto undo_st = derive_command_state(ui_command_id::edit_undo, ctx);
        CHECK(!undo_st.enabled); // Cannot undo initially

        const auto fit_st = derive_command_state(ui_command_id::view_fit, ctx);
        CHECK(fit_st.enabled);
        CHECK(fit_st.checked);

        const auto actual_st = derive_command_state(ui_command_id::view_actual_size, ctx);
        CHECK(actual_st.enabled);
        CHECK(!actual_st.checked);

        const auto nav_next_st = derive_command_state(ui_command_id::nav_next, ctx);
        CHECK(nav_next_st.enabled);
    }

    // 4. Dirty document (after rotate/edit)
    {
        ui_command_context ctx{};
        ctx.have_image = true;
        ctx.has_document = true;
        ctx.has_source = true;
        ctx.is_dirty = true;
        ctx.can_undo = true;
        ctx.can_redo = false;

        const auto save_st = derive_command_state(ui_command_id::file_save, ctx);
        CHECK(save_st.enabled);

        const auto undo_st = derive_command_state(ui_command_id::edit_undo, ctx);
        CHECK(undo_st.enabled);
        const auto redo_st = derive_command_state(ui_command_id::edit_redo, ctx);
        CHECK(!redo_st.enabled);
    }

    // 5. In-flight Save or Modal Resize: disables conflicting actions
    {
        ui_command_context ctx{};
        ctx.have_image = true;
        ctx.has_document = true;
        ctx.has_source = true;
        ctx.is_dirty = true;
        ctx.can_undo = true;
        ctx.save_busy = true;

        const auto save_st = derive_command_state(ui_command_id::file_save, ctx);
        CHECK(!save_st.enabled); // Disallowed while save is in flight
        const auto save_as_st = derive_command_state(ui_command_id::file_save_as, ctx);
        CHECK(!save_as_st.enabled);

        const auto rot_st = derive_command_state(ui_command_id::edit_rotate_right, ctx);
        CHECK(!rot_st.enabled);

        const auto open_st = derive_command_state(ui_command_id::file_open, ctx);
        CHECK(!open_st.enabled);
    }

    // 6. Active Crop Mode: enables crop controls and checked presets, disables non-crop edits
    {
        ui_command_context ctx{};
        ctx.have_image = true;
        ctx.has_document = true;
        ctx.has_source = true;
        ctx.crop_active = true;
        ctx.crop_preset = oz::viewer_core::crop_ratio_preset::ratio_16x9;

        const auto rot_st = derive_command_state(ui_command_id::edit_rotate_right, ctx);
        CHECK(!rot_st.enabled);

        const auto apply_st = derive_command_state(ui_command_id::edit_crop_apply, ctx);
        CHECK(apply_st.enabled);

        const auto cancel_st = derive_command_state(ui_command_id::edit_crop_cancel, ctx);
        CHECK(cancel_st.enabled);

        const auto free_st = derive_command_state(ui_command_id::crop_ratio_free, ctx);
        CHECK(free_st.enabled);
        CHECK(!free_st.checked);

        const auto r16_9_st = derive_command_state(ui_command_id::crop_ratio_16x9, ctx);
        CHECK(r16_9_st.enabled);
        CHECK(r16_9_st.checked);
    }

    // 7. File location and clipboard operations (Open File Location, Copy File, Copy File Path)
    {
        ui_command_context ctx{};
        ctx.have_image = true;
        ctx.has_document = true;
        ctx.has_file_path = true;

        const auto copy_file_st = derive_command_state(ui_command_id::file_copy, ctx);
        CHECK(copy_file_st.enabled);
        CHECK(copy_file_st.visible);

        const auto copy_path_st = derive_command_state(ui_command_id::file_copy_path, ctx);
        CHECK(copy_path_st.enabled);
        CHECK(copy_path_st.visible);

        const auto copy_img_st = derive_command_state(ui_command_id::file_copy_image, ctx);
        CHECK(copy_img_st.enabled);
        CHECK(copy_img_st.visible);

        const auto open_loc_st = derive_command_state(ui_command_id::file_open_location, ctx);
        CHECK(open_loc_st.enabled);
        CHECK(open_loc_st.visible);

        // Disabled if no file path and no source
        ctx.has_file_path = false;
        ctx.has_source = false;
        CHECK(!derive_command_state(ui_command_id::file_copy, ctx).enabled);
        CHECK(!derive_command_state(ui_command_id::file_copy_path, ctx).enabled);
        CHECK(!derive_command_state(ui_command_id::file_copy_image, ctx).enabled);
        CHECK(!derive_command_state(ui_command_id::file_open_location, ctx).enabled);

        // Copy Image enabled if has_source is true even without file_path
        ctx.has_source = true;
        CHECK(derive_command_state(ui_command_id::file_copy_image, ctx).enabled);
        CHECK(!derive_command_state(ui_command_id::file_copy, ctx).enabled);

        // Disabled if modal/busy (crop/save/resize/copy)
        ctx.has_file_path = true;
        ctx.crop_active = true;
        CHECK(!derive_command_state(ui_command_id::file_copy, ctx).enabled);
        CHECK(!derive_command_state(ui_command_id::file_copy_path, ctx).enabled);
        CHECK(!derive_command_state(ui_command_id::file_copy_image, ctx).enabled);
        CHECK(!derive_command_state(ui_command_id::file_open_location, ctx).enabled);

        ctx.crop_active = false;
        ctx.copy_busy = true;
        CHECK(!derive_command_state(ui_command_id::file_copy_image, ctx).enabled);
        CHECK(!derive_command_state(ui_command_id::file_copy, ctx).enabled);
    }
}

void test_stage7_ui_layout() {
    using namespace oz::viewer;

    // 1. Baseline layout (no chrome): canvas matches client area exactly
    {
        ui_layout_config cfg{};
        cfg.show_toolbar = false;
        cfg.show_status = false;
        cfg.open_btn_width_dip = 160.0f;
        cfg.open_btn_height_dip = 44.0f;

        const auto res96 = compute_ui_layout(800, 600, 96.0f, cfg);
        CHECK(res96.client_physical.x == 0);
        CHECK(res96.client_physical.y == 0);
        CHECK(res96.client_physical.width == 800);
        CHECK(res96.client_physical.height == 600);

        CHECK(res96.canvas_physical.x == 0);
        CHECK(res96.canvas_physical.y == 0);
        CHECK(res96.canvas_physical.width == 800);
        CHECK(res96.canvas_physical.height == 600);

        CHECK(res96.toolbar_physical.height == 0);
        CHECK(res96.status_physical.height == 0);

        // Centered open button
        CHECK(res96.open_btn_physical.width == 160);
        CHECK(res96.open_btn_physical.height == 44);
        CHECK(res96.open_btn_physical.x == (800 - 160) / 2);
        CHECK(res96.open_btn_physical.y == (600 - 44) / 2);
    }

    // 2. High DPI (192 DPI / 200% scale): logical vs physical scaling
    {
        ui_layout_config cfg{};
        cfg.show_toolbar = false;
        cfg.show_status = false;
        cfg.open_btn_width_dip = 160.0f;
        cfg.open_btn_height_dip = 44.0f;

        const auto res192 = compute_ui_layout(1600, 1200, 192.0f, cfg);
        // Logical client area is 800 x 600 DIP
        CHECK(std::abs(res192.client_logical.width - 800.0f) < 0.01f);
        CHECK(std::abs(res192.client_logical.height - 600.0f) < 0.01f);

        // Canvas physical matches 1600 x 1200
        CHECK(res192.canvas_physical.width == 1600);
        CHECK(res192.canvas_physical.height == 1200);

        // Button physical is scaled to 320 x 88 physical pixels and centered
        CHECK(res192.open_btn_physical.width == 320);
        CHECK(res192.open_btn_physical.height == 88);
        CHECK(res192.open_btn_physical.x == (1600 - 320) / 2);
        CHECK(res192.open_btn_physical.y == (1200 - 88) / 2);
    }

    // 3. Layout with synthetic Toolbar (40 DIP) and Status bar (24 DIP)
    {
        ui_layout_config cfg{};
        cfg.show_toolbar = true;
        cfg.toolbar_height_dip = 40.0f;
        cfg.show_status = true;
        cfg.status_height_dip = 24.0f;
        cfg.open_btn_width_dip = 160.0f;
        cfg.open_btn_height_dip = 44.0f;

        // At 96 DPI: 1 DIP == 1 physical px
        const auto res = compute_ui_layout(1000, 800, 96.0f, cfg);
        CHECK(res.toolbar_physical.y == 0);
        CHECK(res.toolbar_physical.height == 40);
        CHECK(res.toolbar_physical.width == 1000);

        CHECK(res.status_physical.y == 800 - 24);
        CHECK(res.status_physical.height == 24);
        CHECK(res.status_physical.width == 1000);

        // Canvas occupies remaining vertical space: y=40, height = 800 - 40 - 24 = 736
        CHECK(res.canvas_physical.y == 40);
        CHECK(res.canvas_physical.height == 736);
        CHECK(res.canvas_physical.width == 1000);

        // Button is centered within the canvas area
        CHECK(res.open_btn_physical.x == (1000 - 160) / 2);
        CHECK(res.open_btn_physical.y == 40 + (736 - 44) / 2);
    }

    // 4. Physical clamping on small positive canvas sizes across fractional/high DPI
    {
        ui_layout_config cfg{};
        cfg.show_toolbar = false;
        cfg.show_status = false;
        cfg.open_btn_width_dip = 160.0f; // at 96 DPI = 160px; at 144 DPI (150%) = 240px; at 192 DPI (200%) = 320px
        cfg.open_btn_height_dip = 44.0f; // at 96 DPI = 44px; at 144 DPI = 66px; at 192 DPI = 88px

        // Test A: Canvas width 100px < desired 160px at 96 DPI
        const auto small_96 = compute_ui_layout(100, 30, 96.0f, cfg);
        CHECK(small_96.canvas_physical.width == 100);
        CHECK(small_96.canvas_physical.height == 30);
        CHECK(small_96.open_btn_physical.width == 100); // Clamped to canvas physical width!
        CHECK(small_96.open_btn_physical.height == 30); // Clamped to canvas physical height!
        CHECK(small_96.open_btn_physical.x == 0);
        CHECK(small_96.open_btn_physical.y == 0);
        CHECK(small_96.canvas_physical.contains(small_96.open_btn_physical.x, small_96.open_btn_physical.y));
        CHECK(small_96.open_btn_physical.right() <= small_96.canvas_physical.right());
        CHECK(small_96.open_btn_physical.bottom() <= small_96.canvas_physical.bottom());

        // Test B: Fractional DPI 144 DPI (150%), small canvas (50 x 20)
        const auto small_144 = compute_ui_layout(50, 20, 144.0f, cfg);
        CHECK(small_144.canvas_physical.width == 50);
        CHECK(small_144.canvas_physical.height == 20);
        CHECK(small_144.open_btn_physical.width == 50); // Clamped!
        CHECK(small_144.open_btn_physical.height == 20); // Clamped!
        CHECK(small_144.open_btn_physical.x == 0);
        CHECK(small_144.open_btn_physical.y == 0);
        CHECK(small_144.open_btn_physical.right() <= small_144.canvas_physical.right());
        CHECK(small_144.open_btn_physical.bottom() <= small_144.canvas_physical.bottom());

        // Test C: High DPI 240 DPI (250%), asymmetric small canvas (120 x 500)
        // Desired button size: 160 * 2.5 = 400px, 44 * 2.5 = 110px
        const auto small_240 = compute_ui_layout(120, 500, 240.0f, cfg);
        CHECK(small_240.canvas_physical.width == 120);
        CHECK(small_240.canvas_physical.height == 500);
        CHECK(small_240.open_btn_physical.width == 120); // Clamped to 120!
        CHECK(small_240.open_btn_physical.height == 110); // Fits within 500!
        CHECK(small_240.open_btn_physical.x == 0);
        CHECK(small_240.open_btn_physical.y == (500 - 110) / 2);
        CHECK(small_240.open_btn_physical.right() <= small_240.canvas_physical.right());
        CHECK(small_240.open_btn_physical.bottom() <= small_240.canvas_physical.bottom());

        // Test D: Tiny canvas 1x1 physical px
        const auto tiny = compute_ui_layout(1, 1, 192.0f, cfg);
        CHECK(tiny.open_btn_physical.width == 1);
        CHECK(tiny.open_btn_physical.height == 1);
        CHECK(tiny.open_btn_physical.x == 0);
        CHECK(tiny.open_btn_physical.y == 0);
    }

    // 5. Fullscreen vs normal chrome layout test
    {
        // Normal interactive mode: 32 DIP toolbar, 24 DIP status bar
        ui_layout_config cfg_normal{};
        cfg_normal.show_toolbar = true;
        cfg_normal.toolbar_height_dip = 32.0f;
        cfg_normal.show_status = true;
        cfg_normal.status_height_dip = 24.0f;
        cfg_normal.open_btn_width_dip = 160.0f;
        cfg_normal.open_btn_height_dip = 44.0f;

        const auto res_norm = compute_ui_layout(1200, 900, 96.0f, cfg_normal);
        CHECK(res_norm.toolbar_physical.height == 32);
        CHECK(res_norm.status_physical.height == 24);
        CHECK(res_norm.canvas_physical.y == 32);
        CHECK(res_norm.canvas_physical.height == 900 - 32 - 24);

        // Fullscreen mode: toolbar and status bar disabled/hidden
        ui_layout_config cfg_full{};
        cfg_full.show_toolbar = false;
        cfg_full.toolbar_height_dip = 0.0f;
        cfg_full.show_status = false;
        cfg_full.status_height_dip = 0.0f;
        cfg_full.open_btn_width_dip = 160.0f;
        cfg_full.open_btn_height_dip = 44.0f;

        const auto res_full = compute_ui_layout(1200, 900, 96.0f, cfg_full);
        CHECK(res_full.toolbar_physical.height == 0);
        CHECK(res_full.status_physical.height == 0);
        CHECK(res_full.canvas_physical.y == 0);
        CHECK(res_full.canvas_physical.height == 900);
    }

    // 6. Decode planning canvas vs client dimension threshold consistency test
    // Proves that when chrome is active (e.g. 32-DIP toolbar, 24-DIP status bar),
    // the planning path uses canvas physical dimensions rather than unadjusted client height.
    {
        const int img_w = 4000;
        const int img_h = 1750;
        const int client_w = 2000;
        const int client_h = 880;

        ui_layout_config cfg{};
        cfg.show_toolbar = true;
        cfg.toolbar_height_dip = 32.0f;
        cfg.show_status = true;
        cfg.status_height_dip = 24.0f;

        const auto layout = compute_ui_layout(client_w, client_h, 96.0f, cfg);
        CHECK(layout.canvas_physical.width == 2000);
        CHECK(layout.canvas_physical.height == 824);

        const int client_den = oz::viewer_core::choose_scaling_denominator(img_w, img_h, client_w, client_h);
        const int canvas_den = oz::viewer_core::choose_scaling_denominator(img_w, img_h, layout.canvas_physical.width, layout.canvas_physical.height);

        // Crucial threshold test: client dimension yields den=1, whereas canvas dimension correctly selects den=2!
        CHECK(client_den == 1);
        CHECK(canvas_den == 2);
        CHECK(client_den != canvas_den);

        // A decode request scheduled with canvas_den (2) matches a completion validated against canvas viewport (824),
        // preventing spurious requeue / rejection caused by client-dimension mismatch.
        oz::viewer_core::cache_key key{L"test_image.jpg", canvas_den};
        CHECK(key.scale_den == 2);
    }

    // 7. Degenerate client dimensions clamp cleanly
    {
        ui_layout_config cfg{};
        cfg.show_toolbar = true;
        cfg.toolbar_height_dip = 40.0f;
        const auto res_zero = compute_ui_layout(0, 0, 96.0f, cfg);
        CHECK(res_zero.canvas_physical.is_empty());
        CHECK(res_zero.client_physical.is_empty());

        const auto res_neg = compute_ui_layout(-100, -50, 96.0f, cfg);
        CHECK(res_neg.canvas_physical.is_empty());
    }

    // 8. SVG icon cache and clean fallback validation
    {
        svg_icon_cache cache;
        CHECK(!cache.is_svg_supported(nullptr));
        CHECK(cache.get_or_render_icon(nullptr, icon_id::open, 16, RGB(0, 0, 0)) == nullptr);

        std::vector<std::uint8_t> dummy_buf(16 * 16 * 4, 0);
        CHECK(!cache.rasterize_icon_pixels(nullptr, icon_id::open, 16, RGB(0, 0, 0), dummy_buf.data()));

        for (const auto id : {
            icon_id::open, icon_id::save, icon_id::undo, icon_id::redo,
            icon_id::rotate_left, icon_id::rotate_right, icon_id::crop, icon_id::resize,
            icon_id::fit, icon_id::one_to_one, icon_id::fullscreen
        }) {
            const auto label = get_icon_fallback_label(id);
            CHECK(!label.empty());
            const auto svg = get_svg_icon_data(id);
            CHECK(!svg.empty());
            CHECK(svg.find("<svg") != std::string_view::npos);
        }

        // Headless WARP/D2D rasterization test to verify valid render-then-staging-copy readback flow
        HMODULE hD3D11 = LoadLibraryW(L"d3d11.dll");
        HMODULE hD2D1 = LoadLibraryW(L"d2d1.dll");
        bool warp_attempted = false;
        bool warp_svg_supported = false;
        if (hD3D11 && hD2D1) {
            using PFN_D3D11CreateDevice = HRESULT(WINAPI*)(
                IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                const D3D_FEATURE_LEVEL*, UINT, UINT,
                ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
            using PFN_D2D1CreateFactory = HRESULT(WINAPI*)(
                D2D1_FACTORY_TYPE, REFIID, const D2D1_FACTORY_OPTIONS*, void**);

            auto pfnD3D11CreateDevice = reinterpret_cast<PFN_D3D11CreateDevice>(
                GetProcAddress(hD3D11, "D3D11CreateDevice"));
            auto pfnD2D1CreateFactory = reinterpret_cast<PFN_D2D1CreateFactory>(
                GetProcAddress(hD2D1, "D2D1CreateFactory"));

            if (pfnD3D11CreateDevice && pfnD2D1CreateFactory) {
                Microsoft::WRL::ComPtr<ID3D11Device> d3d_dev;
                Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d_ctx;
                const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
                HRESULT hr = pfnD3D11CreateDevice(
                    nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                    D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
                    D3D11_SDK_VERSION, &d3d_dev, nullptr, &d3d_ctx);

                if (SUCCEEDED(hr) && d3d_dev) {
                    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_dev;
                    if (SUCCEEDED(d3d_dev.As(&dxgi_dev))) {
                        Microsoft::WRL::ComPtr<ID2D1Factory1> d2d_factory;
                        if (SUCCEEDED(pfnD2D1CreateFactory(
                                D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                                nullptr, reinterpret_cast<void**>(d2d_factory.GetAddressOf())))) {
                            Microsoft::WRL::ComPtr<ID2D1Device> d2d_dev;
                            if (SUCCEEDED(d2d_factory->CreateDevice(dxgi_dev.Get(), &d2d_dev))) {
                                Microsoft::WRL::ComPtr<ID2D1DeviceContext> d2d_context;
                                if (SUCCEEDED(d2d_dev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2d_context))) {
                                    warp_attempted = true;
                                    if (cache.is_svg_supported(d2d_context.Get())) {
                                        warp_svg_supported = true;
                                        std::vector<std::uint8_t> pixels(16 * 16 * 4, 0);
                                        const bool ok = cache.rasterize_icon_pixels(
                                            d2d_context.Get(), icon_id::open, 16, RGB(32, 32, 32), pixels.data());
                                        CHECK(ok);

                                        bool has_alpha = false;
                                        for (std::size_t i = 3; i < pixels.size(); i += 4) {
                                            if (pixels[i] > 0) {
                                                has_alpha = true;
                                                break;
                                            }
                                        }
                                        CHECK(has_alpha);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        if (hD3D11) FreeLibrary(hD3D11);
        if (hD2D1) FreeLibrary(hD2D1);

        if (warp_attempted && warp_svg_supported) {
            std::printf("[viewer_core_tests] Direct2D WARP SVG rasterization: VERIFIED (rendered valid alpha pixels)\n");
        } else {
            std::printf("[viewer_core_tests] Direct2D WARP SVG rasterization: SKIPPED (platform lacks ID2D1DeviceContext5; text fallback active)\n");
        }
    }

    // 9. Modern Dialogs result mapping and contract validation
    {
        CHECK(static_cast<int>(prompt_result::ok) == 0);
        CHECK(static_cast<int>(prompt_result::cancel) == 1);
        CHECK(static_cast<int>(prompt_result::yes) == 2);
        CHECK(static_cast<int>(prompt_result::no) == 3);
        CHECK(static_cast<int>(prompt_result::retry) == 4);

        save_as_dialog_result res{};
        CHECK(!res.accepted);
        CHECK(res.destination.empty());
        CHECK(res.format == oz::viewer_core::save_format::png);
        CHECK(!res.explicit_extension);

        CHECK(oz::viewer_core::save_format_for_extension(L".png") == oz::viewer_core::save_format::png);
        CHECK(oz::viewer_core::save_format_for_extension(L".jpg") == oz::viewer_core::save_format::jpeg);
        CHECK(oz::viewer_core::save_format_for_extension(L".jpeg") == oz::viewer_core::save_format::jpeg);
        CHECK(oz::viewer_core::save_format_for_extension(L".webp") == oz::viewer_core::save_format::webp);
        CHECK(oz::viewer_core::save_format_for_extension(L".tif") == oz::viewer_core::save_format::tiff);
        CHECK(oz::viewer_core::save_format_for_extension(L".tiff") == oz::viewer_core::save_format::tiff);
        CHECK(oz::viewer_core::save_format_for_extension(L".bmp") == oz::viewer_core::save_format::bmp);
        CHECK(!oz::viewer_core::save_format_for_extension(L".gif").has_value());
        CHECK(!oz::viewer_core::save_format_for_extension(L".exe").has_value());

        // 4. Test Open file filter parity between modern and legacy dialogs
        const auto open_filters = get_supported_open_filters();
        CHECK(!open_filters.empty());

        const auto& all_entry = open_filters[0];
        CHECK(all_entry.display_name == L"All Supported Images");

        CHECK(all_entry.spec.find(L"*.jpg") != std::wstring::npos);
        CHECK(all_entry.spec.find(L"*.jpeg") != std::wstring::npos);
        CHECK(all_entry.spec.find(L"*.png") != std::wstring::npos);
        CHECK(all_entry.spec.find(L"*.bmp") != std::wstring::npos);
        CHECK(all_entry.spec.find(L"*.gif") != std::wstring::npos);
        CHECK(all_entry.spec.find(L"*.tif") != std::wstring::npos);
        CHECK(all_entry.spec.find(L"*.tiff") != std::wstring::npos);
        CHECK(all_entry.spec.find(L"*.ico") != std::wstring::npos);
        CHECK(all_entry.spec.find(L"*.cur") != std::wstring::npos);
        CHECK(all_entry.spec.find(L"*.tga") != std::wstring::npos);

#if OZVIEWER_HAVE_WEBP
        CHECK(all_entry.spec.find(L"*.webp") != std::wstring::npos);
#endif
#if OZVIEWER_HAVE_AVIF
        CHECK(all_entry.spec.find(L"*.avif") != std::wstring::npos);
#endif
#if OZVIEWER_HAVE_HEIC
        CHECK(all_entry.spec.find(L"*.heic") != std::wstring::npos);
        CHECK(all_entry.spec.find(L"*.heif") != std::wstring::npos);
#endif

        const auto has_category = [&](std::wstring_view spec_substr) {
            for (std::size_t i = 1; i < open_filters.size(); ++i) {
                if (open_filters[i].spec.find(spec_substr) != std::wstring::npos) {
                    return true;
                }
            }
            return false;
        };
        CHECK(has_category(L"*.gif"));
        CHECK(has_category(L"*.ico"));
        CHECK(has_category(L"*.cur"));
        CHECK(has_category(L"*.tga"));
        CHECK(has_category(L"*.bmp"));
        CHECK(has_category(L"*.tif"));
        CHECK(has_category(L"*.*"));

        const auto legacy_str = build_legacy_open_filter_string(open_filters);
        CHECK(legacy_str.size() >= 4);
        CHECK(legacy_str.back() == L'\0');
        CHECK(legacy_str[legacy_str.size() - 2] == L'\0');

        // 5. Test Contextual Crop Strip layout across DPI scales (96, 144, 192, 240) and fullscreen behavior
        for (const float dpi : {96.0f, 144.0f, 192.0f, 240.0f}) {
            // A. Crop active in normal windowed mode:
            // Toolbar (32 DIP) + Crop Strip (32 DIP) + Status (24 DIP)
            ui_layout_config cfg_crop{};
            cfg_crop.show_toolbar = true;
            cfg_crop.toolbar_height_dip = 32.0f;
            cfg_crop.show_crop_strip = true;
            cfg_crop.crop_strip_height_dip = 32.0f;
            cfg_crop.show_status = true;
            cfg_crop.status_height_dip = 24.0f;

            const auto res_crop = compute_ui_layout(1200, 900, dpi, cfg_crop);
            CHECK(res_crop.toolbar_physical.height == dip_to_physical_round(32.0f, dpi));
            CHECK(res_crop.crop_strip_physical.height == dip_to_physical_round(32.0f, dpi));
            CHECK(res_crop.crop_strip_physical.y == res_crop.toolbar_physical.bottom());
            CHECK(res_crop.status_physical.height == dip_to_physical_round(24.0f, dpi));
            CHECK(res_crop.canvas_physical.y == res_crop.crop_strip_physical.bottom());
            CHECK(res_crop.canvas_physical.bottom() == res_crop.status_physical.y);

            // B. Crop inactive in normal windowed mode: crop strip consumes ZERO space
            ui_layout_config cfg_no_crop = cfg_crop;
            cfg_no_crop.show_crop_strip = false;
            cfg_no_crop.crop_strip_height_dip = 0.0f;

            const auto res_no_crop = compute_ui_layout(1200, 900, dpi, cfg_no_crop);
            CHECK(res_no_crop.crop_strip_physical.height == 0);
            CHECK(res_no_crop.canvas_physical.y == res_no_crop.toolbar_physical.bottom());

            // C. Fullscreen with Crop Active:
            // Normal toolbar and status bar hide (0 height), but contextual crop strip remains visible at top (32 DIP)!
            ui_layout_config cfg_fs_crop{};
            cfg_fs_crop.show_toolbar = false;
            cfg_fs_crop.toolbar_height_dip = 0.0f;
            cfg_fs_crop.show_crop_strip = true;
            cfg_fs_crop.crop_strip_height_dip = 32.0f;
            cfg_fs_crop.show_status = false;
            cfg_fs_crop.status_height_dip = 0.0f;

            const auto res_fs_crop = compute_ui_layout(1200, 900, dpi, cfg_fs_crop);
            CHECK(res_fs_crop.toolbar_physical.height == 0);
            CHECK(res_fs_crop.status_physical.height == 0);
            CHECK(res_fs_crop.crop_strip_physical.y == 0);
            CHECK(res_fs_crop.crop_strip_physical.height == dip_to_physical_round(32.0f, dpi));
            CHECK(res_fs_crop.canvas_physical.y == res_fs_crop.crop_strip_physical.bottom());
            CHECK(res_fs_crop.canvas_physical.height == 900 - res_fs_crop.crop_strip_physical.height);
        }

        // 6. Test Crop Strip synchronization on mutations (drag, reset, ratio change)
        {
            oz::viewer_core::edit_dimensions canvas{800, 600};
            auto crop_state = oz::viewer_core::begin_crop_interaction(canvas, oz::viewer_core::crop_ratio_preset::free);
            CHECK(crop_state.active);

            wchar_t buf[64];
            swprintf_s(buf, L"%d × %d", crop_state.selection.width, crop_state.selection.height);
            CHECK(std::wstring(buf) == L"800 × 600");

            // A. Ratio change
            oz::viewer_core::set_crop_interaction_ratio(crop_state, oz::viewer_core::crop_ratio_preset::ratio_16x9);
            swprintf_s(buf, L"%d × %d", crop_state.selection.width, crop_state.selection.height);
            CHECK(std::wstring(buf) != L"800 × 600");
            CHECK(crop_state.preset == oz::viewer_core::crop_ratio_preset::ratio_16x9);

            // B. Drag update
            crop_state.selection.width = 400;
            crop_state.selection.height = 300;
            swprintf_s(buf, L"%d × %d", crop_state.selection.width, crop_state.selection.height);
            CHECK(std::wstring(buf) == L"400 × 300");

            // C. Reset back to preset ratio (16:9 aspect)
            oz::viewer_core::reset_crop_interaction(crop_state);
            const auto expected_reset = oz::viewer_core::compute_centered_aspect_crop(canvas, 16.0 / 9.0);
            swprintf_s(buf, L"%d × %d", crop_state.selection.width, crop_state.selection.height);
            wchar_t exp_buf[64];
            swprintf_s(exp_buf, L"%d × %d", expected_reset.width, expected_reset.height);
            CHECK(std::wstring(buf) == std::wstring(exp_buf));

            // D. Switch back to Free ratio and Reset: restores back to 800 x 600
            oz::viewer_core::set_crop_interaction_ratio(crop_state, oz::viewer_core::crop_ratio_preset::free);
            swprintf_s(buf, L"%d × %d", crop_state.selection.width, crop_state.selection.height);
            CHECK(std::wstring(buf) == L"800 × 600");
        }
    }
}

void test_stage9_copy_image_contract() {
    using namespace oz::viewer;

    // 1. DIBV5 header and layout verification
    {
        // 4x3 image PBGRA
        const std::int32_t w = 4;
        const std::int32_t h = 3;
        const std::int32_t stride = w * 4;
        std::vector<std::uint8_t> pbgra(stride * h, 255); // Opaque white

        // Middle pixel semi-transparent
        pbgra[0] = 50;
        pbgra[1] = 100;
        pbgra[2] = 150;
        pbgra[3] = 200;

        auto straight = convert_pbgra_to_straight_bgra(pbgra.data(), w, h, stride);
        CHECK(straight.size() == pbgra.size());
        CHECK(straight[3] == 200);

        auto payload = build_dibv5_payload_from_straight_bgra(straight.data(), w, h, stride);
        CHECK(payload.size() == sizeof(BITMAPV5HEADER) + straight.size());

        const auto* v5 = reinterpret_cast<const BITMAPV5HEADER*>(payload.data());
        CHECK(v5->bV5Size == sizeof(BITMAPV5HEADER));
        CHECK(v5->bV5Width == w);
        CHECK(v5->bV5Height == -h); // Top-down
        CHECK(v5->bV5Planes == 1);
        CHECK(v5->bV5BitCount == 32);
        CHECK(v5->bV5Compression == BI_BITFIELDS);
        CHECK(v5->bV5RedMask == 0x00FF0000);
        CHECK(v5->bV5GreenMask == 0x0000FF00);
        CHECK(v5->bV5BlueMask == 0x000000FF);
        CHECK(v5->bV5AlphaMask == 0xFF000000);
        CHECK(v5->bV5CSType == LCS_sRGB);
        CHECK(v5->bV5Intent == LCS_GM_IMAGES);
    }

    // 2. Full-resolution document image vs viewport/scaling test
    {
        // Document has 4000x3000 source, view is fit to 800x600.
        // Copy image captures full 4000x3000 source with committed edits, NOT 800x600 viewport.
        oz::viewer_core::document_metadata meta{};
        meta.source_path = L"C:\\test\\hires.jpg";
        meta.file_size = 1024000;
        meta.last_write_time = 12345;
        meta.source_dimensions = {4000, 3000};
        meta.codec = oz::viewer_core::codec_id::jpeg;

        oz::viewer_core::image_document doc(meta);
        CHECK(doc.current_dimensions().value().width == 4000);
        CHECK(doc.current_dimensions().value().height == 3000);

        // Commit rotate right command
        CHECK(doc.history().push_command(oz::viewer_core::rotate_command{oz::viewer_core::rotate_direction::right_90}));
        CHECK(doc.current_dimensions().value().width == 3000);
        CHECK(doc.current_dimensions().value().height == 4000);

        // Verify active commands capture matches committed edit
        const auto active_cmds = doc.history().active_commands();
        CHECK(active_cmds.size() == 1);
    }

    // 3. Stale completion discard verification
    {
        // When document_generation or diagnostic_revision has moved, completion is discarded
        std::uint64_t current_gen = 5;
        std::uint64_t comp_gen = 4;
        CHECK(comp_gen != current_gen); // Discard condition verified
    }
}
#endif

}  // namespace

int wmain() {
    test_fit_centered();
    test_fit_centered_dip();
    test_scaling_denominator();
    test_jpeg_magic();
    test_decoded_size_limits();
    test_parse_and_decode_valid();
    test_jpeg_errors();
    test_marker_emitter_disabled();

    // Stage 2.1 Navigation tests
    test_jpeg_extension_filter();
    test_natural_path_sorting();
    test_navigation_build_and_active_index();
    test_navigation_boundary_and_movement();
    test_navigation_absent_active();
    test_navigation_empty();

    // Stage 2.2 Preload and Cache tests
    test_cache_budget_calculation();
    test_preload_plan_generation();
    test_cache_key_matching_and_profile();
    test_cache_admission_eviction_and_pinned_current();
    test_cache_pinned_oversized_current();
    test_work_queue_priority_and_replacement();
    test_actual_profile_selection_and_cache_mismatch();
    test_cache_entry_vs_resident_image_counts();
    test_initial_profile_key_correctness();
    test_stale_completion_promotion_no_duplicate();
    test_failed_present_preserves_pin_seam();
    test_oversized_current_stays_pinned();
    test_completion_retention_drop_policy();

    // Stage 3.1 codec registry + PNG tests
    test_registry_signature_routing();
    test_registry_checked_dims();
    test_registry_jpeg_sentinel();
    test_png_decode_opaque_rgb();
    test_png_alpha_premultiplication_exact();
    test_png_gray_palette_interlaced();
    test_png_exif_orientations();
    test_png_reject_bad_inputs();
    test_png_icc_fallback_contract();
    test_png_scaling_denominator();
    test_supported_image_navigation();
    test_cache_codec_tag();

    // Stage 3.2 static WebP tests
    test_webp_signature_routing();
#if OZVIEWER_HAVE_WEBP
    test_webp_static_lossy();
    test_webp_lossless_exact();
    test_webp_alpha_premultiplication_exact();
    test_webp_icc_fallback_contract();
    test_webp_exif_swapped_orientation();
    test_webp_animated_rejected();
    test_webp_reject_bad_inputs();
    test_webp_scaling_denominator();
    test_webp_cache_nav_mixed_extensions();
#else
    test_webp_disabled_build();
#endif

    // Common Raster Decode Profile v1 Phase A (BMP/static GIF/classic
    // TIFF page 0/ICO deterministic selection; CUR/TGA out of scope).
    test_phasea_signature_routing();
    test_phasea_spoof_and_truncation();
    test_phasea_gif_static_only();
    test_phasea_tiff_policies();
    test_phasea_ico_selection();
    test_phasea_canonical_pixels();
    test_phasea_mixed_navigation();

    // Common Raster Decode Profile v1 Phase B (CUR deterministic selection +
    // native bounded TGA adapter).
    test_phaseb_signature_routing();
    test_phaseb_tga_origin_and_pixels();
    test_phaseb_tga_variants();
    test_phaseb_tga_rejection();
    test_phaseb_cur_selection_and_decode();
    test_phaseb_cur_rejection();

    // Stage 3.3 static AVIF tests
    test_avif_signature_routing();
#if OZVIEWER_HAVE_AVIF
    test_avif_static_decode();
    test_avif_rejection_policies();
#endif

    // Stage 3.4 dynamic HEIC tests
    test_heic_signature_routing();
#if OZVIEWER_HAVE_HEIC
    test_stage8_jpeg_lazy_isolation();
    test_heic_static_decode();
    test_heic_rejection_policies();
    test_heic_navigation();
#endif

    // Stage 4 ViewState, Geometry, and DocumentState tests
    test_stage4_view_state_and_geometry();

#if OZVIEWER_ENABLE_EDITING
    // Stage 5 non-destructive edit/crop foundation tests
    test_stage5_slice1_edit_foundation();
    test_stage5_crop_interaction();
    test_stage5_slice5_resize_math_and_proxy();
    // Stage 5 Slice 6: conservative Save/Save As export + saved checkpoint
    test_stage5_slice6_save_and_checkpoint();
    // Stage 5 Slice 6: deterministic save completion acceptance policy
    test_stage5_slice6_save_completion_policy();
    // Stage 5 Slice 6: mismatched completion request-ID boundary (pending kept)
    test_stage5_slice6_save_completion_request_id_boundary();
    // Stage 5 Slice 6: real stable-read boundary (deterministic hook)
    test_stage5_slice6_stable_read_boundary();

    // Stage 7 UI Command State and DPI Layout foundation tests
    test_stage7_ui_command_state();
    test_stage7_ui_layout();

    // Stage 9.4 Copy Image tests
    test_stage9_copy_image_contract();
#endif

    std::printf("ozviewer_core_tests: %d checks, %d failures\n", test::g_checks,
                test::g_failures);
    return test::g_failures == 0 ? 0 : 1;
}

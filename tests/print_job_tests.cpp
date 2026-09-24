// print_job_tests.cpp - focused unit tests for the Stage 9.8 pure print
// seams (src/viewer/print_job.hpp) and the File Print command state
// (src/viewer/ui_command_state.hpp).
// Dependency-free; builds in every preset including no-vcpkg and
// Editing-OFF. No printer, no dialog, no GDI spool call, no filesystem
// access: the spool sequencer runs only over recording fakes.
#include "print_job.hpp"
#include "ui_command_state.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(expr) do { \
    ++g_checks; \
    if (!(expr)) { \
        ++g_failures; \
        std::fprintf(stderr, "FAIL: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
    } \
} while (0)

using oz::viewer::print_bgr24_dib;
using oz::viewer::print_caps;
using oz::viewer::print_command_enabled;
using oz::viewer::print_completion_is_for_pending;
using oz::viewer::print_effective_dpi;
using oz::viewer::print_eligibility_input;
using oz::viewer::print_error;
using oz::viewer::print_page_rect;
using oz::viewer::compose_pbgra_over_white_to_bgr24;
using oz::viewer::compute_print_page_layout;
using oz::viewer::print_caps_valid;
using oz::viewer::print_spool_outcome;
using oz::viewer::print_spool_seam;
using oz::viewer::run_single_page_spool;

print_eligibility_input make_eligible() {
    print_eligibility_input in{};
    in.interactive = true;
    in.have_image = true;
    in.has_document = true;
    in.has_source = true;
    in.has_file_path = true;
    return in;
}

oz::viewer::ui_command_context to_ctx(const print_eligibility_input& in) {
    oz::viewer::ui_command_context ctx{};
    ctx.benchmark_mode = !in.interactive;
    ctx.have_image = in.have_image;
    ctx.has_document = in.has_document;
    ctx.has_source = in.has_source;
    ctx.has_file_path = in.has_file_path;
    ctx.crop_active = in.crop_active;
    ctx.resize_busy = in.resize_busy;
    ctx.save_busy = in.save_busy;
    ctx.copy_busy = in.copy_busy;
    ctx.print_busy = in.print_busy;
    return ctx;
}

void test_eligibility_and_command_state_agree() {
    // Fully eligible interactive document.
    CHECK(print_command_enabled(make_eligible()));
    {
        const auto ctx = to_ctx(make_eligible());
        const auto st =
            oz::viewer::derive_command_state(oz::viewer::ui_command_id::file_print, ctx);
        CHECK(st.enabled);
        CHECK(st.visible);
    }

    // Every single ineligibility dimension disables both seams together.
    for (int dim = 0; dim < 10; ++dim) {
        auto in = make_eligible();
        switch (dim) {
            case 0: in.interactive = false; break;  // benchmark / noninteractive
            case 1: in.have_image = false; break;
            case 2: in.has_document = false; break;
            case 3: in.has_source = false; break;  // missing source
            case 4: in.has_file_path = false; break;  // pathless clipboard document
            case 5: in.crop_active = true; break;
            case 6: in.resize_busy = true; break;
            case 7: in.save_busy = true; break;
            case 8: in.copy_busy = true; break;
            case 9: in.print_busy = true; break;  // single-flight re-entry
        }
        CHECK(!print_command_enabled(in));
        const auto ctx = to_ctx(in);
        const auto st =
            oz::viewer::derive_command_state(oz::viewer::ui_command_id::file_print, ctx);
        if (!st.enabled) {
            continue;
        }
        std::fprintf(stderr, "FAIL: dim %d enabled in derive but not in seam\n", dim);
        ++g_failures;
    }

    // Benchmark mode hides the command (no noninteractive printer work).
    {
        oz::viewer::ui_command_context ctx{};
        ctx.benchmark_mode = true;
        ctx.have_image = true;
        ctx.has_document = true;
        ctx.has_source = true;
        ctx.has_file_path = true;
        const auto st =
            oz::viewer::derive_command_state(oz::viewer::ui_command_id::file_print, ctx);
        CHECK(!st.enabled);
        CHECK(!st.visible);
    }

    // A pending/in-flight print disables every sibling command (menu,
// context-derived, and shortcut states share the same context).
    {
        oz::viewer::ui_command_context ctx = to_ctx(make_eligible());
        ctx.print_busy = true;
        CHECK(!oz::viewer::derive_command_state(oz::viewer::ui_command_id::file_print, ctx).enabled);
        CHECK(!oz::viewer::derive_command_state(oz::viewer::ui_command_id::file_copy_image, ctx).enabled);
        CHECK(!oz::viewer::derive_command_state(oz::viewer::ui_command_id::file_copy, ctx).enabled);
        CHECK(!oz::viewer::derive_command_state(oz::viewer::ui_command_id::file_delete, ctx).enabled);
        CHECK(!oz::viewer::derive_command_state(oz::viewer::ui_command_id::edit_rotate_right, ctx).enabled);
        CHECK(!oz::viewer::derive_command_state(oz::viewer::ui_command_id::edit_undo, ctx).enabled);
    }
}

void test_page_layout() {
    // Landscape source centered on a square page.
    {
        const print_page_rect r = compute_print_page_layout(800, 600, 1000, 1000);
        CHECK(r.valid);
        CHECK(r.width == 1000);
        CHECK(r.height == 750);
        CHECK(r.x == 0);
        CHECK(r.y == 125);
    }
    // Portrait source centered on a square page.
    {
        const print_page_rect r = compute_print_page_layout(600, 800, 1000, 1000);
        CHECK(r.valid);
        CHECK(r.width == 750);
        CHECK(r.height == 1000);
        CHECK(r.x == 125);
        CHECK(r.y == 0);
    }
    // Exact fit covers the whole page.
    {
        const print_page_rect r = compute_print_page_layout(100, 100, 100, 100);
        CHECK(r.valid);
        CHECK(r.x == 0 && r.y == 0 && r.width == 100 && r.height == 100);
    }
    // Tiny source upscales to the page (single page, no tiling).
    {
        const print_page_rect r = compute_print_page_layout(1, 1, 100, 100);
        CHECK(r.valid);
        CHECK(r.width == 100 && r.height == 100);
    }
    // Destination never exceeds the caps and preserves aspect.
    {
        const print_page_rect r = compute_print_page_layout(1600, 900, 1024, 768);
        CHECK(r.valid);
        CHECK(r.width <= 1024 && r.height <= 768);
        CHECK(r.x >= 0 && r.y >= 0);
        CHECK(r.x + r.width <= 1024 && r.y + r.height <= 768);
        // 16:9 into 4:3 caps is width-bound.
        CHECK(r.width == 1024);
        CHECK(r.height == 576);
    }
    // Invalid inputs are rejected, never clamped silently.
    CHECK(!compute_print_page_layout(0, 600, 1000, 1000).valid);
    CHECK(!compute_print_page_layout(800, 0, 1000, 1000).valid);
    CHECK(!compute_print_page_layout(800, 600, 0, 1000).valid);
    CHECK(!compute_print_page_layout(800, 600, 1000, 0).valid);
    CHECK(!compute_print_page_layout(-800, 600, 1000, 1000).valid);
}

void test_caps_and_dpi() {
    CHECK(print_caps_valid(print_caps{100, 100, 300, 300}));
    CHECK(!print_caps_valid(print_caps{0, 100, 300, 300}));
    CHECK(!print_caps_valid(print_caps{100, 0, 300, 300}));
    CHECK(!print_caps_valid(print_caps{-100, 100, 300, 300}));

    // LOGPIXELS is diagnostics-only with a 300-DPI fallback.
    CHECK(print_effective_dpi(300) == 300);
    CHECK(print_effective_dpi(600) == 600);
    CHECK(print_effective_dpi(96) == 96);
    CHECK(print_effective_dpi(0) == 300);
    CHECK(print_effective_dpi(-1) == 300);
    CHECK(print_effective_dpi(99999) == 300);
}

void test_composition() {
    // Opaque pixels pass through exactly.
    {
        const std::uint8_t src[4] = {10, 20, 30, 255};
        const print_bgr24_dib dib = compose_pbgra_over_white_to_bgr24(src, 1, 1, 4, 0);
        CHECK(dib.is_valid());
        CHECK(dib.width == 1 && dib.height == 1);
        CHECK(dib.stride == 4);  // 3 payload bytes, DWORD-aligned
        CHECK(dib.pixels.size() == 4);
        CHECK(dib.pixels[0] == 10 && dib.pixels[1] == 20 && dib.pixels[2] == 30);
    }
    // Fully transparent composites to opaque white.
    {
        const std::uint8_t src[4] = {0, 0, 0, 0};
        const print_bgr24_dib dib = compose_pbgra_over_white_to_bgr24(src, 1, 1, 4, 0);
        CHECK(dib.is_valid());
        CHECK(dib.pixels[0] == 255 && dib.pixels[1] == 255 && dib.pixels[2] == 255);
    }
    // Semi-transparent matte math: out = premul + (255 - alpha).
    {
        // alpha 128, premultiplied channels 64/32/16 -> 191/159/143.
        const std::uint8_t src[8] = {64, 32, 16, 128, 0, 0, 0, 255};
        const print_bgr24_dib dib = compose_pbgra_over_white_to_bgr24(src, 2, 1, 8, 0);
        CHECK(dib.is_valid());
        CHECK(dib.width == 2 && dib.stride == 8);  // 6 payload bytes, aligned to 8
        CHECK(dib.pixels[0] == 191 && dib.pixels[1] == 159 && dib.pixels[2] == 143);
        CHECK(dib.pixels[3] == 0 && dib.pixels[4] == 0 && dib.pixels[5] == 0);
    }
    // Top-down row order with source stride padding ignored.
    {
        std::vector<std::uint8_t> src(2 * 12, 0);  // 2x2, stride 12 (4 pad bytes)
        src[0] = 1; src[1] = 2; src[2] = 3; src[3] = 255;      // row 0, px 0
        src[4] = 4; src[5] = 5; src[6] = 6; src[7] = 255;      // row 0, px 1
        src[12] = 7; src[13] = 8; src[14] = 9; src[15] = 255;  // row 1, px 0
        src[16] = 10; src[17] = 11; src[18] = 12; src[19] = 255;
        const print_bgr24_dib dib =
            compose_pbgra_over_white_to_bgr24(src.data(), 2, 2, 12, 0);
        CHECK(dib.is_valid());
        CHECK(dib.stride == 8);
        CHECK(dib.pixels[0] == 1 && dib.pixels[1] == 2 && dib.pixels[2] == 3);
        CHECK(dib.pixels[3] == 4 && dib.pixels[4] == 5 && dib.pixels[5] == 6);
        CHECK(dib.pixels[8] == 7 && dib.pixels[9] == 8 && dib.pixels[10] == 9);
        CHECK(dib.pixels[11] == 10 && dib.pixels[12] == 11 && dib.pixels[13] == 12);
    }
    // DWORD alignment across widths.
    for (int w = 1; w <= 6; ++w) {
        std::vector<std::uint8_t> src(static_cast<std::size_t>(w) * 4, 255);
        const print_bgr24_dib dib =
            compose_pbgra_over_white_to_bgr24(src.data(), w, 1, w * 4, 0);
        CHECK(dib.is_valid());
        CHECK(dib.stride % 4 == 0);
        CHECK(dib.stride == ((w * 3 + 3) & ~3));
    }
    // Admission budget bounds the composed allocation.
    {
        std::vector<std::uint8_t> src(4 * 4, 255);
        const print_bgr24_dib denied =
            compose_pbgra_over_white_to_bgr24(src.data(), 1, 1, 4, 1);
        CHECK(!denied.is_valid());
        CHECK(denied.error == print_error::admission_denied);
        const print_bgr24_dib exact =
            compose_pbgra_over_white_to_bgr24(src.data(), 1, 1, 4, 4);
        CHECK(exact.is_valid());
    }
    // Invalid inputs fail closed.
    {
        const std::uint8_t src[4] = {0, 0, 0, 255};
        CHECK(compose_pbgra_over_white_to_bgr24(nullptr, 1, 1, 4, 0).error ==
              print_error::invalid_request);
        CHECK(compose_pbgra_over_white_to_bgr24(src, 0, 1, 4, 0).error ==
              print_error::invalid_request);
        CHECK(compose_pbgra_over_white_to_bgr24(src, 1, 1, 3, 0).error ==
              print_error::invalid_request);
        CHECK(compose_pbgra_over_white_to_bgr24(src, 70000, 1, 70000 * 4, 0).error ==
              print_error::invalid_request);
    }
}

void test_request_identity() {
    CHECK(print_completion_is_for_pending(true, 5, 5));
    CHECK(!print_completion_is_for_pending(false, 5, 5));  // no pending print
    CHECK(!print_completion_is_for_pending(true, 5, 6));   // stale completion
    CHECK(!print_completion_is_for_pending(true, 0, 0));   // zero ids never match
    CHECK(!print_completion_is_for_pending(true, 5, 0));
}

struct recording_seam {
    std::vector<std::string> calls;
    bool doc_ok = true;
    bool page_ok = true;
    bool stretch_ok = true;
    bool end_page_ok = true;
    bool end_doc_ok = true;
    bool abort_called = false;
    int abort_count = 0;

    print_spool_seam seam() {
        print_spool_seam s;
        s.begin_doc = [&]() -> bool {
            calls.push_back("begin_doc");
            return doc_ok;
        };
        s.begin_page = [&]() -> bool {
            calls.push_back("begin_page");
            return page_ok;
        };
        s.stretch = [&]() -> bool {
            calls.push_back("stretch");
            return stretch_ok;
        };
        s.end_page = [&]() -> bool {
            calls.push_back("end_page");
            return end_page_ok;
        };
        s.end_doc = [&]() -> bool {
            calls.push_back("end_doc");
            return end_doc_ok;
        };
        s.abort = [&]() { abort_called = true; ++abort_count; };
        return s;
    }
};

void test_spool_success_order() {
    recording_seam rec;
    const std::function<bool()> never_cancel;
    const auto out = run_single_page_spool(rec.seam(), never_cancel);
    CHECK(out == print_spool_outcome::printed);
    CHECK(rec.calls.size() == 5);
    if (rec.calls.size() == 5) {
        CHECK(rec.calls[0] == "begin_doc");
        CHECK(rec.calls[1] == "begin_page");
        CHECK(rec.calls[2] == "stretch");
        CHECK(rec.calls[3] == "end_page");
        CHECK(rec.calls[4] == "end_doc");
    }
    CHECK(!rec.abort_called);
    CHECK(rec.abort_count == 0);
}

void test_spool_cancel_before_start_makes_no_call() {
    recording_seam rec;
    const auto out = run_single_page_spool(rec.seam(), [] { return true; });
    CHECK(out == print_spool_outcome::cancelled);
    CHECK(rec.calls.empty());
    CHECK(!rec.abort_called);
    CHECK(rec.abort_count == 0);
}

void test_spool_failure_abort_paths() {
    // StartDoc failure: no page started, no abort needed.
    {
        recording_seam rec;
        rec.doc_ok = false;
        const auto out = run_single_page_spool(rec.seam(), {});
        CHECK(out == print_spool_outcome::spool_failed);
        CHECK(rec.calls.size() == 1);
        CHECK(!rec.abort_called);
        CHECK(rec.abort_count == 0);
    }
    // Every post-StartDoc failure aborts the partial job.
    for (int stage = 0; stage < 3; ++stage) {
        recording_seam rec;
        if (stage == 0) rec.page_ok = false;
        if (stage == 1) rec.stretch_ok = false;
        if (stage == 2) rec.end_page_ok = false;
        const auto out = run_single_page_spool(rec.seam(), {});
        CHECK(out == print_spool_outcome::spool_failed);
        CHECK(rec.abort_called);
        CHECK(rec.abort_count == 1);
    }
    // EndDoc failure: post-StartDoc failure, so abort the job once.
    {
        recording_seam rec;
        rec.end_doc_ok = false;
        const auto out = run_single_page_spool(rec.seam(), {});
        CHECK(out == print_spool_outcome::spool_failed);
        CHECK(rec.calls.size() == 5);
        CHECK(rec.abort_called);
        CHECK(rec.abort_count == 1);
    }
    // Mid-job cancellation aborts and reports cancelled.
    {
        recording_seam rec;
        int polls = 0;
        auto cancel_after_start = [&]() -> bool { return ++polls >= 2; };
        const auto out = run_single_page_spool(rec.seam(), cancel_after_start);
        CHECK(out == print_spool_outcome::cancelled);
        CHECK(rec.abort_called);
        CHECK(rec.abort_count == 1);
        CHECK(rec.calls.size() < 5);
    }
    // A null seam fails closed without crashing.
    {
        print_spool_seam empty;
        const auto out = run_single_page_spool(empty, {});
        CHECK(out == print_spool_outcome::spool_failed);
    }
}

}  // namespace

int main() {
    test_eligibility_and_command_state_agree();
    test_page_layout();
    test_caps_and_dpi();
    test_composition();
    test_request_identity();
    test_spool_success_order();
    test_spool_cancel_before_start_makes_no_call();
    test_spool_failure_abort_paths();

    std::printf("print_job_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

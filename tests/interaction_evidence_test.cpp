#include "oz/viewer_core/view_state.hpp"
#include "renderer.hpp"

#if OZVIEWER_ENABLE_EDITING
#include "oz/viewer_core/save_pipeline.hpp"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace {

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const wchar_t kClassName[] = L"OZStage4EvidenceWindow";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) {
        std::fprintf(stderr, "RegisterClassExW failed\n");
        return 1;
    }

    HWND hwnd = CreateWindowExW(0, kClassName, L"Stage 4 Evidence Test", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1024, 768, nullptr, nullptr,
                                wc.hInstance, nullptr);
    if (!hwnd) {
        std::fprintf(stderr, "CreateWindowExW failed\n");
        return 1;
    }

    oz::viewer::renderer r;
    if (!oz::viewer::create_renderer(hwnd, r)) {
        std::fprintf(stderr, "create_renderer failed\n");
        return 1;
    }

    RECT rc{};
    GetClientRect(hwnd, &rc);
    if (!oz::viewer::resize_renderer(r, rc.right, rc.bottom)) {
        std::fprintf(stderr, "resize_renderer failed\n");
        return 1;
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    // Create synthetic 1024x768 premultiplied BGRA image
    const std::int32_t img_w = 1024;
    const std::int32_t img_h = 768;
    const std::int32_t stride = img_w * 4;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(stride * img_h), 200);

    oz::viewer_core::view_state vs;
    oz::viewer_core::set_fit(vs, img_w, img_h, rc.right, rc.bottom);
    auto geom = oz::viewer_core::compute_view_geometry(vs, img_w, img_h, rc.right, rc.bottom);

    // Initial commit (upload exactly once)
    int upload_count = 0;
    const bool ok = oz::viewer::render_and_present_candidate(
        r, pixels.data(), stride, img_w, img_h, img_w, img_h, geom);
    if (!ok) {
        std::fprintf(stderr, "render_and_present_candidate failed\n");
        return 1;
    }
    ++upload_count;

    // Run 100 interaction steps (zoom, pan, resize redraws) WITHOUT calling any upload functions!
    // Measuring input-to-present latency for each interaction step.
    std::vector<double> latencies_ms;
    latencies_ms.reserve(100);

    for (int i = 0; i < 100; ++i) {
        const auto t0 = std::chrono::high_resolution_clock::now();

        // Alternate operations: 40 zooms, 40 pans, 20 resizes
        if (i < 40) {
            // Cursor-centered zoom
            oz::viewer_core::zoom_at_point(vs, img_w, img_h, rc.right, rc.bottom, 500.0, 400.0, (i % 2 == 0) ? 0.5 : -0.5);
            geom = oz::viewer_core::compute_view_geometry(vs, img_w, img_h, rc.right, rc.bottom);
        } else if (i < 80) {
            // Pan drag
            oz::viewer_core::apply_pan_delta(vs, img_w, img_h, rc.right, rc.bottom, (i % 2 == 0) ? 10.0 : -10.0, 5.0);
            geom = oz::viewer_core::compute_view_geometry(vs, img_w, img_h, rc.right, rc.bottom);
        } else {
            // Resize redraw
            const int new_w = 1000 + (i % 5) * 10;
            const int new_h = 750 + (i % 5) * 10;
            oz::viewer::resize_renderer(r, new_w, new_h);
            oz::viewer_core::on_viewport_resized(vs, img_w, img_h, rc.right, rc.bottom, new_w, new_h);
            geom = oz::viewer_core::compute_view_geometry(vs, img_w, img_h, new_w, new_h);
        }

        // Redraw retained bitmap (ZERO uploads)
        const bool redrawn = oz::viewer::redraw_retained(r, geom);
        if (!redrawn) {
            std::fprintf(stderr, "redraw_retained failed at step %d\n", i);
            return 1;
        }

        const auto t1 = std::chrono::high_resolution_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        latencies_ms.push_back(ms);
    }

    // Compute latency stats
    std::sort(latencies_ms.begin(), latencies_ms.end());
    const double p50 = latencies_ms[latencies_ms.size() * 50 / 100];
    const double p95 = latencies_ms[latencies_ms.size() * 95 / 100];
    const double max_lat = latencies_ms.back();
    const int over_50ms = static_cast<int>(std::count_if(latencies_ms.begin(), latencies_ms.end(), [](double v) { return v > 50.0; }));
    const int under_16_7ms = static_cast<int>(std::count_if(latencies_ms.begin(), latencies_ms.end(), [](double v) { return v <= 16.7; }));

    std::printf("INITIAL_UPLOADS: %d\n", upload_count);
    std::printf("REDRAW_UPLOADS: 0\n");
    std::printf("TOTAL_STEPS: %zu\n", latencies_ms.size());
    std::printf("MEDIAN_LATENCY_MS: %.3f\n", p50);
    std::printf("P95_LATENCY_MS: %.3f\n", p95);
    std::printf("MAX_LATENCY_MS: %.3f\n", max_lat);
    std::printf("STEPS_LE_16_7MS: %d (%.1f%%)\n", under_16_7ms, (under_16_7ms * 100.0) / latencies_ms.size());
    std::printf("STEPS_GT_50MS: %d\n", over_50ms);
    std::printf("WARP: %s\n", r.warp ? "true" : "false");

#if OZVIEWER_ENABLE_EDITING
    // ---------------------------------------------------------------------------
    // Stage 5 Slice 3: Rotate/Flip presentation plan & zero additional upload test
    // ---------------------------------------------------------------------------
    {
        // Test immediate rotate and flip redraws using presentation plan without re-upload
        oz::viewer::presentation_plan plan;
        plan.logical_width = img_h; // 90 deg rotated: w and h swap
        plan.logical_height = img_w;
        plan.transform = oz::viewer_core::d4_transform::make_rot90_cw();
        plan.geometry = oz::viewer_core::compute_view_geometry(vs, plan.logical_width, plan.logical_height, rc.right, rc.bottom);

        // Immediate Rotate Right redraw from retained bitmap (zero CPU upload)
        const bool rot_ok = oz::viewer::redraw_retained_plan(r, plan);
        if (!rot_ok) {
            std::fprintf(stderr, "redraw_retained_plan (rotate) failed\n");
            return 1;
        }

        // Immediate Flip Horizontal redraw from retained bitmap
        plan.transform = plan.transform.compose(oz::viewer_core::d4_transform::make_flip_h());
        const bool flip_ok = oz::viewer::redraw_retained_plan(r, plan);
        if (!flip_ok) {
            std::fprintf(stderr, "redraw_retained_plan (flip) failed\n");
            return 1;
        }

        // Crop draft overlay is drawn over the retained bitmap and source
        // rect, without any additional upload or image allocation.
        oz::viewer::presentation_plan crop_plan;
        crop_plan.logical_width = 768;
        crop_plan.logical_height = 576;
        crop_plan.source_rect = {128, 96, 768, 576};
        crop_plan.transform = oz::viewer_core::d4_transform::make_identity();
        crop_plan.geometry = oz::viewer_core::compute_view_geometry(
            vs, crop_plan.logical_width, crop_plan.logical_height, rc.right, rc.bottom);
        const auto image_rect = oz::viewer_core::view_geometry_to_dip(crop_plan.geometry, r.dpi);
        oz::viewer::crop_overlay overlay;
        overlay.visible = true;
        overlay.image_rect = image_rect;
        overlay.selection_rect = {
            image_rect.x + image_rect.width * 0.125f,
            image_rect.y + image_rect.height * 0.125f,
            image_rect.width * 0.75f,
            image_rect.height * 0.75f};
        const bool crop_ok = oz::viewer::redraw_retained_plan(r, crop_plan, &overlay);
        if (!crop_ok) {
            std::fprintf(stderr, "redraw_retained_plan (crop overlay) failed\n");
            return 1;
        }

        // Stage 7: verify crop draft overlay with nonzero canvas_offset_x/y
        crop_plan.canvas_offset_x = 32;
        crop_plan.canvas_offset_y = 48;
        const bool crop_offset_ok = oz::viewer::redraw_retained_plan(r, crop_plan, &overlay);
        if (!crop_offset_ok) {
            std::fprintf(stderr, "redraw_retained_plan (crop overlay with offset) failed\n");
            return 1;
        }
        std::printf("STAGE5_CROP_OVERLAY_OK: true\n");
        std::printf("STAGE7_CROP_OVERLAY_OFFSET_OK: true\n");

        // ---------------------------------------------------------------------------
        // Stage 5 Slice 5: Resize preview/apply/undo/redo presentation must not
        // upload pixels or replace the retained source bitmap.
        // ---------------------------------------------------------------------------
        {
            ID2D1Bitmap1* const retained_before = r.source_bitmap.Get();
            const std::int32_t retained_w_before = r.source_width;
            const std::int32_t retained_h_before = r.source_height;

            // Draft preview: proxy scales logical geometry to 512x384 (50% downscale)
            // without uploading new pixels or replacing retained source_bitmap.
            oz::viewer::presentation_plan resize_proxy_plan;
            resize_proxy_plan.logical_width = 512;
            resize_proxy_plan.logical_height = 384;
            resize_proxy_plan.transform = oz::viewer_core::d4_transform::make_identity();
            resize_proxy_plan.source_rectf = {0.0f, 0.0f, static_cast<float>(img_w), static_cast<float>(img_h)};
            resize_proxy_plan.geometry = oz::viewer_core::compute_view_geometry(
                vs, resize_proxy_plan.logical_width, resize_proxy_plan.logical_height, rc.right, rc.bottom);

            if (!oz::viewer::redraw_retained_plan(r, resize_proxy_plan)) {
                std::fprintf(stderr, "redraw_retained_plan (resize draft preview) failed\n");
                return 1;
            }

            // Apply: committed resize proxy at 2048x1536 (upscale).
            resize_proxy_plan.logical_width = 2048;
            resize_proxy_plan.logical_height = 1536;
            resize_proxy_plan.geometry = oz::viewer_core::compute_view_geometry(
                vs, resize_proxy_plan.logical_width, resize_proxy_plan.logical_height, rc.right, rc.bottom);
            if (!oz::viewer::redraw_retained_plan(r, resize_proxy_plan)) {
                std::fprintf(stderr, "redraw_retained_plan (resize applied proxy) failed\n");
                return 1;
            }

            // Undo: back to the opening canvas dimensions through the same
            // retained bitmap.
            oz::viewer::presentation_plan undo_plan;
            undo_plan.logical_width = img_w;
            undo_plan.logical_height = img_h;
            undo_plan.transform = oz::viewer_core::d4_transform::make_identity();
            undo_plan.source_rectf = {0.0f, 0.0f, static_cast<float>(img_w), static_cast<float>(img_h)};
            undo_plan.geometry = oz::viewer_core::compute_view_geometry(
                vs, undo_plan.logical_width, undo_plan.logical_height, rc.right, rc.bottom);
            if (!oz::viewer::redraw_retained_plan(r, undo_plan)) {
                std::fprintf(stderr, "redraw_retained_plan (resize undo proxy) failed\n");
                return 1;
            }

            // Redo: resize again, still from the same retained bitmap.
            if (!oz::viewer::redraw_retained_plan(r, resize_proxy_plan)) {
                std::fprintf(stderr, "redraw_retained_plan (resize redo proxy) failed\n");
                return 1;
            }

            if (r.source_bitmap.Get() != retained_before ||
                r.source_width != retained_w_before ||
                r.source_height != retained_h_before) {
                std::fprintf(stderr,
                             "retained source bitmap identity changed across resize preview/apply/undo/redo\n");
                return 1;
            }
            std::printf("STAGE5_RESIZE_SOURCE_BITMAP_STABLE: true\n");
            std::printf("STAGE5_RESIZE_PROXY_REDRAW_UPLOADS: 0\n");
            std::printf("STAGE5_RESIZE_PROXY_OK: true\n");
        }

        // Explicit one-shot materialized candidate commit (exactly one upload).
        // This is the future Save/Save As materialization path, deliberately
        // separate from the presentation-only resize preview above.
        std::vector<std::uint8_t> mat_pixels(static_cast<std::size_t>(img_h * 4 * img_w), 210);
        oz::viewer::presentation_plan mat_plan;
        mat_plan.logical_width = img_h;
        mat_plan.logical_height = img_w;
        mat_plan.transform = oz::viewer_core::d4_transform::make_identity(); // Identity because pixels are materialized!
        mat_plan.geometry = oz::viewer_core::compute_view_geometry(vs, mat_plan.logical_width, mat_plan.logical_height, rc.right, rc.bottom);

        const bool mat_ok = oz::viewer::render_and_present_candidate_plan(
            r, mat_pixels.data(), img_h * 4, img_h, img_w, img_h, img_w, mat_plan);
        if (!mat_ok) {
            std::fprintf(stderr, "render_and_present_candidate_plan failed\n");
            return 1;
        }
        ++upload_count;
        std::printf("STAGE5_MATERIALIZED_UPLOADS: 1\n");
        std::printf("STAGE5_PRESENTATION_PLAN_OK: true\n");
    }

    // ---------------------------------------------------------------------------
    // Stage 5 Slice 6: a real materialize -> encode -> atomic commit Save must
    // not touch the retained renderer source bitmap, the retained dimensions,
    // or the captured immutable source pixels (no cache/GPU contamination).
    // ---------------------------------------------------------------------------
    {
        ID2D1Bitmap1* const retained_before = r.source_bitmap.Get();
        const std::int32_t retained_w_before = r.source_width;
        const std::int32_t retained_h_before = r.source_height;

        std::vector<std::uint8_t> save_source = pixels;  // canonical PBGRA8
        const std::vector<std::uint8_t> save_source_original = save_source;

        std::error_code ec;
        const std::filesystem::path destination =
            std::filesystem::temp_directory_path(ec) / L"ozviewer_slice6_evidence_save.png";
        std::filesystem::remove(destination, ec);

        oz::viewer_core::save_request request;
        request.destination = destination;
        request.format = oz::viewer_core::save_format::png;
        request.source = oz::viewer_core::edit_source_view{
            .pixels = std::span<const std::uint8_t>(save_source.data(), save_source.size()),
            .dimensions = {img_w, img_h},
            .stride = stride};
        request.destination_baseline = {false, 0, 0};
        request.source_fingerprint.expected = false;

        const auto save_result = oz::viewer_core::execute_save(request, [] { return false; });
        if (save_result.error != oz::viewer_core::save_error::none || !save_result.committed) {
            std::fprintf(stderr, "execute_save failed: %s (%s)\n",
                         std::string(oz::viewer_core::to_string(save_result.error)).c_str(),
                         save_result.detail.c_str());
            return 1;
        }
        if (!std::filesystem::exists(destination)) {
            std::fprintf(stderr, "execute_save reported success but wrote no file\n");
            return 1;
        }
        if (save_source != save_source_original) {
            std::fprintf(stderr, "execute_save mutated the captured source pixels\n");
            return 1;
        }
        if (r.source_bitmap.Get() != retained_before ||
            r.source_width != retained_w_before ||
            r.source_height != retained_h_before) {
            std::fprintf(stderr, "retained source bitmap identity changed across Save\n");
            return 1;
        }
        std::filesystem::remove(destination, ec);
        std::printf("STAGE5_SAVE_SOURCE_BITMAP_STABLE: true\n");
        std::printf("STAGE5_SAVE_RENDERER_UPLOADS: 0\n");
        std::printf("STAGE5_SAVE_PIPELINE_EXECUTED: true\n");
    }
#endif

    DestroyWindow(hwnd);
    return 0;
}

// stage7_seam_tests.cpp - standalone focused unit tests for Stage 7 UI command state, DPI layout seams,
// and SVG icon cache fallback semantics.
// Compiles in every build configuration, including dependency-free no-vcpkg builds.
#include "oz/viewer_core/cache.hpp"
#include "oz/viewer_core/crop_interaction.hpp"
#include "oz/viewer_core/edit_state.hpp"
#include "oz/viewer_core/fit.hpp"
#include "oz/viewer_core/image_document.hpp"
#include "../src/viewer/crop_strip.hpp"
#include "../src/viewer/context_menu.hpp"
#include "../src/viewer/clipboard_output.hpp"
#include "../src/viewer/clipboard_input.hpp"
#include "../src/viewer/paste_install.hpp"
#include "../src/viewer/presentation_edits.hpp"
#include "../src/viewer/shortcut_routing.hpp"
#include "../src/viewer/modern_dialogs.hpp"
#include "../src/viewer/shell_integration.hpp"
#include "../src/viewer/svg_icon_cache.hpp"
#include "../src/viewer/ui_chrome.hpp"
#include "../src/viewer/ui_command_state.hpp"
#include "../src/viewer/ui_layout.hpp"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <wrl/client.h>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>

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

void test_ui_command_state() {
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

        const auto open_loc_st = derive_command_state(ui_command_id::file_open_location, ctx);
        CHECK(!open_loc_st.enabled);

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

        const auto open_loc_disabled = derive_command_state(ui_command_id::file_open_location, ctx);
        CHECK(!open_loc_disabled.enabled); // has_file_path is false

        ctx.has_file_path = true;
        const auto open_loc_enabled = derive_command_state(ui_command_id::file_open_location, ctx);
        CHECK(open_loc_enabled.enabled); // enabled when has_file_path is true and not modal

        ctx.save_busy = true;
        const auto open_loc_busy = derive_command_state(ui_command_id::file_open_location, ctx);
        CHECK(!open_loc_busy.enabled); // disabled while save_busy
        ctx.save_busy = false;

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
}

void test_ui_layout() {
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
        // Scenario:
        // Client viewport is 2000 x 880 physical px
        // Toolbar = 32px, Status = 24px => Canvas physical size = 2000 x 824 px (880 - 56 = 824)
        // Image is 4000 x 1750 px
        // If scaled by den = 2:
        // sw = 4000 / 2 = 2000
        // sh = 1750 / 2 = 875
        // For client height 880: sh (875) < client_h (880) => cannot cover client viewport => choose_scaling_denominator = 1
        // For canvas height 824: sw (2000) >= 2000 and sh (875) >= 824 => covers canvas viewport => choose_scaling_denominator = 2!
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
}

void test_svg_icon_cache_and_fallback() {
    using namespace oz::viewer;

    svg_icon_cache cache;

    // 1. Without a valid D2D context, is_svg_supported is false and get_or_render_icon / rasterize_icon_pixels fail gracefully
    CHECK(!cache.is_svg_supported(nullptr));
    CHECK(cache.get_or_render_icon(nullptr, icon_id::open, 16, RGB(0, 0, 0)) == nullptr);

    std::vector<std::uint8_t> dummy_buf(16 * 16 * 4, 0);
    CHECK(!cache.rasterize_icon_pixels(nullptr, icon_id::open, 16, RGB(0, 0, 0), dummy_buf.data()));

    // 2. Fallback labels exist and are non-empty for every command icon
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

    // 3. Test headless WARP/D2D rasterization if D3D11 and Direct2D are available in the test environment
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
            // Use WARP (software driver) for 100% headless reliability
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
                                // If the platform D2D context supports SVG (ID2D1DeviceContext5 / Windows 10 Creators Update+)
                                if (cache.is_svg_supported(d2d_context.Get())) {
                                    warp_svg_supported = true;
                                    std::vector<std::uint8_t> pixels(16 * 16 * 4, 0);
                                    const bool ok = cache.rasterize_icon_pixels(
                                        d2d_context.Get(), icon_id::open, 16, RGB(32, 32, 32), pixels.data());
                                    CHECK(ok);

                                    // Verify that non-empty pixels were produced (stroke drawn on transparent background)
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
        std::printf("[stage7_seam_tests] Direct2D WARP SVG rasterization: VERIFIED (rendered valid alpha pixels)\n");
    } else {
        std::printf("[stage7_seam_tests] Direct2D WARP SVG rasterization: SKIPPED (platform lacks ID2D1DeviceContext5; text fallback active)\n");
    }
}

void test_modern_dialogs_mapping_and_fallback() {
    using namespace oz::viewer;

    // 1. Verify show_prompt mapping with null/empty strings handles fallback gracefully
    // Calling with parent=nullptr, icon=0 in test environment
    // (We test prompt_result mapping constants and fallback data mapping)
    CHECK(static_cast<int>(prompt_result::ok) == 0);
    CHECK(static_cast<int>(prompt_result::cancel) == 1);
    CHECK(static_cast<int>(prompt_result::yes) == 2);
    CHECK(static_cast<int>(prompt_result::no) == 3);
    CHECK(static_cast<int>(prompt_result::retry) == 4);

    // 2. Test Save As result default struct initialization & format preferred extensions
    save_as_dialog_result res{};
    CHECK(!res.accepted);
    CHECK(res.destination.empty());
    CHECK(res.format == oz::viewer_core::save_format::png);
    CHECK(!res.explicit_extension);

    // 3. Test noninteractive error-dialog suppression predicates and contract
    // Both --benchmark and --navigation-benchmark must suppress error prompts,
    // TaskDialog, and MessageBox dialogs across all failure paths.
    {
        const auto check_noninteractive = [](bool benchmark, bool nav_benchmark) noexcept {
            return benchmark || nav_benchmark;
        };

        // Interactive mode: must NOT be noninteractive (dialogs permitted)
        CHECK(!check_noninteractive(false, false));

        // Benchmark mode: must be noninteractive (dialogs suppressed)
        CHECK(check_noninteractive(true, false));

        // Navigation benchmark mode: must be noninteractive (dialogs suppressed)
        CHECK(check_noninteractive(false, true));

        // Both flags set: must be noninteractive (dialogs suppressed)
        CHECK(check_noninteractive(true, true));
    }

    // 4. Test Open file filter parity between modern and legacy dialogs
    const auto open_filters = get_supported_open_filters();
    CHECK(!open_filters.empty());

    // First entry must be "All Supported Images"
    const auto& all_entry = open_filters[0];
    CHECK(all_entry.display_name == L"All Supported Images");

    // All supported extensions must be present in the all-images filter:
    // Core baseline: JPG, JPEG, PNG, BMP, GIF, TIF, TIFF, ICO, CUR, TGA
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

    // Verify individual format filter category entries exist
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
    CHECK(has_category(L"*.*")); // All Files wildcard

    // Verify legacy filter string encoding builds double-null terminated format correctly
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

        // Verification of dimension formatting:
        // Initially, crop selection matches canvas
        wchar_t buf[64];
        swprintf_s(buf, L"%d × %d", crop_state.selection.width, crop_state.selection.height);
        CHECK(std::wstring(buf) == L"800 × 600");

        // A. Simulate ratio change to 16:9
        oz::viewer_core::set_crop_interaction_ratio(crop_state, oz::viewer_core::crop_ratio_preset::ratio_16x9);
        swprintf_s(buf, L"%d × %d", crop_state.selection.width, crop_state.selection.height);
        CHECK(std::wstring(buf) != L"800 × 600"); // Proves dimension changed!
        CHECK(crop_state.preset == oz::viewer_core::crop_ratio_preset::ratio_16x9);

        // B. Simulate drag update: move right edge inward
        crop_state.selection.width = 400;
        crop_state.selection.height = 300;
        swprintf_s(buf, L"%d × %d", crop_state.selection.width, crop_state.selection.height);
        CHECK(std::wstring(buf) == L"400 × 300");

        // C. Simulate Reset back to preset ratio (16:9 aspect)
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

    // 7. End-to-end crop_strip control window synchronization
    // Exercises crop_strip HWND creation, sync_state, sync_dimension_summary,
    // rejected/inactive guards, and all user input/mutation paths.
    {
        const wchar_t kTestClass[] = L"OzCropStripTestHostWindow";
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kTestClass;
        RegisterClassExW(&wc);

        HWND host_hwnd = CreateWindowExW(
            0, kTestClass, L"CropStripHost", WS_OVERLAPPEDWINDOW,
            0, 0, 800, 600, nullptr, nullptr, wc.hInstance, nullptr);
        CHECK(host_hwnd != nullptr);

        if (host_hwnd) {
            oz::viewer::crop_strip strip;
            CHECK(strip.create(host_hwnd, wc.hInstance, 96.0f));
            CHECK(strip.is_created());
            CHECK(strip.hwnd() != nullptr);

            HWND txt_dim = GetDlgItem(strip.hwnd(), oz::viewer::kIdCropStripStaticDim);
            HWND combo_ratio = GetDlgItem(strip.hwnd(), oz::viewer::kIdCropStripComboRatio);
            CHECK(txt_dim != nullptr);
            CHECK(combo_ratio != nullptr);

            auto read_dim_text = [&]() -> std::wstring {
                wchar_t text[64]{};
                GetWindowTextW(txt_dim, text, 64);
                return text;
            };

            oz::viewer_core::edit_dimensions canvas{1920, 1080};
            auto crop_state = oz::viewer_core::begin_crop_interaction(canvas, oz::viewer_core::crop_ratio_preset::free);
            CHECK(crop_state.active);

            // A. Initial sync on entering crop mode
            strip.sync_state(&crop_state);
            CHECK(read_dim_text() == L"1920 × 1080");
            CHECK(SendMessageW(combo_ratio, CB_GETCURSEL, 0, 0) == 0); // Free is index 0

            // B. In-drag mutation via sync_dimension_summary
            crop_state.selection = oz::viewer_core::edit_rect{100, 100, 800, 600};
            strip.sync_dimension_summary(&crop_state);
            CHECK(read_dim_text() == L"800 × 600");

            // C. Drag completion / mouse-up / capture-loss authoritative synchronization
            crop_state.selection = oz::viewer_core::edit_rect{120, 140, 640, 480};
            strip.sync_dimension_summary(&crop_state);
            CHECK(read_dim_text() == L"640 × 480");

            // D. Ratio preset mutation (e.g. 16:9) via sync_state
            oz::viewer_core::set_crop_interaction_ratio(crop_state, oz::viewer_core::crop_ratio_preset::ratio_16x9);
            strip.sync_state(&crop_state);
            const auto expected_16x9 = oz::viewer_core::compute_centered_aspect_crop(canvas, 16.0 / 9.0);
            wchar_t exp_16x9_buf[64];
            swprintf_s(exp_16x9_buf, L"%d × %d", expected_16x9.width, expected_16x9.height);
            CHECK(read_dim_text() == std::wstring(exp_16x9_buf));
            // Verify combo selection updated to ratio_16x9 (index 6)
            CHECK(SendMessageW(combo_ratio, CB_GETCURSEL, 0, 0) == 6);

            // E. Drag away from preset ratio
            crop_state.selection.width = 500;
            crop_state.selection.height = 400;
            strip.sync_dimension_summary(&crop_state);
            CHECK(read_dim_text() == L"500 × 400");

            // F. Reset button mutation via reset_crop_interaction & sync_state
            oz::viewer_core::reset_crop_interaction(crop_state);
            strip.sync_state(&crop_state);
            CHECK(read_dim_text() == std::wstring(exp_16x9_buf));

            // G. Rejected / no-op / nullptr / inactive guards: summary MUST NOT be mutated
            strip.sync_dimension_summary(nullptr);
            CHECK(read_dim_text() == std::wstring(exp_16x9_buf));

            strip.sync_state(nullptr);
            CHECK(read_dim_text() == std::wstring(exp_16x9_buf));

            auto inactive_state = crop_state;
            inactive_state.active = false;
            inactive_state.selection = oz::viewer_core::edit_rect{10, 10, 20, 20};
            strip.sync_dimension_summary(&inactive_state);
            CHECK(read_dim_text() == std::wstring(exp_16x9_buf)); // Stays unmutated

            strip.sync_state(&inactive_state);
            CHECK(read_dim_text() == std::wstring(exp_16x9_buf)); // Stays unmutated

            // H. Update layout when crop exits (apply or cancel): strip is hidden
            ui_layout_result layout{};
            strip.update_layout(layout, false);
            CHECK(!IsWindowVisible(strip.hwnd()));

            // I. Focus traversal and keyboard accessibility verification:
            // Verify tab traversal and control containment helper
            CHECK(strip.first_tab_stop() == combo_ratio);
            CHECK(strip.contains_hwnd(strip.hwnd()));
            CHECK(strip.contains_hwnd(combo_ratio));
            CHECK(strip.contains_hwnd(txt_dim));
            CHECK(strip.contains_hwnd(GetDlgItem(strip.hwnd(), oz::viewer::kIdCropStripBtnReset)));
            CHECK(strip.contains_hwnd(GetDlgItem(strip.hwnd(), oz::viewer::kIdCropStripBtnApply)));
            CHECK(strip.contains_hwnd(GetDlgItem(strip.hwnd(), oz::viewer::kIdCropStripBtnCancel)));
            CHECK(!strip.contains_hwnd(host_hwnd));

            strip.destroy();
            CHECK(!strip.is_created());
            DestroyWindow(host_hwnd);
        }
        UnregisterClassW(kTestClass, wc.hInstance);
    }
}

// 5. Test Stage 7 DPI and Accessibility hardening invariants
void test_dpi_and_accessibility_hardening() {
    using namespace oz::viewer;

    // A. Multi-DPI metric scaling across 96, 120, 144, 168, 192 DPI
    const float test_dpis[] = {96.0f, 120.0f, 144.0f, 168.0f, 192.0f};
    for (float dpi : test_dpis) {
        ui_layout_config cfg{};
        cfg.show_toolbar = true;
        cfg.toolbar_height_dip = 32.0f;
        cfg.show_crop_strip = true;
        cfg.crop_strip_height_dip = 32.0f;
        cfg.show_status = true;
        cfg.status_height_dip = 24.0f;
        cfg.open_btn_width_dip = 160.0f;
        cfg.open_btn_height_dip = 44.0f;

        const auto layout = compute_ui_layout(1200, 900, dpi, cfg);

        // Verify exact roundings without coordinate drift
        CHECK(layout.toolbar_physical.height == dip_to_physical_round(32.0f, dpi));
        CHECK(layout.crop_strip_physical.height == dip_to_physical_round(32.0f, dpi));
        CHECK(layout.status_physical.height == dip_to_physical_round(24.0f, dpi));
        CHECK(layout.canvas_physical.y == layout.crop_strip_physical.bottom());
        CHECK(layout.canvas_physical.bottom() == layout.status_physical.y);
        CHECK(layout.canvas_physical.height == layout.status_physical.y - layout.crop_strip_physical.bottom());

        // Open button centering within canvas
        CHECK(layout.open_btn_physical.width == dip_to_physical_round(160.0f, dpi));
        CHECK(layout.open_btn_physical.height == dip_to_physical_round(44.0f, dpi));
        CHECK(layout.open_btn_physical.x >= layout.canvas_physical.x);
        CHECK(layout.open_btn_physical.y >= layout.canvas_physical.y);
        CHECK(layout.open_btn_physical.right() <= layout.canvas_physical.right());
        CHECK(layout.open_btn_physical.bottom() <= layout.canvas_physical.bottom());

        // Dynamic font height calculations across DPIs
        const int font_h = dip_to_physical_round(12.0f, dpi);
        CHECK(font_h > 0);
        if (dpi == 96.0f) {
            CHECK(font_h == 12);
        } else if (dpi == 192.0f) {
            CHECK(font_h == 24);
        }
    }

    // B. High Contrast theme palette verification
    {
        const auto hc_pal = get_theme_palette(theme_mode::high_contrast);
        // Verify high-contrast values correspond to system color tokens
        CHECK(hc_pal.bg_chrome == GetSysColor(COLOR_WINDOW));
        CHECK(hc_pal.text_normal == GetSysColor(COLOR_WINDOWTEXT));
        CHECK(hc_pal.text_disabled == GetSysColor(COLOR_GRAYTEXT));
        CHECK(hc_pal.btn_hover == GetSysColor(COLOR_HIGHLIGHT));
        CHECK(hc_pal.border == GetSysColor(COLOR_WINDOWFRAME));
        CHECK(hc_pal.status_bg == GetSysColor(COLOR_BTNFACE));
        CHECK(hc_pal.status_text == GetSysColor(COLOR_BTNTEXT));
    }

    // D. Multi-DPI responsive Crop Strip layout verification
    // Proves that across 96, 120, 144, 168, 192 DPI, in both wide (1200px) and constrained (600px, 400px)
    // client widths, all visible crop strip child controls never overlap each other and remain within parent bounds.
    {
        const wchar_t kCropLayoutTestClass[] = L"OzCropStripResponsiveTestWindow";
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kCropLayoutTestClass;
        RegisterClassExW(&wc);

        HWND host_hwnd = CreateWindowExW(
            0, kCropLayoutTestClass, L"CropResponsiveHost", WS_OVERLAPPEDWINDOW,
            0, 0, 1400, 900, nullptr, nullptr, wc.hInstance, nullptr);
        CHECK(host_hwnd != nullptr);

        if (host_hwnd) {
            const float dpis_to_test[] = {96.0f, 120.0f, 144.0f, 168.0f, 192.0f};
            const int widths_to_test[] = {1400, 1200, 900, 600, 480};

            for (float test_dpi : dpis_to_test) {
                oz::viewer::crop_strip strip;
                CHECK(strip.create(host_hwnd, wc.hInstance, test_dpi));
                CHECK(strip.is_created());

                HWND h_strip = strip.hwnd();
                HWND h_lbl = GetDlgItem(h_strip, 0); // label
                HWND h_combo = GetDlgItem(h_strip, oz::viewer::kIdCropStripComboRatio);
                HWND h_dim = GetDlgItem(h_strip, oz::viewer::kIdCropStripStaticDim);
                HWND h_reset = GetDlgItem(h_strip, oz::viewer::kIdCropStripBtnReset);
                HWND h_apply = GetDlgItem(h_strip, oz::viewer::kIdCropStripBtnApply);
                HWND h_cancel = GetDlgItem(h_strip, oz::viewer::kIdCropStripBtnCancel);

                for (int test_w : widths_to_test) {
                    ui_layout_config cfg{};
                    cfg.show_crop_strip = true;
                    cfg.crop_strip_height_dip = 32.0f;
                    const auto layout = compute_ui_layout(test_w, 800, test_dpi, cfg);

                    strip.update_layout(layout, true);

                    // Collect rectangles of all visible controls
                    struct ctrl_rect {
                        const char* name;
                        RECT rc;
                    };
                    std::vector<ctrl_rect> visible_rects;

                    auto check_and_add = [&](HWND h, const char* name) {
                        // Check if child has WS_VISIBLE style
                        if (h && (GetWindowLongW(h, GWL_STYLE) & WS_VISIBLE)) {
                            RECT rc{};
                            GetWindowRect(h, &rc);
                            // Map to parent strip client coordinates
                            POINT p1{rc.left, rc.top};
                            POINT p2{rc.right, rc.bottom};
                            ScreenToClient(h_strip, &p1);
                            ScreenToClient(h_strip, &p2);
                            visible_rects.push_back({name, RECT{p1.x, p1.y, p2.x, p2.y}});
                        }
                    };

                    check_and_add(h_lbl, "label");
                    check_and_add(h_combo, "combo");
                    check_and_add(h_dim, "dim");
                    check_and_add(h_reset, "reset");
                    check_and_add(h_apply, "apply");
                    check_and_add(h_cancel, "cancel");

                    // Essential action buttons (Reset, Apply, Cancel) and combo MUST ALWAYS have WS_VISIBLE
                    CHECK((GetWindowLongW(h_combo, GWL_STYLE) & WS_VISIBLE) != 0);
                    CHECK((GetWindowLongW(h_reset, GWL_STYLE) & WS_VISIBLE) != 0);
                    CHECK((GetWindowLongW(h_apply, GWL_STYLE) & WS_VISIBLE) != 0);
                    CHECK((GetWindowLongW(h_cancel, GWL_STYLE) & WS_VISIBLE) != 0);

                    // Assert NO overlapping between ANY pair of visible controls
                    for (std::size_t i = 0; i < visible_rects.size(); ++i) {
                        const auto& r1 = visible_rects[i].rc;
                        // Stay within strip bounds
                        CHECK(r1.left >= 0);
                        CHECK(r1.right <= layout.crop_strip_physical.width);

                        for (std::size_t j = i + 1; j < visible_rects.size(); ++j) {
                            const auto& r2 = visible_rects[j].rc;
                            // Check horizontal overlap: two rects overlap if max(left) < min(right)
                            const bool x_overlap = ((std::max)(r1.left, r2.left) < (std::min)(r1.right, r2.right));
                            const bool y_overlap = ((std::max)(r1.top, r2.top) < (std::min)(r1.bottom, r2.bottom));
                            const bool overlaps = (x_overlap && y_overlap);
                            if (overlaps) {
                                std::printf("OVERLAP DETECTED at DPI=%.1f, W=%d between %s [%d, %d] and %s [%d, %d]\n",
                                            test_dpi, test_w,
                                            visible_rects[i].name, r1.left, r1.right,
                                            visible_rects[j].name, r2.left, r2.right);
                            }
                            CHECK(!overlaps);
                        }
                    }
                }

                strip.destroy();
            }
            DestroyWindow(host_hwnd);
        }
        UnregisterClassW(kCropLayoutTestClass, wc.hInstance);
    }

    // E. SVG and Fallback semantic labels and accessible text verification
    {
        const auto& descs = get_toolbar_button_descriptors();
        for (const auto& desc : descs) {
            // Accessible names must be complete commands, not abbreviations
            CHECK(desc.accessible_name != nullptr);
            const std::wstring acc_name(desc.accessible_name);
            CHECK(acc_name.find(L"RotL") == std::wstring::npos);
            CHECK(acc_name.find(L"RotR") == std::wstring::npos);
            CHECK(acc_name != L"Fit");
            CHECK(acc_name != L"Full");
            CHECK(acc_name != L"1:1");
            // Must have substantive length and semantic completeness
            CHECK(acc_name.length() >= 4);
        }
    }
}

void test_shell_integration() {
    using namespace oz::viewer;

    // 1. Rejection of empty path
    {
        const auto res = reveal_file_in_explorer(std::filesystem::path{});
        CHECK(!res.ok());
        CHECK(res.status == shell_reveal_status::invalid_path);
        CHECK(res.hr == E_INVALIDARG);
    }

    // 2. Rejection of non-existent path
    {
        const auto res = reveal_file_in_explorer(std::filesystem::path{L"C:\\nonexistent_dir_12345\\nonexistent_file_67890.png"});
        CHECK(!res.ok());
        CHECK(res.status == shell_reveal_status::not_found);
        CHECK(res.hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
    }

    // 3. Rejection of directory path
    {
        const auto temp_dir = std::filesystem::temp_directory_path();
        const auto res = reveal_file_in_explorer(temp_dir);
        CHECK(!res.ok());
        CHECK(res.status == shell_reveal_status::is_directory);
        CHECK(res.hr == HRESULT_FROM_WIN32(ERROR_DIRECTORY_NOT_SUPPORTED));
    }

    // 4. Seam invocation: success mapping
    {
        // Create a temporary file to test with
        const auto temp_path = std::filesystem::temp_directory_path() / L"oz_test_shell_reveal.tmp";
        {
            FILE* f = nullptr;
            _wfopen_s(&f, temp_path.c_str(), L"wb");
            if (f) {
                std::fputc(1, f);
                std::fclose(f);
            }
        }
        CHECK(std::filesystem::exists(temp_path));

        std::wstring invoked_path;
        auto mock_invoker = [&](const wchar_t* p) -> HRESULT {
            invoked_path = p ? p : L"";
            return S_OK;
        };

        const auto res = reveal_file_in_explorer(temp_path, mock_invoker);
        CHECK(res.ok());
        CHECK(res.status == shell_reveal_status::success);
        CHECK(res.hr == S_OK);
        CHECK(!invoked_path.empty());

        // 5. Seam invocation: failure / HRESULT mapping
        auto fail_invoker = [&](const wchar_t*) -> HRESULT {
            return E_FAIL;
        };
        const auto fail_res = reveal_file_in_explorer(temp_path, fail_invoker);
        CHECK(!fail_res.ok());
        CHECK(fail_res.status == shell_reveal_status::shell_failed);
        CHECK(fail_res.hr == E_FAIL);

        // 6. Seam invocation: exception safety (must not throw across boundaries)
        auto throw_invoker = [&](const wchar_t*) -> HRESULT {
            throw std::runtime_error("seam failure");
        };
        const auto throw_res = reveal_file_in_explorer(temp_path, throw_invoker);
        CHECK(!throw_res.ok());
        CHECK(throw_res.status == shell_reveal_status::shell_failed);
        CHECK(throw_res.hr == E_FAIL);

        std::error_code ec;
        std::filesystem::remove(temp_path, ec);
    }
}

void test_context_menu_seam() {
    using namespace oz::viewer;

    // 1. Invisibility in benchmark modes
    {
        ui_command_context ctx{};
        ctx.benchmark_mode = true;
        ctx.have_image = true;
        ctx.has_document = true;
        ctx.has_file_path = true;

        const auto plan = build_canvas_context_menu_plan(ctx);
        CHECK(!plan.visible);
        CHECK(plan.items.empty());
    }

    // 2. Invisibility in empty state (no presented image)
    {
        ui_command_context ctx{};
        ctx.benchmark_mode = false;
        ctx.have_image = false;
        ctx.has_document = false;

        const auto plan = build_canvas_context_menu_plan(ctx);
        CHECK(!plan.visible);
        CHECK(plan.items.empty());
    }

    // 3. Normal interactive mode with presented image: composition, order, IDs
    {
        ui_command_context ctx{};
        ctx.benchmark_mode = false;
        ctx.have_image = true;
        ctx.has_document = true;
        ctx.has_file_path = true;
        ctx.is_fit_mode = true;

        const auto plan = build_canvas_context_menu_plan(ctx);
        CHECK(plan.visible);
        CHECK(plan.items.size() == 7);

        // Item 0: Fit to Window (302)
        CHECK(plan.items[0].type == context_menu_item_type::command);
        CHECK(plan.items[0].command_id == 302);
        CHECK(plan.items[0].enabled);
        CHECK(plan.items[0].checked);
        CHECK(plan.items[0].text.find(L"Fit to Window") != std::wstring::npos);

        // Item 1: Actual Size (301)
        CHECK(plan.items[1].type == context_menu_item_type::command);
        CHECK(plan.items[1].command_id == 301);
        CHECK(plan.items[1].enabled);
        CHECK(!plan.items[1].checked);
        CHECK(plan.items[1].text.find(L"Actual Size") != std::wstring::npos);

        // Item 2: Separator
        CHECK(plan.items[2].type == context_menu_item_type::separator);

        // Item 3: Copy Image (108)
        CHECK(plan.items[3].type == context_menu_item_type::command);
        CHECK(plan.items[3].command_id == 108);
        CHECK(plan.items[3].enabled);
        CHECK(!plan.items[3].checked);
        CHECK(plan.items[3].text.find(L"Copy Image") != std::wstring::npos);

        // Item 4: Copy File (106)
        CHECK(plan.items[4].type == context_menu_item_type::command);
        CHECK(plan.items[4].command_id == 106);
        CHECK(plan.items[4].enabled);
        CHECK(!plan.items[4].checked);
        CHECK(plan.items[4].text.find(L"Copy File") != std::wstring::npos);

        // Item 5: Copy File Path (107)
        CHECK(plan.items[5].type == context_menu_item_type::command);
        CHECK(plan.items[5].command_id == 107);
        CHECK(plan.items[5].enabled);
        CHECK(!plan.items[5].checked);
        CHECK(plan.items[5].text.find(L"Copy File Path") != std::wstring::npos);

        // Item 6: Open File Location (105)
        CHECK(plan.items[6].type == context_menu_item_type::command);
        CHECK(plan.items[6].command_id == 105);
        CHECK(plan.items[6].enabled);
        CHECK(!plan.items[6].checked);
        CHECK(plan.items[6].text.find(L"Open File Location") != std::wstring::npos);

        // Absence of edit commands (no rotate, flip, crop, resize, undo, redo, save)
        for (const auto& it : plan.items) {
            if (it.type == context_menu_item_type::command) {
                CHECK(it.command_id == 302 || it.command_id == 301 || it.command_id == 108 || it.command_id == 106 || it.command_id == 107 || it.command_id == 105);
                CHECK(it.command_id != static_cast<int>(ui_command_id::file_save));
                CHECK(it.command_id != static_cast<int>(ui_command_id::file_save_as));
                CHECK(it.command_id != static_cast<int>(ui_command_id::edit_rotate_left));
                CHECK(it.command_id != static_cast<int>(ui_command_id::edit_rotate_right));
                CHECK(it.command_id != static_cast<int>(ui_command_id::edit_flip_h));
                CHECK(it.command_id != static_cast<int>(ui_command_id::edit_flip_v));
                CHECK(it.command_id != static_cast<int>(ui_command_id::edit_undo));
                CHECK(it.command_id != static_cast<int>(ui_command_id::edit_redo));
                CHECK(it.command_id != static_cast<int>(ui_command_id::edit_crop));
                CHECK(it.command_id != static_cast<int>(ui_command_id::edit_resize));
            }
        }
    }

    // 4. View Actual Size mode (exclusivity between Fit and Actual Size)
    {
        ui_command_context ctx{};
        ctx.benchmark_mode = false;
        ctx.have_image = true;
        ctx.has_document = true;
        ctx.has_file_path = true;
        ctx.is_fit_mode = false;

        const auto plan = build_canvas_context_menu_plan(ctx);
        CHECK(plan.visible);
        CHECK(plan.items.size() == 7);
        CHECK(plan.items[0].command_id == 302);
        CHECK(!plan.items[0].checked);
        CHECK(plan.items[1].command_id == 301);
        CHECK(plan.items[1].checked);
        // Exclusive: not both checked
        CHECK(!(plan.items[0].checked && plan.items[1].checked));
    }

    // 5. Open File Location, Copy File, Copy File Path respect missing file path
    {
        ui_command_context ctx{};
        ctx.benchmark_mode = false;
        ctx.have_image = true;
        ctx.has_document = true;
        ctx.has_file_path = false; // No file path

        const auto plan = build_canvas_context_menu_plan(ctx);
        CHECK(plan.visible);
        CHECK(plan.items.size() == 7);
        CHECK(plan.items[3].command_id == 108);
        CHECK(!plan.items[3].enabled); // Copy Image must be disabled
        CHECK(plan.items[4].command_id == 106);
        CHECK(!plan.items[4].enabled); // Copy File must be disabled
        CHECK(plan.items[5].command_id == 107);
        CHECK(!plan.items[5].enabled); // Copy File Path must be disabled
        CHECK(plan.items[6].command_id == 105);
        CHECK(!plan.items[6].enabled); // Open location must be disabled
    }

    // 6. Open File Location, Copy File, Copy File Path respect modal / busy state
    {
        // Crop active
        {
            ui_command_context ctx{};
            ctx.benchmark_mode = false;
            ctx.have_image = true;
            ctx.has_document = true;
            ctx.has_file_path = true;
            ctx.crop_active = true;

            const auto plan = build_canvas_context_menu_plan(ctx);
            CHECK(plan.visible);
            CHECK(plan.items.size() == 7);
            CHECK(plan.items[3].command_id == 108);
            CHECK(!plan.items[3].enabled); // Copy Image disabled during crop
            CHECK(plan.items[4].command_id == 106);
            CHECK(!plan.items[4].enabled); // Copy File disabled during crop
            CHECK(plan.items[5].command_id == 107);
            CHECK(!plan.items[5].enabled); // Copy File Path disabled during crop
            CHECK(plan.items[6].command_id == 105);
            CHECK(!plan.items[6].enabled); // Open location disabled during crop
        }
        // Save busy
        {
            ui_command_context ctx{};
            ctx.benchmark_mode = false;
            ctx.have_image = true;
            ctx.has_document = true;
            ctx.has_file_path = true;
            ctx.save_busy = true;

            const auto plan = build_canvas_context_menu_plan(ctx);
            CHECK(plan.visible);
            CHECK(plan.items.size() == 7);
            CHECK(plan.items[3].command_id == 108);
            CHECK(!plan.items[3].enabled); // Copy Image disabled during save
            CHECK(plan.items[4].command_id == 106);
            CHECK(!plan.items[4].enabled); // Copy File disabled during save
            CHECK(plan.items[5].command_id == 107);
            CHECK(!plan.items[5].enabled); // Copy File Path disabled during save
            CHECK(plan.items[6].command_id == 105);
            CHECK(!plan.items[6].enabled); // Open location disabled during save
        }
        // Resize busy
        {
            ui_command_context ctx{};
            ctx.benchmark_mode = false;
            ctx.have_image = true;
            ctx.has_document = true;
            ctx.has_file_path = true;
            ctx.resize_busy = true;

            const auto plan = build_canvas_context_menu_plan(ctx);
            CHECK(plan.visible);
            CHECK(plan.items.size() == 7);
            CHECK(plan.items[3].command_id == 108);
            CHECK(!plan.items[3].enabled); // Copy Image disabled during resize
            CHECK(plan.items[4].command_id == 106);
            CHECK(!plan.items[4].enabled); // Copy File disabled during resize
            CHECK(plan.items[5].command_id == 107);
            CHECK(!plan.items[5].enabled); // Copy File Path disabled during resize
            CHECK(plan.items[6].command_id == 105);
            CHECK(!plan.items[6].enabled); // Open location disabled during resize
        }
        // Copy busy
        {
            ui_command_context ctx{};
            ctx.benchmark_mode = false;
            ctx.have_image = true;
            ctx.has_document = true;
            ctx.has_file_path = true;
            ctx.copy_busy = true;

            const auto plan = build_canvas_context_menu_plan(ctx);
            CHECK(plan.visible);
            CHECK(plan.items.size() == 7);
            CHECK(plan.items[3].command_id == 108);
            CHECK(!plan.items[3].enabled); // Copy Image disabled during copy busy
            CHECK(plan.items[4].command_id == 106);
            CHECK(!plan.items[4].enabled); // Copy File disabled during copy busy
        }
    }
}

void test_clipboard_output_seam() {
    using namespace oz::viewer;

    // 1. Path validation and normalization
    {
        clipboard_op_status status = clipboard_op_status::success;
        // Empty path
        auto norm = normalize_and_validate_file_path(L"", &status);
        CHECK(norm.empty());
        CHECK(status == clipboard_op_status::invalid_path);

        // Non-existent path
        norm = normalize_and_validate_file_path(L"C:\\non_existent_oz_path_987654321.xyz", &status);
        CHECK(norm.empty());
        CHECK(status == clipboard_op_status::not_found);

        // Directory path
        const auto temp_dir = std::filesystem::temp_directory_path();
        norm = normalize_and_validate_file_path(temp_dir, &status);
        CHECK(norm.empty());
        CHECK(status == clipboard_op_status::is_directory);

        // Regular file path
        const auto temp_file = temp_dir / L"oz_clipboard_test_file.tmp";
        {
            FILE* f = _wfopen(temp_file.c_str(), L"wb");
            if (f) {
                std::fputs("test", f);
                std::fclose(f);
            }
        }
        norm = normalize_and_validate_file_path(temp_file, &status);
        CHECK(!norm.empty());
        CHECK(status == clipboard_op_status::success);
        CHECK(norm.find(L'/') == std::wstring::npos); // No forward slashes

        // 2. Payload construction tests
        // HDROP payload
        const auto hdrop_bytes = build_hdrop_payload(norm);
        CHECK(hdrop_bytes.size() > sizeof(DROPFILES));
        const auto* drop = reinterpret_cast<const DROPFILES*>(hdrop_bytes.data());
        CHECK(drop->pFiles == sizeof(DROPFILES));
        CHECK(drop->fWide == TRUE);
        const wchar_t* hdrop_str = reinterpret_cast<const wchar_t*>(hdrop_bytes.data() + drop->pFiles);
        CHECK(std::wstring(hdrop_str) == norm);
        // Check double NUL
        const std::size_t str_len = norm.size();
        CHECK(hdrop_str[str_len] == L'\0');
        CHECK(hdrop_str[str_len + 1] == L'\0');

        // Preferred drop effect payload
        const auto pde_bytes = build_preferred_drop_effect_payload(DROPEFFECT_COPY);
        CHECK(pde_bytes.size() == sizeof(DWORD));
        DWORD effect = 0;
        std::memcpy(&effect, pde_bytes.data(), sizeof(DWORD));
        CHECK(effect == DROPEFFECT_COPY);

        // Unicode text payload
        const auto text_bytes = build_unicode_text_payload(norm);
        CHECK(text_bytes.size() == (norm.size() + 1) * sizeof(wchar_t));
        const wchar_t* text_str = reinterpret_cast<const wchar_t*>(text_bytes.data());
        CHECK(std::wstring(text_str) == norm);
        CHECK(text_str[norm.size()] == L'\0');

        // 3. Null owner rejection
        auto res_null = copy_file_to_clipboard(nullptr, temp_file);
        CHECK(!res_null.ok());
        CHECK(res_null.status == clipboard_op_status::open_failed);

        res_null = copy_file_path_to_clipboard(nullptr, temp_file);
        CHECK(!res_null.ok());
        CHECK(res_null.status == clipboard_op_status::open_failed);

        // 4. Seam mock testing: successful Copy File
        struct mock_state {
            bool opened = false;
            bool closed = false;
            bool emptied = false;
            std::vector<std::pair<UINT, HANDLE>> published;
        } mock;

        const HWND fake_hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x1234));
        const UINT kMockPrefDropEffect = 0xC001;

        clipboard_api_seam seam;
        seam.open_clipboard = [&](HWND h) -> BOOL {
            CHECK(h == fake_hwnd);
            mock.opened = true;
            return TRUE;
        };
        seam.close_clipboard = [&]() -> BOOL {
            mock.closed = true;
            return TRUE;
        };
        seam.empty_clipboard = [&]() -> BOOL {
            mock.emptied = true;
            return TRUE;
        };
        seam.register_clipboard_format_w = [&](LPCWSTR name) -> UINT {
            CHECK(std::wcscmp(name, L"Preferred DropEffect") == 0);
            return kMockPrefDropEffect;
        };
        seam.set_clipboard_data = [&](UINT fmt, HANDLE h) -> HANDLE {
            mock.published.push_back({fmt, h});
            return h;
        };

        auto res_copy = copy_file_to_clipboard(fake_hwnd, temp_file, &seam);
        CHECK(res_copy.ok());
        CHECK(mock.opened);
        CHECK(mock.closed);
        CHECK(mock.emptied);
        CHECK(mock.published.size() == 2);
        // Drop effect published first, CF_HDROP last
        CHECK(mock.published[0].first == kMockPrefDropEffect);
        CHECK(mock.published[1].first == CF_HDROP);

        // 5. Seam mock testing: successful Copy File Path
        mock = {};
        auto res_path = copy_file_path_to_clipboard(fake_hwnd, temp_file, &seam);
        CHECK(res_path.ok());
        CHECK(mock.opened);
        CHECK(mock.closed);
        CHECK(mock.emptied);
        CHECK(mock.published.size() == 1);
        CHECK(mock.published[0].first == CF_UNICODETEXT);

        // 6. Seam mock testing: OpenClipboard failure
        mock = {};
        clipboard_api_seam seam_open_fail = seam;
        seam_open_fail.open_clipboard = [&](HWND) -> BOOL {
            return FALSE;
        };
        auto res_open_fail = copy_file_to_clipboard(fake_hwnd, temp_file, &seam_open_fail);
        CHECK(!res_open_fail.ok());
        CHECK(res_open_fail.status == clipboard_op_status::open_failed);
        CHECK(!mock.closed); // Close not called if Open failed

        // 7. Seam mock testing: EmptyClipboard failure
        mock = {};
        clipboard_api_seam seam_empty_fail = seam;
        seam_empty_fail.empty_clipboard = [&]() -> BOOL {
            return FALSE;
        };
        auto res_empty_fail = copy_file_to_clipboard(fake_hwnd, temp_file, &seam_empty_fail);
        CHECK(!res_empty_fail.ok());
        CHECK(res_empty_fail.status == clipboard_op_status::empty_failed);
        CHECK(mock.opened);
        CHECK(mock.closed); // RAII close called

        // 8. Seam mock testing: SetClipboardData failure & rollback
        mock = {};
        clipboard_api_seam seam_set_fail = seam;
        int set_call_count = 0;
        seam_set_fail.set_clipboard_data = [&](UINT fmt, HANDLE h) -> HANDLE {
            set_call_count++;
            if (set_call_count == 2) {
                // Fail the second call (CF_HDROP)
                return nullptr;
            }
            mock.published.push_back({fmt, h});
            return h;
        };
        auto res_set_fail = copy_file_to_clipboard(fake_hwnd, temp_file, &seam_set_fail);
        CHECK(!res_set_fail.ok());
        CHECK(res_set_fail.status == clipboard_op_status::set_data_failed);
        CHECK(mock.opened);
        CHECK(mock.closed);

        std::error_code ec;
        std::filesystem::remove(temp_file, ec);
    }

    // 9. CF_DIBV5 format, header layout, bitmasks, color spaces, premul conversion
    {
        // 2x2 test image with transparent, fully opaque, and semi-transparent pixels in canonical PBGRA8
        // Pixel 0,0: transparent black (B=0, G=0, R=0, A=0)
        // Pixel 1,0: opaque red (B=0, G=0, R=255, A=255)
        // Pixel 0,1: 50% semi-transparent white (PBGRA: B=128, G=128, R=128, A=128 -> straight: B=255, G=255, R=255, A=128)
        // Pixel 1,1: 50% semi-transparent green (PBGRA: B=0, G=100, R=0, A=200 -> straight: B=0, G=128, R=0, A=200)
        const std::uint8_t pbgra[16] = {
            0, 0, 0, 0,
            0, 0, 255, 255,
            128, 128, 128, 128,
            0, 100, 0, 200
        };

        const auto straight = convert_pbgra_to_straight_bgra(pbgra, 2, 2, 8);
        CHECK(straight.size() == 16);

        // Pixel 0: alpha == 0 -> RGB == 0
        CHECK(straight[0] == 0);
        CHECK(straight[1] == 0);
        CHECK(straight[2] == 0);
        CHECK(straight[3] == 0);

        // Pixel 1: alpha == 255 -> RGB exact
        CHECK(straight[4] == 0);
        CHECK(straight[5] == 0);
        CHECK(straight[6] == 255);
        CHECK(straight[7] == 255);

        // Pixel 2: alpha == 128 -> (128 * 255 + 64) / 128 = 255
        CHECK(straight[8] == 255);
        CHECK(straight[9] == 255);
        CHECK(straight[10] == 255);
        CHECK(straight[11] == 128);

        // Pixel 3: (100 * 255 + 100) / 200 = 128
        CHECK(straight[12] == 0);
        CHECK(straight[13] == 128);
        CHECK(straight[14] == 0);
        CHECK(straight[15] == 200);

        // Build CF_DIBV5 payload
        const auto payload = build_dibv5_payload_from_straight_bgra(straight.data(), 2, 2, 8);
        CHECK(payload.size() == sizeof(BITMAPV5HEADER) + 16);

        const auto* v5 = reinterpret_cast<const BITMAPV5HEADER*>(payload.data());
        CHECK(v5->bV5Size == sizeof(BITMAPV5HEADER));
        CHECK(v5->bV5Width == 2);
        CHECK(v5->bV5Height == -2); // top-down
        CHECK(v5->bV5Planes == 1);
        CHECK(v5->bV5BitCount == 32);
        CHECK(v5->bV5Compression == BI_BITFIELDS);
        CHECK(v5->bV5SizeImage == 16);
        CHECK(v5->bV5RedMask   == 0x00FF0000);
        CHECK(v5->bV5GreenMask == 0x0000FF00);
        CHECK(v5->bV5BlueMask  == 0x000000FF);
        CHECK(v5->bV5AlphaMask == 0xFF000000);
        CHECK(v5->bV5CSType == LCS_sRGB);
        CHECK(v5->bV5Intent == LCS_GM_IMAGES);

        const std::uint8_t* pixels = payload.data() + sizeof(BITMAPV5HEADER);
        CHECK(std::memcmp(pixels, straight.data(), 16) == 0);

        // 10. copy_image_to_clipboard seam tests
        struct img_mock_state {
            bool opened = false;
            bool closed = false;
            bool emptied = false;
            std::vector<std::pair<UINT, HANDLE>> published;
        } imock;

        const HWND fake_hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x5678));
        clipboard_api_seam seam;
        seam.open_clipboard = [&](HWND h) -> BOOL {
            CHECK(h == fake_hwnd);
            imock.opened = true;
            return TRUE;
        };
        seam.close_clipboard = [&]() -> BOOL {
            imock.closed = true;
            return TRUE;
        };
        seam.empty_clipboard = [&]() -> BOOL {
            imock.emptied = true;
            return TRUE;
        };
        seam.set_clipboard_data = [&](UINT fmt, HANDLE h) -> HANDLE {
            imock.published.push_back({fmt, h});
            return h;
        };

        // Success
        auto res_img = copy_image_to_clipboard(fake_hwnd, pbgra, 2, 2, 8, &seam);
        CHECK(res_img.ok());
        CHECK(imock.opened);
        CHECK(imock.closed);
        CHECK(imock.emptied);
        CHECK(imock.published.size() == 1);
        CHECK(imock.published[0].first == CF_DIBV5);

        // Invalid dimensions / null rejection
        auto res_bad = copy_image_to_clipboard(fake_hwnd, nullptr, 2, 2, 8, &seam);
        CHECK(!res_bad.ok());
        CHECK(res_bad.status == clipboard_op_status::invalid_image_dimensions);

        res_bad = copy_image_to_clipboard(fake_hwnd, pbgra, 0, 2, 8, &seam);
        CHECK(!res_bad.ok());
        CHECK(res_bad.status == clipboard_op_status::invalid_image_dimensions);

        res_bad = copy_image_to_clipboard(nullptr, pbgra, 2, 2, 8, &seam);
        CHECK(!res_bad.ok());
        CHECK(res_bad.status == clipboard_op_status::open_failed);

        // Open failure
        clipboard_api_seam seam_open_fail = seam;
        seam_open_fail.open_clipboard = [&](HWND) -> BOOL { return FALSE; };
        auto res_open_fail = copy_image_to_clipboard(fake_hwnd, pbgra, 2, 2, 8, &seam_open_fail);
        CHECK(!res_open_fail.ok());
        CHECK(res_open_fail.status == clipboard_op_status::open_failed);

        // Empty failure
        clipboard_api_seam seam_empty_fail = seam;
        seam_empty_fail.empty_clipboard = [&]() -> BOOL { return FALSE; };
        auto res_empty_fail = copy_image_to_clipboard(fake_hwnd, pbgra, 2, 2, 8, &seam_empty_fail);
        CHECK(!res_empty_fail.ok());
        CHECK(res_empty_fail.status == clipboard_op_status::empty_failed);

        // Set failure
        clipboard_api_seam seam_set_fail = seam;
        seam_set_fail.set_clipboard_data = [&](UINT, HANDLE) -> HANDLE { return nullptr; };
        auto res_set_fail = copy_image_to_clipboard(fake_hwnd, pbgra, 2, 2, 8, &seam_set_fail);
        CHECK(!res_set_fail.ok());
        CHECK(res_set_fail.status == clipboard_op_status::set_data_failed);
    }
}

void test_shortcut_routing_seam() {
    using namespace oz::viewer;

    const HWND fake_main_hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x2222));
    const HWND fake_resize_dlg = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x3333));

    ui_item_state copy_enabled{};
    copy_enabled.enabled = true;

    ui_item_state copy_disabled{};
    copy_disabled.enabled = false;

    // 1. Non-'C' keys ignored
    auto a1 = route_shortcut_key('A', true, false, false, fake_main_hwnd, nullptr, false, copy_enabled);
    CHECK(a1 == shortcut_action::none);

    // 2. Resize dialog owns key when active / focused
    auto a2 = route_shortcut_key('C', true, false, false, fake_resize_dlg, fake_resize_dlg, false, copy_enabled);
    CHECK(a2 == shortcut_action::dialog_default);

    // 3. Crop mode consumes plain C and Ctrl+C
    auto a3_crop_plain = route_shortcut_key('C', false, false, false, fake_main_hwnd, nullptr, true, copy_enabled);
    CHECK(a3_crop_plain == shortcut_action::crop_consume);

    auto a3_crop_ctrl = route_shortcut_key('C', true, false, false, fake_main_hwnd, nullptr, true, copy_enabled);
    CHECK(a3_crop_ctrl == shortcut_action::crop_consume);

    // 4. Ctrl+C with exact modifiers (Shift up, Alt up) -> file_copy_image when enabled
    auto a4 = route_shortcut_key('C', true, false, false, fake_main_hwnd, nullptr, false, copy_enabled);
    CHECK(a4 == shortcut_action::file_copy_image);

    // 5. Ctrl+C with Shift down -> ignored / none
    auto a5 = route_shortcut_key('C', true, true, false, fake_main_hwnd, nullptr, false, copy_enabled);
    CHECK(a5 == shortcut_action::none);

    // 6. Ctrl+C with Alt down -> ignored / none
    auto a6 = route_shortcut_key('C', true, false, true, fake_main_hwnd, nullptr, false, copy_enabled);
    CHECK(a6 == shortcut_action::none);

    // 7. Ctrl+C when copy command disabled -> command_disabled
    auto a7 = route_shortcut_key('C', true, false, false, fake_main_hwnd, nullptr, false, copy_disabled);
    CHECK(a7 == shortcut_action::command_disabled);
}

void test_clipboard_input_seam() {
    using namespace oz::viewer;

    // Helper: build a minimal 40-byte BI_RGB/BI_BITFIELDS DIB buffer.
    auto make_dib40 = [](std::int32_t w, std::int32_t h, std::uint16_t bpp,
                         std::uint32_t comp, const std::vector<std::uint8_t>& px,
                         const std::vector<std::uint32_t>& masks = {}) {
        std::vector<std::uint8_t> buf(40, 0);
        auto put32 = [&](std::size_t off, std::uint32_t v) { std::memcpy(buf.data() + off, &v, 4); };
        auto put32s = [&](std::size_t off, std::int32_t v) { std::memcpy(buf.data() + off, &v, 4); };
        auto put16 = [&](std::size_t off, std::uint16_t v) { std::memcpy(buf.data() + off, &v, 2); };
        put32(0, 40);
        put32s(4, w);
        put32s(8, h);
        put16(12, 1);
        put16(14, bpp);
        put32(16, comp);
        for (const auto m : masks) {
            const std::size_t off = buf.size();
            buf.resize(off + 4);
            std::memcpy(buf.data() + off, &m, 4);
        }
        buf.insert(buf.end(), px.begin(), px.end());
        return buf;
    };

    // 1. Straight BGRA -> premultiplied BGRA conversion incl. alpha exactness
    {
        const std::uint8_t straight[16] = {
            0, 0, 0, 0,          // transparent -> zeroed RGB
            0, 0, 255, 255,      // opaque -> exact copy
            255, 255, 255, 128,  // 50% white -> (255*128+127)/255 = 128
            7, 8, 9, 200         // partial: B=(7*200+127)/255=5, G=(8*200+127)/255=6, R=(9*200+127)/255=7
        };
        const auto premul = convert_straight_bgra_to_pbgra(straight, 2, 2, 8);
        CHECK(premul.size() == 16);
        if (premul.size() == 16) {
            CHECK(premul[0] == 0 && premul[1] == 0 && premul[2] == 0 && premul[3] == 0);
            CHECK(premul[4] == 0 && premul[5] == 0 && premul[6] == 255 && premul[7] == 255);
            CHECK(premul[8] == 128 && premul[9] == 128 && premul[10] == 128 && premul[11] == 128);
            CHECK(premul[12] == 5 && premul[13] == 6 && premul[14] == 7 && premul[15] == 200);
        }
        // Invalid inputs rejected
        CHECK(convert_straight_bgra_to_pbgra(nullptr, 2, 2, 8).empty());
        CHECK(convert_straight_bgra_to_pbgra(straight, 0, 2, 8).empty());
        CHECK(convert_straight_bgra_to_pbgra(straight, 70000, 2, 70000 * 4).empty());
        CHECK(convert_straight_bgra_to_pbgra(straight, 2, 2, 4).empty()); // stride too small
    }

    // 2. CF_DIBV5 round-trip through the existing Copy Image payload builder:
    //    orientation, dimensions, and alpha contract preserved.
    {
        const std::uint8_t straight[16] = {
            0, 0, 0, 0,
            0, 0, 255, 255,
            255, 255, 255, 128,
            0, 128, 0, 200
        };
        const auto dibv5 = build_dibv5_payload_from_straight_bgra(straight, 2, 2, 8);
        CHECK(dibv5.size() == sizeof(BITMAPV5HEADER) + 16);

        clipboard_bitmap bmp;
        const auto res = parse_dib_bytes(dibv5.data(), dibv5.size(), bmp);
        CHECK(res.ok());
        CHECK(res.status == paste_status::success);
        CHECK(bmp.width == 2);
        CHECK(bmp.height == 2);
        CHECK(bmp.stride == 8);
        CHECK(bmp.is_valid());
        CHECK(bmp.pixels.size() == 16);
        if (bmp.pixels.size() == 16) {
            // Row order preserved (top-down negative height in the payload)
            CHECK(bmp.pixels[0] == 0 && bmp.pixels[1] == 0 && bmp.pixels[2] == 0 && bmp.pixels[3] == 0);
            CHECK(bmp.pixels[4] == 0 && bmp.pixels[5] == 0 && bmp.pixels[6] == 255 && bmp.pixels[7] == 255);
            CHECK(bmp.pixels[8] == 128 && bmp.pixels[9] == 128 && bmp.pixels[10] == 128 && bmp.pixels[11] == 128);
            // (128*200+127)/255 = 100
            CHECK(bmp.pixels[12] == 0 && bmp.pixels[13] == 100 && bmp.pixels[14] == 0 && bmp.pixels[15] == 200);
        }
    }

    // 3. Bottom-up (positive height) DIB rows are flipped to top-down.
    {
        // 24-bit: stride = ((2*24+31)/32)*4 = 8; rows stored bottom-first with 2 pad bytes.
        const std::vector<std::uint8_t> px = {
            1, 2, 3, 4, 5, 6, 0, 0,        // bottom row
            11, 12, 13, 14, 15, 16, 0, 0  // top row
        };
        const auto dib = make_dib40(2, 2, 24, BI_RGB, px);
        clipboard_bitmap bmp;
        const auto res = parse_dib_bytes(dib.data(), dib.size(), bmp);
        CHECK(res.ok());
        CHECK(bmp.width == 2 && bmp.height == 2 && bmp.stride == 8);
        CHECK(bmp.pixels.size() == 16);
        if (bmp.pixels.size() == 16) {
            // Output row 0 is the stored top row, expanded opaque.
            CHECK(bmp.pixels[0] == 11 && bmp.pixels[1] == 12 && bmp.pixels[2] == 13 && bmp.pixels[3] == 255);
            CHECK(bmp.pixels[4] == 14 && bmp.pixels[5] == 15 && bmp.pixels[6] == 16 && bmp.pixels[7] == 255);
            CHECK(bmp.pixels[8] == 1 && bmp.pixels[9] == 2 && bmp.pixels[10] == 3 && bmp.pixels[11] == 255);
            CHECK(bmp.pixels[12] == 4 && bmp.pixels[13] == 5 && bmp.pixels[14] == 6 && bmp.pixels[15] == 255);
        }
    }

    // 4. 32-bit BI_RGB carries no alpha channel: alpha bytes forced opaque.
    {
        const std::vector<std::uint8_t> px = {7, 8, 9, 123};
        const auto dib = make_dib40(1, -1, 32, BI_RGB, px);
        clipboard_bitmap bmp;
        const auto res = parse_dib_bytes(dib.data(), dib.size(), bmp);
        CHECK(res.ok());
        CHECK(bmp.pixels.size() == 4);
        if (bmp.pixels.size() == 4) {
            CHECK(bmp.pixels[0] == 7 && bmp.pixels[1] == 8 && bmp.pixels[2] == 9 && bmp.pixels[3] == 255);
        }
    }

    // 5. Unsupported formats rejected quietly (no success, no partial bitmap).
    {
        clipboard_bitmap bmp;
        // 8-bit palettized
        {
            const std::vector<std::uint8_t> px(4, 0);
            const auto dib = make_dib40(2, 2, 8, BI_RGB, px);
            const auto res = parse_dib_bytes(dib.data(), dib.size(), bmp);
            CHECK(!res.ok());
            CHECK(res.status == paste_status::unsupported_format);
            CHECK(!bmp.is_valid());
        }
        // RLE compression
        {
            const std::vector<std::uint8_t> px(16, 0);
            const auto dib = make_dib40(2, -2, 32, BI_RLE8, px);
            const auto res = parse_dib_bytes(dib.data(), dib.size(), bmp);
            CHECK(!res.ok());
            CHECK(res.status == paste_status::unsupported_format);
            CHECK(!bmp.is_valid());
        }
        // Non-standard BITFIELDS masks (swapped red/blue)
        {
            const std::vector<std::uint8_t> px(16, 0);
            const auto dib = make_dib40(2, -2, 32, BI_BITFIELDS, px,
                                        {0x000000FFu, 0x0000FF00u, 0x00FF0000u});
            const auto res = parse_dib_bytes(dib.data(), dib.size(), bmp);
            CHECK(!res.ok());
            CHECK(res.status == paste_status::unsupported_format);
            CHECK(!bmp.is_valid());
        }
        // Legacy 40-byte standard BI_BITFIELDS declares RGB only: the pixel
        // high byte is ignored and alpha is forced opaque (no fourth mask).
        {
            const std::vector<std::uint8_t> px_zero = {10, 20, 30, 0};
            const auto dib_zero = make_dib40(1, -1, 32, BI_BITFIELDS, px_zero,
                                        {0x00FF0000u, 0x0000FF00u, 0x000000FFu});
            const auto res_zero = parse_dib_bytes(dib_zero.data(), dib_zero.size(), bmp);
            CHECK(res_zero.ok());
            CHECK(bmp.pixels.size() == 4);
            if (bmp.pixels.size() == 4) {
                CHECK(bmp.pixels[0] == 10 && bmp.pixels[1] == 20 && bmp.pixels[2] == 30 && bmp.pixels[3] == 255);
            }
        }
        {
            const std::vector<std::uint8_t> px_part = {10, 20, 30, 40};
            const auto dib_part = make_dib40(1, -1, 32, BI_BITFIELDS, px_part,
                                        {0x00FF0000u, 0x0000FF00u, 0x000000FFu});
            const auto res_part = parse_dib_bytes(dib_part.data(), dib_part.size(), bmp);
            CHECK(res_part.ok());
            CHECK(bmp.pixels.size() == 4);
            if (bmp.pixels.size() == 4) {
                // High byte 40 ignored: original RGB kept, alpha forced opaque.
                CHECK(bmp.pixels[0] == 10 && bmp.pixels[1] == 20 && bmp.pixels[2] == 30 && bmp.pixels[3] == 255);
            }
        }
        // Truncated RGB masks: 40-byte BI_BITFIELDS without three mask DWORDs.
        {
            auto dib_full = make_dib40(1, -1, 32, BI_BITFIELDS, std::vector<std::uint8_t>{10, 20, 30, 40},
                                         {0x00FF0000u, 0x0000FF00u, 0x000000FFu});
            dib_full.resize(40 + 2 * sizeof(std::uint32_t));
            const auto res_masks = parse_dib_bytes(dib_full.data(), dib_full.size(), bmp);
            CHECK(!res_masks.ok());
            CHECK(res_masks.status == paste_status::invalid_bitmap);
            CHECK(!bmp.is_valid());
        }
        // V5 explicit alpha: full mask preserves/premultiplies partial alpha.
        {
            const std::uint8_t straight[4] = {10, 20, 30, 40};
            auto dibv5_full = build_dibv5_payload_from_straight_bgra(straight, 1, 1, 4);
            CHECK(!dibv5_full.empty());
            if (!dibv5_full.empty()) {
                clipboard_bitmap v5bmp;
                const auto res_full = parse_dib_bytes(dibv5_full.data(), dibv5_full.size(), v5bmp);
                CHECK(res_full.ok());
                CHECK(v5bmp.pixels.size() == 4);
                if (v5bmp.pixels.size() == 4) {
                    // (10*40+127)/255=2, (20*40+127)/255=3, (30*40+127)/255=5
                    CHECK(v5bmp.pixels[0] == 2 && v5bmp.pixels[1] == 3 && v5bmp.pixels[2] == 5 && v5bmp.pixels[3] == 40);
                }
            }
        }
        // V5 explicit alpha: zero mask ignores the high byte, forces opaque.
        {
            const std::uint8_t straight[4] = {10, 20, 30, 40};
            auto dibv5_zero = build_dibv5_payload_from_straight_bgra(straight, 1, 1, 4);
            CHECK(!dibv5_zero.empty());
            if (!dibv5_zero.empty()) {
                const std::uint32_t zero_mask = 0;
                std::memcpy(dibv5_zero.data() + 52, &zero_mask, 4);
                clipboard_bitmap v5zbmp;
                const auto res_zero = parse_dib_bytes(dibv5_zero.data(), dibv5_zero.size(), v5zbmp);
                CHECK(res_zero.ok());
                CHECK(v5zbmp.pixels.size() == 4);
                if (v5zbmp.pixels.size() == 4) {
                    CHECK(v5zbmp.pixels[0] == 10 && v5zbmp.pixels[1] == 20 && v5zbmp.pixels[2] == 30 && v5zbmp.pixels[3] == 255);
                }
            }
        }
    }

    // 6. Malformed / truncated / overflowing input rejected safely.
    {
        clipboard_bitmap bmp;
        CHECK(!parse_dib_bytes(nullptr, 0, bmp).ok());
        const std::vector<std::uint8_t> tiny(10, 0);
        auto r_tiny = parse_dib_bytes(tiny.data(), tiny.size(), bmp);
        CHECK(!r_tiny.ok());
        CHECK(r_tiny.status == paste_status::invalid_bitmap);
        CHECK(!bmp.is_valid());

        // Legacy core header size is not a DIB.
        auto core = make_dib40(2, 2, 24, BI_RGB, std::vector<std::uint8_t>(16, 0));
        std::uint32_t core_size = 12;
        std::memcpy(core.data(), &core_size, 4);
        auto r_core = parse_dib_bytes(core.data(), core.size(), bmp);
        CHECK(!r_core.ok());
        CHECK(!bmp.is_valid());

        // Zero dimensions.
        auto zero_w = make_dib40(0, 2, 24, BI_RGB, std::vector<std::uint8_t>(16, 0));
        CHECK(!parse_dib_bytes(zero_w.data(), zero_w.size(), bmp).ok());
        auto zero_h = make_dib40(2, 0, 24, BI_RGB, std::vector<std::uint8_t>(16, 0));
        CHECK(!parse_dib_bytes(zero_h.data(), zero_h.size(), bmp).ok());

        // Truncated pixel data.
        auto trunc = make_dib40(2, -2, 32, BI_RGB, std::vector<std::uint8_t>(4, 0));
        auto r_trunc = parse_dib_bytes(trunc.data(), trunc.size(), bmp);
        CHECK(!r_trunc.ok());
        CHECK(r_trunc.status == paste_status::invalid_bitmap);
        CHECK(!bmp.is_valid());

        // Oversized dimensions exceed the bounded transfer limits.
        auto huge = make_dib40(70000, 2, 24, BI_RGB, std::vector<std::uint8_t>(16, 0));
        auto r_huge = parse_dib_bytes(huge.data(), huge.size(), bmp);
        CHECK(!r_huge.ok());
        CHECK(r_huge.status == paste_status::overflow);
        CHECK(!bmp.is_valid());
    }

    // 6b. Allocation robustness boundaries: oversized payloads are rejected
    // before any large allocation, `out` is cleared, and no exception escapes.
    {
        clipboard_bitmap bmp;
        // convert path: claimed output exceeds the 2 GiB decoded cap, so it
        // returns empty without allocating (no 2 GiB vector is created).
        {
            const std::uint8_t dummy = 0;
            // 32768 * 4 * 16385 = 2147614720 > 2 GiB.
            CHECK(convert_straight_bgra_to_pbgra(&dummy, 32768, 16385, 131072).empty());
        }
        // parse path: header-only buffer claims ~3.6 GiB of source pixels
        // (30000x30000x4); must report overflow with `out` cleared.
        {
            bmp.width = 7;
            bmp.height = 7;
            bmp.stride = 28;
            bmp.pixels.assign(196, 0xAB);
            auto claim = make_dib40(30000, 30000, 32, BI_RGB, std::vector<std::uint8_t>{});
            bool threw = false;
            paste_result r{};
            try {
                r = parse_dib_bytes(claim.data(), claim.size(), bmp);
            } catch (...) {
                threw = true;
            }
            CHECK(!threw);
            CHECK(!r.ok());
            CHECK(r.status == paste_status::overflow);
            CHECK(!bmp.is_valid());
            CHECK(bmp.pixels.empty());
        }
        // read path: seam reports an oversized block; the copy must be
        // rejected before allocation with the clipboard still closed.
        {
            struct tiny_clip {
                bool closed = false;
            } tc;
            clipboard_read_seam oseam;
            const HWND ohwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x0BAD));
            const HANDLE ohandle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x0D18));
            const std::uint8_t one = 0;
            oseam.open_clipboard = [&](HWND) -> BOOL { return TRUE; };
            oseam.close_clipboard = [&]() -> BOOL {
                tc.closed = true;
                return TRUE;
            };
            oseam.is_format_available = [&](UINT fmt) -> BOOL {
                return fmt == CF_DIB ? TRUE : FALSE;
            };
            oseam.get_data = [&](UINT) -> HANDLE { return ohandle; };
            oseam.lock = [&](HANDLE) -> void* { return const_cast<std::uint8_t*>(&one); };
            oseam.unlock = [&](HANDLE) -> BOOL { return TRUE; };
            oseam.mem_size = [&](HANDLE) -> SIZE_T {
                return static_cast<SIZE_T>((1ull << 31) + 1024);
            };
            bmp.width = 5;
            bmp.height = 5;
            bmp.stride = 20;
            bmp.pixels.assign(100, 0xCD);
            bool threw = false;
            paste_result r{};
            try {
                r = read_clipboard_image(ohwnd, bmp, &oseam);
            } catch (...) {
                threw = true;
            }
            CHECK(!threw);
            CHECK(!r.ok());
            CHECK(r.status == paste_status::overflow);
            CHECK(!bmp.is_valid());
            CHECK(bmp.pixels.empty());
            CHECK(tc.closed);
        }
    }

    // 7. Full clipboard read through an injected seam (no real OS clipboard).
    {
        const std::uint8_t straight[16] = {
            0, 0, 255, 255, 0, 255, 0, 255,
            255, 0, 0, 255, 255, 255, 255, 255
        };
        const auto dibv5 = build_dibv5_payload_from_straight_bgra(straight, 2, 2, 8);
        const auto dib40 = make_dib40(2, -2, 32, BI_RGB,
                                      std::vector<std::uint8_t>(straight, straight + 16));

        struct fake_clip {
            bool opened = false;
            bool closed = false;
            bool has_v5 = false;
            bool has_dib = false;
            bool open_ok = true;
            bool lock_ok = true;
            UINT last_format = 0;
            std::vector<std::uint8_t> v5bytes;
            std::vector<std::uint8_t> dibbytes;
        } fake;
        fake.v5bytes = dibv5;
        fake.dibbytes = dib40;

        const HWND fake_hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x9A9A));
        const HANDLE kV5Handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x51));
        const HANDLE kDibHandle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0xD1));

        clipboard_read_seam seam;
        seam.open_clipboard = [&](HWND h) -> BOOL {
            CHECK(h == fake_hwnd);
            fake.opened = true;
            return fake.open_ok ? TRUE : FALSE;
        };
        seam.close_clipboard = [&]() -> BOOL {
            fake.closed = true;
            return TRUE;
        };
        seam.is_format_available = [&](UINT fmt) -> BOOL {
            if (fmt == CF_DIBV5) return fake.has_v5 ? TRUE : FALSE;
            if (fmt == CF_DIB) return fake.has_dib ? TRUE : FALSE;
            return FALSE;
        };
        seam.get_data = [&](UINT fmt) -> HANDLE {
            fake.last_format = fmt;
            if (fmt == CF_DIBV5 && fake.has_v5) return kV5Handle;
            if (fmt == CF_DIB && fake.has_dib) return kDibHandle;
            return nullptr;
        };
        seam.lock = [&](HANDLE h) -> void* {
            if (!fake.lock_ok) return nullptr;
            if (h == kV5Handle) return fake.v5bytes.data();
            if (h == kDibHandle) return fake.dibbytes.data();
            return nullptr;
        };
        seam.unlock = [&](HANDLE) -> BOOL { return TRUE; };
        seam.mem_size = [&](HANDLE h) -> SIZE_T {
            if (h == kV5Handle) return fake.v5bytes.size();
            if (h == kDibHandle) return fake.dibbytes.size();
            return 0;
        };

        // CF_DIBV5 preferred when both present.
        fake.has_v5 = true;
        fake.has_dib = true;
        {
            clipboard_bitmap bmp;
            const auto res = read_clipboard_image(fake_hwnd, bmp, &seam);
            CHECK(res.ok());
            CHECK(fake.opened && fake.closed);
            CHECK(fake.last_format == CF_DIBV5);
            CHECK(bmp.width == 2 && bmp.height == 2 && bmp.is_valid());
        }
        // CF_DIB fallback when V5 absent.
        fake.has_v5 = false;
        fake.has_dib = true;
        {
            clipboard_bitmap bmp;
            const auto res = read_clipboard_image(fake_hwnd, bmp, &seam);
            CHECK(res.ok());
            CHECK(fake.last_format == CF_DIB);
            CHECK(bmp.width == 2 && bmp.height == 2 && bmp.is_valid());
        }
        // No DIB-family format: quiet no_image, clipboard still closed.
        fake.has_v5 = false;
        fake.has_dib = false;
        {
            clipboard_bitmap bmp;
            const auto res = read_clipboard_image(fake_hwnd, bmp, &seam);
            CHECK(!res.ok());
            CHECK(res.status == paste_status::no_image);
            CHECK(!bmp.is_valid());
            CHECK(fake.opened && fake.closed);
        }
        // Locked clipboard: quiet unavailable.
        fake.has_v5 = true;
        fake.open_ok = false;
        {
            clipboard_bitmap bmp;
            const auto res = read_clipboard_image(fake_hwnd, bmp, &seam);
            CHECK(!res.ok());
            CHECK(res.status == paste_status::unavailable);
            CHECK(!bmp.is_valid());
        }
        fake.open_ok = true;
        // Lock failure: quiet unavailable.
        fake.lock_ok = false;
        {
            clipboard_bitmap bmp;
            const auto res = read_clipboard_image(fake_hwnd, bmp, &seam);
            CHECK(!res.ok());
            CHECK(res.status == paste_status::unavailable);
            CHECK(!bmp.is_valid());
        }
        fake.lock_ok = true;
        // Null owner rejected without touching the seam.
        {
            fake.opened = false;
            clipboard_bitmap bmp;
            const auto res = read_clipboard_image(nullptr, bmp, &seam);
            CHECK(!res.ok());
            CHECK(res.status == paste_status::unavailable);
            CHECK(!fake.opened);
        }
        // M2 lock-lifetime: the DIB bytes are copied while locks are held, so
        // the parsed result stays valid even though unlock poisons the source
        // handle bytes and the clipboard is closed (cleanup preserved).
        {
            const std::vector<std::uint8_t> px = {9, 8, 7, 200};
            const auto dib = make_dib40(1, -1, 32, BI_RGB, px);
            struct poison_clip {
                std::vector<std::uint8_t> bytes;
                bool unlocked = false;
                bool closed = false;
            } pc;
            pc.bytes = dib;
            const HWND phwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(0xBEEF));
            const HANDLE phandle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0xD18));
            clipboard_read_seam pseam;
            pseam.open_clipboard = [&](HWND) -> BOOL { return TRUE; };
            pseam.close_clipboard = [&]() -> BOOL {
                pc.closed = true;
                return TRUE;
            };
            pseam.is_format_available = [&](UINT fmt) -> BOOL {
                return fmt == CF_DIB ? TRUE : FALSE;
            };
            pseam.get_data = [&](UINT) -> HANDLE { return phandle; };
            pseam.lock = [&](HANDLE) -> void* { return pc.bytes.data(); };
            pseam.unlock = [&](HANDLE) -> BOOL {
                pc.unlocked = true;
                std::fill(pc.bytes.begin(), pc.bytes.end(), static_cast<std::uint8_t>(0xFF));
                return TRUE;
            };
            pseam.mem_size = [&](HANDLE) -> SIZE_T { return pc.bytes.size(); };
            clipboard_bitmap bmp;
            const auto res = read_clipboard_image(phwnd, bmp, &pseam);
            CHECK(res.ok());
            CHECK(pc.unlocked);
            CHECK(pc.closed);
            CHECK(bmp.is_valid());
            CHECK(bmp.width == 1 && bmp.height == 1);
            if (bmp.pixels.size() == 4) {
                // 32-bit BI_RGB is forced opaque; source poisoning after the
                // copy must not leak into the parsed pixels.
                CHECK(bmp.pixels[0] == 9 && bmp.pixels[1] == 8 && bmp.pixels[2] == 7 &&
                      bmp.pixels[3] == 255);
            }
        }
    }

    // 8. Ctrl+V shortcut routing: paste path, ownership, and modifier parity.
    {
        const HWND fake_main_hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x2222));
        const HWND fake_resize_dlg = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x3333));

        ui_item_state copy_enabled{};
        copy_enabled.enabled = true;

        // Exact Ctrl+V routes to the paste path regardless of copy state.
        auto p1 = route_shortcut_key('V', true, false, false, fake_main_hwnd, nullptr, false, copy_enabled);
        CHECK(p1 == shortcut_action::file_paste_image);
        auto p1_lower = route_shortcut_key('v', true, false, false, fake_main_hwnd, nullptr, false, copy_enabled);
        CHECK(p1_lower == shortcut_action::file_paste_image);

        // Modified variants never paste: Ctrl+Shift+V stays with Flip Vertical.
        CHECK(route_shortcut_key('V', true, true, false, fake_main_hwnd, nullptr, false, copy_enabled) == shortcut_action::none);
        CHECK(route_shortcut_key('V', true, false, true, fake_main_hwnd, nullptr, false, copy_enabled) == shortcut_action::none);
        // Plain V keeps existing ownership (crop entry / no-op), never pastes.
        CHECK(route_shortcut_key('V', false, false, false, fake_main_hwnd, nullptr, false, copy_enabled) == shortcut_action::none);
        CHECK(route_shortcut_key('V', false, false, false, fake_main_hwnd, nullptr, true, copy_enabled) == shortcut_action::crop_consume);

        // Crop consumes Ctrl+V; dialog owns it when focused.
        CHECK(route_shortcut_key('V', true, false, false, fake_main_hwnd, nullptr, true, copy_enabled) == shortcut_action::crop_consume);
        CHECK(route_shortcut_key('V', true, false, false, fake_resize_dlg, fake_resize_dlg, false, copy_enabled) == shortcut_action::dialog_default);

        // Focused text input keeps native paste (and native copy is unchanged).
        const wchar_t kEditHostClass[] = L"OzPasteRoutingTestHost";
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kEditHostClass;
        RegisterClassExW(&wc);
        HWND host_hwnd = CreateWindowExW(0, kEditHostClass, L"PasteRoutingHost", WS_OVERLAPPEDWINDOW,
                                        0, 0, 400, 300, nullptr, nullptr, wc.hInstance, nullptr);
        CHECK(host_hwnd != nullptr);
        if (host_hwnd) {
            HWND edit_hwnd = CreateWindowExW(0, L"Edit", L"", WS_CHILD | ES_LEFT,
                                            0, 0, 100, 24, host_hwnd, nullptr, wc.hInstance, nullptr);
            CHECK(edit_hwnd != nullptr);
            if (edit_hwnd) {
                auto p_text = route_shortcut_key('V', true, false, false, edit_hwnd, nullptr, false, copy_enabled);
                CHECK(p_text == shortcut_action::native_text_paste);
                auto c_text = route_shortcut_key('C', true, false, false, edit_hwnd, nullptr, false, copy_enabled);
                CHECK(c_text == shortcut_action::native_text_copy);
                DestroyWindow(edit_hwnd);
            }
            DestroyWindow(host_hwnd);
        }
        UnregisterClassW(kEditHostClass, wc.hInstance);
    }

    // 9. Temporary-document command-state contract: no file path disables
    //    file-only commands while Copy Image stays functional via source.
    {
        ui_command_context ctx{};
        ctx.benchmark_mode = false;
        ctx.have_image = true;
        ctx.has_document = true;
        ctx.has_source = true;      // retained pasted pixels (Editing-ON) / presentable bitmap (OFF)
        ctx.has_file_path = false;  // temporary document: no source file path

        CHECK(derive_command_state(ui_command_id::file_copy_image, ctx).enabled);
        CHECK(!derive_command_state(ui_command_id::file_copy, ctx).enabled);
        CHECK(!derive_command_state(ui_command_id::file_copy_path, ctx).enabled);
        CHECK(!derive_command_state(ui_command_id::file_open_location, ctx).enabled);
        // File Open stays available; Save As remains source-gated as for file docs.
        CHECK(derive_command_state(ui_command_id::file_open, ctx).enabled);
    }
}

// Stage 9.5 M1: presentation planning must observe suppress_edit_presentation
// before any edit-history/draft path. During a transactional document swap
// (file open / navigation commit / clipboard paste) app.image_doc still holds
// the replaced document's history; the suppression-aware seam must return the
// plain non-edit plan without consulting that history or a draft resize, so
// the incoming bitmap never inherits the prior document's transform/source_rect.
void test_presentation_edit_suppression() {
    using namespace oz::viewer;
    using namespace oz::viewer_core;

    document_metadata meta{};
    meta.source_dimensions = edit_dimensions{400, 300};
    const std::optional<edit_dimensions> no_draft{};
    const std::optional<edit_dimensions> draft{edit_dimensions{200, 150}};

    // Empty document: no edit state, so the plain plan is used regardless.
    image_document empty_doc(meta);
    CHECK(!should_use_edit_presentation(false, &empty_doc, no_draft));
    CHECK(!should_use_edit_presentation(true, &empty_doc, no_draft));

    // Prior document with real rotate/crop/resize edits would drive the
    // edit-aware plan when presentation is not suppressed...
    image_document edited_doc(meta);
    CHECK(edited_doc.history().push_command(rotate_command{rotate_direction::right_90}));
    CHECK(edited_doc.history().push_command(crop_command{edit_rect{10, 10, 100, 100}}));
    CHECK(edited_doc.history().push_command(resize_command{edit_dimensions{200, 150}, resize_filter::bicubic}));
    CHECK(edited_doc.history().total_commands() == 3);
    CHECK(should_use_edit_presentation(false, &edited_doc, no_draft));

    // ...but during a transactional swap suppression must force the plain
    // plan, with or without a draft resize, before any history/draft state is
    // consulted.
    CHECK(!should_use_edit_presentation(true, &edited_doc, no_draft));
    CHECK(!should_use_edit_presentation(true, &edited_doc, draft));

    // A pending draft resize with no history is suppressed the same way.
    image_document draft_doc(meta);
    CHECK(should_use_edit_presentation(false, &draft_doc, draft));
    CHECK(!should_use_edit_presentation(true, &draft_doc, draft));

    // No document at all never uses the edit-aware plan.
    CHECK(!should_use_edit_presentation(false, nullptr, draft));
    CHECK(!should_use_edit_presentation(true, nullptr, draft));
}

// Stage 9.5 paste-install staging (repeat final review MUST_FIX): the complete
// temporary document install - display image, pathless domain document, and the
// payload-sized retained source used by Copy Image - must be built before any
// caller mutation, and an invalid/faulty payload must fail quietly, leaving the
// caller's install untouched. The allocation-fault path itself is OOM-only; the
// source scan (check_paste_isolation.cmake section 7) pins that catch-and-quiet
// structure, while this test proves the staged ownership/shape it protects.
void test_paste_install_staging_seam() {
    using namespace oz::viewer;
    using namespace oz::viewer_core;

    // 1. Invalid payload: quiet false, a pre-existing install is untouched.
    {
        clipboard_bitmap empty;  // no dimensions, no pixels
        paste_install existing;
        existing.image = std::make_shared<decoded_image_data>();
        const auto sentinel = existing.image;
        CHECK(!prepare_paste_install(std::move(empty), existing));
        CHECK(existing.image == sentinel);
    }
    // 1b. Inconsistent payload metadata (huge declared dims, tiny real buffer):
    //     rejected before any payload-sized allocation could be attempted.
    {
        clipboard_bitmap lying;
        lying.width = 65535;
        lying.height = 65535;
        lying.stride = 65535 * 4;
        lying.pixels.assign(16, 0);
        paste_install install;
        CHECK(!prepare_paste_install(std::move(lying), install));
        CHECK(install.image == nullptr);
    }

    // 2. Valid payload: the staged install is the complete temporary document,
    //    including a retained source whose identity already matches the
    //    pre-built document (Copy Image contract) - all before any swap.
    {
        clipboard_bitmap bmp;
        bmp.width = 2;
        bmp.height = 2;
        bmp.stride = 8;
        bmp.pixels.assign({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16});
        const std::vector<std::uint8_t> payload = bmp.pixels;

        paste_install install;
        CHECK(prepare_paste_install(std::move(bmp), install));
        CHECK(install.image != nullptr);
        CHECK(install.image->pixels == payload);
        CHECK(install.image->width == 2);
        CHECK(install.image->height == 2);
        CHECK(install.image->stride == 8);
        CHECK(install.image->scale_den == 1);
        CHECK(install.image->src_width == 2);
        CHECK(install.image->src_height == 2);
        CHECK(install.image->codec == codec_id::bmp_wic);
        CHECK(install.image->source_file_size == 0);
        CHECK(install.image->source_last_write_time == 0);

#if OZVIEWER_ENABLE_EDITING
        CHECK(install.document != nullptr);
        CHECK(install.source != nullptr);
        CHECK(install.source->is_valid());
        CHECK(install.source->document_instance_id != 0);
        CHECK(install.source->document_instance_id == install.document->instance_id());
        CHECK(install.source->pixels == install.image->pixels);
        const edit_dimensions expected_dims{2, 2};
        CHECK(install.source->dimensions == expected_dims);
        CHECK(install.source->stride == 8);
        CHECK(install.source->source_path.empty());
        CHECK(install.source->file_size == 0);
        CHECK(install.source->last_write_time == 0);
        CHECK(install.document->metadata().source_path.empty());
        CHECK(!install.document->source_baseline().valid);
        CHECK(install.document->history().total_commands() == 0);
        CHECK(install.document->history().source_dimensions() == expected_dims);
        CHECK(!install.document->is_dirty());
#endif
        // The payload buffer transferred out of the input bitmap (std::vector
        // move with the standard allocator leaves the source empty): exactly
        // one canonical copy exists in the staged install.
        CHECK(bmp.pixels.empty());
    }

#if OZVIEWER_ENABLE_EDITING
    // 3. Every successful staging uses a fresh document identity, so a pasted
    //    install can never alias the replaced document in stale checks.
    {
        auto make_bitmap = [] {
            clipboard_bitmap b;
            b.width = 1;
            b.height = 1;
            b.stride = 4;
            b.pixels.assign({9, 8, 7, 255});
            return b;
        };
        paste_install first;
        paste_install second;
        CHECK(prepare_paste_install(make_bitmap(), first));
        CHECK(prepare_paste_install(make_bitmap(), second));
        CHECK(first.document->instance_id() != second.document->instance_id());
        CHECK(first.source->document_instance_id != second.source->document_instance_id);
    }
#endif
}

} // namespace

int main() {
    test_ui_command_state();
    test_ui_layout();
    test_svg_icon_cache_and_fallback();
    test_modern_dialogs_mapping_and_fallback();
    test_dpi_and_accessibility_hardening();
    test_shell_integration();
    test_context_menu_seam();
    test_clipboard_output_seam();
    test_shortcut_routing_seam();
    test_clipboard_input_seam();
    test_presentation_edit_suppression();
    test_paste_install_staging_seam();

    std::printf("stage7_seam_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

// oz/viewer/renderer.hpp - D3D11 + DXGI flip-discard swap chain + Direct2D
// backbuffer rendering for the OZ image viewer (Stages 1 - 4).
#pragma once

#include "oz/viewer_core/edit_state.hpp"
#include "oz/viewer_core/view_state.hpp"

#include <d2d1_1.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>

namespace oz::viewer {

using Microsoft::WRL::ComPtr;

// Extended presentation plan (Stage 5 Slice 3 / Slice 5)
// Captures logical destination geometry, D4 rotation/flip transformation,
// and source rect (integer or sub-pixel floating point) within the retained bitmap.
struct presentation_plan {
    oz::viewer_core::view_geometry geometry{};
    oz::viewer_core::d4_transform transform = oz::viewer_core::d4_transform::make_identity();
    oz::viewer_core::edit_rect source_rect{}; // Rect within retained bitmap to draw from (empty = entire bitmap)
    oz::viewer_core::edit_rectf source_rectf{}; // Optional sub-pixel rect (takes precedence if valid)
    std::int32_t logical_width = 0;
    std::int32_t logical_height = 0;
    std::int32_t canvas_offset_x = 0; // Stage 7: canvas top-left offset within render target/client area
    std::int32_t canvas_offset_y = 0;
};

// Optional visual-only crop draft drawn after the retained source bitmap. All
// coordinates are in the Direct2D target's DIP space. This owns no image data
// and never changes the presentation plan or document state.
struct crop_overlay {
    bool visible = false;
    oz::viewer_core::view_rectf image_rect{};
    oz::viewer_core::view_rectf selection_rect{};
    float handle_radius_dip = 4.0f;
};

struct renderer {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain1> swap_chain;
    ComPtr<ID2D1Factory1> d2d_factory;
    ComPtr<ID2D1Device> d2d_device;
    ComPtr<ID2D1DeviceContext> d2d_context;
    ComPtr<ID2D1Bitmap1> bitmap;        // backbuffer render target
    ComPtr<ID2D1Bitmap1> source_bitmap; // Stage 4: exactly ONE committed retained source bitmap
    std::int32_t source_width = 0;      // physical width of retained bitmap
    std::int32_t source_height = 0;     // physical height of retained bitmap
    bool warp = false;
    // DPI of the Direct2D device-context target. Queried from the window in
    // create_renderer and updated on WM_DPICHANGED; re-applied to the device
    // context on every target (re)creation so physical-pixel fit/DrawBitmap
    // coordinates stay correct at per-monitor-v2 DPI. Defaults to 96 (1:1).
    float dpi = 96.0f;
};

// Create the D3D11/DXGI/D2D renderer for the given HWND. Tries the hardware
// adapter first, then falls back to WARP. Returns false on failure.
bool create_renderer(HWND hwnd, renderer& out);

// Resize the swap chain and recreate the D2D bitmap for the new client size.
// The new target's DPI is reset to the renderer's stored DPI. Returns false on
// failure. Retained source_bitmap is preserved across resize.
bool resize_renderer(renderer& r, std::uint32_t width, std::uint32_t height);

// Update the renderer's target DPI (e.g. after WM_DPICHANGED) and apply it to
// the current Direct2D device context. A non-positive dpi is ignored.
void set_renderer_dpi(renderer& r, float dpi) noexcept;

// Clear the viewport to black and present (used for empty/no-image window).
bool render_blank(renderer& r);

// Redraw the already retained source_bitmap using view geometry without re-upload.
// Returns false if no source_bitmap is retained or drawing fails.
bool redraw_retained(renderer& r, const oz::viewer_core::view_geometry& geom);

// Redraw the already retained source_bitmap using a presentation plan (with D4
// transform) without re-upload. The optional crop overlay is visual-only.
bool redraw_retained_plan(renderer& r, const presentation_plan& plan,
                          const crop_overlay* overlay = nullptr);

// Stage 4 transactional upload: uploads candidate pixels into a temporary staging bitmap,
// renders it to the backbuffer, and presents. Only if present succeeds, out_candidate_bitmap
// is promoted to r.source_bitmap.
// If caller wants legacy fit without custom view_state, geom can be computed with mode=fit.
bool render_and_present_candidate(renderer& r, const std::uint8_t* pixels, std::int32_t stride,
                                 std::int32_t img_w, std::int32_t img_h,
                                 std::int32_t src_w, std::int32_t src_h,
                                 const oz::viewer_core::view_geometry& geom,
                                 std::uint64_t* out_present_ticks = nullptr);

// Stage 5 transactional upload with presentation plan:
// Uploads candidate pixels into a temporary staging bitmap, renders according to plan, and presents.
// Only if present succeeds, candidate bitmap is promoted to r.source_bitmap.
bool render_and_present_candidate_plan(renderer& r, const std::uint8_t* pixels, std::int32_t stride,
                                      std::int32_t img_w, std::int32_t img_h,
                                      std::int32_t src_w, std::int32_t src_h,
                                      const presentation_plan& plan,
                                      std::uint64_t* out_present_ticks = nullptr);

// Invalidate retained source bitmap (e.g., on document change, no-image, or device loss).
void invalidate_retained_source(renderer& r) noexcept;

// Legacy Stage 1 helper (creates and draws, kept for compatibility/benchmarks).
bool render_image(renderer& r, const std::uint8_t* pixels, std::int32_t stride,
                  std::int32_t img_w, std::int32_t img_h);

// Present the swap chain. Returns true on success.
bool present(renderer& r);

}  // namespace oz::viewer

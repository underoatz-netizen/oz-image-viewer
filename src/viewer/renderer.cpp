#include "renderer.hpp"

#include "oz/viewer_core/fit.hpp"
#include "oz/viewer_core/navigation_benchmark.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>  // GetDpiForWindow

#include <d2d1helper.h>

#include <algorithm>
#include <iterator>  // std::size
#include <utility>   // std::move

namespace oz::viewer {
namespace {

constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

}  // namespace

bool create_renderer(HWND hwnd, renderer& out) {
    // --- D3D11 device (hardware, then WARP fallback) -----------------------
    const D3D_DRIVER_TYPE driver_types[] = {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP};
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    bool warp = false;
    for (const auto dt : driver_types) {
        const HRESULT hr = D3D11CreateDevice(
            nullptr, dt, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
            static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &device, nullptr, &context);
        if (SUCCEEDED(hr)) {
            warp = (dt == D3D_DRIVER_TYPE_WARP);
            break;
        }
    }
    if (!device || !context) {
        return false;
    }

    // --- DXGI flip-discard swap chain (2 buffers) --------------------------
    ComPtr<IDXGIDevice> dxgi_device;
    if (FAILED(device.As(&dxgi_device))) {
        return false;
    }
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgi_device->GetAdapter(&adapter))) {
        return false;
    }
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = 0;  // use the window client size
    desc.Height = 0;
    desc.Format = kBackBufferFormat;
    desc.Stereo = FALSE;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = 0;

    ComPtr<IDXGISwapChain1> swap_chain;
    if (FAILED(factory->CreateSwapChainForHwnd(device.Get(), hwnd, &desc, nullptr, nullptr,
                                               &swap_chain))) {
        return false;
    }
    // Disable the automatic Alt+Enter fullscreen transition.
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    // --- Direct2D device context over the swap chain backbuffer ------------
    ComPtr<ID2D1Factory1> d2d_factory;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&d2d_factory)))) {
        return false;
    }
    ComPtr<ID2D1Device> d2d_device;
    if (FAILED(d2d_factory->CreateDevice(dxgi_device.Get(), &d2d_device))) {
        return false;
    }
    ComPtr<ID2D1DeviceContext> d2d_context;
    if (FAILED(d2d_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2d_context))) {
        return false;
    }

    out.device = std::move(device);
    out.context = std::move(context);
    out.swap_chain = std::move(swap_chain);
    out.d2d_factory = std::move(d2d_factory);
    out.d2d_device = std::move(d2d_device);
    out.d2d_context = std::move(d2d_context);
    out.source_bitmap.Reset();
    out.source_width = 0;
    out.source_height = 0;
    out.warp = warp;
    // Query the window's current DPI (per-monitor-v2 aware). WM_DPICHANGED
    // updates it thereafter; resize_renderer applies it to the target.
    const UINT window_dpi = GetDpiForWindow(hwnd);
    out.dpi = (window_dpi != 0) ? static_cast<float>(window_dpi) : 96.0f;
    return true;
}

bool resize_renderer(renderer& r, std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0) {
        return false;
    }
    r.bitmap.Reset();
    r.d2d_context->SetTarget(nullptr);
    if (FAILED(r.swap_chain->ResizeBuffers(0, width, height, kBackBufferFormat, 0))) {
        return false;
    }
    ComPtr<IDXGISurface> surface;
    if (FAILED(r.swap_chain->GetBuffer(0, IID_PPV_ARGS(&surface)))) {
        return false;
    }
    const D2D1_BITMAP_PROPERTIES1 props =
        D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                D2D1::PixelFormat(kBackBufferFormat, D2D1_ALPHA_MODE_IGNORE));
    if (FAILED(r.d2d_context->CreateBitmapFromDxgiSurface(surface.Get(), &props, &r.bitmap))) {
        return false;
    }
    r.d2d_context->SetTarget(r.bitmap.Get());
    // SetTarget resets the device-context DPI to the new bitmap's default (96);
    // re-apply the window DPI so DrawBitmap DIP coordinates map to physical
    // pixels correctly at per-monitor-v2 DPI.
    r.d2d_context->SetDpi(r.dpi, r.dpi);
    return true;
}

void set_renderer_dpi(renderer& r, float dpi) noexcept {
    if (dpi <= 0.0f) {
        return;
    }
    r.dpi = dpi;
    if (r.d2d_context) {
        r.d2d_context->SetDpi(r.dpi, r.dpi);
    }
}

void invalidate_retained_source(renderer& r) noexcept {
    r.source_bitmap.Reset();
    r.source_width = 0;
    r.source_height = 0;
}

bool render_blank(renderer& r) {
    if (!r.bitmap || !r.d2d_context) {
        return false;
    }
    r.d2d_context->BeginDraw();
    r.d2d_context->Clear(D2D1::ColorF(D2D1::ColorF::Black));
    const HRESULT hr = r.d2d_context->EndDraw();
    if (FAILED(hr)) {
        return false;
    }
    return present(r);
}

static void apply_presentation_plan_drawing(ID2D1DeviceContext* ctx,
                                            ID2D1Bitmap1* bmp,
                                            const presentation_plan& plan,
                                            float dpi) {
    const auto dip_rect = oz::viewer_core::view_geometry_to_dip(plan.geometry, dpi);
    if (dip_rect.width <= 0.0f || dip_rect.height <= 0.0f) {
        return;
    }

    // Retained bitmap physical dimensions
    const D2D1_SIZE_U bmp_size = bmp->GetPixelSize();
    const float dip_scale = (dpi > 0.0f) ? (96.0f / dpi) : 1.0f;

    // Logical dimensions before viewport scaling
    const float log_w_dip = (plan.logical_width > 0)
        ? static_cast<float>(plan.logical_width) * dip_scale
        : dip_rect.width;
    const float log_h_dip = (plan.logical_height > 0)
        ? static_cast<float>(plan.logical_height) * dip_scale
        : dip_rect.height;

    // Direct2D destination rectangle if drawn without rotation
    // Take into account canvas_offset in client space
    const float offset_x_dip = (plan.canvas_offset_x != 0) ? static_cast<float>(plan.canvas_offset_x) * dip_scale : 0.0f;
    const float offset_y_dip = (plan.canvas_offset_y != 0) ? static_cast<float>(plan.canvas_offset_y) * dip_scale : 0.0f;
    const float draw_x = dip_rect.x + offset_x_dip;
    const float draw_y = dip_rect.y + offset_y_dip;

    // Center of this rectangle in DIP coordinates:
    const float cx = draw_x + dip_rect.width * 0.5f;
    const float cy = draw_y + dip_rect.height * 0.5f;

    // Check if transform is identity
    const auto& t = plan.transform;
    const bool is_identity = (t.m00 == 1 && t.m01 == 0 && t.m10 == 0 && t.m11 == 1);

    D2D1_MATRIX_3X2_F old_transform{};
    ctx->GetTransform(&old_transform);

    if (is_identity) {
        const D2D1_RECT_F dest_rect = D2D1::RectF(draw_x, draw_y,
                                                  draw_x + dip_rect.width,
                                                  draw_y + dip_rect.height);
        if (plan.source_rectf.is_valid()) {
            const D2D1_RECT_F src_rect = D2D1::RectF(
                plan.source_rectf.x,
                plan.source_rectf.y,
                plan.source_rectf.right(),
                plan.source_rectf.bottom());
            ctx->DrawBitmap(bmp, dest_rect, 1.0f,
                            D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
                            &src_rect);
        } else if (plan.source_rect.is_valid()) {
            const D2D1_RECT_F src_rect = D2D1::RectF(
                static_cast<float>(plan.source_rect.x),
                static_cast<float>(plan.source_rect.y),
                static_cast<float>(plan.source_rect.right()),
                static_cast<float>(plan.source_rect.bottom()));
            ctx->DrawBitmap(bmp, dest_rect, 1.0f,
                            D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
                            &src_rect);
        } else {
            ctx->DrawBitmap(bmp, dest_rect, 1.0f,
                            D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        }
    } else {
        // Physical untransformed bitmap size in DIP:
        // Notice: if plan.transform swaps dimensions, bmp physical width/height are unswapped.
        // The destination bounds for the unrotated bitmap centered at (0, 0):
        const float unrot_w_dip = t.swaps_dimensions() ? log_h_dip : log_w_dip;
        const float unrot_h_dip = t.swaps_dimensions() ? log_w_dip : log_h_dip;

        const float half_w = (unrot_w_dip / log_w_dip) * (dip_rect.width * 0.5f);
        const float half_h = (unrot_h_dip / log_h_dip) * (dip_rect.height * 0.5f);

        // We construct a 2D affine matrix:
        // [ x_dest ]   [ m00  m01 ] [ x_unrot ]   [ cx ]
        // [ y_dest ] = [ m10  m11 ] [ y_unrot ] + [ cy ]
        // where x_unrot in [-half_w, half_w], y_unrot in [-half_h, half_h].
        // Notice: D2D matrix multiplication order: P * M.
        // Direct2D Matrix3x2F:
        // [ _11  _12 ]
        // [ _21  _22 ]
        // [ _31  _32 ]
        // so x' = x*_11 + y*_21 + _31
        //    y' = x*_12 + y*_22 + _32
        // Comparing with [x', y'] = [x, y] * [m00, m10; m01, m11]:
        // _11 = m00, _12 = m10
        // _21 = m01, _22 = m11
        // _31 = cx,  _32 = cy
        D2D1_MATRIX_3X2_F d2d_mat;
        d2d_mat._11 = static_cast<float>(t.m00);
        d2d_mat._12 = static_cast<float>(t.m10);
        d2d_mat._21 = static_cast<float>(t.m01);
        d2d_mat._22 = static_cast<float>(t.m11);
        d2d_mat._31 = cx;
        d2d_mat._32 = cy;

        ctx->SetTransform(d2d_mat * old_transform);

        const D2D1_RECT_F local_dest = D2D1::RectF(-half_w, -half_h, half_w, half_h);

        if (plan.source_rectf.is_valid()) {
            const D2D1_RECT_F src_rect = D2D1::RectF(
                plan.source_rectf.x,
                plan.source_rectf.y,
                plan.source_rectf.right(),
                plan.source_rectf.bottom());
            ctx->DrawBitmap(bmp, local_dest, 1.0f,
                            D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
                            &src_rect);
        } else if (plan.source_rect.is_valid()) {
            const D2D1_RECT_F src_rect = D2D1::RectF(
                static_cast<float>(plan.source_rect.x),
                static_cast<float>(plan.source_rect.y),
                static_cast<float>(plan.source_rect.right()),
                static_cast<float>(plan.source_rect.bottom()));
            ctx->DrawBitmap(bmp, local_dest, 1.0f,
                            D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
                            &src_rect);
        } else {
            ctx->DrawBitmap(bmp, local_dest, 1.0f,
                            D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        }

        ctx->SetTransform(old_transform);
    }
}

static void draw_crop_overlay(ID2D1DeviceContext* ctx, const crop_overlay& overlay,
                              std::int32_t canvas_offset_x = 0, std::int32_t canvas_offset_y = 0,
                              float dpi = 96.0f) {
    if (!overlay.visible || overlay.image_rect.width <= 0.0f ||
        overlay.image_rect.height <= 0.0f || overlay.selection_rect.width <= 0.0f ||
        overlay.selection_rect.height <= 0.0f) {
        return;
    }

    const float dip_scale = (dpi > 0.0f) ? (96.0f / dpi) : 1.0f;
    const float offset_x_dip = (canvas_offset_x != 0) ? static_cast<float>(canvas_offset_x) * dip_scale : 0.0f;
    const float offset_y_dip = (canvas_offset_y != 0) ? static_cast<float>(canvas_offset_y) * dip_scale : 0.0f;

    const float image_left = overlay.image_rect.x + offset_x_dip;
    const float image_top = overlay.image_rect.y + offset_y_dip;
    const float image_right = image_left + overlay.image_rect.width;
    const float image_bottom = image_top + overlay.image_rect.height;
    const float sel_left = (std::max)(image_left, overlay.selection_rect.x + offset_x_dip);
    const float sel_top = (std::max)(image_top, overlay.selection_rect.y + offset_y_dip);
    const float sel_right = (std::min)(image_right,
                                       overlay.selection_rect.x + offset_x_dip + overlay.selection_rect.width);
    const float sel_bottom = (std::min)(image_bottom,
                                        overlay.selection_rect.y + offset_y_dip + overlay.selection_rect.height);
    if (sel_right <= sel_left || sel_bottom <= sel_top) {
        return;
    }

    ComPtr<ID2D1SolidColorBrush> shade;
    ComPtr<ID2D1SolidColorBrush> line;
    if (FAILED(ctx->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.55f), &shade)) ||
        FAILED(ctx->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f), &line))) {
        return;
    }

    // Dim the portions of the visible image outside the half-open selection.
    ctx->FillRectangle(D2D1::RectF(image_left, image_top, image_right, sel_top), shade.Get());
    ctx->FillRectangle(D2D1::RectF(image_left, sel_bottom, image_right, image_bottom), shade.Get());
    ctx->FillRectangle(D2D1::RectF(image_left, sel_top, sel_left, sel_bottom), shade.Get());
    ctx->FillRectangle(D2D1::RectF(sel_right, sel_top, image_right, sel_bottom), shade.Get());

    const D2D1_RECT_F selection = D2D1::RectF(sel_left, sel_top, sel_right, sel_bottom);
    ctx->DrawRectangle(selection, line.Get(), 1.5f);

    // Rule-of-thirds guides give a clear crop affordance without retaining any
    // additional GPU resource.
    const float third_w = (sel_right - sel_left) / 3.0f;
    const float third_h = (sel_bottom - sel_top) / 3.0f;
    for (int i = 1; i < 3; ++i) {
        const float x = sel_left + third_w * static_cast<float>(i);
        const float y = sel_top + third_h * static_cast<float>(i);
        ctx->DrawLine(D2D1::Point2F(x, sel_top), D2D1::Point2F(x, sel_bottom), line.Get(), 0.75f);
        ctx->DrawLine(D2D1::Point2F(sel_left, y), D2D1::Point2F(sel_right, y), line.Get(), 0.75f);
    }

    const float radius = (std::max)(1.0f, overlay.handle_radius_dip);
    const float cx = (sel_left + sel_right) * 0.5f;
    const float cy = (sel_top + sel_bottom) * 0.5f;
    const D2D1_POINT_2F handles[] = {
        D2D1::Point2F(sel_left, sel_top), D2D1::Point2F(cx, sel_top),
        D2D1::Point2F(sel_right, sel_top), D2D1::Point2F(sel_right, cy),
        D2D1::Point2F(sel_right, sel_bottom), D2D1::Point2F(cx, sel_bottom),
        D2D1::Point2F(sel_left, sel_bottom), D2D1::Point2F(sel_left, cy)};
    for (const auto& point : handles) {
        ctx->FillRectangle(D2D1::RectF(point.x - radius, point.y - radius,
                                       point.x + radius, point.y + radius), line.Get());
    }
}

bool redraw_retained(renderer& r, const oz::viewer_core::view_geometry& geom) {
    if (!r.bitmap || !r.source_bitmap || !r.d2d_context) {
        return false;
    }
    const auto dip_rect = oz::viewer_core::view_geometry_to_dip(geom, r.dpi);
    const D2D1_RECT_F dest_rect = D2D1::RectF(dip_rect.x, dip_rect.y,
                                              dip_rect.x + dip_rect.width,
                                              dip_rect.y + dip_rect.height);

    r.d2d_context->BeginDraw();
    r.d2d_context->Clear(D2D1::ColorF(D2D1::ColorF::Black));
    if (dip_rect.width > 0.0f && dip_rect.height > 0.0f) {
        r.d2d_context->DrawBitmap(r.source_bitmap.Get(), dest_rect, 1.0f,
                                  D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
    }
    const HRESULT hr = r.d2d_context->EndDraw();
    if (FAILED(hr)) {
        return false;
    }
    return present(r);
}

bool redraw_retained_plan(renderer& r, const presentation_plan& plan,
                          const crop_overlay* overlay) {
    if (!r.bitmap || !r.source_bitmap || !r.d2d_context) {
        return false;
    }
    r.d2d_context->BeginDraw();
    r.d2d_context->Clear(D2D1::ColorF(D2D1::ColorF::Black));
    apply_presentation_plan_drawing(r.d2d_context.Get(), r.source_bitmap.Get(), plan, r.dpi);
    if (overlay != nullptr) {
        draw_crop_overlay(r.d2d_context.Get(), *overlay, plan.canvas_offset_x, plan.canvas_offset_y, r.dpi);
    }
    const HRESULT hr = r.d2d_context->EndDraw();
    if (FAILED(hr)) {
        return false;
    }
    return present(r);
}

bool render_and_present_candidate_plan(renderer& r, const std::uint8_t* pixels, std::int32_t stride,
                                      std::int32_t img_w, std::int32_t img_h,
                                      std::int32_t src_w, std::int32_t src_h,
                                      const presentation_plan& plan,
                                      std::uint64_t* out_present_ticks) {
    const auto& t = plan.transform;
    const bool is_identity = (t.m00 == 1 && t.m01 == 0 && t.m10 == 0 && t.m11 == 1);
    if (is_identity && !plan.source_rect.is_valid() && !plan.source_rectf.is_valid() &&
        plan.canvas_offset_x == 0 && plan.canvas_offset_y == 0) {
        return render_and_present_candidate(r, pixels, stride, img_w, img_h, src_w, src_h, plan.geometry, out_present_ticks);
    }

    ComPtr<ID2D1Bitmap1> candidate_bitmap;
    const D2D1_BITMAP_PROPERTIES1 props =
        D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(r.d2d_context->CreateBitmap(D2D1::SizeU(static_cast<UINT>(img_w),
                                                       static_cast<UINT>(img_h)),
                                           pixels, static_cast<UINT>(stride), &props, &candidate_bitmap))) {
        return false;
    }

    r.d2d_context->BeginDraw();
    r.d2d_context->Clear(D2D1::ColorF(D2D1::ColorF::Black));
    apply_presentation_plan_drawing(r.d2d_context.Get(), candidate_bitmap.Get(), plan, r.dpi);
    const HRESULT hr = r.d2d_context->EndDraw();
    if (FAILED(hr)) {
        return false;
    }

    if (!present(r)) {
        return false;
    }

    if (out_present_ticks != nullptr) {
        *out_present_ticks = oz::viewer_core::navigation_benchmark_session::query_ticks();
    }

    // Present succeeded: commit candidate bitmap as the ONE retained source bitmap.
    r.source_bitmap = std::move(candidate_bitmap);
    r.source_width = (src_w > 0) ? src_w : img_w;
    r.source_height = (src_h > 0) ? src_h : img_h;
    return true;
}

bool render_and_present_candidate(renderer& r, const std::uint8_t* pixels, std::int32_t stride,
                                 std::int32_t img_w, std::int32_t img_h,
                                 std::int32_t src_w, std::int32_t src_h,
                                 const oz::viewer_core::view_geometry& geom,
                                 std::uint64_t* out_present_ticks) {
    if (!r.bitmap || !pixels || img_w <= 0 || img_h <= 0 || !r.d2d_context) {
        return false;
    }

    // Upload into a candidate D2D bitmap.
    // The candidate bitmap does NOT replace r.source_bitmap until Present succeeds.
    ComPtr<ID2D1Bitmap1> candidate_bitmap;
    const D2D1_BITMAP_PROPERTIES1 props =
        D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(r.d2d_context->CreateBitmap(D2D1::SizeU(static_cast<UINT>(img_w),
                                                       static_cast<UINT>(img_h)),
                                           pixels, static_cast<UINT>(stride), &props, &candidate_bitmap))) {
        return false;
    }

    const auto dip_rect = oz::viewer_core::view_geometry_to_dip(geom, r.dpi);
    const D2D1_RECT_F dest_rect = D2D1::RectF(dip_rect.x, dip_rect.y,
                                              dip_rect.x + dip_rect.width,
                                              dip_rect.y + dip_rect.height);

    r.d2d_context->BeginDraw();
    r.d2d_context->Clear(D2D1::ColorF(D2D1::ColorF::Black));
    if (dip_rect.width > 0.0f && dip_rect.height > 0.0f) {
        r.d2d_context->DrawBitmap(candidate_bitmap.Get(), dest_rect, 1.0f,
                                  D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
    }
    const HRESULT hr = r.d2d_context->EndDraw();
    if (FAILED(hr)) {
        return false;
    }

    if (!present(r)) {
        return false;
    }

    if (out_present_ticks != nullptr) {
        *out_present_ticks = oz::viewer_core::navigation_benchmark_session::query_ticks();
    }

    // Present succeeded: commit candidate bitmap as the ONE retained source bitmap.
    r.source_bitmap = std::move(candidate_bitmap);
    r.source_width = (src_w > 0) ? src_w : img_w;
    r.source_height = (src_h > 0) ? src_h : img_h;
    return true;
}

bool render_image(renderer& r, const std::uint8_t* pixels, std::int32_t stride,
                  std::int32_t img_w, std::int32_t img_h) {
    if (!r.bitmap || !pixels || img_w <= 0 || img_h <= 0) {
        return false;
    }
    D2D1_SIZE_U size = r.bitmap->GetPixelSize();
    if (size.width == 0 || size.height == 0) {
        return false;
    }
    // Physical-pixel no-upscale fit, expressed in the device-context DIP space
    // for the current target DPI (matches the SetDpi applied in resize_renderer).
    const auto dest = oz::viewer_core::fit_centered_dip(
        img_w, img_h, static_cast<std::int32_t>(size.width),
        static_cast<std::int32_t>(size.height), r.dpi);
    if (dest.width <= 0.0f || dest.height <= 0.0f) {
        return false;
    }

    // Upload the decoded premultiplied-BGRA pixels into a staging bitmap,
    // then draw it.
    ComPtr<ID2D1Bitmap1> staging;
    const D2D1_BITMAP_PROPERTIES1 props =
        D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(r.d2d_context->CreateBitmap(D2D1::SizeU(static_cast<UINT>(img_w),
                                                       static_cast<UINT>(img_h)),
                                           pixels, static_cast<UINT>(stride), &props, &staging))) {
        return false;
    }

    r.d2d_context->BeginDraw();
    r.d2d_context->Clear(D2D1::ColorF(D2D1::ColorF::Black));
    const D2D1_RECT_F dest_rect =
        D2D1::RectF(dest.x, dest.y, dest.x + dest.width, dest.y + dest.height);
    r.d2d_context->DrawBitmap(staging.Get(), dest_rect, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
    const HRESULT hr = r.d2d_context->EndDraw();
    if (SUCCEEDED(hr)) {
        // Also retain as source_bitmap
        r.source_bitmap = std::move(staging);
        r.source_width = img_w;
        r.source_height = img_h;
        return true;
    }
    return false;
}

bool present(renderer& r) {
    if (!r.swap_chain) {
        return false;
    }
    const HRESULT hr = r.swap_chain->Present(0, 0);
    return SUCCEEDED(hr);
}

}  // namespace oz::viewer

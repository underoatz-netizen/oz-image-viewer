// oz/viewer/svg_icon_cache.cpp - SVG icon resources and lazy cache implementation.
#include "svg_icon_cache.hpp"

#include <shlwapi.h> // SHCreateMemStream
#include <d2d1_3.h> // ID2D1DeviceContext5, ID2D1SvgDocument

namespace oz::viewer {

std::string_view get_svg_icon_data(icon_id id) noexcept {
    switch (id) {
        case icon_id::open:
            return R"(<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z"/></svg>)";
        case icon_id::save:
            return R"(<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M19 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h11l5 5v11a2 2 0 0 1-2 2z"/><polyline points="17 21 17 13 7 13 7 21"/><polyline points="7 3 7 8 15 8"/></svg>)";
        case icon_id::undo:
            return R"(<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polyline points="1 4 1 10 7 10"/><path d="M3.51 15a9 9 0 1 0 2.13-9.36L1 10"/></svg>)";
        case icon_id::redo:
            return R"(<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polyline points="23 4 23 10 17 10"/><path d="M20.49 15a9 9 0 1 1-2.12-9.36L23 10"/></svg>)";
        case icon_id::rotate_left:
            return R"(<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polyline points="1 4 1 10 7 10"/><path d="M3.51 15a9 9 0 1 0 2.13-9.36L1 10"/></svg>)";
        case icon_id::rotate_right:
            return R"(<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polyline points="23 4 23 10 17 10"/><path d="M20.49 15a9 9 0 1 1-2.12-9.36L23 10"/></svg>)";
        case icon_id::crop:
            return R"(<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M6 2v14a2 2 0 0 0 2 2h14"/><path d="M18 22V8a2 2 0 0 0-2-2H2"/></svg>)";
        case icon_id::resize:
            return R"(<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polyline points="15 3 21 3 21 9"/><polyline points="9 21 3 21 3 15"/><line x1="21" y1="3" x2="14" y2="10"/><line x1="3" y1="21" x2="10" y2="14"/></svg>)";
        case icon_id::fit:
            return R"(<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="3" width="18" height="18" rx="2" ry="2"/><circle cx="8.5" cy="8.5" r="1.5"/><polyline points="21 15 16 10 5 21"/></svg>)";
        case icon_id::one_to_one:
            return R"(<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="11" cy="11" r="8"/><line x1="21" y1="21" x2="16.65" y2="16.65"/><line x1="11" y1="8" x2="11" y2="14"/><line x1="8" y1="11" x2="14" y2="11"/></svg>)";
        case icon_id::fullscreen:
            return R"(<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M8 3H5a2 2 0 0 0-2 2v3m18 0V5a2 2 0 0 0-2-2h-3m0 18h3a2 2 0 0 0 2-2v-3M3 16v3a2 2 0 0 0 2 2h3"/></svg>)";
        default:
            return {};
    }
}

std::wstring_view get_icon_fallback_label(icon_id id) noexcept {
    switch (id) {
        case icon_id::open: return L"Open";
        case icon_id::save: return L"Save";
        case icon_id::undo: return L"Undo";
        case icon_id::redo: return L"Redo";
        case icon_id::rotate_left: return L"RotL";
        case icon_id::rotate_right: return L"RotR";
        case icon_id::crop: return L"Crop";
        case icon_id::resize: return L"Resize";
        case icon_id::fit: return L"Fit";
        case icon_id::one_to_one: return L"1:1";
        case icon_id::fullscreen: return L"Full";
        default: return L"";
    }
}

bool svg_icon_cache::is_svg_supported(ID2D1DeviceContext* d2d_context) noexcept {
    if (!d2d_context) {
        return false;
    }
    if (!checked_svg_support_) {
        checked_svg_support_ = true;
        has_svg_support_ = false;
        ComPtr<ID2D1DeviceContext5> d2d5;
        if (SUCCEEDED(d2d_context->QueryInterface(IID_PPV_ARGS(&d2d5)))) {
            has_svg_support_ = true;
        }
    }
    return has_svg_support_;
}

ID2D1Bitmap* svg_icon_cache::get_or_render_icon(ID2D1DeviceContext* d2d_context,
                                                icon_id id,
                                                std::int32_t size_px,
                                                COLORREF color) {
    if (!d2d_context || size_px <= 0) {
        return nullptr;
    }

    const icon_cache_key key{id, size_px, color};
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        return it->second.Get();
    }

    if (!is_svg_supported(d2d_context)) {
        return nullptr;
    }

    ComPtr<ID2D1DeviceContext5> d2d5;
    if (FAILED(d2d_context->QueryInterface(IID_PPV_ARGS(&d2d5)))) {
        return nullptr;
    }

    const std::string_view svg_data = get_svg_icon_data(id);
    if (svg_data.empty()) {
        return nullptr;
    }

    // Wrap in stream
    IStream* stream = SHCreateMemStream(
        reinterpret_cast<const BYTE*>(svg_data.data()),
        static_cast<UINT>(svg_data.size()));
    if (!stream) {
        return nullptr;
    }

    ComPtr<ID2D1SvgDocument> svg_doc;
    const HRESULT hr = d2d5->CreateSvgDocument(
        stream,
        D2D1::SizeF(static_cast<float>(size_px), static_cast<float>(size_px)),
        &svg_doc);
    stream->Release();

    if (FAILED(hr) || !svg_doc) {
        return nullptr;
    }

    // Set SVG root stroke color to the requested color
    ComPtr<ID2D1SvgElement> root;
    svg_doc->GetRoot(&root);
    if (root) {
        const D2D1_COLOR_F d2d_color = D2D1::ColorF(
            GetRValue(color) / 255.0f,
            GetGValue(color) / 255.0f,
            GetBValue(color) / 255.0f,
            1.0f);
        root->SetAttributeValue(L"stroke", D2D1_SVG_ATTRIBUTE_STRING_TYPE_SVG,
                                (color == RGB(235, 235, 235)) ? L"#EBEBEB" :
                                (color == RGB(32, 32, 32)) ? L"#202020" :
                                (color == RGB(160, 160, 160)) ? L"#A0A0A0" : L"#707070");
    }

    // Create intermediate bitmap target to render the SVG document
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

    ComPtr<ID2D1Bitmap1> target_bmp;
    if (FAILED(d2d_context->CreateBitmap(
            D2D1::SizeU(static_cast<UINT>(size_px), static_cast<UINT>(size_px)),
            nullptr, 0, &props, &target_bmp))) {
        return nullptr;
    }

    // Save previous target and render SVG
    ComPtr<ID2D1Image> old_target;
    d2d_context->GetTarget(&old_target);
    d2d_context->SetTarget(target_bmp.Get());

    d2d_context->BeginDraw();
    d2d_context->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f)); // Transparent
    d2d5->DrawSvgDocument(svg_doc.Get());
    d2d_context->EndDraw();

    d2d_context->SetTarget(old_target.Get());

    cache_[key] = target_bmp;
    return target_bmp.Get();
}

bool svg_icon_cache::rasterize_icon_pixels(ID2D1DeviceContext* d2d_context,
                                          icon_id id,
                                          std::int32_t size_px,
                                          COLORREF color,
                                          void* out_pixels_bgra32) {
    if (!d2d_context || size_px <= 0 || !out_pixels_bgra32) {
        return false;
    }

    if (!is_svg_supported(d2d_context)) {
        return false;
    }

    ComPtr<ID2D1DeviceContext5> d2d5;
    if (FAILED(d2d_context->QueryInterface(IID_PPV_ARGS(&d2d5)))) {
        return false;
    }

    const std::string_view svg_data = get_svg_icon_data(id);
    if (svg_data.empty()) {
        return false;
    }

    IStream* stream = SHCreateMemStream(
        reinterpret_cast<const BYTE*>(svg_data.data()),
        static_cast<UINT>(svg_data.size()));
    if (!stream) {
        return false;
    }

    ComPtr<ID2D1SvgDocument> svg_doc;
    const HRESULT hr = d2d5->CreateSvgDocument(
        stream,
        D2D1::SizeF(static_cast<float>(size_px), static_cast<float>(size_px)),
        &svg_doc);
    stream->Release();

    if (FAILED(hr) || !svg_doc) {
        return false;
    }

    ComPtr<ID2D1SvgElement> root;
    svg_doc->GetRoot(&root);
    if (root) {
        root->SetAttributeValue(L"stroke", D2D1_SVG_ATTRIBUTE_STRING_TYPE_SVG,
                                (color == RGB(235, 235, 235)) ? L"#EBEBEB" :
                                (color == RGB(32, 32, 32)) ? L"#202020" :
                                (color == RGB(160, 160, 160)) ? L"#A0A0A0" : L"#707070");
    }

    // 1. Render SVG into a TARGET-capable drawing bitmap
    D2D1_BITMAP_PROPERTIES1 render_props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

    ComPtr<ID2D1Bitmap1> render_bmp;
    if (FAILED(d2d_context->CreateBitmap(
            D2D1::SizeU(static_cast<UINT>(size_px), static_cast<UINT>(size_px)),
            nullptr, 0, &render_props, &render_bmp))) {
        return false;
    }

    ComPtr<ID2D1Image> old_target;
    d2d_context->GetTarget(&old_target);
    d2d_context->SetTarget(render_bmp.Get());

    d2d_context->BeginDraw();
    d2d_context->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    d2d5->DrawSvgDocument(svg_doc.Get());
    if (FAILED(d2d_context->EndDraw())) {
        d2d_context->SetTarget(old_target.Get());
        return false;
    }

    d2d_context->SetTarget(old_target.Get());

    // 2. Copy into a separate staging bitmap configured with CPU_READ | CANNOT_DRAW
    D2D1_BITMAP_PROPERTIES1 staging_props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_CANNOT_DRAW | D2D1_BITMAP_OPTIONS_CPU_READ,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

    ComPtr<ID2D1Bitmap1> staging_bmp;
    if (FAILED(d2d_context->CreateBitmap(
            D2D1::SizeU(static_cast<UINT>(size_px), static_cast<UINT>(size_px)),
            nullptr, 0, &staging_props, &staging_bmp))) {
        return false;
    }

    D2D1_POINT_2U dst_pt{0, 0};
    D2D1_RECT_U src_rect{0, 0, static_cast<UINT32>(size_px), static_cast<UINT32>(size_px)};
    if (FAILED(staging_bmp->CopyFromBitmap(&dst_pt, render_bmp.Get(), &src_rect))) {
        return false;
    }

    D2D1_MAPPED_RECT mapped{};
    if (FAILED(staging_bmp->Map(D2D1_MAP_OPTIONS_READ, &mapped))) {
        return false;
    }

    const UINT bytes_per_row = static_cast<UINT>(size_px * 4);
    for (std::int32_t y = 0; y < size_px; ++y) {
        const BYTE* src_row = mapped.bits + static_cast<std::size_t>(y) * mapped.pitch;
        BYTE* dst_row = reinterpret_cast<BYTE*>(out_pixels_bgra32) + static_cast<std::size_t>(y) * bytes_per_row;
        std::memcpy(dst_row, src_row, bytes_per_row);
    }

    staging_bmp->Unmap();
    return true;
}

}  // namespace oz::viewer

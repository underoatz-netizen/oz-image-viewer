// oz/viewer/svg_icon_cache.cpp - SVG icon resources and lazy cache implementation.
#include "svg_icon_cache.hpp"

#include <shlwapi.h> // SHCreateMemStream
#include <d2d1_3.h> // ID2D1DeviceContext5, ID2D1SvgDocument
#include <d2d1helper.h>

#include <cstdio>
#include <cstring>

namespace oz::viewer {

namespace {

void format_svg_hex_color(COLORREF color, wchar_t (&out)[8]) noexcept {
    std::swprintf(out, 8, L"#%02X%02X%02X",
                  static_cast<unsigned>(GetRValue(color)),
                  static_cast<unsigned>(GetGValue(color)),
                  static_cast<unsigned>(GetBValue(color)));
}

ComPtr<ID2D1Bitmap1> render_svg_bitmap(ID2D1DeviceContext* d2d_context,
                                       icon_id id,
                                       std::int32_t size_px,
                                       COLORREF color) {
    if (!d2d_context || size_px <= 0) {
        return nullptr;
    }

    ComPtr<ID2D1DeviceContext5> d2d5;
    if (FAILED(d2d_context->QueryInterface(IID_PPV_ARGS(&d2d5)))) {
        return nullptr;
    }

    const std::string svg_data = get_svg_icon_data(id);
    if (svg_data.empty()) {
        return nullptr;
    }

    IStream* stream = SHCreateMemStream(
        reinterpret_cast<const BYTE*>(svg_data.data()),
        static_cast<UINT>(svg_data.size()));
    if (!stream) {
        return nullptr;
    }

    const float grid = static_cast<float>(lucide::kGridUnits);
    ComPtr<ID2D1SvgDocument> svg_doc;
    const HRESULT hr = d2d5->CreateSvgDocument(
        stream,
        D2D1::SizeF(grid, grid),
        &svg_doc);
    stream->Release();

    if (FAILED(hr) || !svg_doc) {
        return nullptr;
    }

    ComPtr<ID2D1SvgElement> root;
    svg_doc->GetRoot(&root);
    if (root) {
        wchar_t hex[8]{};
        format_svg_hex_color(color, hex);
        root->SetAttributeValue(L"stroke", D2D1_SVG_ATTRIBUTE_STRING_TYPE_SVG, hex);
    }

    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

    ComPtr<ID2D1Bitmap1> target_bmp;
    if (FAILED(d2d_context->CreateBitmap(
            D2D1::SizeU(static_cast<UINT>(size_px), static_cast<UINT>(size_px)),
            nullptr, 0, &props, &target_bmp))) {
        return nullptr;
    }

    ComPtr<ID2D1Image> old_target;
    d2d_context->GetTarget(&old_target);
    float old_dpi_x = 96.0f;
    float old_dpi_y = 96.0f;
    d2d_context->GetDpi(&old_dpi_x, &old_dpi_y);
    D2D1_MATRIX_3X2_F old_transform{};
    d2d_context->GetTransform(&old_transform);

    const float scale = static_cast<float>(size_px) / grid;
    d2d_context->SetTarget(target_bmp.Get());
    d2d_context->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));

    d2d_context->BeginDraw();
    d2d_context->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    d2d5->DrawSvgDocument(svg_doc.Get());
    const HRESULT draw_hr = d2d_context->EndDraw();

    d2d_context->SetTransform(old_transform);
    d2d_context->SetTarget(old_target.Get());
    d2d_context->SetDpi(old_dpi_x, old_dpi_y);

    if (FAILED(draw_hr)) {
        return nullptr;
    }
    return target_bmp;
}

}  // namespace

std::string get_svg_icon_data(icon_id id) noexcept {
    const lucide::icon_asset* asset = lucide::FindAsset(id);
    if (!asset || !asset->markup || asset->markup[0] == '\0') {
        return {};
    }
    try {
        std::string svg =
            "<svg width=\"24\" height=\"24\" viewBox=\"0 0 24 24\" fill=\"none\" "
            "stroke=\"currentColor\" stroke-width=\"2\" stroke-linecap=\"round\" "
            "stroke-linejoin=\"round\">";
        svg += asset->markup;
        svg += "</svg>";
        return svg;
    } catch (...) {
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
        case icon_id::paste_image: return L"Paste";
        case icon_id::save_as: return L"SaveAs";
        case icon_id::open_location: return L"OpenLoc";
        case icon_id::copy_image: return L"CopyImg";
        case icon_id::copy_file: return L"CopyFile";
        case icon_id::copy_path: return L"CopyPath";
        case icon_id::print: return L"Print";
        case icon_id::delete_image: return L"Delete";
        case icon_id::flip_h: return L"FlipH";
        case icon_id::flip_v: return L"FlipV";
        case icon_id::zoom_in: return L"ZoomIn";
        case icon_id::zoom_out: return L"ZoomOut";
        case icon_id::nav_first: return L"First";
        case icon_id::nav_prev: return L"Prev";
        case icon_id::nav_next: return L"Next";
        case icon_id::nav_last: return L"Last";
        case icon_id::overflow: return L"More";
        case icon_id::crop_free: return L"Free";
        case icon_id::crop_original: return L"Orig";
        case icon_id::crop_1x1: return L"1:1";
        case icon_id::crop_3x2: return L"3:2";
        case icon_id::crop_4x3: return L"4:3";
        case icon_id::crop_5x4: return L"5:4";
        case icon_id::crop_16x9: return L"16:9";
        case icon_id::crop_9x16: return L"9:16";
        case icon_id::crop_a_series: return L"A-ser";
        case icon_id::crop_reset: return L"Reset";
        case icon_id::crop_apply: return L"Apply";
        case icon_id::crop_cancel: return L"Cancel";
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

    ComPtr<ID2D1Bitmap1> target_bmp = render_svg_bitmap(d2d_context, id, size_px, color);
    if (!target_bmp) {
        return nullptr;
    }

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

    ComPtr<ID2D1Bitmap1> render_bmp = render_svg_bitmap(d2d_context, id, size_px, color);
    if (!render_bmp) {
        return false;
    }

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

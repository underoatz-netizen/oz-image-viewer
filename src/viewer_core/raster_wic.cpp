// raster_wic.cpp - Common Raster Decode Profile v1 Phase A: in-box WIC
// adapters for BMP, static GIF, classic TIFF (page/frame 0) and ICO
// (deterministic entry selection). See raster_wic.hpp for the contract.
//
// Security model (all formats):
// 1. A bounded, endian-aware, decoder-independent structural pre-parse runs
//    on the raw bytes and rejects malformed/truncated/oversized containers,
//    bad offsets/lengths, IFD loops, animated GIF, RAW/DNG/CFA TIFFs and
//    unsafe ICO entries BEFORE any COM/WIC object is created.
// 2. Only after the pre-parse succeeds is COM initialized lazily on the
//    calling worker thread, and exactly one explicit WIC container decoder
//    GUID is used (BMP/GIF/TIFF/ICO). There is no WIC decoder enumeration,
//    no auto-detection, no generic fallback, no extension-based selection.
// 3. One explicit frame is requested (0 for BMP/GIF/TIFF; ICO decodes the
//    sliced payload of the deterministically selected directory entry with
//    the matching explicit PNG/BMP container decoder) and pixels are converted to the common
//    canonical contract: top-down SDR sRGB premultiplied BGRA8, checked
//    stride/size math, <= 2 GiB output, <= 65535 per dimension, 1/2/4/8 ceil
//    scaling. All failures map to stable generic codec_error values.
//
// COM/WIC usage mirrors png_wic.cpp: CoInitializeEx(COINIT_MULTITHREADED) is
// attempted lazily per decode and balanced on the same thread when this call
// initialized COM; RPC_E_CHANGED_MODE is tolerated as a bounded path. No
// WIC/COM handle ever reaches the UI thread and no global mutable state is
// used.
#include "raster_wic.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstring>
#include <vector>

#include "png_wic.hpp"

namespace oz::viewer_core {
namespace {

using Microsoft::WRL::ComPtr;

// --- endian helpers ---------------------------------------------------------

std::uint16_t rd_le16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
}

std::uint32_t rd_le32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

std::int32_t rd_le_i32(const std::uint8_t* p) noexcept {
    return static_cast<std::int32_t>(rd_le32(p));
}

std::uint16_t rd_be16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) | p[1]);
}

std::uint32_t rd_be32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

// --- COM/WIC bootstrap (worker-thread only, lazy, balanced) -----------------

struct com_mta_guard {
    HRESULT hr = S_OK;
    bool owns = false;
    com_mta_guard() {
        hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        owns = (hr == S_OK);
    }
    ~com_mta_guard() {
        if (owns) {
            CoUninitialize();
        }
    }
    bool usable() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
};

struct wic_context {
    com_mta_guard com;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;

    // Lazily initializes COM MTA + imaging factory + memory stream. Only
    // called after the byte-level structural pre-parse succeeded.
    codec_error open(const std::uint8_t* data, std::size_t size) noexcept {
        if (data == nullptr || size == 0) {
            return codec_error::invalid_header;
        }
        if (!com.usable()) {
            return codec_error::decode_failed;
        }
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                      CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        if (FAILED(hr) || !factory) {
            return codec_error::decode_failed;
        }
        hr = factory->CreateStream(&stream);
        if (FAILED(hr) || !stream) {
            return codec_error::decode_failed;
        }
        hr = stream->InitializeFromMemory(const_cast<BYTE*>(data),
                                          static_cast<DWORD>(size));
        if (FAILED(hr)) {
            return codec_error::decode_failed;
        }
        return codec_error::none;
    }

    // Creates exactly the requested explicit container decoder and
    // initializes it over the memory stream. No decoder enumeration.
    codec_error create_decoder(const GUID& container,
                               ComPtr<IWICBitmapDecoder>& decoder) noexcept {
        HRESULT hr = factory->CreateDecoder(container, nullptr, &decoder);
        if (FAILED(hr) || !decoder) {
            return codec_error::decode_failed;
        }
        hr = decoder->Initialize(stream.Get(), WICDecodeMetadataCacheOnDemand);
        if (FAILED(hr)) {
            return codec_error::decode_failed;
        }
        return codec_error::none;
    }
};

// EXIF orientation transform on PBGRA rows: maps destination (dx,dy) in
// oriented space back to source (sx,sy) in stored space. Same canonical
// mapping as the PNG/WebP paths.
void apply_orientation(const std::uint8_t* src, std::int32_t sw, std::int32_t sh,
                       std::uint8_t* dst, int orientation) noexcept {
    const std::size_t sstride = static_cast<std::size_t>(sw) * 4u;
    const bool swap = orientation >= 5 && orientation <= 8;
    const std::int32_t dw = swap ? sh : sw;
    const std::int32_t dh = swap ? sw : sh;
    const std::size_t dstride = static_cast<std::size_t>(dw) * 4u;
    for (std::int32_t dy = 0; dy < dh; ++dy) {
        for (std::int32_t dx = 0; dx < dw; ++dx) {
            std::int32_t sx = dx;
            std::int32_t sy = dy;
            switch (orientation) {
                case 2: sx = sw - 1 - dx; sy = dy; break;
                case 3: sx = sw - 1 - dx; sy = sh - 1 - dy; break;
                case 4: sx = dx; sy = sh - 1 - dy; break;
                case 5: sx = dy; sy = dx; break;
                case 6: sx = sy; sy = sh - 1 - dx; break;
                case 7: sx = sw - 1 - dy; sy = sh - 1 - dx; break;
                case 8: sx = sw - 1 - dy; sy = dx; break;
                default: break;
            }
            const std::uint8_t* sp = src + static_cast<std::size_t>(sy) * sstride +
                                     static_cast<std::size_t>(sx) * 4u;
            std::uint8_t* dp = dst + static_cast<std::size_t>(dy) * dstride +
                               static_cast<std::size_t>(dx) * 4u;
            dp[0] = sp[0];
            dp[1] = sp[1];
            dp[2] = sp[2];
            dp[3] = sp[3];
        }
    }
}

int normalize_denominator(int den) noexcept {
    return (den == 1 || den == 2 || den == 4 || den == 8) ? den : 1;
}

// Builds one IWICBitmapSource in the canonical pixel format: embedded ICC
// converted to sRGB when WIC accepts it, otherwise a plain pixel-format
// conversion (explicit sRGB / unprofiled / invalid profile all fall back
// safely). Mirrors the PNG/WebP color contract.
codec_error make_color_source(IWICImagingFactory* factory,
                              IWICBitmapFrameDecode* frame,
                              ComPtr<IWICBitmapSource>& color_source) noexcept {
    HRESULT hr = S_OK;
    ComPtr<IWICColorTransform> transformer;
    bool transformed = false;
    UINT ctx_count = 0;
    if (SUCCEEDED(frame->GetColorContexts(0, nullptr, &ctx_count)) && ctx_count > 0) {
        std::vector<IWICColorContext*> raw(ctx_count, nullptr);
        UINT actual = 0;
        if (SUCCEEDED(frame->GetColorContexts(ctx_count, raw.data(), &actual)) &&
            actual > 0 && raw[0] != nullptr) {
            ComPtr<IWICColorContext> src_ctx;
            src_ctx.Attach(raw[0]);
            for (UINT i = 1; i < actual; ++i) {
                if (raw[i] != nullptr) {
                    raw[i]->Release();
                }
            }
            ComPtr<IWICColorContext> dst_ctx;
            if (SUCCEEDED(factory->CreateColorContext(&dst_ctx)) && dst_ctx &&
                SUCCEEDED(dst_ctx->InitializeFromExifColorSpace(1)) &&
                SUCCEEDED(factory->CreateColorTransformer(&transformer)) && transformer &&
                SUCCEEDED(transformer->Initialize(frame, src_ctx.Get(), dst_ctx.Get(),
                                                  GUID_WICPixelFormat32bppPBGRA)) &&
                SUCCEEDED(transformer.As(&color_source))) {
                transformed = true;
            }
        } else {
            for (UINT i = 0; i < ctx_count; ++i) {
                if (raw[i] != nullptr) {
                    raw[i]->Release();
                }
            }
        }
    }
    if (transformed) {
        return codec_error::none;
    }
    ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr) || !converter) {
        return codec_error::decode_failed;
    }
    hr = converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA,
                               WICBitmapDitherTypeNone, nullptr, 0.0,
                               WICBitmapPaletteTypeCustom);
    if (FAILED(hr) || FAILED(converter.As(&color_source))) {
        return codec_error::decode_failed;
    }
    return codec_error::none;
}

// Shared canonical output stage: explicit frame -> checked scaled/oriented
// top-down PBGRA8. All limits are applied before any output allocation.
decode_result canonical_frame_decode(IWICImagingFactory* factory,
                                     IWICBitmapFrameDecode* frame,
                                     int scaling_denominator, int orientation,
                                     codec_id id) noexcept {
    decode_result out;
    if (factory == nullptr || frame == nullptr) {
        out.error = codec_error::decode_failed;
        return out;
    }
    const int den = normalize_denominator(scaling_denominator);

    UINT fw = 0;
    UINT fh = 0;
    HRESULT hr = frame->GetSize(&fw, &fh);
    if (FAILED(hr) || fw == 0 || fh == 0) {
        out.error = codec_error::decode_failed;
        return out;
    }
    if (fw > static_cast<UINT>(kMaxImageDimension) ||
        fh > static_cast<UINT>(kMaxImageDimension)) {
        out.error = codec_error::dimension_limit;
        return out;
    }
    if (!decoded_size_within_limits(static_cast<std::int32_t>(fw),
                                    static_cast<std::int32_t>(fh))) {
        out.error = codec_error::dimension_limit;
        return out;
    }

    if (orientation < 1 || orientation > 8) {
        orientation = 1;
    }
    const bool swap = orientation >= 5 && orientation <= 8;
    const std::uint64_t ow64 = swap ? fh : fw;
    const std::uint64_t oh64 = swap ? fw : fh;
    if (ow64 > static_cast<std::uint64_t>(kMaxImageDimension) ||
        oh64 > static_cast<std::uint64_t>(kMaxImageDimension)) {
        out.error = codec_error::dimension_limit;
        return out;
    }
    const std::int32_t ow = static_cast<std::int32_t>(ow64);
    const std::int32_t oh = static_cast<std::int32_t>(oh64);
    if (!decoded_size_within_limits(ow, oh)) {
        out.error = codec_error::dimension_limit;
        return out;
    }

    // Scaled stored-space dimensions (ceil semantics, as JPEG/PNG/WebP).
    const std::uint64_t suw64 =
        (static_cast<std::uint64_t>(fw) + static_cast<std::uint64_t>(den) - 1) /
        static_cast<std::uint64_t>(den);
    const std::uint64_t suh64 =
        (static_cast<std::uint64_t>(fh) + static_cast<std::uint64_t>(den) - 1) /
        static_cast<std::uint64_t>(den);
    if (suw64 == 0 || suh64 == 0 ||
        suw64 > static_cast<std::uint64_t>(kMaxImageDimension) ||
        suh64 > static_cast<std::uint64_t>(kMaxImageDimension)) {
        out.error = codec_error::allocation_limit;
        return out;
    }
    const std::int32_t suw = static_cast<std::int32_t>(suw64);
    const std::int32_t suh = static_cast<std::int32_t>(suh64);
    const std::int32_t final_w = swap ? suh : suw;
    const std::int32_t final_h = swap ? suw : suh;
    if (!decoded_size_within_limits(final_w, final_h)) {
        out.error = codec_error::allocation_limit;
        return out;
    }

    ComPtr<IWICBitmapSource> color_source;
    const codec_error color_err = make_color_source(factory, frame, color_source);
    if (color_err != codec_error::none) {
        out.error = color_err;
        return out;
    }

    // Scale only when the denominator actually reduces the stored size.
    ComPtr<IWICBitmapSource> pixel_source = color_source;
    ComPtr<IWICBitmapScaler> scaler;
    if (den > 1) {
        hr = factory->CreateBitmapScaler(&scaler);
        if (FAILED(hr) || !scaler) {
            out.error = codec_error::decode_failed;
            return out;
        }
        hr = scaler->Initialize(color_source.Get(), static_cast<UINT>(suw),
                                static_cast<UINT>(suh), WICBitmapInterpolationModeFant);
        if (FAILED(hr)) {
            out.error = codec_error::decode_failed;
            return out;
        }
        pixel_source = scaler;
    }

    std::int32_t stage_stride = 0;
    std::size_t stage_bytes = 0;
    if (!checked_stride_and_size(suw, suh, stage_stride, stage_bytes)) {
        out.error = codec_error::allocation_limit;
        return out;
    }
    if (stage_bytes > static_cast<std::size_t>(UINT32_MAX)) {
        out.error = codec_error::allocation_limit;
        return out;
    }
    std::vector<std::uint8_t> staged(stage_bytes);
    hr = pixel_source->CopyPixels(nullptr, static_cast<UINT>(stage_stride),
                                  static_cast<UINT>(stage_bytes), staged.data());
    if (FAILED(hr)) {
        out.error = codec_error::decode_failed;
        return out;
    }

    decoded_image img;
    img.codec = id;
    img.scale_den = den;
    img.src_width = ow;
    img.src_height = oh;
    img.width = final_w;
    img.height = final_h;
    std::size_t final_bytes = 0;
    if (!checked_stride_and_size(final_w, final_h, img.stride, final_bytes)) {
        out.error = codec_error::allocation_limit;
        return out;
    }
    img.pixels.resize(final_bytes);
    if (orientation == 1) {
        std::memcpy(img.pixels.data(), staged.data(), final_bytes);
    } else {
        apply_orientation(staged.data(), suw, suh, img.pixels.data(), orientation);
    }
    out.image = std::move(img);
    out.error = codec_error::none;
    return out;
}

// --- BMP --------------------------------------------------------------------

bool is_known_dib_size(std::uint32_t dib) noexcept {
    switch (dib) {
        case 12:   // BITMAPCOREHEADER
        case 40:   // BITMAPINFOHEADER
        case 52:   // BITMAPV2INFOHEADER
        case 56:   // BITMAPV3INFOHEADER
        case 64:   // OS/2 2.x BITMAPINFOHEADER2
        case 108:  // BITMAPV4HEADER
        case 124:  // BITMAPV5HEADER
            return true;
        default:
            return false;
    }
}

struct bmp_header_info {
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::uint16_t bit_count = 0;
    std::uint32_t compression = 0;
};

codec_error parse_bmp_header(const std::uint8_t* data, std::size_t size,
                             bmp_header_info& out) noexcept {
    out = bmp_header_info{};
    if (data == nullptr) {
        return codec_error::unsupported;
    }
    if (size < 18) {
        return codec_error::truncated;
    }
    if (data[0] != 'B' || data[1] != 'M') {
        return codec_error::unsupported;
    }
    const std::uint32_t file_offset = rd_le32(data + 10);
    const std::uint32_t dib = rd_le32(data + 14);
    if (!is_known_dib_size(dib)) {
        return codec_error::invalid_header;
    }
    const std::uint64_t dib_end = 14ull + dib;
    if (dib_end > size) {
        return codec_error::truncated;
    }
    if (file_offset > size || file_offset < dib_end) {
        return codec_error::invalid_header;
    }

    std::uint32_t width = 0;
    std::uint32_t height_abs = 0;
    std::uint16_t planes = 0;
    std::uint16_t bpp = 0;
    std::uint32_t compression = 0;
    if (dib == 12) {
        width = rd_le16(data + 18);
        height_abs = rd_le16(data + 20);
        planes = rd_le16(data + 22);
        bpp = rd_le16(data + 24);
        compression = 0;
    } else {
        const std::int32_t w32 = rd_le_i32(data + 18);
        const std::int32_t h32 = rd_le_i32(data + 22);
        if (w32 <= 0 || h32 == 0 || h32 == INT32_MIN) {
            return codec_error::invalid_header;
        }
        width = static_cast<std::uint32_t>(w32);
        height_abs = h32 < 0 ? static_cast<std::uint32_t>(-h32)
                             : static_cast<std::uint32_t>(h32);
        planes = rd_le16(data + 26);
        bpp = rd_le16(data + 28);
        compression = rd_le32(data + 30);
        // Negative height (top-down) is valid for uncompressed layouts only.
        if (h32 < 0 && (compression == 1 || compression == 2)) {
            return codec_error::invalid_header;
        }
        switch (compression) {
            case 0:   // BI_RGB
            case 3:   // BI_BITFIELDS
            case 6:   // BI_ALPHABITFIELDS
                break;
            case 1:   // BI_RLE8 (8bpp)
                if (bpp != 8) return codec_error::invalid_header;
                break;
            case 2:   // BI_RLE4 (4bpp)
                if (bpp != 4) return codec_error::invalid_header;
                break;
            default:
                return codec_error::invalid_header;  // embedded JPEG/PNG etc.
        }
    }
    if (width == 0 || height_abs == 0 || planes != 1) {
        return codec_error::invalid_header;
    }
    if (width > static_cast<std::uint32_t>(kMaxImageDimension) ||
        height_abs > static_cast<std::uint32_t>(kMaxImageDimension)) {
        return codec_error::dimension_limit;
    }
    switch (bpp) {
        case 1:
        case 2:
        case 4:
        case 8:
        case 16:
        case 24:
        case 32:
            break;
        default:
            return codec_error::invalid_header;
    }
    if (!decoded_size_within_limits(static_cast<std::int32_t>(width),
                                    static_cast<std::int32_t>(height_abs))) {
        return codec_error::dimension_limit;
    }
    // Unstrided (BITMAPINFOHEADER-style) layouts: the exact pixel extent must
    // exist inside the buffer. RLE payloads are bounded by WIC's own walk.
    if (compression == 0 || compression == 3 || compression == 6) {
        const std::uint64_t stride =
            ((static_cast<std::uint64_t>(width) * bpp + 31u) / 32u) * 4u;
        const std::uint64_t bytes = stride * static_cast<std::uint64_t>(height_abs);
        if (bytes > kMaxDecodedBytes) {
            return codec_error::allocation_limit;
        }
        if (static_cast<std::uint64_t>(file_offset) + bytes > size) {
            return codec_error::truncated;
        }
    }
    out.width = static_cast<std::int32_t>(width);
    out.height = static_cast<std::int32_t>(height_abs);
    out.bit_count = bpp;
    out.compression = compression;
    return codec_error::none;
}

}  // namespace

namespace bmp_wic {

codec_error parse_header(const std::uint8_t* data, std::size_t size,
                         std::int32_t& oriented_w_out,
                         std::int32_t& oriented_h_out) noexcept {
    oriented_w_out = 0;
    oriented_h_out = 0;
    bmp_header_info info;
    const codec_error err = parse_bmp_header(data, size, info);
    if (err != codec_error::none) {
        return err;
    }
    oriented_w_out = info.width;
    oriented_h_out = info.height;
    return codec_error::none;
}

decode_result decode(const std::uint8_t* data, std::size_t size,
                     int scaling_denominator) noexcept {
    decode_result out;
    bmp_header_info info;
    const codec_error err = parse_bmp_header(data, size, info);
    if (err != codec_error::none) {
        out.error = err;
        return out;
    }
    (void)info;

    wic_context ctx;
    codec_error wic_err = ctx.open(data, size);
    if (wic_err != codec_error::none) {
        out.error = wic_err;
        out.detail = "bmp COM/WIC initialization failed";
        return out;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    wic_err = ctx.create_decoder(GUID_ContainerFormatBmp, decoder);
    if (wic_err != codec_error::none) {
        out.error = wic_err;
        out.detail = "bmp decoder creation failed";
        return out;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    HRESULT hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr) || !frame) {
        out.error = codec_error::decode_failed;
        out.detail = "bmp frame unavailable";
        return out;
    }
    return canonical_frame_decode(ctx.factory.Get(), frame.Get(),
                                  scaling_denominator, 1, codec_id::bmp_wic);
}

}  // namespace bmp_wic

namespace {

// --- GIF --------------------------------------------------------------------

bool gif_signature_ok(const std::uint8_t* data, std::size_t size) noexcept {
    if (data == nullptr || size < 6) {
        return false;
    }
    return (std::memcmp(data, "GIF87a", 6) == 0 || std::memcmp(data, "GIF89a", 6) == 0);
}

// Skips one GIF sub-block sequence starting at pos (a size byte, optional
// data bytes, terminated by a zero size byte). Bounded; never reads past
// size. pos is advanced past the zero terminator on success.
bool skip_gif_sub_blocks(const std::uint8_t* data, std::size_t size,
                         std::size_t& pos) noexcept {
    while (true) {
        if (pos >= size) {
            return false;
        }
        const std::uint8_t n = data[pos];
        ++pos;
        if (n == 0) {
            return true;
        }
        if (n > size - pos) {
            return false;
        }
        pos += n;
    }
}

struct gif_header_info {
    std::int32_t screen_w = 0;
    std::int32_t screen_h = 0;
};

// Full bounded block walk: global color table, extensions with sub-blocks,
// exactly one image descriptor (LCT, LZW min code size, data sub-blocks),
// then the trailer. Any truncation/malformed block is rejected; a second
// image descriptor (animation) is rejected before any pixel allocation.
codec_error parse_gif_header(const std::uint8_t* data, std::size_t size,
                             gif_header_info& out) noexcept {
    out = gif_header_info{};
    if (data == nullptr) {
        return codec_error::unsupported;
    }
    if (!gif_signature_ok(data, size)) {
        return size < 6 ? codec_error::truncated : codec_error::unsupported;
    }
    if (size < 13) {
        return codec_error::truncated;
    }
    const std::uint32_t w = rd_le16(data + 6);
    const std::uint32_t h = rd_le16(data + 8);
    if (w == 0 || h == 0) {
        return codec_error::invalid_header;
    }
    if (w > static_cast<std::uint32_t>(kMaxImageDimension) ||
        h > static_cast<std::uint32_t>(kMaxImageDimension)) {
        return codec_error::dimension_limit;
    }
    if (!decoded_size_within_limits(static_cast<std::int32_t>(w),
                                    static_cast<std::int32_t>(h))) {
        return codec_error::dimension_limit;
    }
    const std::uint8_t packed = data[10];
    std::size_t pos = 13;
    if ((packed & 0x80u) != 0) {
        const std::uint32_t gct_entries = 2u << (packed & 0x07u);  // 2..256
        const std::uint64_t gct_bytes = 3ull * gct_entries;
        if (gct_bytes > size - pos) {
            return codec_error::truncated;
        }
        pos += static_cast<std::size_t>(gct_bytes);
    }

    std::uint32_t image_descriptors = 0;
    while (true) {
        if (pos >= size) {
            return codec_error::truncated;  // missing trailer
        }
        const std::uint8_t block = data[pos];
        if (block == 0x3B) {  // trailer
            break;
        }
        if (block == 0x21) {  // extension: label + bounded sub-blocks
            if (size - pos < 2) {
                return codec_error::truncated;
            }
            pos += 2;
            if (!skip_gif_sub_blocks(data, size, pos)) {
                return codec_error::truncated;
            }
            continue;
        }
        if (block == 0x2C) {  // image descriptor
            if (size - pos < 10) {
                return codec_error::truncated;
            }
            const std::uint32_t iw = rd_le16(data + pos + 5);
            const std::uint32_t ih = rd_le16(data + pos + 7);
            if (iw == 0 || ih == 0) {
                return codec_error::invalid_header;
            }
            if (iw > static_cast<std::uint32_t>(kMaxImageDimension) ||
                ih > static_cast<std::uint32_t>(kMaxImageDimension)) {
                return codec_error::dimension_limit;
            }
            ++image_descriptors;
            if (image_descriptors > kMaxGifImageDescriptors) {
                // Animated GIF: deterministic reject, never the first frame.
                return codec_error::decode_failed;
            }
            const std::uint8_t ipacked = data[pos + 9];
            pos += 10;
            if ((ipacked & 0x80u) != 0) {
                const std::uint32_t lct_entries = 2u << (ipacked & 0x07u);
                const std::uint64_t lct_bytes = 3ull * lct_entries;
                if (lct_bytes > size - pos) {
                    return codec_error::truncated;
                }
                pos += static_cast<std::size_t>(lct_bytes);
            }
            if (pos >= size) {
                return codec_error::truncated;  // missing LZW min code size
            }
            ++pos;  // LZW minimum code size
            if (!skip_gif_sub_blocks(data, size, pos)) {
                return codec_error::truncated;
            }
            continue;
        }
        return codec_error::invalid_header;  // unknown block id
    }
    if (image_descriptors == 0) {
        return codec_error::invalid_header;  // no image
    }
    if (image_descriptors != 1) {
        return codec_error::decode_failed;  // static-only profile
    }
    out.screen_w = static_cast<std::int32_t>(w);
    out.screen_h = static_cast<std::int32_t>(h);
    return codec_error::none;
}

}  // namespace

namespace gif_wic {

codec_error parse_header(const std::uint8_t* data, std::size_t size,
                         std::int32_t& oriented_w_out,
                         std::int32_t& oriented_h_out) noexcept {
    oriented_w_out = 0;
    oriented_h_out = 0;
    gif_header_info info;
    const codec_error err = parse_gif_header(data, size, info);
    if (err != codec_error::none) {
        return err;
    }
    oriented_w_out = info.screen_w;
    oriented_h_out = info.screen_h;
    return codec_error::none;
}

decode_result decode(const std::uint8_t* data, std::size_t size,
                     int scaling_denominator) noexcept {
    decode_result out;
    gif_header_info info;
    const codec_error err = parse_gif_header(data, size, info);
    if (err != codec_error::none) {
        out.error = err;
        return out;
    }
    (void)info;

    wic_context ctx;
    codec_error wic_err = ctx.open(data, size);
    if (wic_err != codec_error::none) {
        out.error = wic_err;
        out.detail = "gif COM/WIC initialization failed";
        return out;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    wic_err = ctx.create_decoder(GUID_ContainerFormatGif, decoder);
    if (wic_err != codec_error::none) {
        out.error = wic_err;
        out.detail = "gif decoder creation failed";
        return out;
    }
    // The static-only walk guarantees one image descriptor; re-check the
    // decoder frame count defensively before requesting frame 0.
    UINT frame_count = 0;
    if (FAILED(decoder->GetFrameCount(&frame_count)) || frame_count != 1) {
        out.error = codec_error::decode_failed;
        out.detail = "gif frame count mismatch";
        return out;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    HRESULT hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr) || !frame) {
        out.error = codec_error::decode_failed;
        out.detail = "gif frame unavailable";
        return out;
    }
    return canonical_frame_decode(ctx.factory.Get(), frame.Get(),
                                  scaling_denominator, 1, codec_id::gif_wic);
}

}  // namespace gif_wic

namespace {

// --- classic TIFF -----------------------------------------------------------

struct tiff_reader {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    bool little = true;

    bool u16(std::uint64_t off, std::uint16_t& out) const noexcept {
        if (off + 2 > size) {
            return false;
        }
        out = little ? rd_le16(data + off) : rd_be16(data + off);
        return true;
    }
    bool u32(std::uint64_t off, std::uint32_t& out) const noexcept {
        if (off + 4 > size) {
            return false;
        }
        out = little ? rd_le32(data + off) : rd_be32(data + off);
        return true;
    }
};

std::uint32_t tiff_type_size(std::uint16_t type) noexcept {
    switch (type) {
        case 1:  // BYTE
        case 2:  // ASCII
        case 6:  // SBYTE
        case 7:  // UNDEFINED
            return 1;
        case 3:  // SHORT
        case 8:  // SSHORT
            return 2;
        case 4:  // LONG
        case 9:  // SLONG
        case 11: // FLOAT
        case 13: // IFD
            return 4;
        case 5:  // RATIONAL
        case 10: // SRATIONAL
        case 12: // DOUBLE
            return 8;
        default:
            return 0;  // unknown/unsupported type: entry skipped (spec-compatible)
    }
}

// Reads a scalar (count == 1) value of the given type. Returns false when
// the type/count is not a supported single scalar.
bool tiff_scalar(const tiff_reader& r, std::uint64_t entry_off, std::uint16_t type,
                 std::uint32_t count, std::uint32_t& out) noexcept {
    if (count != 1) {
        return false;
    }
    switch (type) {
        case 1:
        case 6:
        case 7: {
            if (entry_off + 8 >= r.size) return false;
            out = r.data[entry_off + 8];
            return true;
        }
        case 3:
        case 8: {
            std::uint16_t v = 0;
            if (!r.u16(entry_off + 8, v)) return false;
            out = v;
            return true;
        }
        case 4:
        case 9:
        case 13: {
            std::uint32_t v = 0;
            if (!r.u32(entry_off + 8, v)) return false;
            out = v;
            return true;
        }
        default:
            return false;
    }
}

// Reads element i of a LONG (or IFD) array value (inline or external).
bool tiff_u32_array_element(const tiff_reader& r, std::uint64_t entry_off,
                            std::uint16_t type, std::uint32_t count, std::uint32_t index,
                            std::uint32_t& out) noexcept {
    if (type != 4 && type != 13) {
        return false;
    }
    if (index >= count) {
        return false;
    }
    if (count == 1) {
        return r.u32(entry_off + 8, out);  // inline 4-byte value
    }
    std::uint32_t base = 0;
    if (!r.u32(entry_off + 8, base)) {
        return false;
    }
    return r.u32(static_cast<std::uint64_t>(base) + static_cast<std::uint64_t>(index) * 4u, out);
}

// DNG/raw private tag range (DNGVersion 0xC612 upward through the DNG
// private range). Presence anywhere in the bounded scan is a RAW indicator.
bool tiff_tag_is_raw_indicator(std::uint16_t tag) noexcept {
    if (tag >= 0xC612 && tag <= 0xCFFF) {
        return true;  // DNG private range (DNGVersion, CFA plane/pattern, ...)
    }
    switch (tag) {
        case 0x828D:  // CFARepeatPatternDim
        case 0x828E:  // CFAPattern
        case 0x828F:  // BatteryLevel (TIFF/EP, follows CFA tags)
            return true;
        default:
            return false;
    }
}

struct tiff_header_info {
    std::int32_t width = 0;
    std::int32_t height = 0;
    int orientation = 0;  // 0/1-8 from IFD0 tag 0x0112
};

// Bounded endian-aware classic TIFF scan. Walks the IFD chain plus bounded
// SubIFD (0x014A) and ExifIFD (0x8769) descent, enforcing checked offsets,
// types, counts, next-IFD loop detection and the documented IFD/entry caps.
// Page 0 (first IFD) supplies width/height/orientation. Any DNG/CFA/CR2
// indicator rejects the container so no embedded RAW preview can reach WIC.
codec_error parse_tiff_header(const std::uint8_t* data, std::size_t size,
                              tiff_header_info& out) noexcept {
    out = tiff_header_info{};
    if (data == nullptr) {
        return codec_error::unsupported;
    }
    if (size < 8) {
        return codec_error::truncated;
    }
    const bool little = (data[0] == 'I' && data[1] == 'I');
    const bool big = (data[0] == 'M' && data[1] == 'M');
    if (!little && !big) {
        return codec_error::unsupported;
    }
    // Classic TIFF magic 42 only; BigTIFF (43) is never routed here.
    const std::uint16_t magic = little ? rd_le16(data + 2) : rd_be16(data + 2);
    if (magic != 42) {
        return codec_error::unsupported;
    }
    // Canon CR2 is a TIFF container with 'CR' at offset 8: RAW, reject
    // before any IFD work so no embedded preview is shown.
    if (size >= 10 && data[8] == 'C' && data[9] == 'R') {
        return codec_error::unsupported;
    }

    tiff_reader r{data, size, little};
    std::uint32_t first_ifd = 0;
    if (!r.u32(4, first_ifd)) {
        return codec_error::truncated;
    }
    if (first_ifd < 8) {
        return codec_error::invalid_header;  // must point past the header
    }

    std::vector<std::uint32_t> pending;
    std::vector<std::uint32_t> visited;
    pending.push_back(first_ifd);
    bool first_ifd_seen = false;
    bool raw_indicator = false;

    while (!pending.empty()) {
        const std::uint32_t ifd_off = pending.back();
        pending.pop_back();
        for (const std::uint32_t v : visited) {
            if (v == ifd_off) {
                return codec_error::invalid_header;  // IFD loop
            }
        }
        if (visited.size() >= kMaxTiffIfds) {
            return codec_error::invalid_header;  // bounded IFD cap
        }
        visited.push_back(ifd_off);

        std::uint16_t entry_count = 0;
        if (!r.u16(ifd_off, entry_count)) {
            return codec_error::invalid_header;  // IFD offset outside buffer
        }
        if (entry_count > kMaxTiffIfdEntries) {
            return codec_error::invalid_header;
        }
        const std::uint64_t entries_off = static_cast<std::uint64_t>(ifd_off) + 2ull;
        const std::uint64_t entries_end =
            entries_off + static_cast<std::uint64_t>(entry_count) * 12ull;
        if (entries_end + 4ull > size) {
            return codec_error::truncated;
        }

        const bool page0 = !first_ifd_seen;
        for (std::uint32_t i = 0; i < entry_count; ++i) {
            const std::uint64_t e = entries_off + static_cast<std::uint64_t>(i) * 12ull;
            std::uint16_t tag = 0;
            std::uint16_t type = 0;
            std::uint32_t count = 0;
            if (!r.u16(e, tag) || !r.u16(e + 2, type) || !r.u32(e + 4, count)) {
                return codec_error::invalid_header;
            }
            const std::uint32_t type_size = tiff_type_size(type);
            if (type_size != 0) {
                // Checked external value extent (inline values need no offset).
                const std::uint64_t need =
                    static_cast<std::uint64_t>(type_size) * static_cast<std::uint64_t>(count);
                if (need > 4ull) {
                    std::uint32_t value_off = 0;
                    if (!r.u32(e + 8, value_off)) {
                        return codec_error::invalid_header;
                    }
                    if (static_cast<std::uint64_t>(value_off) + need > size) {
                        return codec_error::invalid_header;
                    }
                }
            }
            if (tiff_tag_is_raw_indicator(tag)) {
                raw_indicator = true;
            }
            if (tag == 0x0106 && type == 3) {  // PhotometricInterpretation
                std::uint32_t pi = 0;
                if (tiff_scalar(r, e, type, count, pi) &&
                    (pi == 32803u /*CFA*/ || pi == 34892u /*LinearRaw*/)) {
                    raw_indicator = true;
                }
            }
            if (page0) {
                if (tag == 0x0100) {  // ImageWidth
                    std::uint32_t v = 0;
                    if (tiff_scalar(r, e, type, count, v)) {
                        out.width = v > static_cast<std::uint32_t>(kMaxImageDimension)
                                       ? kMaxImageDimension + 1
                                       : static_cast<std::int32_t>(v);
                    }
                } else if (tag == 0x0101) {  // ImageLength
                    std::uint32_t v = 0;
                    if (tiff_scalar(r, e, type, count, v)) {
                        out.height = v > static_cast<std::uint32_t>(kMaxImageDimension)
                                        ? kMaxImageDimension + 1
                                        : static_cast<std::int32_t>(v);
                    }
                } else if (tag == 0x0112) {  // Orientation
                    std::uint32_t v = 0;
                    if (tiff_scalar(r, e, type, count, v) && v >= 1 && v <= 8) {
                        out.orientation = static_cast<int>(v);
                    }
                }
            }
            if (tag == 0x8769) {  // ExifIFD pointer
                std::uint32_t ptr = 0;
                if (tiff_scalar(r, e, type, count, ptr) && ptr >= 8) {
                    pending.push_back(ptr);
                }
            } else if (tag == 0x014A) {  // SubIFDs (array of offsets)
                const std::uint32_t n =
                    count < kMaxTiffSubIfdPointers ? count : kMaxTiffSubIfdPointers;
                for (std::uint32_t k = 0; k < n; ++k) {
                    std::uint32_t ptr = 0;
                    if (tiff_u32_array_element(r, e, type, count, k, ptr) && ptr >= 8) {
                        pending.push_back(ptr);
                    }
                }
            }
        }

        std::uint32_t next_ifd = 0;
        if (!r.u32(entries_end, next_ifd)) {
            return codec_error::invalid_header;
        }
        if (next_ifd != 0) {
            pending.push_back(next_ifd);
        }
        first_ifd_seen = true;
    }

    if (raw_indicator) {
        // DNG/CFA/CR2-derived container: unsupported by the static raster
        // profile so no embedded RAW preview is ever displayed. Unrecognized
        // RAW variants may not carry these markers and can remain
        // "unsupported" for other reasons; this list is not exhaustive.
        return codec_error::unsupported;
    }
    if (out.width <= 0 || out.height <= 0) {
        return codec_error::invalid_header;
    }
    if (out.width > kMaxImageDimension || out.height > kMaxImageDimension) {
        return codec_error::dimension_limit;
    }
    if (!decoded_size_within_limits(out.width, out.height)) {
        return codec_error::dimension_limit;
    }
    return codec_error::none;
}

}  // namespace

namespace tiff_wic {

codec_error parse_header(const std::uint8_t* data, std::size_t size,
                         std::int32_t& oriented_w_out,
                         std::int32_t& oriented_h_out) noexcept {
    oriented_w_out = 0;
    oriented_h_out = 0;
    tiff_header_info info;
    const codec_error err = parse_tiff_header(data, size, info);
    if (err != codec_error::none) {
        return err;
    }
    int orientation = info.orientation;
    if (orientation < 1 || orientation > 8) {
        orientation = 1;
    }
    const bool swap = orientation >= 5;
    oriented_w_out = swap ? info.height : info.width;
    oriented_h_out = swap ? info.width : info.height;
    return codec_error::none;
}

decode_result decode(const std::uint8_t* data, std::size_t size,
                     int scaling_denominator) noexcept {
    decode_result out;
    tiff_header_info info;
    const codec_error err = parse_tiff_header(data, size, info);
    if (err != codec_error::none) {
        out.error = err;
        return out;
    }

    wic_context ctx;
    codec_error wic_err = ctx.open(data, size);
    if (wic_err != codec_error::none) {
        out.error = wic_err;
        out.detail = "tiff COM/WIC initialization failed";
        return out;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    wic_err = ctx.create_decoder(GUID_ContainerFormatTiff, decoder);
    if (wic_err != codec_error::none) {
        out.error = wic_err;
        out.detail = "tiff decoder creation failed";
        return out;
    }
    // Page 0 only; later pages are never composited or decoded.
    ComPtr<IWICBitmapFrameDecode> frame;
    HRESULT hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr) || !frame) {
        out.error = codec_error::decode_failed;
        out.detail = "tiff frame unavailable";
        return out;
    }
    int orientation = info.orientation;
    if (orientation < 1 || orientation > 8) {
        orientation = 1;
    }
    return canonical_frame_decode(ctx.factory.Get(), frame.Get(),
                                  scaling_denominator, orientation, codec_id::tiff_wic);
}

}  // namespace tiff_wic

namespace {

// --- ICO / CUR --------------------------------------------------------------

struct dir_entry {
    std::uint16_t index = 0;
    std::int32_t width = 0;   // 0 in the directory means 256
    std::int32_t height = 0;
    std::uint16_t dir_word = 0;  // wBitCount (ICO) / wHotspotY (CUR, metadata)
    std::uint32_t bytes = 0;
    std::uint32_t offset = 0;
};

// Parses a type-1 (ICO) or type-2 (CUR) directory. The two words at entry
// offsets 4/6 are wPlanes/wBitCount for ICO but wHotspotX/wHotspotY for CUR;
// they are read as opaque metadata and never validated as planes/bpp here.
codec_error parse_directory(const std::uint8_t* data, std::size_t size,
                            std::uint16_t expected_type,
                            std::vector<dir_entry>& out) noexcept {
    out.clear();
    if (data == nullptr) {
        return codec_error::unsupported;
    }
    if (size < 6) {
        return codec_error::truncated;
    }
    if (rd_le16(data) != 0 || rd_le16(data + 2) != expected_type) {
        return codec_error::invalid_header;  // reserved must be 0, type exact
    }
    const std::uint32_t count = rd_le16(data + 4);
    if (count == 0 || count > kMaxIcoDirectoryEntries) {
        return codec_error::invalid_header;
    }
    const std::uint64_t dir_end = 6ull + static_cast<std::uint64_t>(count) * 16ull;
    if (dir_end > size) {
        return codec_error::truncated;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t* e = data + 6 + static_cast<std::size_t>(i) * 16;
        if (e[3] != 0) {
            return codec_error::invalid_header;  // bReserved
        }
        dir_entry de;
        de.index = static_cast<std::uint16_t>(i);
        de.width = e[0] == 0 ? 256 : e[0];
        de.height = e[1] == 0 ? 256 : e[1];
        de.dir_word = rd_le16(e + 6);
        de.bytes = rd_le32(e + 8);
        de.offset = rd_le32(e + 12);
        if (de.bytes == 0 ||
            static_cast<std::uint64_t>(de.offset) < dir_end ||
            static_cast<std::uint64_t>(de.offset) + de.bytes > size) {
            return codec_error::invalid_header;  // payload range outside buffer
        }
        out.push_back(de);
    }
    return codec_error::none;
}

// Effective embedded-PNG bit depth (IHDR bit depth times samples) used only
// for the deterministic tie-break; 32 when the IHDR shape is unexpected.
std::uint16_t png_effective_bit_depth(const std::uint8_t* p, std::size_t len) noexcept {
    if (len < 26) {
        return 32;
    }
    const std::uint8_t depth = p[24];
    const std::uint8_t color = p[25];
    int samples = 0;
    switch (color) {
        case 0: samples = 1; break;
        case 2: samples = 3; break;
        case 3: samples = 1; break;
        case 4: samples = 2; break;
        case 6: samples = 4; break;
        default: return 32;
    }
    if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16) {
        return 32;
    }
    const int bits = static_cast<int>(depth) * samples;
    return bits > 0 ? static_cast<std::uint16_t>(bits) : 32;
}

// Validates one directory entry's payload without decoding: embedded PNG
// headers go through the decoder-independent PNG header gate; DIB payloads
// are checked for known header size, geometry, bit depth, compression and
// the exact XOR + AND-mask extent inside the declared payload range. For CUR
// (`cursor` true) the directory bit-count word is hotspot metadata, so the
// payload derives its own bit depth instead of trusting the directory.
codec_error validate_entry_payload(const std::uint8_t* data, std::size_t size,
                                   const dir_entry& de, bool cursor,
                                   raster_frame_entry& out) noexcept {
    (void)size;
    out = raster_frame_entry{};
    const std::uint8_t* p = data + de.offset;
    const std::size_t len = de.bytes;
    out.index = de.index;
    out.payload_offset = de.offset;
    out.payload_bytes = de.bytes;

    if (len >= 8 && p[0] == 0x89 && p[1] == 0x50 && p[2] == 0x4E && p[3] == 0x47 &&
        p[4] == 0x0D && p[5] == 0x0A && p[6] == 0x1A && p[7] == 0x0A) {
        std::int32_t pw = 0;
        std::int32_t ph = 0;
        const codec_error perr = png_wic::parse_header(p, len, pw, ph);
        if (perr != codec_error::none) {
            return perr;  // unsafe embedded PNG: skipped by selection
        }
        out.png_payload = true;
        out.width = pw;
        out.height = ph;
        out.bit_depth = cursor ? png_effective_bit_depth(p, len)
                               : (de.dir_word != 0 ? de.dir_word : 32);
        return codec_error::none;
    }

    // DIB payload: BITMAPINFOHEADER (or a later compatible header) followed
    // by the XOR bitmap and the 1bpp AND mask, both row-padded to 4 bytes.
    if (len < 40) {
        return codec_error::invalid_header;
    }
    const std::uint32_t dib = rd_le32(p);
    if (!is_known_dib_size(dib) || dib < 40 || dib > len) {
        return codec_error::invalid_header;
    }
    const std::int32_t dw = rd_le_i32(p + 4);
    const std::int32_t dh = rd_le_i32(p + 8);
    const std::uint16_t planes = rd_le16(p + 12);
    const std::uint16_t bpp = rd_le16(p + 14);
    const std::uint32_t compression = rd_le32(p + 16);
    if (dw <= 0 || dw > 256 || dh <= 0) {
        return codec_error::invalid_header;
    }
    // ICO DIB heights are stored doubled (XOR bitmap + AND mask).
    const std::int32_t actual_h = (dh % 2 == 0) ? dh / 2 : dh;
    if (actual_h <= 0 || actual_h > 256) {
        return codec_error::invalid_header;
    }
    if (planes != 1) {
        return codec_error::invalid_header;
    }
    switch (bpp) {
        case 1:
        case 4:
        case 8:
        case 16:
        case 24:
        case 32:
            break;
        default:
            return codec_error::invalid_header;
    }
    if (compression != 0 && compression != 3 && compression != 6) {
        return codec_error::invalid_header;
    }
    std::uint32_t palette_entries = 0;
    if (bpp <= 8) {
        const std::uint32_t clr_used = rd_le32(p + 32);  // biClrUsed
        palette_entries = clr_used != 0 ? clr_used : (1u << bpp);
        if (palette_entries > 256) {
            return codec_error::invalid_header;
        }
    }
    const std::uint64_t row_xor =
        ((static_cast<std::uint64_t>(dw) * bpp + 31u) / 32u) * 4u;
    const std::uint64_t row_and =
        ((static_cast<std::uint64_t>(dw) + 31u) / 32u) * 4u;
    std::uint64_t need = static_cast<std::uint64_t>(dib) +
                         static_cast<std::uint64_t>(palette_entries) * 4u +
                         row_xor * static_cast<std::uint64_t>(actual_h) +
                         row_and * static_cast<std::uint64_t>(actual_h);
    if (compression == 3 || compression == 6) {
        need += 12ull;  // color masks after the header (no palette)
    }
    if (need > len) {
        return codec_error::invalid_header;
    }
    out.width = dw;
    out.height = actual_h;
    out.bit_depth = bpp;
    out.png_payload = false;
    out.dib_size = dib;
    out.palette_entries = palette_entries;
    out.has_color_masks = (compression == 3 || compression == 6);
    return codec_error::none;
}

bool frame_entry_better(const raster_frame_entry& candidate,
                        const raster_frame_entry& current) noexcept {
    const std::uint64_t ca = static_cast<std::uint64_t>(candidate.width) * candidate.height;
    const std::uint64_t cb = static_cast<std::uint64_t>(current.width) * current.height;
    if (ca != cb) {
        return ca > cb;  // largest image first
    }
    if (candidate.bit_depth != current.bit_depth) {
        return candidate.bit_depth > current.bit_depth;  // higher bit depth
    }
    return candidate.index < current.index;  // earlier directory entry
}

// Shared bounded selection over a type-1 (ICO) or type-2 (CUR) directory.
// The deterministic policy is largest safe area, then higher validated bit
// depth, then directory order.
codec_error select_frame(const std::uint8_t* data, std::size_t size, bool cursor,
                         raster_frame_entry& out) noexcept {
    out = raster_frame_entry{};
    std::vector<dir_entry> dir;
    const codec_error err =
        parse_directory(data, size, cursor ? 2u : 1u, dir);
    if (err != codec_error::none) {
        return err;
    }
    bool found = false;
    raster_frame_entry best;
    for (const dir_entry& de : dir) {
        raster_frame_entry candidate;
        const codec_error perr =
            validate_entry_payload(data, size, de, cursor, candidate);
        if (perr != codec_error::none) {
            continue;  // "largest safe image": unsafe entries are skipped
        }
        if (!found || frame_entry_better(candidate, best)) {
            best = candidate;
            found = true;
        }
    }
    if (!found) {
        return codec_error::invalid_header;  // no safely decodable entry
    }
    out = best;
    return codec_error::none;
}

// Shared payload-slice decode for the selected ICO/CUR entry. Only the
// selected slice reaches an explicit in-box WIC decoder: embedded PNG via
// GUID_ContainerFormatPng, DIB via a bounded BITMAPFILEHEADER wrapper and
// GUID_ContainerFormatBmp with the 1bpp AND mask applied afterwards.
decode_result decode_frame(const std::uint8_t* data, std::size_t size,
                           const raster_frame_entry& selected,
                           int scaling_denominator, codec_id id,
                           const char* label) noexcept {
    decode_result out;
    if (selected.payload_offset >= size ||
        selected.payload_bytes > size - selected.payload_offset) {
        out.error = codec_error::invalid_header;
        return out;
    }
    const std::uint8_t* payload = data + selected.payload_offset;
    const std::size_t payload_len = selected.payload_bytes;

    if (selected.png_payload) {
        wic_context ctx;
        codec_error wic_err = ctx.open(payload, payload_len);
        if (wic_err != codec_error::none) {
            out.error = wic_err;
            out.detail = std::string(label) + " COM/WIC initialization failed";
            return out;
        }
        ComPtr<IWICBitmapDecoder> decoder;
        wic_err = ctx.create_decoder(GUID_ContainerFormatPng, decoder);
        if (wic_err != codec_error::none) {
            out.error = wic_err;
            out.detail = std::string(label) + " decoder creation failed";
            return out;
        }
        ComPtr<IWICBitmapFrameDecode> frame;
        HRESULT hr = decoder->GetFrame(0, &frame);
        if (FAILED(hr) || !frame) {
            out.error = codec_error::decode_failed;
            out.detail = std::string(label) + " frame unavailable";
            return out;
        }
        return canonical_frame_decode(ctx.factory.Get(), frame.Get(),
                                      scaling_denominator, 1, id);
    }

    // DIB payloads are wrapped in a bounded BITMAPFILEHEADER slice and
    // decoded with the explicit in-box WIC BMP container decoder. Slicing
    // (rather than indexing frames of the container) keeps the deterministic
    // directory selection exact regardless of how WIC orders frames. The
    // doubled ICO/CUR DIB height is patched to the validated XOR height.
    if (selected.dib_size < 40 || selected.dib_size > payload_len ||
        selected.width <= 0 || selected.height <= 0) {
        out.error = codec_error::invalid_header;
        return out;
    }
    const std::uint64_t masks = selected.has_color_masks ? 12ull : 0ull;
    const std::uint64_t pal_bytes =
        static_cast<std::uint64_t>(selected.palette_entries) * 4u;
    const std::uint64_t pixel_off = 14ull + selected.dib_size + pal_bytes + masks;
    const std::uint64_t bmp_len = 14ull + payload_len;
    if (bmp_len > size + 14ull || pixel_off > bmp_len) {
        out.error = codec_error::invalid_header;
        return out;
    }
    std::vector<std::uint8_t> bmp;
    try {
        bmp.resize(static_cast<std::size_t>(bmp_len));
    } catch (...) {
        out.error = codec_error::allocation_limit;
        return out;
    }
    bmp[0] = 'B';
    bmp[1] = 'M';
    const std::uint32_t file_size = static_cast<std::uint32_t>(bmp_len);
    std::memcpy(bmp.data() + 2, &file_size, 4);
    bmp[6] = bmp[7] = bmp[8] = bmp[9] = 0;
    const std::uint32_t off_bits = static_cast<std::uint32_t>(pixel_off);
    std::memcpy(bmp.data() + 10, &off_bits, 4);
    std::memcpy(bmp.data() + 14, payload, payload_len);
    const std::int32_t actual_h = selected.height;
    std::memcpy(bmp.data() + 14 + 8, &actual_h, 4);

    wic_context ctx;
    codec_error wic_err = ctx.open(bmp.data(), bmp.size());
    if (wic_err != codec_error::none) {
        out.error = wic_err;
        out.detail = std::string(label) + " COM/WIC initialization failed";
        return out;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    wic_err = ctx.create_decoder(GUID_ContainerFormatBmp, decoder);
    if (wic_err != codec_error::none) {
        out.error = wic_err;
        out.detail = std::string(label) + " decoder creation failed";
        return out;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    HRESULT hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr) || !frame) {
        out.error = codec_error::decode_failed;
        out.detail = std::string(label) + " frame unavailable";
        return out;
    }
    decode_result decoded = canonical_frame_decode(
        ctx.factory.Get(), frame.Get(), scaling_denominator, 1, id);
    if (decoded.error != codec_error::none) {
        return decoded;
    }
    // Compare against the validated stored dimensions (src_*) so a scaled
    // decode (den > 1) is not mistaken for a payload dimension mismatch.
    if (decoded.image.src_width != selected.width ||
        decoded.image.src_height != selected.height) {
        decoded.error = codec_error::decode_failed;
        decoded.detail = std::string(label) + " payload dimension mismatch";
        decoded.image = decoded_image{};
        return decoded;
    }
    // Apply the 1bpp AND mask (bottom-up, MSB-first, row-padded): set bits
    // mark transparent pixels, cleared to (0,0,0,0) to preserve the
    // canonical premultiplied contract. The mask extent was validated
    // during payload validation. Output-space coordinates map back to the
    // stored mask so a scaled decode stays consistent (nearest sample).
    const std::uint64_t w = static_cast<std::uint64_t>(selected.width);
    const std::uint64_t h = static_cast<std::uint64_t>(selected.height);
    const std::uint64_t bpp = selected.bit_depth;
    const std::uint64_t row_xor = ((w * bpp + 31u) / 32u) * 4u;
    const std::uint64_t row_and = ((w + 31u) / 32u) * 4u;
    const std::uint64_t mask_base = static_cast<std::uint64_t>(selected.dib_size) +
                                    pal_bytes + masks + row_xor * h;
    const std::int32_t ow = decoded.image.width;
    const std::int32_t oh = decoded.image.height;
    if (mask_base + row_and * h <= payload_len && ow > 0 && oh > 0) {
        const std::size_t stride = static_cast<std::size_t>(decoded.image.stride);
        for (std::int32_t y = 0; y < oh; ++y) {
            const std::uint64_t sy =
                static_cast<std::uint64_t>(y) * h / static_cast<std::uint64_t>(oh);
            const std::uint64_t clamped_sy = sy < h ? sy : h - 1u;
            const std::uint64_t mask_row = h - 1u - clamped_sy;
            const std::uint8_t* mask = payload + mask_base + mask_row * row_and;
            std::uint8_t* dst =
                decoded.image.pixels.data() + static_cast<std::size_t>(y) * stride;
            for (std::int32_t x = 0; x < ow; ++x) {
                const std::uint64_t sx =
                    static_cast<std::uint64_t>(x) * w / static_cast<std::uint64_t>(ow);
                const std::uint64_t clamped_sx = sx < w ? sx : w - 1u;
                const std::uint8_t bit = static_cast<std::uint8_t>(
                    0x80u >> (static_cast<std::uint32_t>(clamped_sx) & 7u));
                if ((mask[static_cast<std::size_t>(clamped_sx) >> 3] & bit) != 0) {
                    dst[static_cast<std::size_t>(x) * 4u + 0] = 0;
                    dst[static_cast<std::size_t>(x) * 4u + 1] = 0;
                    dst[static_cast<std::size_t>(x) * 4u + 2] = 0;
                    dst[static_cast<std::size_t>(x) * 4u + 3] = 0;
                }
            }
        }
    }
    out = std::move(decoded);
    out.error = codec_error::none;
    return out;
}

}  // namespace

namespace ico_wic {

codec_error select_entry(const std::uint8_t* data, std::size_t size,
                         ico_entry& out) noexcept {
    return select_frame(data, size, false, out);
}

codec_error parse_header(const std::uint8_t* data, std::size_t size,
                         std::int32_t& oriented_w_out,
                         std::int32_t& oriented_h_out) noexcept {
    oriented_w_out = 0;
    oriented_h_out = 0;
    ico_entry selected;
    const codec_error err = select_entry(data, size, selected);
    if (err != codec_error::none) {
        return err;
    }
    oriented_w_out = selected.width;
    oriented_h_out = selected.height;
    return codec_error::none;
}

decode_result decode(const std::uint8_t* data, std::size_t size,
                     int scaling_denominator) noexcept {
    ico_entry selected;
    const codec_error err = select_entry(data, size, selected);
    if (err != codec_error::none) {
        decode_result out;
        out.error = err;
        return out;
    }
    return decode_frame(data, size, selected, scaling_denominator,
                        codec_id::ico_wic, "ico");
}

}  // namespace ico_wic

namespace cur_wic {

codec_error select_entry(const std::uint8_t* data, std::size_t size,
                         cur_entry& out) noexcept {
    return select_frame(data, size, true, out);
}

codec_error parse_header(const std::uint8_t* data, std::size_t size,
                         std::int32_t& oriented_w_out,
                         std::int32_t& oriented_h_out) noexcept {
    oriented_w_out = 0;
    oriented_h_out = 0;
    cur_entry selected;
    const codec_error err = select_entry(data, size, selected);
    if (err != codec_error::none) {
        return err;
    }
    oriented_w_out = selected.width;
    oriented_h_out = selected.height;
    return codec_error::none;
}

decode_result decode(const std::uint8_t* data, std::size_t size,
                     int scaling_denominator) noexcept {
    cur_entry selected;
    const codec_error err = select_entry(data, size, selected);
    if (err != codec_error::none) {
        decode_result out;
        out.error = err;
        return out;
    }
    return decode_frame(data, size, selected, scaling_denominator,
                        codec_id::cur_wic, "cur");
}

}  // namespace cur_wic

}  // namespace oz::viewer_core

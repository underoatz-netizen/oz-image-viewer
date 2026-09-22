// png_wic.cpp - WIC PNG decoder (Stage 3.1). PNG only; there is no generic
// WIC fallback. Decodes frame 0 to premultiplied BGRA8 (sRGB) with valid
// EXIF orientation applied on the worker thread before scale selection.
//
// Policy (stable contract, deterministic across SDKs):
// - Explicit sRGB / unprofiled input is treated as sRGB.
// - An embedded ICC profile is converted to sRGB when WIC color support
//   accepts it; an invalid/unsupported profile falls back internally to a
//   plain pixel-format conversion (decode still succeeds).
// - Valid EXIF orientations 1-8 are applied; missing/invalid orientation
//   decodes as normal (1). Orientation is read via WIC metadata when the
//   SDK exposes it, otherwise via a manual eXIf byte scan; both feed the
//   same transform, so behavior does not depend on SDK variance.
// - APNG decodes as frame 0 only; 16-bit/HDR sources are reduced to 8-bit
//   SDR by the WIC pipeline (no 16-bit preservation, no advanced gamut).
//
// COM/WIC is used only on an existing worker thread: CoInitializeEx with
// COINIT_MULTITHREADED is attempted lazily per decode and balanced with
// CoUninitialize on the same thread when this call initialized COM.
// RPC_E_CHANGED_MODE (thread already STA) is tolerated as a bounded path:
// WIC is still usable and no uninitialize is issued. No WIC/COM handle ever
// touches the UI thread, and no worker/thread/service is added here.
#include "png_wic.hpp"

#include <windows.h>

#include <wincodec.h>
#include <objbase.h>
#include <wrl/client.h>

#include <cstring>

namespace oz::viewer_core::png_wic {
namespace {

using Microsoft::WRL::ComPtr;

constexpr std::uint8_t kPngSig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

// Balanced MTA initializer for the calling worker thread.
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

std::uint32_t read_be32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

// Bounds-checked chunk walk. Invokes visit(type4, chunk_data, chunk_len) for
// each chunk; stops when visit returns false. Never reads past size.
template <typename F>
bool walk_chunks(const std::uint8_t* data, std::size_t size, F&& visit) noexcept {
    if (data == nullptr || size < 8) {
        return false;
    }
    std::size_t pos = 8;  // skip signature
    while (true) {
        if (size - pos < 8) {
            return false;  // truncated chunk header
        }
        const std::uint32_t len = read_be32(data + pos);
        const std::uint8_t* type = data + pos + 4;
        if (len > size - pos - 12) {
            return false;  // chunk overruns buffer
        }
        const std::uint8_t* chunk = data + pos + 8;
        if (!visit(type, chunk, len)) {
            return true;
        }
        if (std::memcmp(type, "IEND", 4) == 0) {
            return true;
        }
        pos += 8 + static_cast<std::size_t>(len) + 4;  // header + data + CRC
        if (pos >= size) {
            return true;
        }
    }
}

std::uint16_t read_le16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
}

std::uint16_t read_be16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) | p[1]);
}

std::uint32_t read_le32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

// Parse a TIFF/EXIF structure for tag 0x0112 (orientation). Returns 1-8 when
// a valid SHORT value is found, else 0.
int parse_tiff_orientation(const std::uint8_t* tiff, std::size_t len) noexcept {
    if (tiff == nullptr || len < 8) {
        return 0;
    }
    const bool little = (tiff[0] == 'I' && tiff[1] == 'I');
    const bool big = (tiff[0] == 'M' && tiff[1] == 'M');
    if (!little && !big) {
        return 0;
    }
    auto u16 = [&](std::size_t o, bool& ok) -> std::uint16_t {
        if (o + 2 > len) {
            ok = false;
            return 0;
        }
        return little ? read_le16(tiff + o) : read_be16(tiff + o);
    };
    auto u32 = [&](std::size_t o, bool& ok) -> std::uint32_t {
        if (o + 4 > len) {
            ok = false;
            return 0;
        }
        return little ? read_le32(tiff + o) : read_be32(tiff + o);
    };
    bool ok = true;
    if (u16(2, ok) != 42 || !ok) {
        return 0;
    }
    const std::uint32_t ifd_off = u32(4, ok);
    if (!ok || ifd_off > len) {
        return 0;
    }
    const std::size_t count = u16(ifd_off, ok);
    if (!ok) {
        return 0;
    }
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t e = static_cast<std::size_t>(ifd_off) + 2 + i * 12;
        if (e + 12 > len) {
            return 0;
        }
        if (u16(e, ok) != 0x0112 || !ok) {
            continue;
        }
        const std::uint16_t type = u16(e + 2, ok);
        const std::uint32_t n = u32(e + 4, ok);
        if (!ok || n < 1) {
            return 0;
        }
        std::uint32_t value = 0;
        if (type == 3) {  // SHORT: inline in the value field
            value = little ? read_le16(tiff + e + 8) : read_be16(tiff + e + 8);
        } else if (type == 4) {  // LONG: inline 32-bit value
            value = u32(e + 8, ok);
        } else {
            return 0;  // unexpected type: treat as invalid
        }
        if (!ok || value < 1 || value > 8) {
            return 0;
        }
        return static_cast<int>(value);
    }
    return 0;
}

// EXIF orientation transform on PBGRA rows: maps destination (dx,dy)
// in oriented space back to source (sx,sy) in stored space.
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
                case 2: sx = sw - 1 - dx; sy = dy; break;              // flip H
                case 3: sx = sw - 1 - dx; sy = sh - 1 - dy; break;     // rot 180
                case 4: sx = dx; sy = sh - 1 - dy; break;              // flip V
                case 5: sx = dy; sy = dx; break;                       // transpose
                case 6: sx = sy; sy = sh - 1 - dx; break;              // rot 90 CW
                case 7: sx = sw - 1 - dy; sy = sh - 1 - dx; break;     // transverse
                case 8: sx = sw - 1 - dy; sy = dx; break;              // rot 90 CCW
                default: break;                                       // 1: normal
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

// Best-effort WIC metadata orientation read. Returns 1-8 on success, else 0.
// Candidate query paths cover the layouts WIC SDKs expose; failure simply
// means the caller falls back to the manual eXIf scan.
int read_wic_orientation(IWICBitmapFrameDecode* frame) noexcept {
    if (frame == nullptr) {
        return 0;
    }
    ComPtr<IWICMetadataQueryReader> qr;
    if (FAILED(frame->GetMetadataQueryReader(&qr)) || !qr) {
        return 0;
    }
    static const wchar_t* kPaths[] = {
        L"/app1/ifd/{ushort=274}",  // EXIF via APP1 (JPEG-style layout)
        L"/ifd/{ushort=274}",       // top-level IFD
        L"/eXIf/{ushort=274}",      // PNG eXIf chunk projection
        L"System.Photo.Orientation",
    };
    for (const wchar_t* path : kPaths) {
        PROPVARIANT var{};
        PropVariantInit(&var);
        HRESULT hr = qr->GetMetadataByName(path, &var);
        int value = 0;
        if (SUCCEEDED(hr)) {
            if (var.vt == VT_UI2) {
                value = var.uiVal;
            } else if (var.vt == VT_UI4 && var.ulVal <= 65535) {
                value = static_cast<int>(var.ulVal);
            } else if (var.vt == VT_UI1) {
                value = var.bVal;
            }
        }
        PropVariantClear(&var);
        if (value >= 1 && value <= 8) {
            return value;
        }
    }
    return 0;
}

}  // namespace

int read_exif_orientation(const std::uint8_t* data, std::size_t size) noexcept {
    if (data == nullptr || size < 8 ||
        std::memcmp(data, kPngSig, 8) != 0) {
        return 0;
    }
    int found = 0;
    walk_chunks(data, size, [&](const std::uint8_t* type, const std::uint8_t* chunk,
                                std::uint32_t len) noexcept {
        if (std::memcmp(type, "eXIf", 4) == 0) {
            found = parse_tiff_orientation(chunk, len);
            return false;  // first eXIf wins
        }
        return true;
    });
    return (found >= 1 && found <= 8) ? found : 0;
}

codec_error parse_header(const std::uint8_t* data, std::size_t size,
                         std::int32_t& oriented_w_out,
                         std::int32_t& oriented_h_out) noexcept {
    oriented_w_out = 0;
    oriented_h_out = 0;
    if (data == nullptr || size < 8 ||
        std::memcmp(data, kPngSig, 8) != 0) {
        return codec_error::unsupported;
    }
    if (size > kMaxEncodedBytes) {
        return codec_error::allocation_limit;
    }
    // IHDR must be the first chunk: 4-byte length (13) + "IHDR" + 13 bytes.
    if (size < 8 + 8 + 13) {
        return codec_error::truncated;
    }
    if (read_be32(data + 8) != 13 || std::memcmp(data + 12, "IHDR", 4) != 0) {
        return codec_error::invalid_header;
    }
    const std::uint32_t w = read_be32(data + 16);
    const std::uint32_t h = read_be32(data + 20);
    if (w == 0 || h == 0) {
        return codec_error::invalid_header;
    }
    if (w > static_cast<std::uint32_t>(kMaxImageDimension) ||
        h > static_cast<std::uint32_t>(kMaxImageDimension)) {
        return codec_error::dimension_limit;
    }
    const int orientation = read_exif_orientation(data, size);
    const bool swap = orientation >= 5 && orientation <= 8;
    const std::int32_t ow = static_cast<std::int32_t>(swap ? h : w);
    const std::int32_t oh = static_cast<std::int32_t>(swap ? w : h);
    if (!decoded_size_within_limits(ow, oh)) {
        return codec_error::dimension_limit;
    }
    oriented_w_out = ow;
    oriented_h_out = oh;
    return codec_error::none;
}

decode_result decode(const std::uint8_t* data, std::size_t size,
                     int scaling_denominator) noexcept {
    decode_result out;
    std::int32_t src_w = 0;
    std::int32_t src_h = 0;
    // Byte-level header gate first (no COM yet).
    {
        std::int32_t ow = 0;
        std::int32_t oh = 0;
        const codec_error herr = parse_header(data, size, ow, oh);
        if (herr != codec_error::none) {
            out.error = herr;
            return out;
        }
        src_w = ow;
        src_h = oh;
    }

    if (scaling_denominator != 1 && scaling_denominator != 2 &&
        scaling_denominator != 4 && scaling_denominator != 8) {
        scaling_denominator = 1;
    }
    const int den = scaling_denominator;

    // Lazy COM on this worker thread only.
    com_mta_guard com;
    if (!com.usable()) {
        out.error = codec_error::decode_failed;
        out.detail = "png COM initialization failed";
        return out;
    }

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) {
        out.error = codec_error::decode_failed;
        out.detail = "png imaging factory unavailable";
        return out;
    }

    ComPtr<IWICStream> stream;
    hr = factory->CreateStream(&stream);
    if (FAILED(hr) || !stream) {
        out.error = codec_error::decode_failed;
        out.detail = "png stream creation failed";
        return out;
    }
    hr = stream->InitializeFromMemory(const_cast<BYTE*>(data),
                                      static_cast<DWORD>(size));
    if (FAILED(hr)) {
        out.error = codec_error::decode_failed;
        out.detail = "png stream initialization failed";
        return out;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoder(GUID_ContainerFormatPng, nullptr, &decoder);
    if (FAILED(hr) || !decoder) {
        out.error = codec_error::decode_failed;
        out.detail = "png decoder creation failed";
        return out;
    }
    hr = decoder->Initialize(stream.Get(), WICDecodeMetadataCacheOnDemand);
    if (FAILED(hr)) {
        out.error = codec_error::decode_failed;
        out.detail = "png decoder initialization failed";
        return out;
    }

    // Frame 0 only (APNG: default image; no animation).
    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr) || !frame) {
        out.error = codec_error::decode_failed;
        out.detail = "png frame unavailable";
        return out;
    }

    UINT fw = 0;
    UINT fh = 0;
    hr = frame->GetSize(&fw, &fh);
    if (FAILED(hr) || fw == 0 || fh == 0) {
        out.error = codec_error::decode_failed;
        out.detail = "png frame size unavailable";
        return out;
    }
    if (fw > static_cast<UINT>(kMaxImageDimension) ||
        fh > static_cast<UINT>(kMaxImageDimension)) {
        out.error = codec_error::dimension_limit;
        return out;
    }

    // Valid EXIF orientation (WIC metadata when exposed, else the manual
    // eXIf scan); invalid/missing decodes as normal.
    int orientation = read_wic_orientation(frame.Get());
    if (orientation < 1 || orientation > 8) {
        orientation = read_exif_orientation(data, size);
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
    // Prefer the registry header dims when they agree; WIC is authoritative
    // for the pixel pipeline below either way.
    (void)src_w;
    (void)src_h;

    // Scaled stored-space dimensions (TurboJPEG ceil semantics).
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

    // Color pipeline: embedded ICC -> sRGB when WIC accepts it, else a
    // plain pixel-format conversion (explicit sRGB / unprofiled input and
    // invalid profiles all land here safely).
    ComPtr<IWICBitmapSource> color_source;
    {
        ComPtr<IWICColorTransform> transformer;
        bool transformed = false;
        UINT ctx_count = 0;
        if (SUCCEEDED(frame->GetColorContexts(0, nullptr, &ctx_count)) &&
            ctx_count > 0) {
            std::vector<ComPtr<IWICColorContext>> ctx_holder;
            ctx_holder.resize(ctx_count);
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
                    SUCCEEDED(factory->CreateColorTransformer(&transformer)) &&
                    transformer &&
                    SUCCEEDED(transformer->Initialize(
                        frame.Get(), src_ctx.Get(), dst_ctx.Get(),
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
        if (!transformed) {
            ComPtr<IWICFormatConverter> converter;
            hr = factory->CreateFormatConverter(&converter);
            if (FAILED(hr) || !converter) {
                out.error = codec_error::decode_failed;
                out.detail = "png pixel converter unavailable";
                return out;
            }
            hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                       WICBitmapDitherTypeNone, nullptr, 0.0,
                                       WICBitmapPaletteTypeCustom);
            if (FAILED(hr) || FAILED(converter.As(&color_source))) {
                out.error = codec_error::decode_failed;
                out.detail = "png pixel conversion failed";
                return out;
            }
        }
    }

    // WIC scaler to the stored-space scaled size (Fant interpolation).
    ComPtr<IWICBitmapScaler> scaler;
    hr = factory->CreateBitmapScaler(&scaler);
    if (FAILED(hr) || !scaler) {
        out.error = codec_error::decode_failed;
        out.detail = "png scaler unavailable";
        return out;
    }
    hr = scaler->Initialize(color_source.Get(), static_cast<UINT>(suw),
                            static_cast<UINT>(suh), WICBitmapInterpolationModeFant);
    if (FAILED(hr)) {
        out.error = codec_error::decode_failed;
        out.detail = "png scaling failed";
        return out;
    }

    std::int32_t stride = 0;
    std::size_t bytes = 0;
    if (!checked_stride_and_size(suw, suh, stride, bytes)) {
        out.error = codec_error::allocation_limit;
        return out;
    }
    if (bytes > static_cast<std::size_t>(UINT32_MAX)) {
        out.error = codec_error::allocation_limit;
        return out;
    }
    std::vector<std::uint8_t> staged(bytes);
    hr = scaler->CopyPixels(nullptr, static_cast<UINT>(stride),
                            static_cast<UINT>(bytes), staged.data());
    if (FAILED(hr)) {
        out.error = codec_error::decode_failed;
        out.detail = "png pixel copy failed";
        return out;
    }

    decoded_image img;
    img.codec = codec_id::png_wic;
    img.scale_den = den;
    img.src_width = ow;
    img.src_height = oh;
    img.width = final_w;
    img.height = final_h;
    if (!checked_stride_and_size(final_w, final_h, img.stride, bytes)) {
        out.error = codec_error::allocation_limit;
        return out;
    }
    img.pixels.resize(bytes);
    if (orientation == 1) {
        std::memcpy(img.pixels.data(), staged.data(), bytes);
    } else {
        apply_orientation(staged.data(), suw, suh, img.pixels.data(), orientation);
    }
    out.image = std::move(img);
    out.error = codec_error::none;
    return out;
}

}  // namespace oz::viewer_core::png_wic

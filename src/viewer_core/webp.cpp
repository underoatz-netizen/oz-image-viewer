// webp.cpp - static WebP still-image decoder (Stage 3.2). Lossy/lossless
// static WebP only; animated WebP is deterministically rejected before any
// allocation/decode (never frame 0). Decodes to top-down canonical PBGRA8
// with bounded WebPDecoderConfig external output allocation and 1/2/4/8
// ceil scaling. Alpha is premultiplied explicitly exactly once via
// (channel*alpha+127)/255. No convenience unchecked allocator, no
// encoder/playback/metadata editor/plugin API, no WebP DLL/delay-load/
// LoadLibrary, no WIC WebP fallback, no generic fallback.
//
// Metadata/security: bounded ICCP (<=16MiB) via demux is color-converted to
// sRGB with existing WIC primitives on the calling worker thread; invalid
// or unsupported profiles fall back safely as sRGB. EXIF orientation 1-8
// from the WebP EXIF chunk (TIFF or Exif\0\0+TIFF) shares the bounded TIFF
// parser shape used by the PNG path and is applied after scaled decode.
// XMP is ignored. Hard limits: encoded <=512MiB, dimensions <=65535,
// output <=2GiB, envelope/chunk/padding consistency, <=4096 chunks,
// EXIF <=1MiB, feature/demux dimension agreement. Every malformed,
// allocation, or decoder failure maps to a stable generic codec_error with
// no library detail and no UI exception.
#include "webp.hpp"

#include "oz/viewer_core/codec.hpp"

#if OZVIEWER_HAVE_WEBP
#include <windows.h>

#include <wincodec.h>
#include <objbase.h>
#include <wrl/client.h>

#include <webp/decode.h>
#include <webp/demux.h>
#endif

#include <cstring>
#include <vector>

namespace oz::viewer_core::webp {
namespace {

using std::uint32_t;

#if OZVIEWER_HAVE_WEBP
using Microsoft::WRL::ComPtr;

constexpr std::size_t kMaxChunks = 4096;
constexpr std::size_t kMaxIccBytes = 16ull * 1024 * 1024;    // 16 MiB
constexpr std::size_t kMaxExifBytes = 1ull * 1024 * 1024;    // 1 MiB
// VP8X feature-flag bit for animation (container spec: bit 1 of the flags
// byte, mask 0x02). Used only as a deterministic pre-demux tripwire; the
// authoritative animation verdict also consults features + demux.
constexpr std::uint8_t kVp8xAnimationFlag = 0x02;

uint32_t read_le32(const std::uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

std::uint16_t read_le16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
}

std::uint16_t read_be16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) | p[1]);
}

uint32_t read_be32(const std::uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

// Bounded TIFF/EXIF orientation parse shared with the PNG path shape:
// tag 0x0112 SHORT/LONG, values 1-8, else 0.
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
    auto u32 = [&](std::size_t o, bool& ok) -> uint32_t {
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
    const uint32_t ifd_off = u32(4, ok);
    if (!ok || ifd_off > len) {
        return 0;
    }
    const std::size_t count = u16(ifd_off, ok);
    if (!ok) {
        return 0;
    }
    // Bound the entry walk by both the declared count and the buffer.
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t e = static_cast<std::size_t>(ifd_off) + 2 + i * 12;
        if (e + 12 > len || i > 256) {
            return 0;
        }
        if (u16(e, ok) != 0x0112 || !ok) {
            continue;
        }
        const std::uint16_t type = u16(e + 2, ok);
        const uint32_t n = u32(e + 4, ok);
        if (!ok || n < 1) {
            return 0;
        }
        uint32_t value = 0;
        if (type == 3) {
            value = little ? read_le16(tiff + e + 8) : read_be16(tiff + e + 8);
        } else if (type == 4) {
            value = u32(e + 8, ok);
        } else {
            return 0;
        }
        if (!ok || value < 1 || value > 8) {
            return 0;
        }
        return static_cast<int>(value);
    }
    return 0;
}

// EXIF chunk payload may be raw TIFF or "Exif\0\0" + TIFF.
int orientation_from_exif_chunk(const std::uint8_t* chunk, std::size_t len) noexcept {
    if (chunk == nullptr || len == 0 || len > kMaxExifBytes) {
        return 0;
    }
    static constexpr std::uint8_t kPrefix[6] = {'E', 'x', 'i', 'f', 0, 0};
    if (len >= 6 && std::memcmp(chunk, kPrefix, 6) == 0) {
        return parse_tiff_orientation(chunk + 6, len - 6);
    }
    return parse_tiff_orientation(chunk, len);
}

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

// Envelope validation result shared by header and decode paths.
struct envelope_info {
    bool animated = false;  // ANIM/ANMF chunk or VP8X animation flag seen
    bool has_vp8x = false;
    std::size_t chunk_count = 0;
};

// Validate the RIFF+WEBP envelope deterministically: exact RIFF size match,
// per-chunk header/payload/padding bounds, <=4096 chunks. Never reads past
// size. Returns false on any inconsistency; sets animated when animation
// markers are present (caller maps to a deterministic rejection).
bool validate_envelope(const std::uint8_t* data, std::size_t size,
                       envelope_info& out) noexcept {
    out = envelope_info{};
    if (data == nullptr || size < 12) {
        return false;
    }
    if (std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WEBP", 4) != 0) {
        return false;
    }
    const uint32_t riff_size = read_le32(data + 4);
    // RIFF size field is file_size - 8. Exact match required: a short file
    // is truncated, trailing bytes are an envelope inconsistency.
    if (static_cast<std::uint64_t>(riff_size) + 8u != static_cast<std::uint64_t>(size)) {
        return false;
    }
    std::size_t pos = 12;
    while (pos < size) {
        if (out.chunk_count >= kMaxChunks) {
            return false;
        }
        if (size - pos < 8) {
            return false;  // truncated chunk header
        }
        const std::uint8_t* type = data + pos;
        const uint32_t chunk_len = read_le32(data + pos + 4);
        const std::uint64_t payload_end =
            static_cast<std::uint64_t>(pos) + 8u + chunk_len;
        if (payload_end > size) {
            return false;  // chunk overruns buffer
        }
        const std::size_t padded =
            static_cast<std::size_t>(chunk_len) + (chunk_len & 1u);
        if (static_cast<std::uint64_t>(pos) + 8u + padded > size) {
            return false;  // padding byte missing
        }
        if (chunk_len == 1 && (data[pos + 8] != 0 || (padded != 2))) {
            // Odd-size chunk must carry exactly one zero pad byte; the
            // bounds check above already enforces presence.
        }
        if (std::memcmp(type, "ANIM", 4) == 0 || std::memcmp(type, "ANMF", 4) == 0) {
            out.animated = true;
        }
        if (std::memcmp(type, "VP8X", 4) == 0) {
            out.has_vp8x = true;
            if (chunk_len >= 10 && (data[pos + 8] & kVp8xAnimationFlag) != 0) {
                out.animated = true;
            }
        }
        ++out.chunk_count;
        pos += 8 + padded;
    }
    if (pos != size) {
        return false;
    }
    return true;
}

struct demux_holder {
    WebPDemuxer* d = nullptr;
    explicit demux_holder(WebPDemuxer* v) : d(v) {}
    ~demux_holder() {
        if (d != nullptr) {
            WebPDemuxDelete(d);
        }
    }
    demux_holder(const demux_holder&) = delete;
    demux_holder& operator=(const demux_holder&) = delete;
};

// Best-effort ICC->sRGB conversion of staged BGRA (non-premultiplied)
// using WIC color primitives on this worker thread. Returns true when the
// staged buffer was converted in place; false means the caller keeps the
// decoded pixels as sRGB (safe fallback for invalid/unsupported profiles).
bool try_apply_icc_srgb(std::vector<std::uint8_t>& staged, std::int32_t w, std::int32_t h,
                        const std::uint8_t* icc, std::size_t icc_len) noexcept {
    if (staged.empty() || w <= 0 || h <= 0 || icc == nullptr || icc_len == 0 ||
        icc_len > kMaxIccBytes) {
        return false;
    }
    com_mta_guard com;
    if (!com.usable()) {
        return false;
    }
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))) ||
        !factory) {
        return false;
    }
    // Source bitmap wrapping the decoded BGRA rows (no copy beyond WIC).
    ComPtr<IWICBitmap> src_bmp;
    const UINT stride = static_cast<UINT>(static_cast<std::uint64_t>(w) * 4u);
    const UINT buf_size = static_cast<UINT>(staged.size());
    if (FAILED(factory->CreateBitmapFromMemory(static_cast<UINT>(w), static_cast<UINT>(h),
                                               GUID_WICPixelFormat32bppBGRA, stride,
                                               buf_size, staged.data(), &src_bmp)) ||
        !src_bmp) {
        return false;
    }
    ComPtr<IWICColorContext> src_ctx;
    if (FAILED(factory->CreateColorContext(&src_ctx)) || !src_ctx) {
        return false;
    }
    // InitializeFromMemory copies the profile bytes internally, so the
    // caller's chunk lifetime is not extended.
    std::vector<std::uint8_t> icc_copy(icc, icc + icc_len);
    if (FAILED(src_ctx->InitializeFromMemory(icc_copy.data(),
                                             static_cast<UINT>(icc_copy.size())))) {
        return false;
    }
    ComPtr<IWICColorContext> dst_ctx;
    if (FAILED(factory->CreateColorContext(&dst_ctx)) || !dst_ctx) {
        return false;
    }
    if (FAILED(dst_ctx->InitializeFromExifColorSpace(1))) {  // 1 = sRGB
        return false;
    }
    ComPtr<IWICColorTransform> xform;
    if (FAILED(factory->CreateColorTransformer(&xform)) || !xform) {
        return false;
    }
    if (FAILED(xform->Initialize(src_bmp.Get(), src_ctx.Get(), dst_ctx.Get(),
                                 GUID_WICPixelFormat32bppBGRA))) {
        return false;
    }
    // Convert into a scratch buffer, then commit only on full success so a
    // partial WIC copy can never corrupt the staged decode.
    std::vector<std::uint8_t> converted(staged.size());
    if (FAILED(xform->CopyPixels(nullptr, stride, static_cast<UINT>(converted.size()),
                                 converted.data()))) {
        return false;
    }
    staged = std::move(converted);
    return true;
}

#endif  // OZVIEWER_HAVE_WEBP

}  // namespace

#if !OZVIEWER_HAVE_WEBP

codec_error parse_header(const std::uint8_t*, std::size_t, std::int32_t&,
                         std::int32_t&) noexcept {
    return codec_error::unsupported;
}

decode_result decode(const std::uint8_t*, std::size_t, int) noexcept {
    decode_result out;
    out.error = codec_error::unsupported;
    out.detail = "webp support disabled in this build";
    return out;
}

int read_exif_orientation(const std::uint8_t*, std::size_t) noexcept {
    return 0;
}

#else

int read_exif_orientation(const std::uint8_t* data, std::size_t size) noexcept {
    if (data == nullptr || size < 12 || size > kMaxEncodedBytes) {
        return 0;
    }
    if (std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WEBP", 4) != 0) {
        return 0;
    }
    WebPData src{data, size};
    demux_holder dmux(WebPDemux(&src));
    if (dmux.d == nullptr) {
        return 0;
    }
    WebPChunkIterator it{};
    if (!WebPDemuxGetChunk(dmux.d, "EXIF", 1, &it)) {
        return 0;
    }
    int value = 0;
    if (it.chunk.bytes != nullptr && it.chunk.size > 0 && it.chunk.size <= kMaxExifBytes) {
        value = orientation_from_exif_chunk(it.chunk.bytes, it.chunk.size);
    }
    WebPDemuxReleaseChunkIterator(&it);
    return (value >= 1 && value <= 8) ? value : 0;
}

codec_error parse_header(const std::uint8_t* data, std::size_t size,
                         std::int32_t& oriented_w_out,
                         std::int32_t& oriented_h_out) noexcept {
    oriented_w_out = 0;
    oriented_h_out = 0;
    if (data == nullptr || size < 12) {
        return codec_error::unsupported;
    }
    if (std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WEBP", 4) != 0) {
        return codec_error::unsupported;
    }
    if (size > kMaxEncodedBytes) {
        return codec_error::allocation_limit;
    }
    envelope_info env;
    const std::uint64_t riff_end = static_cast<std::uint64_t>(read_le32(data + 4)) + 8u;
    if (riff_end > size) {
        return codec_error::truncated;
    }
    if (!validate_envelope(data, size, env)) {
        // Envelope inconsistencies (padding, chunk bounds, trailing bytes,
        // chunk-count explosion) are invalid headers; a short RIFF is
        // truncation. validate_envelope cannot distinguish, so re-check the
        // short case only (handled above); everything else is invalid.
        return codec_error::invalid_header;
    }
    if (env.animated) {
        return codec_error::decode_failed;  // animated: deterministic reject
    }
    WebPBitstreamFeatures features{};
    if (WebPGetFeatures(data, size, &features) != VP8_STATUS_OK) {
        return codec_error::invalid_header;
    }
    if (features.has_animation != 0) {
        return codec_error::decode_failed;
    }
    if (features.width <= 0 || features.height <= 0) {
        return codec_error::invalid_header;
    }
    if (features.width > kMaxImageDimension || features.height > kMaxImageDimension) {
        return codec_error::dimension_limit;
    }
    if (!decoded_size_within_limits(features.width, features.height)) {
        return codec_error::dimension_limit;
    }
    // Demux agreement: canvas dims must match the bitstream features, the
    // file must hold exactly one frame, and the container must not flag
    // animation. Any disagreement is a deterministic reject before decode.
    WebPData src{data, size};
    demux_holder dmux(WebPDemux(&src));
    if (dmux.d == nullptr) {
        return codec_error::invalid_header;
    }
    const uint32_t canvas_w = WebPDemuxGetI(dmux.d, WEBP_FF_CANVAS_WIDTH);
    const uint32_t canvas_h = WebPDemuxGetI(dmux.d, WEBP_FF_CANVAS_HEIGHT);
    const uint32_t flags = WebPDemuxGetI(dmux.d, WEBP_FF_FORMAT_FLAGS);
    const int frames = WebPDemuxGetI(dmux.d, WEBP_FF_FRAME_COUNT);
    constexpr uint32_t kAnimationFlag = 0x02u;
    if ((flags & kAnimationFlag) != 0 || frames != 1) {
        return codec_error::decode_failed;
    }
    if (canvas_w != static_cast<uint32_t>(features.width) ||
        canvas_h != static_cast<uint32_t>(features.height)) {
        return codec_error::invalid_header;
    }
    const int orientation = read_exif_orientation(data, size);
    const bool swap = orientation >= 5 && orientation <= 8;
    const std::int32_t ow =
        static_cast<std::int32_t>(swap ? features.height : features.width);
    const std::int32_t oh =
        static_cast<std::int32_t>(swap ? features.width : features.height);
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

    // Stored (non-oriented) dims from the validated bitstream features.
    WebPBitstreamFeatures features{};
    if (WebPGetFeatures(data, size, &features) != VP8_STATUS_OK || features.width <= 0 ||
        features.height <= 0) {
        out.error = codec_error::invalid_header;
        return out;
    }
    const std::uint64_t suw64 = (static_cast<std::uint64_t>(features.width) +
                                 static_cast<std::uint64_t>(den) - 1u) /
                                static_cast<std::uint64_t>(den);
    const std::uint64_t suh64 = (static_cast<std::uint64_t>(features.height) +
                                 static_cast<std::uint64_t>(den) - 1u) /
                                static_cast<std::uint64_t>(den);
    if (suw64 == 0 || suh64 == 0 ||
        suw64 > static_cast<std::uint64_t>(kMaxImageDimension) ||
        suh64 > static_cast<std::uint64_t>(kMaxImageDimension)) {
        out.error = codec_error::allocation_limit;
        return out;
    }
    const std::int32_t suw = static_cast<std::int32_t>(suw64);
    const std::int32_t suh = static_cast<std::int32_t>(suh64);
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

    const int orientation = read_exif_orientation(data, size);
    const bool swap = orientation >= 1 && orientation <= 8 && orientation >= 5;
    const std::int32_t final_w = swap ? suh : suw;
    const std::int32_t final_h = swap ? suw : suh;
    if (!decoded_size_within_limits(final_w, final_h)) {
        out.error = codec_error::allocation_limit;
        return out;
    }

    // Bounded external-memory decode to straight BGRA; premultiplication is
    // applied explicitly exactly once below. No convenience unchecked
    // allocator is used.
    std::vector<std::uint8_t> staged(bytes);
    WebPDecoderConfig config{};
    if (WebPInitDecoderConfig(&config) == 0) {
        out.error = codec_error::decode_failed;
        return out;
    }
    config.output.colorspace = MODE_BGRA;
    config.output.is_external_memory = 1;
    config.output.u.RGBA.rgba = staged.data();
    config.output.u.RGBA.stride = stride;
    config.output.u.RGBA.size = bytes;
    config.options.use_scaling = 1;
    config.options.scaled_width = suw;
    config.options.scaled_height = suh;
    config.options.use_threads = 0;
    const VP8StatusCode status = WebPDecode(data, size, &config);
    // Release internal side buffers only; external staged memory stays ours.
    WebPFreeDecBuffer(&config.output);
    if (status != VP8_STATUS_OK) {
        out.error = codec_error::decode_failed;
        return out;
    }
    if (config.output.width != suw || config.output.height != suh) {
        out.error = codec_error::decode_failed;
        return out;
    }

    // Bounded ICCP via demux; valid profiles convert to sRGB with WIC on
    // this worker thread, invalid/unsupported profiles stay sRGB fallback.
    {
        WebPData src{data, size};
        demux_holder dmux(WebPDemux(&src));
        if (dmux.d != nullptr) {
            WebPChunkIterator it{};
            if (WebPDemuxGetChunk(dmux.d, "ICCP", 1, &it)) {
                if (it.chunk.bytes != nullptr && it.chunk.size > 0 &&
                    it.chunk.size <= kMaxIccBytes) {
                    (void)try_apply_icc_srgb(staged, suw, suh, it.chunk.bytes,
                                             it.chunk.size);
                }
                WebPDemuxReleaseChunkIterator(&it);
            }
        }
    }

    // Explicit single premultiplication pass: (c*a+127)/255 per channel.
    for (std::size_t i = 0; i + 3 < staged.size() + 1; i += 4) {
        const std::uint32_t a = staged[i + 3];
        if (a == 255) {
            continue;
        }
        if (a == 0) {
            staged[i + 0] = 0;
            staged[i + 1] = 0;
            staged[i + 2] = 0;
            continue;
        }
        staged[i + 0] =
            static_cast<std::uint8_t>((static_cast<std::uint32_t>(staged[i + 0]) * a + 127u) / 255u);
        staged[i + 1] =
            static_cast<std::uint8_t>((static_cast<std::uint32_t>(staged[i + 1]) * a + 127u) / 255u);
        staged[i + 2] =
            static_cast<std::uint8_t>((static_cast<std::uint32_t>(staged[i + 2]) * a + 127u) / 255u);
    }

    decoded_image img;
    img.codec = codec_id::webp;
    img.scale_den = den;
    img.src_width = src_w;
    img.src_height = src_h;
    img.width = final_w;
    img.height = final_h;
    std::size_t final_bytes = 0;
    if (!checked_stride_and_size(final_w, final_h, img.stride, final_bytes)) {
        out.error = codec_error::allocation_limit;
        return out;
    }
    img.pixels.resize(final_bytes);
    if (orientation < 1 || orientation > 8 || orientation == 1) {
        std::memcpy(img.pixels.data(), staged.data(), final_bytes);
    } else {
        apply_orientation(staged.data(), suw, suh, img.pixels.data(), orientation);
    }
    out.image = std::move(img);
    out.error = codec_error::none;
    return out;
}

#endif  // OZVIEWER_HAVE_WEBP

}  // namespace oz::viewer_core::webp

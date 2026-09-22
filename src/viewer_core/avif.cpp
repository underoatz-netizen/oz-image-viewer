// avif.cpp - static AVIF still-image decoder (Stage 3.3). 8-bit SDR sRGB
// static AVIF still images only (alpha allowed). Rejects avis/sequences/
// animations, multiple top-level images, bursts/live photos, auxiliary/depth/
// gainmap, 10/12-bit, HDR/PQ/HLG, unsupported/wide-gamut color, and EXIF/container
// transform conflicts.
//
// Decode path: explicitly selects dav1d decoder, bounded thread count (<=4),
// checked parser/allocation/workset. Output is canonical top-down BGRA8 then
// exactly once PBGRA (c*a+127)/255, with RGB zeroed if alpha is 0.
// Transform contract: irot/imir applied exactly once; reject conflict between
// container transform and non-identity EXIF orientation.
#include "avif.hpp"

#include "oz/viewer_core/codec.hpp"
#include "isobmff.hpp"
#include "exif_orientation.hpp"

#if OZVIEWER_HAVE_AVIF
#include <windows.h>

#include <wincodec.h>
#include <objbase.h>
#include <wrl/client.h>

#include <avif/avif.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

namespace oz::viewer_core::avif {
namespace {

std::atomic<std::uint64_t> g_avif_decoder_creations{0};

using std::uint32_t;
using std::uint64_t;

uint32_t read_be32(const std::uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

uint64_t read_be64(const std::uint8_t* p) noexcept {
    return (static_cast<uint64_t>(read_be32(p)) << 32) |
           static_cast<uint64_t>(read_be32(p + 4));
}

#if OZVIEWER_HAVE_AVIF
using Microsoft::WRL::ComPtr;

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

bool try_apply_icc_srgb(std::vector<std::uint8_t>& staged, std::int32_t w, std::int32_t h,
                        const std::uint8_t* icc, std::size_t icc_len) noexcept {
    if (staged.empty() || w <= 0 || h <= 0 || icc == nullptr || icc_len == 0 ||
        icc_len > 16ull * 1024 * 1024) {
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
    std::vector<std::uint8_t> converted(staged.size());
    if (FAILED(xform->CopyPixels(nullptr, stride, static_cast<UINT>(converted.size()),
                                 converted.data()))) {
        return false;
    }
    staged = std::move(converted);
    return true;
}

struct avif_decoder_cleaner {
    avifDecoder* dec = nullptr;
    explicit avif_decoder_cleaner(avifDecoder* d) : dec(d) {}
    ~avif_decoder_cleaner() {
        if (dec != nullptr) {
            avifDecoderDestroy(dec);
        }
    }
    avif_decoder_cleaner(const avif_decoder_cleaner&) = delete;
    avif_decoder_cleaner& operator=(const avif_decoder_cleaner&) = delete;
};

#endif  // OZVIEWER_HAVE_AVIF

}  // namespace

bool has_avif_signature(const std::uint8_t* data, std::size_t size) noexcept {
    return isobmff::has_avif_signature(data, size);
}

#if !OZVIEWER_HAVE_AVIF

codec_error parse_header(const std::uint8_t*, std::size_t, std::int32_t&,
                         std::int32_t&) noexcept {
    return codec_error::unsupported;
}

decode_result decode(const std::uint8_t*, std::size_t, int) noexcept {
    decode_result out;
    out.error = codec_error::unsupported;
    out.detail = "avif support disabled in this build";
    return out;
}

int read_exif_orientation(const std::uint8_t*, std::size_t) noexcept {
    return 0;
}

#else

int read_exif_orientation(const std::uint8_t* data, std::size_t size) noexcept {
    if (!has_avif_signature(data, size) || size > kMaxEncodedBytes) {
        return 0;
    }
    avifDecoder* dec = avifDecoderCreate();
    if (dec == nullptr) {
        return 0;
    }
    g_avif_decoder_creations.fetch_add(1, std::memory_order_relaxed);
    avif_decoder_cleaner cleaner(dec);
    dec->codecChoice = AVIF_CODEC_CHOICE_DAV1D;
    dec->maxThreads = 1;
    dec->imageSizeLimit = AVIF_DEFAULT_IMAGE_SIZE_LIMIT;
    dec->imageDimensionLimit = kMaxImageDimension;
    dec->imageCountLimit = 1;
    dec->ignoreXMP = AVIF_TRUE;

    if (avifDecoderSetIOMemory(dec, data, size) != AVIF_RESULT_OK) {
        return 0;
    }
    if (avifDecoderParse(dec) != AVIF_RESULT_OK) {
        return 0;
    }
    if (dec->image == nullptr || dec->image->exif.size == 0 || dec->image->exif.data == nullptr) {
        return 0;
    }
    return parse_exif_orientation_payload(dec->image->exif.data, dec->image->exif.size);
}

codec_error parse_header(const std::uint8_t* data, std::size_t size,
                         std::int32_t& oriented_w_out,
                         std::int32_t& oriented_h_out) noexcept {
    oriented_w_out = 0;
    oriented_h_out = 0;
    if (data == nullptr || size < 12) {
        return codec_error::unsupported;
    }
    if (!has_avif_signature(data, size)) {
        return codec_error::unsupported;
    }
    if (size > kMaxEncodedBytes) {
        return codec_error::allocation_limit;
    }

    avifDecoder* dec = avifDecoderCreate();
    if (dec == nullptr) {
        return codec_error::allocation_limit;
    }
    g_avif_decoder_creations.fetch_add(1, std::memory_order_relaxed);
    avif_decoder_cleaner cleaner(dec);

    dec->codecChoice = AVIF_CODEC_CHOICE_DAV1D;
    dec->maxThreads = 1;
    dec->imageSizeLimit = AVIF_DEFAULT_IMAGE_SIZE_LIMIT;
    dec->imageDimensionLimit = kMaxImageDimension;
    dec->imageCountLimit = 1;
    dec->strictFlags = AVIF_STRICT_ENABLED;

    if (avifDecoderSetIOMemory(dec, data, size) != AVIF_RESULT_OK) {
        return codec_error::invalid_header;
    }

    const avifResult parse_res = avifDecoderParse(dec);
    if (parse_res != AVIF_RESULT_OK) {
        if (parse_res == AVIF_RESULT_BMFF_PARSE_FAILED || parse_res == AVIF_RESULT_TRUNCATED_DATA) {
            return codec_error::truncated;
        }
        return codec_error::invalid_header;
    }

    // Enforce static image policy: exactly 1 image, no image sequence track
    if (dec->imageSequenceTrackPresent || dec->imageCount != 1) {
        return codec_error::decode_failed;
    }
    if (dec->progressiveState == AVIF_PROGRESSIVE_STATE_ACTIVE) {
        return codec_error::decode_failed;
    }
    if (dec->image == nullptr) {
        return codec_error::invalid_header;
    }

    const uint32_t raw_w = dec->image->width;
    const uint32_t raw_h = dec->image->height;
    if (raw_w == 0 || raw_h == 0) {
        return codec_error::invalid_header;
    }
    if (raw_w > static_cast<uint32_t>(kMaxImageDimension) ||
        raw_h > static_cast<uint32_t>(kMaxImageDimension)) {
        return codec_error::dimension_limit;
    }
    if (!decoded_size_within_limits(static_cast<std::int32_t>(raw_w),
                                    static_cast<std::int32_t>(raw_h))) {
        return codec_error::dimension_limit;
    }

    // SDR 8-bit contract: reject 10/12-bit
    if (dec->image->depth != 8) {
        return codec_error::unsupported;
    }

    // Policy: reject auxiliary / gainMap
    if (dec->image->gainMap != nullptr) {
        return codec_error::unsupported;
    }

    // Color/CICP policy:
    // If ICC is present: bounded <=16 MiB.
    // If no ICC: check CICP colorPrimaries and transferCharacteristics.
    if (dec->image->icc.size > 0) {
        if (dec->image->icc.size > 16ull * 1024 * 1024) {
            return codec_error::unsupported;
        }
    } else {
        const avifColorPrimaries prim = dec->image->colorPrimaries;
        const avifTransferCharacteristics trc = dec->image->transferCharacteristics;
        // Primaries: BT709 / sRGB (1), or unspecified (2)
        if (prim != AVIF_COLOR_PRIMARIES_BT709 && prim != AVIF_COLOR_PRIMARIES_UNSPECIFIED) {
            return codec_error::unsupported;
        }
        // Matrix: BT709 (1), unspecified (2), IEC61966_2_1/sRGB (13), BT601 (6), identity/RGB (0)
        const avifMatrixCoefficients mc = dec->image->matrixCoefficients;
        if (mc != AVIF_MATRIX_COEFFICIENTS_BT709 &&
            mc != AVIF_MATRIX_COEFFICIENTS_UNSPECIFIED &&
            mc != AVIF_MATRIX_COEFFICIENTS_BT601 &&
            mc != AVIF_MATRIX_COEFFICIENTS_IDENTITY &&
            mc != 13) {
            return codec_error::unsupported;
        }
        // Transfer: BT709 (1), unspecified (2), IEC61966_2_1/sRGB (13)
        // Reject PQ (16), HLG (18), SMPTE428 (17), etc.
        if (trc != AVIF_TRANSFER_CHARACTERISTICS_BT709 &&
            trc != AVIF_TRANSFER_CHARACTERISTICS_UNSPECIFIED &&
            trc != AVIF_TRANSFER_CHARACTERISTICS_SRGB) {
            return codec_error::unsupported;
        }
    }

    // Container transform flags (irot / imir)
    const bool has_irot = (dec->image->transformFlags & AVIF_TRANSFORM_IROT) != 0;
    const bool has_imir = (dec->image->transformFlags & AVIF_TRANSFORM_IMIR) != 0;
    const uint8_t irot_val = dec->image->irot.angle;
    const uint8_t imir_val = dec->image->imir.axis;

    int container_op = 1;
    if (has_irot || has_imir) {
        container_op = compose_irot_imir(has_irot, irot_val, has_imir, imir_val);
    }

    // EXIF orientation
    int exif_op = 0;
    if (dec->image->exif.size > 0 && dec->image->exif.data != nullptr) {
        exif_op = parse_exif_orientation_payload(dec->image->exif.data, dec->image->exif.size);
    }

    // Reject conflict between container transform and non-identity EXIF orientation
    if ((has_irot || has_imir) && (exif_op > 1 && exif_op != container_op)) {
        return codec_error::invalid_header;
    }

    const int final_op = (has_irot || has_imir) ? container_op : ((exif_op >= 1 && exif_op <= 8) ? exif_op : 1);
    const bool swap = (final_op >= 5 && final_op <= 8);

    const std::int32_t ow = static_cast<std::int32_t>(swap ? raw_h : raw_w);
    const std::int32_t oh = static_cast<std::int32_t>(swap ? raw_w : raw_h);

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
    const codec_error herr = parse_header(data, size, src_w, src_h);
    if (herr != codec_error::none) {
        out.error = herr;
        return out;
    }

    if (scaling_denominator != 1 && scaling_denominator != 2 &&
        scaling_denominator != 4 && scaling_denominator != 8) {
        scaling_denominator = 1;
    }
    const int den = scaling_denominator;

    avifDecoder* dec = avifDecoderCreate();
    if (dec == nullptr) {
        out.error = codec_error::allocation_limit;
        return out;
    }
    g_avif_decoder_creations.fetch_add(1, std::memory_order_relaxed);
    avif_decoder_cleaner cleaner(dec);

    dec->codecChoice = AVIF_CODEC_CHOICE_DAV1D;
    // Bounded worker thread count: max 4
    dec->maxThreads = std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1, 4);
    dec->imageSizeLimit = AVIF_DEFAULT_IMAGE_SIZE_LIMIT;
    dec->imageDimensionLimit = kMaxImageDimension;
    dec->imageCountLimit = 1;
    dec->strictFlags = AVIF_STRICT_ENABLED;

    if (avifDecoderSetIOMemory(dec, data, size) != AVIF_RESULT_OK) {
        out.error = codec_error::invalid_header;
        return out;
    }
    if (avifDecoderParse(dec) != AVIF_RESULT_OK) {
        out.error = codec_error::invalid_header;
        return out;
    }
    if (avifDecoderNextImage(dec) != AVIF_RESULT_OK) {
        out.error = codec_error::decode_failed;
        return out;
    }
    if (dec->image == nullptr) {
        out.error = codec_error::decode_failed;
        return out;
    }

    const uint32_t orig_w = dec->image->width;
    const uint32_t orig_h = dec->image->height;
    if (orig_w == 0 || orig_h == 0) {
        out.error = codec_error::invalid_header;
        return out;
    }

    // Ceil scaling calculations for intermediate scaled dimensions
    const std::uint64_t suw64 = (static_cast<std::uint64_t>(orig_w) +
                                 static_cast<std::uint64_t>(den) - 1u) /
                                static_cast<std::uint64_t>(den);
    const std::uint64_t suh64 = (static_cast<std::uint64_t>(orig_h) +
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

    // Apply scaling if den > 1
    if (den > 1) {
        avifDiagnostics diag{};
        if (avifImageScale(dec->image, static_cast<uint32_t>(suw),
                           static_cast<uint32_t>(suh), &diag) != AVIF_RESULT_OK) {
            out.error = codec_error::decode_failed;
            return out;
        }
    }

    std::int32_t staged_stride = 0;
    std::size_t staged_bytes = 0;
    if (!checked_stride_and_size(suw, suh, staged_stride, staged_bytes)) {
        out.error = codec_error::allocation_limit;
        return out;
    }

    std::vector<std::uint8_t> staged(staged_bytes);

    // Convert YUV to RGB using libavif (libyuv backed) to straight BGRA8
    avifRGBImage rgb{};
    avifRGBImageSetDefaults(&rgb, dec->image);
    rgb.format = AVIF_RGB_FORMAT_BGRA;
    rgb.depth = 8;
    rgb.pixels = staged.data();
    rgb.rowBytes = static_cast<uint32_t>(staged_stride);
    rgb.alphaPremultiplied = AVIF_FALSE;  // We will premultiply explicitly once
    rgb.maxThreads = dec->maxThreads;

    if (avifImageYUVToRGB(dec->image, &rgb) != AVIF_RESULT_OK) {
        out.error = codec_error::decode_failed;
        return out;
    }

    // Optional ICC -> sRGB transform via WIC
    if (dec->image->icc.size > 0 && dec->image->icc.data != nullptr) {
        (void)try_apply_icc_srgb(staged, suw, suh, dec->image->icc.data, dec->image->icc.size);
    }

    // Apply premultiplication exactly once: (c * a + 127) / 255; zero RGB if a == 0
    for (std::size_t i = 0; i + 3 < staged.size(); i += 4) {
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
        staged[i + 0] = static_cast<std::uint8_t>((static_cast<std::uint32_t>(staged[i + 0]) * a + 127u) / 255u);
        staged[i + 1] = static_cast<std::uint8_t>((static_cast<std::uint32_t>(staged[i + 1]) * a + 127u) / 255u);
        staged[i + 2] = static_cast<std::uint8_t>((static_cast<std::uint32_t>(staged[i + 2]) * a + 127u) / 255u);
    }

    // Container transform flags (irot / imir)
    const bool has_irot = (dec->image->transformFlags & AVIF_TRANSFORM_IROT) != 0;
    const bool has_imir = (dec->image->transformFlags & AVIF_TRANSFORM_IMIR) != 0;
    const uint8_t irot_val = dec->image->irot.angle;
    const uint8_t imir_val = dec->image->imir.axis;

    int container_op = 1;
    if (has_irot || has_imir) {
        container_op = compose_irot_imir(has_irot, irot_val, has_imir, imir_val);
    }

    int exif_op = 0;
    if (dec->image->exif.size > 0 && dec->image->exif.data != nullptr) {
        exif_op = parse_exif_orientation_payload(dec->image->exif.data, dec->image->exif.size);
    }

    const int final_op = (has_irot || has_imir) ? container_op : ((exif_op >= 1 && exif_op <= 8) ? exif_op : 1);
    const bool swap = (final_op >= 5 && final_op <= 8);

    const std::int32_t final_w = swap ? suh : suw;
    const std::int32_t final_h = swap ? suw : suh;

    std::int32_t final_stride = 0;
    std::size_t final_bytes = 0;
    if (!checked_stride_and_size(final_w, final_h, final_stride, final_bytes)) {
        out.error = codec_error::allocation_limit;
        return out;
    }

    decoded_image img;
    img.codec = codec_id::avif;
    img.scale_den = den;
    img.src_width = src_w;
    img.src_height = src_h;
    img.width = final_w;
    img.height = final_h;
    img.stride = final_stride;
    img.pixels.resize(final_bytes);

    if (final_op <= 1) {
        std::memcpy(img.pixels.data(), staged.data(), final_bytes);
    } else {
        apply_transform_geometry(staged.data(), suw, suh, img.pixels.data(), final_op);
    }

    out.image = std::move(img);
    out.error = codec_error::none;
    return out;
}

#endif  // OZVIEWER_HAVE_AVIF

std::uint64_t decoder_invocation_count() noexcept {
    return g_avif_decoder_creations.load(std::memory_order_relaxed);
}

void reset_decoder_invocation_count() noexcept {
    g_avif_decoder_creations.store(0, std::memory_order_relaxed);
}

}  // namespace oz::viewer_core::avif

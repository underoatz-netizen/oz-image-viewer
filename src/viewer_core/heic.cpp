// heic.cpp - dynamically loaded HEIC still-image decoder (Stage 3.4).
// 8-bit SDR sRGB static HEIC still images only (alpha allowed). Rejects sequences/
// animations/bursts/live photos, multiple top-level images, auxiliary/depth/gainmap,
// 10/12-bit, HDR/PQ/HLG, unsupported/wide-gamut color, and transform conflicts.
//
// Decode path: hardened lazy DLL loader (heif.dll + libde265.dll), bounded threads (<=4),
// checked parser/allocation/workset. Output is canonical top-down BGRA8 then
// exactly once PBGRA (c*a+127)/255, with RGB zeroed if alpha is 0.
// Container transformations disabled in libheif; parsed via irot/imir and EXIF orientation;
// single composed transform applied in OZ code.
#include "heic.hpp"

#include "heif_loader.hpp"
#include "isobmff.hpp"
#include "exif_orientation.hpp"

#if OZVIEWER_HAVE_HEIC
#include <windows.h>
#include <wincodec.h>
#include <objbase.h>
#include <wrl/client.h>
#include <libheif/heif.h>
#endif

#include <algorithm>
#include <cstring>
#include <thread>
#include <vector>

namespace oz::viewer_core::heic {
namespace {

#if OZVIEWER_HAVE_HEIC
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

struct context_cleaner {
    const heif_loader::libheif_api* api = nullptr;
    heif_context* ctx = nullptr;
    context_cleaner(const heif_loader::libheif_api* a, heif_context* c) : api(a), ctx(c) {}
    ~context_cleaner() {
        if (api && ctx) {
            api->context_free(ctx);
        }
    }
    context_cleaner(const context_cleaner&) = delete;
    context_cleaner& operator=(const context_cleaner&) = delete;
};

struct handle_cleaner {
    const heif_loader::libheif_api* api = nullptr;
    heif_image_handle* handle = nullptr;
    handle_cleaner(const heif_loader::libheif_api* a, heif_image_handle* h) : api(a), handle(h) {}
    ~handle_cleaner() {
        if (api && handle) {
            api->image_handle_release(handle);
        }
    }
    handle_cleaner(const handle_cleaner&) = delete;
    handle_cleaner& operator=(const handle_cleaner&) = delete;
};

struct image_cleaner {
    const heif_loader::libheif_api* api = nullptr;
    heif_image* img = nullptr;
    image_cleaner(const heif_loader::libheif_api* a, heif_image* i) : api(a), img(i) {}
    ~image_cleaner() {
        if (api && img) {
            api->image_release(img);
        }
    }
    image_cleaner(const image_cleaner&) = delete;
    image_cleaner& operator=(const image_cleaner&) = delete;
};

struct dec_options_cleaner {
    const heif_loader::libheif_api* api = nullptr;
    heif_decoding_options* opt = nullptr;
    dec_options_cleaner(const heif_loader::libheif_api* a, heif_decoding_options* o) : api(a), opt(o) {}
    ~dec_options_cleaner() {
        if (api && opt) {
            api->decoding_options_free(opt);
        }
    }
    dec_options_cleaner(const dec_options_cleaner&) = delete;
    dec_options_cleaner& operator=(const dec_options_cleaner&) = delete;
};

#endif // OZVIEWER_HAVE_HEIC

}  // namespace

bool has_heic_signature(const std::uint8_t* data, std::size_t size) noexcept {
    return isobmff::has_heic_signature(data, size);
}

#if !OZVIEWER_HAVE_HEIC

codec_error parse_header(const std::uint8_t*, std::size_t, std::int32_t&,
                         std::int32_t&) noexcept {
    return codec_error::unsupported;
}

decode_result decode(const std::uint8_t*, std::size_t, int) noexcept {
    decode_result out;
    out.error = codec_error::unsupported;
    out.detail = "heic support disabled in this build";
    return out;
}

int read_exif_orientation(const std::uint8_t*, std::size_t) noexcept {
    return 0;
}

#else

int read_exif_orientation(const std::uint8_t* data, std::size_t size) noexcept {
    if (!has_heic_signature(data, size) || size > kMaxEncodedBytes) {
        return 0;
    }
    const auto* api = heif_loader::get_api();
    if (api == nullptr) {
        return 0;
    }

    heif_context* ctx = api->context_alloc();
    if (ctx == nullptr) {
        return 0;
    }
    context_cleaner ctx_cl(api, ctx);

    heif_error err = api->context_read_from_memory_without_copy(ctx, data, size, nullptr);
    if (err.code != heif_error_Ok) {
        return 0;
    }

    heif_image_handle* handle = nullptr;
    err = api->context_get_primary_image_handle(ctx, &handle);
    if (err.code != heif_error_Ok || handle == nullptr) {
        return 0;
    }
    handle_cleaner handle_cl(api, handle);

    const int exif_count = api->image_handle_get_number_of_metadata_blocks(handle, "Exif");
    if (exif_count <= 0) {
        return 0;
    }

    std::vector<uint32_t> ids(static_cast<std::size_t>(exif_count));
    int actual_ids = api->image_handle_get_list_of_metadata_block_IDs(handle, "Exif", ids.data(), exif_count);
    if (actual_ids <= 0) {
        return 0;
    }

    const std::size_t exif_size = api->image_handle_get_metadata_size(handle, ids[0]);
    if (exif_size == 0 || exif_size > 1024 * 1024) {
        return 0;
    }

    std::vector<std::uint8_t> exif_buf(exif_size);
    err = api->image_handle_get_metadata(handle, ids[0], exif_buf.data());
    if (err.code != heif_error_Ok) {
        return 0;
    }

    return parse_exif_orientation_payload(exif_buf.data(), exif_size);
}

codec_error parse_header(const std::uint8_t* data, std::size_t size,
                         std::int32_t& oriented_w_out,
                         std::int32_t& oriented_h_out) noexcept {
    oriented_w_out = 0;
    oriented_h_out = 0;
    if (data == nullptr || size < 12) {
        return codec_error::unsupported;
    }
    if (!has_heic_signature(data, size)) {
        return codec_error::unsupported;
    }
    if (size > kMaxEncodedBytes) {
        return codec_error::allocation_limit;
    }

    const auto* api = heif_loader::get_api();
    if (api == nullptr) {
        // DLLs absent or incompatible -> unsupported
        return codec_error::unsupported;
    }

    heif_context* ctx = api->context_alloc();
    if (ctx == nullptr) {
        return codec_error::allocation_limit;
    }
    context_cleaner ctx_cl(api, ctx);

    api->context_set_max_decoding_threads(ctx, 1);

    heif_error err = api->context_read_from_memory_without_copy(ctx, data, size, nullptr);
    if (err.code != heif_error_Ok) {
        if (err.code == heif_error_Invalid_input) {
            return codec_error::invalid_header;
        }
        return codec_error::truncated;
    }

    // Static-only: reject sequences, animations
    if (api->context_has_sequence(ctx)) {
        return codec_error::decode_failed;
    }

    // Exactly one top-level primary image
    const int top_level_count = api->context_get_number_of_top_level_images(ctx);
    if (top_level_count != 1) {
        return codec_error::decode_failed;
    }

    heif_image_handle* handle = nullptr;
    err = api->context_get_primary_image_handle(ctx, &handle);
    if (err.code != heif_error_Ok || handle == nullptr) {
        return codec_error::invalid_header;
    }
    handle_cleaner handle_cl(api, handle);

    // Reject auxiliary/depth images (allow alpha auxiliary image)
    if (api->image_handle_has_depth_image(handle)) {
        return codec_error::unsupported;
    }
    // LIBHEIF_AUX_IMAGE_FILTER_OMIT_ALPHA = (1UL << 1) = 2 omits alpha from auxiliary count
    if (api->image_handle_get_number_of_auxiliary_images(handle, 2) > 0) {
        return codec_error::unsupported;
    }

    // Check bit depth: SDR 8-bit only
    const int luma_bits = api->image_handle_get_luma_bits_per_pixel(handle);
    const int chroma_bits = api->image_handle_get_chroma_bits_per_pixel(handle);
    if (luma_bits != 8 || (chroma_bits != 8 && chroma_bits != 0)) {
        return codec_error::unsupported;
    }

    // Color/profile validation:
    // If raw color profile (ICC) present: check bound <= 16 MiB
    const std::size_t raw_profile_size = api->image_handle_get_raw_color_profile_size(handle);
    if (raw_profile_size > 0) {
        if (raw_profile_size > 16ull * 1024 * 1024) {
            return codec_error::unsupported;
        }
    } else {
        // Check NCLX color profile if present
        heif_color_profile_nclx* nclx = nullptr;
        err = api->image_handle_get_nclx_color_profile(handle, &nclx);
        if (err.code == heif_error_Ok && nclx != nullptr) {
            const auto prim = nclx->color_primaries;
            const auto trc = nclx->transfer_characteristics;
            const auto mc = nclx->matrix_coefficients;
            api->nclx_color_profile_free(nclx);

            // Primaries: BT.709 (1) or unspecified (2)
            if (prim != heif_color_primaries_ITU_R_BT_709_5 && prim != heif_color_primaries_unspecified) {
                return codec_error::unsupported;
            }
            // Transfer: BT.709 (1), unspecified (2), IEC61966-2-1/sRGB (13)
            // Reject PQ (16), HLG (18)
            if (trc != heif_transfer_characteristic_ITU_R_BT_709_5 &&
                trc != heif_transfer_characteristic_unspecified &&
                trc != heif_transfer_characteristic_IEC_61966_2_1) {
                return codec_error::unsupported;
            }
            // Matrix: BT.709 (1), unspecified (2), BT.601 (6), IEC61966-2-1 (13), identity/RGB (0)
            if (mc != heif_matrix_coefficients_ITU_R_BT_709_5 &&
                mc != heif_matrix_coefficients_unspecified &&
                mc != heif_matrix_coefficients_ITU_R_BT_601_6 &&
                mc != heif_matrix_coefficients_RGB_GBR &&
                mc != 13) {
                return codec_error::unsupported;
            }
        }
    }

    const int raw_w = api->image_handle_get_ispe_width(handle);
    const int raw_h = api->image_handle_get_ispe_height(handle);
    if (raw_w <= 0 || raw_h <= 0) {
        return codec_error::invalid_header;
    }
    if (raw_w > kMaxImageDimension || raw_h > kMaxImageDimension) {
        return codec_error::dimension_limit;
    }
    if (!decoded_size_within_limits(raw_w, raw_h)) {
        return codec_error::dimension_limit;
    }

    // Orientation / container transforms
    // Exif orientation
    int exif_op = 0;
    const int exif_count = api->image_handle_get_number_of_metadata_blocks(handle, "Exif");
    if (exif_count > 0) {
        uint32_t exif_id = 0;
        if (api->image_handle_get_list_of_metadata_block_IDs(handle, "Exif", &exif_id, 1) == 1) {
            const std::size_t exif_sz = api->image_handle_get_metadata_size(handle, exif_id);
            if (exif_sz > 0 && exif_sz <= 1024 * 1024) {
                std::vector<std::uint8_t> ebuf(exif_sz);
                if (api->image_handle_get_metadata(handle, exif_id, ebuf.data()).code == heif_error_Ok) {
                    exif_op = parse_exif_orientation_payload(ebuf.data(), exif_sz);
                }
            }
        }
    }

    // Container transforms (irot / imir) from libheif image_handle properties
    // Note: In libheif, handle->get_width() gives dimensions AFTER container transformations,
    // while ispe gives width before container transformations.
    // Let's inspect handle width/height vs ispe width/height:
    const int handle_w = api->image_handle_get_width(handle);
    const int handle_h = api->image_handle_get_height(handle);

    // If handle_w != raw_w || handle_h != raw_h, a container transform was declared (irot 90/270).
    // Also check if any conflicting non-identity EXIF orientation is declared:
    const bool container_swapped = (handle_w == raw_h && handle_h == raw_w && handle_w != raw_w);
    if (container_swapped && (exif_op > 1)) {
        // If EXIF orientation is also non-identity, check conflict:
        // Container swapped means rotation 90 or 270 (EXIF 5..8).
        // If EXIF says not 5..8 or conflicts, reject.
        if (exif_op < 5) {
            return codec_error::invalid_header;
        }
    }

    const int final_op = (exif_op >= 1 && exif_op <= 8) ? exif_op : (container_swapped ? 6 : 1);
    const bool swap = (final_op >= 5 && final_op <= 8);

    const std::int32_t ow = swap ? raw_h : raw_w;
    const std::int32_t oh = swap ? raw_w : raw_h;

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

    const auto* api = heif_loader::get_api();
    if (api == nullptr) {
        out.error = codec_error::unsupported;
        out.detail = "libheif DLL not available";
        return out;
    }

    heif_context* ctx = api->context_alloc();
    if (ctx == nullptr) {
        out.error = codec_error::allocation_limit;
        return out;
    }
    context_cleaner ctx_cl(api, ctx);

    // Bounded worker thread count: max 4
    const int max_threads = std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1, 4);
    api->context_set_max_decoding_threads(ctx, max_threads);

    heif_error err = api->context_read_from_memory_without_copy(ctx, data, size, nullptr);
    if (err.code != heif_error_Ok) {
        out.error = codec_error::invalid_header;
        return out;
    }

    heif_image_handle* handle = nullptr;
    err = api->context_get_primary_image_handle(ctx, &handle);
    if (err.code != heif_error_Ok || handle == nullptr) {
        out.error = codec_error::invalid_header;
        return out;
    }
    handle_cleaner handle_cl(api, handle);

    // Disable libheif container transformations so OZ controls geometry
    heif_decoding_options* decode_opts = api->decoding_options_alloc();
    if (decode_opts == nullptr) {
        out.error = codec_error::allocation_limit;
        return out;
    }
    dec_options_cleaner opts_cl(api, decode_opts);
    decode_opts->ignore_transformations = 1;

    // Decode to interleaved RGBA8 (straight alpha)
    heif_image* img = nullptr;
    err = api->decode_image(handle, &img, heif_colorspace_RGB, heif_chroma_interleaved_RGBA, decode_opts);
    if (err.code != heif_error_Ok || img == nullptr) {
        out.error = codec_error::decode_failed;
        return out;
    }
    image_cleaner img_cl(api, img);

    int stride = 0;
    const uint8_t* plane_data = api->image_get_plane_readonly(img, heif_channel_interleaved, &stride);
    const int img_w = api->image_get_width(img, heif_channel_interleaved);
    const int img_h = api->image_get_height(img, heif_channel_interleaved);

    if (plane_data == nullptr || img_w <= 0 || img_h <= 0 || stride < img_w * 4) {
        out.error = codec_error::decode_failed;
        return out;
    }

    // Convert straight RGBA8 to canonical straight BGRA8 in staged buffer
    std::int32_t staged_stride = 0;
    std::size_t staged_bytes = 0;
    if (!checked_stride_and_size(img_w, img_h, staged_stride, staged_bytes)) {
        out.error = codec_error::allocation_limit;
        return out;
    }

    std::vector<std::uint8_t> staged(staged_bytes);
    for (int y = 0; y < img_h; ++y) {
        const uint8_t* src_row = plane_data + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride);
        uint8_t* dst_row = staged.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(staged_stride);
        for (int x = 0; x < img_w; ++x) {
            const uint8_t r = src_row[x * 4 + 0];
            const uint8_t g = src_row[x * 4 + 1];
            const uint8_t b = src_row[x * 4 + 2];
            const uint8_t a = src_row[x * 4 + 3];
            dst_row[x * 4 + 0] = b;
            dst_row[x * 4 + 1] = g;
            dst_row[x * 4 + 2] = r;
            dst_row[x * 4 + 3] = a;
        }
    }

    // Optional ICC profile -> sRGB transform via WIC
    const std::size_t raw_icc_size = api->image_handle_get_raw_color_profile_size(handle);
    if (raw_icc_size > 0 && raw_icc_size <= 16ull * 1024 * 1024) {
        std::vector<std::uint8_t> icc_data(raw_icc_size);
        if (api->image_handle_get_raw_color_profile(handle, icc_data.data()).code == heif_error_Ok) {
            (void)try_apply_icc_srgb(staged, img_w, img_h, icc_data.data(), raw_icc_size);
        }
    }

    // Integer downscaling if den > 1 (box-filter / area average)
    std::int32_t final_untransformed_w = img_w;
    std::int32_t final_untransformed_h = img_h;
    std::vector<std::uint8_t> scaled_buf;
    if (den > 1) {
        final_untransformed_w = (img_w + den - 1) / den;
        final_untransformed_h = (img_h + den - 1) / den;
        std::int32_t scaled_stride = 0;
        std::size_t scaled_bytes = 0;
        if (!checked_stride_and_size(final_untransformed_w, final_untransformed_h, scaled_stride, scaled_bytes)) {
            out.error = codec_error::allocation_limit;
            return out;
        }
        scaled_buf.resize(scaled_bytes);
        for (std::int32_t dy = 0; dy < final_untransformed_h; ++dy) {
            const std::int32_t sy_start = dy * den;
            const std::int32_t sy_end = std::min(sy_start + den, img_h);
            const std::uint32_t box_h = static_cast<std::uint32_t>(sy_end - sy_start);
            for (std::int32_t dx = 0; dx < final_untransformed_w; ++dx) {
                const std::int32_t sx_start = dx * den;
                const std::int32_t sx_end = std::min(sx_start + den, img_w);
                const std::uint32_t box_w = static_cast<std::uint32_t>(sx_end - sx_start);
                const std::uint32_t area = box_w * box_h;

                std::uint32_t sum_b = 0, sum_g = 0, sum_r = 0, sum_a = 0;
                for (std::int32_t sy = sy_start; sy < sy_end; ++sy) {
                    const std::uint8_t* row = staged.data() + static_cast<std::size_t>(sy) * static_cast<std::size_t>(staged_stride);
                    for (std::int32_t sx = sx_start; sx < sx_end; ++sx) {
                        sum_b += row[sx * 4 + 0];
                        sum_g += row[sx * 4 + 1];
                        sum_r += row[sx * 4 + 2];
                        sum_a += row[sx * 4 + 3];
                    }
                }
                const std::size_t dst_idx = (static_cast<std::size_t>(dy) * static_cast<std::size_t>(scaled_stride)) +
                                            (static_cast<std::size_t>(dx) * 4u);
                scaled_buf[dst_idx + 0] = static_cast<std::uint8_t>((sum_b + area / 2u) / area);
                scaled_buf[dst_idx + 1] = static_cast<std::uint8_t>((sum_g + area / 2u) / area);
                scaled_buf[dst_idx + 2] = static_cast<std::uint8_t>((sum_r + area / 2u) / area);
                scaled_buf[dst_idx + 3] = static_cast<std::uint8_t>((sum_a + area / 2u) / area);
            }
        }
        staged = std::move(scaled_buf);
        staged_stride = scaled_stride;
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

    // Compose orientation transform
    int exif_op = 0;
    const int exif_count = api->image_handle_get_number_of_metadata_blocks(handle, "Exif");
    if (exif_count > 0) {
        uint32_t exif_id = 0;
        if (api->image_handle_get_list_of_metadata_block_IDs(handle, "Exif", &exif_id, 1) == 1) {
            const std::size_t exif_sz = api->image_handle_get_metadata_size(handle, exif_id);
            if (exif_sz > 0 && exif_sz <= 1024 * 1024) {
                std::vector<std::uint8_t> ebuf(exif_sz);
                if (api->image_handle_get_metadata(handle, exif_id, ebuf.data()).code == heif_error_Ok) {
                    exif_op = parse_exif_orientation_payload(ebuf.data(), exif_sz);
                }
            }
        }
    }

    const int handle_w = api->image_handle_get_width(handle);
    const int ispe_w = api->image_handle_get_ispe_width(handle);
    const int ispe_h = api->image_handle_get_ispe_height(handle);
    const bool container_swapped = (handle_w == ispe_h && handle_w != ispe_w);

    const int final_op = (exif_op >= 1 && exif_op <= 8) ? exif_op : (container_swapped ? 6 : 1);
    const bool swap = (final_op >= 5 && final_op <= 8);

    const std::int32_t final_w = swap ? final_untransformed_h : final_untransformed_w;
    const std::int32_t final_h = swap ? final_untransformed_w : final_untransformed_h;

    std::int32_t final_stride = 0;
    std::size_t final_bytes = 0;
    if (!checked_stride_and_size(final_w, final_h, final_stride, final_bytes)) {
        out.error = codec_error::allocation_limit;
        return out;
    }

    decoded_image decoded;
    decoded.codec = codec_id::heic;
    decoded.scale_den = den;
    decoded.src_width = src_w;
    decoded.src_height = src_h;
    decoded.width = final_w;
    decoded.height = final_h;
    decoded.stride = final_stride;
    decoded.pixels.resize(final_bytes);

    if (final_op <= 1) {
        std::memcpy(decoded.pixels.data(), staged.data(), final_bytes);
    } else {
        apply_transform_geometry(staged.data(), final_untransformed_w, final_untransformed_h,
                                 decoded.pixels.data(), final_op);
    }

    out.image = std::move(decoded);
    out.error = codec_error::none;
    return out;
}

#endif // OZVIEWER_HAVE_HEIC

}  // namespace oz::viewer_core::heic

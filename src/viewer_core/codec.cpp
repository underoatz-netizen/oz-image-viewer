// codec.cpp - narrow codec-neutral registry (Stage 3.2 + Common Raster
// Decode Profile v1 Phase A/B). Signature-first routing to the built-in JPEG
// adapter (TurboJPEG), the WIC PNG decoder, the static WebP decoder
// (libwebp decoder+demux, still images only), the Phase A in-box WIC raster
// adapters for BMP, static GIF, classic TIFF (page 0) and ICO (deterministic
// entry selection), and the Phase B CUR (deterministic cursor entry
// selection, payload-slice decode) and native bounded TGA adapters. The JPEG
// path performs no COM/WIC initialization and never invokes a PNG or WebP
// decoder; PNG lazily initializes COM on the calling worker thread; WebP
// decodes statically and uses WIC only for an optional ICC->sRGB conversion
// on the calling worker thread; the BMP/GIF/TIFF/ICO/CUR adapters run their
// full byte-level structural pre-parse first and only then lazily initialize
// COM with one explicit WIC container GUID. TGA uses no COM/WIC at all and is
// selected last, after every exact signature, by a strict structural
// discriminator. There is no generic WIC decoder auto-detection or fallback.
#include "oz/viewer_core/codec.hpp"

#include "oz/viewer_core/jpeg.hpp"

#include "../png_wic.hpp"
#include "../raster_wic.hpp"
#include "../tga.hpp"
#include "../webp.hpp"
#include "avif.hpp"
#include "heic.hpp"
#include "isobmff.hpp"

#include <cstring>

namespace oz::viewer_core {
namespace {

constexpr std::uint8_t kPngSig[8] = {0x89, 0x50, 0x4E, 0x47,
                                     0x0D, 0x0A, 0x1A, 0x0A};

std::uint16_t read_le16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
}

std::uint32_t read_le32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint16_t read_be16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) | p[1]);
}

bool is_known_dib_size(std::uint32_t dib) noexcept {
    switch (dib) {
        case 12:
        case 40:
        case 52:
        case 56:
        case 64:
        case 108:
        case 124:
            return true;
        default:
            return false;
    }
}

codec_error map_jpeg_error(jpeg_error_kind kind) noexcept {
    switch (kind) {
        case jpeg_error_kind::none: return codec_error::none;
        case jpeg_error_kind::not_jpeg: return codec_error::unsupported;
        case jpeg_error_kind::truncated: return codec_error::truncated;
        case jpeg_error_kind::invalid_header: return codec_error::invalid_header;
        case jpeg_error_kind::dimension_limit: return codec_error::dimension_limit;
        case jpeg_error_kind::allocation_limit: return codec_error::allocation_limit;
        case jpeg_error_kind::decode_failed: return codec_error::decode_failed;
    }
    return codec_error::decode_failed;
}

}  // namespace

bool has_png_signature(const std::uint8_t* data, std::size_t size) noexcept {
    return data != nullptr && size >= 8 && std::memcmp(data, kPngSig, 8) == 0;
}

bool has_webp_signature(const std::uint8_t* data, std::size_t size) noexcept {
    return data != nullptr && size >= 12 && std::memcmp(data, "RIFF", 4) == 0 &&
           std::memcmp(data + 8, "WEBP", 4) == 0;
}

bool has_bmp_signature(const std::uint8_t* data, std::size_t size) noexcept {
    if (data == nullptr || size < 26) {
        return false;
    }
    if (data[0] != 'B' || data[1] != 'M') {
        return false;
    }
    const std::uint32_t dib = read_le32(data + 14);
    if (!is_known_dib_size(dib)) {
        return false;
    }
    if (14ull + dib > size) {
        return false;
    }
    const std::uint32_t file_offset = read_le32(data + 10);
    if (file_offset < 14ull + dib || file_offset > size) {
        return false;
    }
    if (dib == 12) {
        return read_le16(data + 18) != 0 && read_le16(data + 20) != 0;
    }
    const std::int32_t w = static_cast<std::int32_t>(read_le32(data + 18));
    const std::int32_t h = static_cast<std::int32_t>(read_le32(data + 22));
    return w > 0 && h != 0 && h != INT32_MIN;
}

bool has_gif_signature(const std::uint8_t* data, std::size_t size) noexcept {
    return data != nullptr && size >= 6 &&
           (std::memcmp(data, "GIF87a", 6) == 0 ||
            std::memcmp(data, "GIF89a", 6) == 0);
}

bool has_tiff_signature(const std::uint8_t* data, std::size_t size) noexcept {
    if (data == nullptr || size < 4) {
        return false;
    }
    // Classic TIFF only: "II" 2A 00 or "MM" 00 2A (magic 42). BigTIFF
    // (magic 43) is deliberately not a supported signature.
    if (data[0] == 'I' && data[1] == 'I') {
        return read_le16(data + 2) == 42;
    }
    if (data[0] == 'M' && data[1] == 'M') {
        return read_be16(data + 2) == 42;
    }
    return false;
}

bool has_ico_signature(const std::uint8_t* data, std::size_t size) noexcept {
    if (data == nullptr || size < 22) {
        return false;
    }
    if (read_le16(data) != 0 || read_le16(data + 2) != 1) {
        return false;  // reserved 0; type 1 = icon
    }
    const std::uint32_t count = read_le16(data + 4);
    if (count == 0 || count > kMaxIcoDirectoryEntries) {
        return false;
    }
    const std::uint64_t dir_end = 6ull + static_cast<std::uint64_t>(count) * 16ull;
    if (dir_end > size) {
        return false;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t* e = data + 6 + static_cast<std::size_t>(i) * 16;
        if (e[3] != 0) {
            return false;  // bReserved
        }
        const std::uint32_t bytes = read_le32(e + 8);
        const std::uint32_t offset = read_le32(e + 12);
        if (bytes == 0 || static_cast<std::uint64_t>(offset) < dir_end ||
            static_cast<std::uint64_t>(offset) + bytes > size) {
            return false;
        }
    }
    return true;
}

bool has_cur_signature(const std::uint8_t* data, std::size_t size) noexcept {
    if (data == nullptr || size < 22) {
        return false;
    }
    if (read_le16(data) != 0 || read_le16(data + 2) != 2) {
        return false;  // reserved 0; type 2 = cursor
    }
    const std::uint32_t count = read_le16(data + 4);
    if (count == 0 || count > kMaxIcoDirectoryEntries) {
        return false;
    }
    const std::uint64_t dir_end = 6ull + static_cast<std::uint64_t>(count) * 16ull;
    if (dir_end > size) {
        return false;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t* e = data + 6 + static_cast<std::size_t>(i) * 16;
        // Byte 3 is reserved for both ICO and CUR and must be 0.
        if (e[3] != 0) {
            return false;
        }
        const std::uint32_t bytes = read_le32(e + 8);
        const std::uint32_t offset = read_le32(e + 12);
        if (bytes == 0 || static_cast<std::uint64_t>(offset) < dir_end ||
            static_cast<std::uint64_t>(offset) + bytes > size) {
            return false;
        }
    }
    return true;
}

bool has_avif_signature(const std::uint8_t* data, std::size_t size) noexcept {
    return avif::has_avif_signature(data, size);
}

bool has_heic_signature(const std::uint8_t* data, std::size_t size) noexcept {
    return heic::has_heic_signature(data, size);
}

std::optional<codec_id> detect_codec(const std::uint8_t* data,
                                     std::size_t size) noexcept {
    if (has_jpeg_magic(data, size)) {
        return codec_id::jpeg;
    }
    if (has_png_signature(data, size)) {
        return codec_id::png_wic;
    }
    if (has_webp_signature(data, size)) {
#if OZVIEWER_HAVE_WEBP
        return codec_id::webp;
#else
        return std::nullopt;
#endif
    }
    if (has_bmp_signature(data, size)) {
        return codec_id::bmp_wic;
    }
    if (has_gif_signature(data, size)) {
        return codec_id::gif_wic;
    }
    if (has_tiff_signature(data, size)) {
        return codec_id::tiff_wic;
    }
    if (has_ico_signature(data, size)) {
        return codec_id::ico_wic;
    }
    if (has_cur_signature(data, size)) {
        return codec_id::cur_wic;
    }
    if (avif::has_avif_signature(data, size)) {
#if OZVIEWER_HAVE_AVIF
        return codec_id::avif;
#else
        return std::nullopt;
#endif
    }
    if (heic::has_heic_signature(data, size)) {
#if OZVIEWER_HAVE_HEIC
        return codec_id::heic;
#else
        return std::nullopt;
#endif
    }
    // TGA has no universal leading magic: it is discriminated last, after
    // every exact signature, by a strict bounded structural check (2.0 footer
    // or exact legacy file fit). Unknown/ambiguous bytes stay unsupported.
    if (has_tga_signature(data, size)) {
        return codec_id::tga;
    }
    return std::nullopt;
}

bool decoded_size_within_limits(std::int32_t w, std::int32_t h) noexcept {
    if (w <= 0 || h <= 0 || w > kMaxImageDimension || h > kMaxImageDimension) {
        return false;
    }
    const std::uint64_t bytes =
        static_cast<std::uint64_t>(w) * static_cast<std::uint64_t>(h) * 4u;
    return bytes <= kMaxDecodedBytes;
}

bool checked_stride_and_size(std::int32_t w, std::int32_t h, std::int32_t& stride_out,
                             std::size_t& bytes_out) noexcept {
    stride_out = 0;
    bytes_out = 0;
    if (w <= 0 || h <= 0 || w > kMaxImageDimension || h > kMaxImageDimension) {
        return false;
    }
    const std::uint64_t stride = static_cast<std::uint64_t>(w) * 4u;
    if (stride > static_cast<std::uint64_t>(INT32_MAX)) {
        return false;
    }
    const std::uint64_t bytes = stride * static_cast<std::uint64_t>(h);
    if (bytes > kMaxDecodedBytes) {
        return false;
    }
    stride_out = static_cast<std::int32_t>(stride);
    bytes_out = static_cast<std::size_t>(bytes);
    return true;
}

codec_error parse_image_header(const std::uint8_t* data, std::size_t size,
                               image_info& out) noexcept {
    out = image_info{};
    if (data == nullptr || size == 0) {
        return codec_error::unsupported;
    }
    if (size > kMaxEncodedBytes) {
        return codec_error::allocation_limit;
    }
    const auto codec = detect_codec(data, size);
    if (!codec.has_value()) {
        return codec_error::unsupported;
    }
    if (*codec == codec_id::png_wic) {
        std::int32_t ow = 0;
        std::int32_t oh = 0;
        const codec_error err = png_wic::parse_header(data, size, ow, oh);
        if (err != codec_error::none) {
            return err;
        }
        out.codec = codec_id::png_wic;
        out.src_width = ow;
        out.src_height = oh;
        return codec_error::none;
    }
    if (*codec == codec_id::webp) {
        std::int32_t ow = 0;
        std::int32_t oh = 0;
        const codec_error err = webp::parse_header(data, size, ow, oh);
        if (err != codec_error::none) {
            return err;
        }
        out.codec = codec_id::webp;
        out.src_width = ow;
        out.src_height = oh;
        return codec_error::none;
    }
    if (*codec == codec_id::avif) {
        std::int32_t ow = 0;
        std::int32_t oh = 0;
        const codec_error err = avif::parse_header(data, size, ow, oh);
        if (err != codec_error::none) {
            return err;
        }
        out.codec = codec_id::avif;
        out.src_width = ow;
        out.src_height = oh;
        return codec_error::none;
    }
    if (*codec == codec_id::heic) {
        std::int32_t ow = 0;
        std::int32_t oh = 0;
        const codec_error err = heic::parse_header(data, size, ow, oh);
        if (err != codec_error::none) {
            return err;
        }
        out.codec = codec_id::heic;
        out.src_width = ow;
        out.src_height = oh;
        return codec_error::none;
    }
    // Phase A/B in-box WIC raster adapters plus the native TGA adapter:
    // full byte-level structural pre-parse only; no COM/WIC is touched in
    // the header phase (TGA never touches COM/WIC at all).
    if (*codec == codec_id::bmp_wic || *codec == codec_id::gif_wic ||
        *codec == codec_id::tiff_wic || *codec == codec_id::ico_wic ||
        *codec == codec_id::cur_wic || *codec == codec_id::tga) {
        std::int32_t ow = 0;
        std::int32_t oh = 0;
        codec_error err = codec_error::none;
        switch (*codec) {
            case codec_id::bmp_wic:
                err = bmp_wic::parse_header(data, size, ow, oh);
                break;
            case codec_id::gif_wic:
                err = gif_wic::parse_header(data, size, ow, oh);
                break;
            case codec_id::tiff_wic:
                err = tiff_wic::parse_header(data, size, ow, oh);
                break;
            case codec_id::ico_wic:
                err = ico_wic::parse_header(data, size, ow, oh);
                break;
            case codec_id::cur_wic:
                err = cur_wic::parse_header(data, size, ow, oh);
                break;
            case codec_id::tga:
                err = tga::parse_header(data, size, ow, oh);
                break;
            default:
                err = codec_error::unsupported;
                break;
        }
        if (err != codec_error::none) {
            return err;
        }
        out.codec = *codec;
        out.src_width = ow;
        out.src_height = oh;
        return codec_error::none;
    }
    // JPEG adapter path: no COM/WIC involvement.
    jpeg_header header;
    const jpeg_error_kind herr = parse_jpeg_header(data, size, header);
    if (herr != jpeg_error_kind::none) {
        return map_jpeg_error(herr);
    }
    out.codec = codec_id::jpeg;
    out.src_width = header.width;
    out.src_height = header.height;
    return codec_error::none;
}

decode_result decode_image(const std::uint8_t* data, std::size_t size,
                           int scaling_denominator) noexcept {
    decode_result out;
    if (data == nullptr || size == 0) {
        out.error = codec_error::unsupported;
        return out;
    }
    if (size > kMaxEncodedBytes) {
        out.error = codec_error::allocation_limit;
        return out;
    }
    const auto codec = detect_codec(data, size);
    if (!codec.has_value()) {
        out.error = codec_error::unsupported;
        out.detail = "unsupported image format";
        return out;
    }
    if (*codec == codec_id::png_wic) {
        return png_wic::decode(data, size, scaling_denominator);
    }
    if (*codec == codec_id::webp) {
        return webp::decode(data, size, scaling_denominator);
    }
    if (*codec == codec_id::avif) {
        return avif::decode(data, size, scaling_denominator);
    }
    if (*codec == codec_id::heic) {
        return heic::decode(data, size, scaling_denominator);
    }
    if (*codec == codec_id::bmp_wic) {
        return bmp_wic::decode(data, size, scaling_denominator);
    }
    if (*codec == codec_id::gif_wic) {
        return gif_wic::decode(data, size, scaling_denominator);
    }
    if (*codec == codec_id::tiff_wic) {
        return tiff_wic::decode(data, size, scaling_denominator);
    }
    if (*codec == codec_id::ico_wic) {
        return ico_wic::decode(data, size, scaling_denominator);
    }
    if (*codec == codec_id::cur_wic) {
        return cur_wic::decode(data, size, scaling_denominator);
    }
    if (*codec == codec_id::tga) {
        return tga::decode(data, size, scaling_denominator);
    }
    // JPEG adapter path: TurboJPEG only, no COM/WIC, no PNG decoder.
    if (scaling_denominator != 1 && scaling_denominator != 2 &&
        scaling_denominator != 4 && scaling_denominator != 8) {
        scaling_denominator = 1;
    }
    jpeg_result jr = decode_jpeg(data, size, scaling_denominator);
    if (jr.error != jpeg_error_kind::none) {
        out.error = map_jpeg_error(jr.error);
        out.detail = jr.detail;
        return out;
    }
    jpeg_header src{};
    const jpeg_error_kind serr = parse_jpeg_header(data, size, src);
    decoded_image img;
    img.pixels = std::move(jr.pixels);
    img.width = jr.header.width;
    img.height = jr.header.height;
    img.stride = jr.stride;
    img.scale_den = scaling_denominator;
    img.codec = codec_id::jpeg;
    if (serr == jpeg_error_kind::none) {
        img.src_width = src.width;
        img.src_height = src.height;
    } else {
        img.src_width = img.width;
        img.src_height = img.height;
    }
    out.image = std::move(img);
    out.error = codec_error::none;
    return out;
}

}  // namespace oz::viewer_core

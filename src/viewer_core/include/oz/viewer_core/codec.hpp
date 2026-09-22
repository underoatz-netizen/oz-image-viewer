// oz/viewer_core/codec.hpp - narrow codec-neutral image model and registry
// (Stage 3.2 + Common Raster Decode Profile v1 Phase A/B). Bounded,
// signature-first routing over built-in codecs only: JPEG (TurboJPEG
// adapter), PNG (Windows WIC), static WebP decode (libwebp decoder+demux),
// Phase A in-box WIC raster adapters for BMP, static GIF, classic TIFF
// (page 0), and ICO (deterministic entry selection), and the Phase B CUR
// (deterministic cursor entry selection) and native bounded TGA adapters.
// No plugin discovery, dynamic DLL scan, encode APIs, animation, or generic
// WIC fallback (every WIC decoder is selected explicitly by container GUID
// after byte-level structural validation; TGA uses no WIC at all).
//
// Canonical decoded contract: top-down SDR sRGB premultiplied BGRA8.
// Extensions never select a codec; routing inspects bytes only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace oz::viewer_core {

// Shared hard limits for untrusted encoded input. Values mirror the
// established JPEG bounds (512 MiB encoded / 2 GiB decoded, 16-bit
// dimension cap) so both codecs enforce one policy.
inline constexpr std::int32_t kMaxImageDimension = 65535;
inline constexpr std::size_t kMaxEncodedBytes = 512ull * 1024 * 1024;  // 512 MiB
inline constexpr std::size_t kMaxDecodedBytes = 1ull << 31;            // 2 GiB

// Phase A structural caps for the in-box WIC raster adapters. These are
// finite, documented bounds enforced during the byte-level pre-parse,
// before any COM/WIC decoder is created:
// - GIF: exactly one image descriptor may exist (static-only profile).
// - TIFF: classic TIFF only (BigTIFF magic is not a signature at all); at
//   most 64 IFDs (main page chain plus SubIFD/ExifIFD nodes) with at most
//   4096 entries each; at most 16 SubIFD pointers per IFD.
// - ICO/CUR: at most 256 directory entries (every entry validated and range
//   checked before selection).
// - TGA: no leading magic; a strict structural discriminator (optional 2.0
//   footer or exact legacy file fit) plus complete bounded pixel/RLE extent
//   validation must pass before routing.
// - BMP: DIB header size must be one of the known classic sizes.
inline constexpr std::uint32_t kMaxGifImageDescriptors = 1;
inline constexpr std::uint32_t kMaxTiffIfds = 64;
inline constexpr std::uint32_t kMaxTiffIfdEntries = 4096;
inline constexpr std::uint32_t kMaxTiffSubIfdPointers = 16;
inline constexpr std::uint32_t kMaxIcoDirectoryEntries = 256;

// Built-in codec identity. No other values exist.
enum class codec_id {
    jpeg,
    png_wic,
    webp,
    bmp_wic,
    gif_wic,
    tiff_wic,
    ico_wic,
    cur_wic,
    tga,
    avif,
    heic,
};

// Checked, codec-neutral error categories. Internal detail strings are
// logged only; the UI surfaces a fixed generic message.
enum class codec_error {
    none,
    unsupported,        // unknown signature (never guessed from the extension)
    truncated,          // buffer too small / cut short
    invalid_header,     // signature OK but header/metadata unreadable
    dimension_limit,    // dimensions exceed hard limits
    allocation_limit,   // encoded or decoded size exceeds hard limits
    decode_failed,      // pixel decode failed (corrupt data, WIC/COM fault, ...)
};

// Source image description from the registry header parse. Source
// dimensions are orientation-adjusted (EXIF-applied) so scale selection
// never needs file I/O beyond this point.
struct image_info {
    codec_id codec = codec_id::jpeg;
    std::int32_t src_width = 0;   // orientation-adjusted source width
    std::int32_t src_height = 0;  // orientation-adjusted source height
};

// Canonical decoded image: top-down SDR sRGB premultiplied BGRA8.
// scale_den is the actual scaling denominator used (1, 2, 4 or 8);
// src_* repeat the orientation-adjusted source dimensions for viewport
// profile matching without header I/O.
struct decoded_image {
    std::vector<std::uint8_t> pixels;  // premultiplied BGRA8, top-down
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t stride = 0;  // bytes per row (width * 4)
    int scale_den = 1;
    std::int32_t src_width = 0;
    std::int32_t src_height = 0;
    codec_id codec = codec_id::jpeg;
};

struct decode_result {
    codec_error error = codec_error::none;
    std::string detail;  // internal only; never shown to normal users
    decoded_image image;  // valid only when error == none
};

// Exact 8-byte PNG signature (89 50 4E 47 0D 0A 1A 0A).
bool has_png_signature(const std::uint8_t* data, std::size_t size) noexcept;

// Exact 12-byte WebP signature: "RIFF" + 32-bit size + "WEBP" at offset 8.
bool has_webp_signature(const std::uint8_t* data, std::size_t size) noexcept;

// "BM" plus a bounded BITMAPFILEHEADER/BITMAPINFOHEADER read (known DIB
// header size fully present, bfOffBits within the buffer, non-zero
// dimensions). Deep structural and pixel-extent validation happens in the
// adapter parse before any COM/WIC use.
bool has_bmp_signature(const std::uint8_t* data, std::size_t size) noexcept;

// "GIF87a" or "GIF89a" (6 bytes). The static-only block walk is part of the
// adapter parse, not of signature detection.
bool has_gif_signature(const std::uint8_t* data, std::size_t size) noexcept;

// Exact classic-TIFF signature only: "II" 2A 00 or "MM" 00 2A (magic 42).
// BigTIFF (magic 43) is deliberately not a supported signature.
bool has_tiff_signature(const std::uint8_t* data, std::size_t size) noexcept;

// ICO signature 00 00 01 00 (reserved 0, type 1 = icon) with a non-zero
// bounded directory count fully present in the buffer and every directory
// entry's payload range inside the buffer.
bool has_ico_signature(const std::uint8_t* data, std::size_t size) noexcept;

// CUR signature 00 00 02 00 (reserved 0, type 2 = cursor) with the same
// bounded directory/payload-range validation as ICO. Cursor hotspot words
// are metadata only.
bool has_cur_signature(const std::uint8_t* data, std::size_t size) noexcept;

// TGA structural discriminator (no leading magic). True only when the bytes
// form a fully bounded supported TGA: optional TGA 2.0 footer with validated
// extension/developer offsets, or a legacy file whose complete pixel/RLE
// extent ends exactly at the file boundary. Unknown/ambiguous bytes are never
// claimed.
bool has_tga_signature(const std::uint8_t* data, std::size_t size) noexcept;

// Exact AVIF signature detection via bounded ISO-BMFF ftyp parsing:
// requires explicit 'avif' brand and no 'avis' anywhere.
bool has_avif_signature(const std::uint8_t* data, std::size_t size) noexcept;

// Exact HEIC signature detection via bounded ISO-BMFF ftyp parsing:
// requires 'heic' or 'heix' brand, and no 'hevc'/'hevx'/'msf1' sequence brands anywhere.
bool has_heic_signature(const std::uint8_t* data, std::size_t size) noexcept;

// Signature-first codec detection from bytes only. Returns nullopt for
// unsupported data; extensions are never consulted.
std::optional<codec_id> detect_codec(const std::uint8_t* data,
                                     std::size_t size) noexcept;

// True when width*height*4 fits the decoded byte limit and the dimension cap.
bool decoded_size_within_limits(std::int32_t w, std::int32_t h) noexcept;

// Checked stride/size computation before any allocation. On success sets
// stride_out = w*4 and bytes_out = stride*h.
bool checked_stride_and_size(std::int32_t w, std::int32_t h, std::int32_t& stride_out,
                             std::size_t& bytes_out) noexcept;

// Parse the image header (codec + orientation-adjusted source dimensions)
// from an in-memory buffer. Signature-first; no extension use.
codec_error parse_image_header(const std::uint8_t* data, std::size_t size,
                               image_info& out) noexcept;

// Decode a buffer to the canonical contract at the given scaling
// denominator (1, 2, 4 or 8; other values fall back to 1). The JPEG path
// never initializes COM/WIC; the PNG path lazily initializes COM on the
// calling worker thread only; the WebP path decodes statically via
// libwebp (decoder+demux) and uses WIC only for an optional ICC->sRGB
// color conversion on the calling worker thread. The BMP/GIF/TIFF/ICO/CUR
// adapters run their full bounded structural pre-parse first and only then
// lazily initialize COM and create their explicit WIC container decoder on
// the calling worker thread. TGA is a fully native bounded adapter with no
// COM/WIC involvement.
decode_result decode_image(const std::uint8_t* data, std::size_t size,
                           int scaling_denominator) noexcept;

}  // namespace oz::viewer_core

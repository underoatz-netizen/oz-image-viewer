// raster_wic.hpp - Common Raster Decode Profile v1 Phase A in-box WIC
// adapters for BMP, static GIF, classic TIFF (page/frame 0) and ICO
// (deterministic entry selection). Internal to ozviewer_core: the registry
// in codec.cpp is the only caller. Not a public header.
//
// Contract (mirrors the PNG WebP adapters):
// - Every entry point is worker-thread safe and uses no global mutable state.
// - parse_header() performs the full bounded byte-level structural
//   pre-parse (bounds/overflow/offsets/counts/dimensions) and NEVER
//   initializes COM/WIC. It is used for viewport scale selection.
// - decode() repeats the same bounded pre-parse, then lazily initializes
//   COM (MTA) on the calling worker thread and creates exactly one explicit
//   in-box WIC container decoder (BMP/GIF/TIFF/ICO GUID) for the selected
//   frame. There is no generic WIC decoder enumeration or auto-detection.
// - Static-only profile: animated GIF is rejected before any pixel
//   allocation; TIFF renders page 0 only; ICO decodes the entry selected by
//   the deterministic policy in ico_wic::select_entry().
// - All failures map to the stable generic codec_error categories with no
//   library detail or UI exception.
#pragma once

#include "oz/viewer_core/codec.hpp"

namespace oz::viewer_core::bmp_wic {

// Bounded BMP header parse: "BM" file header plus a known classic DIB
// header, checked offset/dimension ranges and (for uncompressed layouts)
// pixel data within the buffer. No orientation concept; oriented dims are
// the stored dims (top-down canonical output is produced on decode).
codec_error parse_header(const std::uint8_t* data, std::size_t size,
                         std::int32_t& oriented_w_out,
                         std::int32_t& oriented_h_out) noexcept;

// Full BMP decode to canonical top-down PBGRA8 at the given denominator
// (1/2/4/8; other values fall back to 1). COM/WIC is initialized lazily on
// this call and only after the structural gate succeeded.
decode_result decode(const std::uint8_t* data, std::size_t size,
                     int scaling_denominator) noexcept;

}  // namespace oz::viewer_core::bmp_wic

namespace oz::viewer_core::gif_wic {

// Bounded GIF parse: logical screen descriptor plus a complete walk of the
// bounded block sequence (image descriptors, extensions, sub-blocks,
// trailer) with no pixel allocation. Exactly one image descriptor is
// required; zero, multiple (animated) or malformed/truncated sequences are
// rejected. Returns the logical screen dimensions.
codec_error parse_header(const std::uint8_t* data, std::size_t size,
                         std::int32_t& oriented_w_out,
                         std::int32_t& oriented_h_out) noexcept;

// Static GIF decode to canonical PBGRA8 (transparency preserved) at the
// given denominator. Re-runs the full static-only block walk before any
// COM/WIC use.
decode_result decode(const std::uint8_t* data, std::size_t size,
                     int scaling_denominator) noexcept;

}  // namespace oz::viewer_core::gif_wic

namespace oz::viewer_core::tiff_wic {

// Bounded endian-aware classic-TIFF scan: header (magic 42 only), bounded
// IFD chain plus bounded SubIFD/ExifIFD descent, checked entry types/
// counts/offsets with loop and overflow protection, page-0 dimensions and
// EXIF orientation (1-8), and RAW/DNG/CFA indicator rejection (DNG tags,
// CFA tags, CFA/LinearRaw photometric values, CR2 magic). BigTIFF never
// reaches this point (magic 43 fails the signature gate). Orientation-
// adjusted page-0 dimensions are returned on success.
codec_error parse_header(const std::uint8_t* data, std::size_t size,
                         std::int32_t& oriented_w_out,
                         std::int32_t& oriented_h_out) noexcept;

// Classic TIFF page/frame 0 decode to canonical top-down PBGRA8 at the
// given denominator, with valid EXIF orientations 1-8 applied on this
// worker thread. Multi-page files render page 0 only; RAW/DNG/CFA
// containers are rejected before WIC so no embedded raw preview is shown.
decode_result decode(const std::uint8_t* data, std::size_t size,
                     int scaling_denominator) noexcept;

}  // namespace oz::viewer_core::tiff_wic

namespace oz::viewer_core {

// Shared validated frame description for the ICO (Phase A) and CUR (Phase B)
// directory formats. Both containers use the same 16-byte ICONDIRENTRY; for
// CUR the planes/bit-count words are actually the hotspot X/Y and are never
// treated as ICO planes/bpp (bit depth is derived/validated from the payload).
struct raster_frame_entry {
    std::uint16_t index = 0;          // directory order
    std::int32_t width = 0;           // validated image width (1..256)
    std::int32_t height = 0;          // validated image height (1..256)
    std::uint16_t bit_depth = 0;      // DIB bpp / derived PNG depth (tie-break only)
    std::uint32_t payload_offset = 0; // payload range inside the buffer
    std::uint32_t payload_bytes = 0;
    bool png_payload = false;         // embedded PNG vs. DIB/AND-mask payload
    // DIB layout for payload-slice decode (valid only when png_payload is
    // false): header size, palette entry count, and color-mask presence.
    std::uint32_t dib_size = 0;
    std::uint32_t palette_entries = 0;
    bool has_color_masks = false;
};

namespace ico_wic {

using ico_entry = raster_frame_entry;

// Deterministic selection policy over the validated directory:
// largest pixel area, ties broken by higher bit depth, then directory
// order. Entries whose payload/header fails bounded validation are skipped
// ("largest safe image"). Exposed for focused tests; touches no COM/WIC.
codec_error select_entry(const std::uint8_t* data, std::size_t size,
                         ico_entry& out) noexcept;

// Bounded ICO parse: validates the directory, every entry's payload range,
// and the selected entry's payload (embedded PNG header or DIB geometry)
// without pixel allocation. Returns the selected entry's dimensions.
codec_error parse_header(const std::uint8_t* data, std::size_t size,
                         std::int32_t& oriented_w_out,
                         std::int32_t& oriented_h_out) noexcept;

// Decode the deterministically selected ICO entry to canonical PBGRA8 at
// the given denominator. The selected entry's payload bytes are sliced out
// and decoded with the matching explicit in-box WIC container decoder
// (PNG payloads via the PNG decoder, DIB payloads via the BMP decoder over
// a bounded file-header wrapper with the AND mask applied afterwards), so
// WIC frame ordering can never substitute a different entry; the ambient
// WIC default selection is never used. COM/WIC only after the full
// structural gate succeeded.
decode_result decode(const std::uint8_t* data, std::size_t size,
                     int scaling_denominator) noexcept;

}  // namespace oz::viewer_core::ico_wic

namespace cur_wic {

using cur_entry = raster_frame_entry;

// Bounded CUR (type 2) detection/selection over the validated directory:
// exact `00 00 02 00` header, nonzero bounded directory count, every entry
// range checked, and the same deterministic "largest safe image" policy as
// ICO (largest area, then derived/validated bit depth, then directory
// order). The cursor hotspot words are metadata only and never interpreted
// as ICO planes/bpp. Exposed for focused tests; touches no COM/WIC.
codec_error select_entry(const std::uint8_t* data, std::size_t size,
                         cur_entry& out) noexcept;

// Bounded CUR parse: validates the directory and the selected entry's
// payload (embedded PNG header or DIB geometry) without pixel allocation.
// Returns the selected entry's dimensions; the hotspot is ignored for image
// presentation.
codec_error parse_header(const std::uint8_t* data, std::size_t size,
                         std::int32_t& oriented_w_out,
                         std::int32_t& oriented_h_out) noexcept;

// Decode the deterministically selected CUR entry to canonical PBGRA8 at
// the given denominator. Only the selected payload slice reaches an explicit
// in-box WIC decoder: valid embedded PNG payloads via GUID_ContainerFormatPng
// and DIB payloads via a bounded BITMAPFILEHEADER wrapper + the explicit BMP
// decoder with the 1bpp AND mask applied. Arbitrary cursor payloads are never
// routed through generic WIC auto-detection/enumeration.
decode_result decode(const std::uint8_t* data, std::size_t size,
                     int scaling_denominator) noexcept;

}  // namespace oz::viewer_core::cur_wic

}  // namespace oz::viewer_core

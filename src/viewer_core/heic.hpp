// heic.hpp - dynamic HEIC still-image decoder internals (Stage 3.4,
// internal to ozviewer_core). Not a public header: the registry in
// codec.cpp is the only caller. All entry points are worker-thread safe,
// use no global mutable decoder state, use hardened lazy-loaded libheif/libde265,
// enforce bounded worker thread counts and checked allocations, and initialize
// COM lazily on the calling thread only for optional ICC->sRGB color conversion.
//
// Scope: static 8-bit SDR sRGB HEIC still image only (alpha allowed).
// Rejects sequences/animations/bursts/live-photos, multiple top-level images,
// auxiliary/depth/gainmap, 10/12-bit, HDR/PQ/HLG, and unsupported wide gamuts.
// Without OZVIEWER_HAVE_HEIC or if runtime DLLs are missing, reports unsupported.
#pragma once

#include "oz/viewer_core/codec.hpp"

namespace oz::viewer_core::heic {

// Parse bounded ISO-BMFF ftyp box ahead of TGA structural detection.
// Returns true ONLY if 'heic' or 'heix' brand is present,
// sequence brands ('hevc', 'hevx', 'msf1') are NOT present anywhere,
// and the box syntax is valid within bounded sizes.
// Note: 'mif1' alone without 'heic'/'heix' is not HEIC.
bool has_heic_signature(const std::uint8_t* data, std::size_t size) noexcept;

// Parse HEIC header dimensions and properties with no pixel allocation and
// report orientation-adjusted source dims. Used by the registry header phase
// so scale selection precedes any decode allocation.
codec_error parse_header(const std::uint8_t* data, std::size_t size,
                         std::int32_t& oriented_w_out,
                         std::int32_t& oriented_h_out) noexcept;

// Full static HEIC decode to the canonical contract at the given
// denominator. Rejects animation, multiple items, auxiliary/depth/gainmap,
// 10/12-bit, HDR/PQ/HLG, and malformed files.
// Maps every failure to codec_error without library detail.
decode_result decode(const std::uint8_t* data, std::size_t size,
                     int scaling_denominator) noexcept;

// Decoder-independent EXIF orientation scan of the HEIC EXIF payload
// (values 1-8 when valid, 0 when missing/invalid). Exposed for unit tests.
int read_exif_orientation(const std::uint8_t* data, std::size_t size) noexcept;

}  // namespace oz::viewer_core::heic

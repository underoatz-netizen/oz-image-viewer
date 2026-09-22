// avif.hpp - static AVIF still-image decoder internals (Stage 3.3,
// internal to ozviewer_core). Not a public header: the registry in
// codec.cpp is the only caller. All entry points are worker-thread safe,
// use no global mutable decoder state, explicitly select dav1d, enforce
// bounded worker thread counts and checked allocations, and initialize
// COM lazily on the calling thread only for the optional ICC->sRGB color
// conversion.
//
// Scope: static 8-bit SDR sRGB AVIF still image only (alpha allowed).
// Rejects avis/sequences/animations, multiple top-level images,
// bursts/live photos, auxiliary/depth/gainmap, 10/12-bit HDR/PQ/HLG,
// and unsupported wide gamuts. No encoding/conversion/plugin/shell handler.
// Without OZVIEWER_HAVE_AVIF the stubs report unsupported and the build
// advertises no AVIF.
#pragma once

#include "oz/viewer_core/codec.hpp"

namespace oz::viewer_core::avif {

// Parse bounded ISO-BMFF ftyp box ahead of TGA structural detection.
// Returns true ONLY if explicit 'avif' brand is present (major or compatible),
// 'avis' is NOT present anywhere (even if paired with 'avif'),
// and the box syntax is valid within bounded sizes.
// Note: 'mif1' alone without 'avif' is not AVIF.
bool has_avif_signature(const std::uint8_t* data, std::size_t size) noexcept;

// Parse AVIF header dimensions and properties with no pixel allocation and
// report orientation-adjusted source dims. Used by the registry header phase
// so scale selection precedes any decode allocation.
codec_error parse_header(const std::uint8_t* data, std::size_t size,
                         std::int32_t& oriented_w_out,
                         std::int32_t& oriented_h_out) noexcept;

// Full static AVIF decode to the canonical contract at the given
// denominator. Rejects animation, multiple items, grids without single
// bounded composed primary, HDR/PQ/HLG, wide gamuts, and malformed files.
// Maps every failure to codec_error without library detail.
decode_result decode(const std::uint8_t* data, std::size_t size,
                     int scaling_denominator) noexcept;

// Decoder-independent EXIF orientation scan of the AVIF EXIF payload
// (values 1-8 when valid, 0 when missing/invalid). Exposed for unit tests.
int read_exif_orientation(const std::uint8_t* data, std::size_t size) noexcept;

// Observability probe for conformance audit: returns cumulative count of
// avifDecoder instances created in this process.
std::uint64_t decoder_invocation_count() noexcept;

// Observability probe for conformance audit: resets cumulative decoder count.
void reset_decoder_invocation_count() noexcept;

}  // namespace oz::viewer_core::avif

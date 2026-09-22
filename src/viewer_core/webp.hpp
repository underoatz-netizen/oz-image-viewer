// webp.hpp - static WebP still-image decoder internals (Stage 3.2,
// internal to ozviewer_core). Not a public header: the registry in
// codec.cpp is the only caller. All entry points are worker-thread safe,
// use no global mutable decoder state, and initialize COM lazily on the
// calling thread only for the optional ICC->sRGB color conversion.
//
// Scope: static lossy/lossless WebP to canonical PBGRA8. Animated WebP is
// deterministically rejected before allocation/decode (never frame 0).
// No encoder, playback, metadata editor, plugin API, WIC WebP fallback,
// delay-load, or LoadLibrary. Without OZVIEWER_HAVE_WEBP the stubs report
// unsupported and the build advertises no WebP.
#pragma once

#include "oz/viewer_core/codec.hpp"

namespace oz::viewer_core::webp {

// Parse WebP header dimensions (RIFF+WEBP envelope, feature/demux
// agreement, EXIF orientation) with no pixel allocation and report
// orientation-adjusted source dims. Used by the registry header phase so
// scale selection precedes any decode allocation.
codec_error parse_header(const std::uint8_t* data, std::size_t size,
                         std::int32_t& oriented_w_out,
                         std::int32_t& oriented_h_out) noexcept;

// Full static WebP decode to the canonical contract at the given
// denominator. Rejects animation and malformed envelopes with bounded
// generic errors; maps every failure to codec_error without library
// detail.
decode_result decode(const std::uint8_t* data, std::size_t size,
                     int scaling_denominator) noexcept;

// Decoder-independent EXIF orientation scan of the WebP EXIF chunk
// (values 1-8 when valid, 0 when missing/invalid). Exposed for unit tests.
int read_exif_orientation(const std::uint8_t* data, std::size_t size) noexcept;

}  // namespace oz::viewer_core::webp

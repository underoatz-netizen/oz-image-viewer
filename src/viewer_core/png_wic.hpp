// png_wic.hpp - WIC PNG decoder internals (Stage 3.1, internal to
// ozviewer_core). Not a public header: the registry in codec.cpp is the
// only caller. All entry points are worker-thread safe, use no global
// mutable state, and initialize COM lazily on the calling thread only.
#pragma once

#include "oz/viewer_core/codec.hpp"

namespace oz::viewer_core::png_wic {

// Parse PNG header dimensions (IHDR + eXIf orientation, manual byte parse,
// no COM/WIC) and report orientation-adjusted source dims. Used by the
// registry header phase so scale selection precedes any COM use.
codec_error parse_header(const std::uint8_t* data, std::size_t size,
                         std::int32_t& oriented_w_out,
                         std::int32_t& oriented_h_out) noexcept;

// Full PNG decode to the canonical contract at the given denominator.
// Lazily initializes COM MTA on the calling worker thread (balanced on the
// same thread) and maps every failure to a bounded codec_error.
decode_result decode(const std::uint8_t* data, std::size_t size,
                     int scaling_denominator) noexcept;

// Manual, decoder-independent eXIf orientation scan (values 1-8 when valid,
// 0 when missing/invalid). Exposed for unit tests of the stable contract.
int read_exif_orientation(const std::uint8_t* data, std::size_t size) noexcept;

}  // namespace oz::viewer_core::png_wic

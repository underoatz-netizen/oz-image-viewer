// oz/viewer_core/jpeg.hpp - bounded, untrusted JPEG validation and decode
// (Stage 1) via the TurboJPEG C API from libjpeg-turbo.
#pragma once

#include "oz/viewer_core/codec.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace oz::viewer_core {

// Hard limits for untrusted JPEG input. JPEG SOF dimensions are 16-bit, so
// 65535 is the absolute maximum; we additionally cap the decoded buffer.
// The byte limits are shared with the codec registry (see codec.hpp).
inline constexpr std::int32_t kMaxJpegDimension = kMaxImageDimension;
inline constexpr std::size_t kMaxCompressedBytes = kMaxEncodedBytes;
static_assert(kMaxDecodedBytes == (1ull << 31));  // registry decoded cap

struct jpeg_header {
    std::int32_t width = 0;
    std::int32_t height = 0;
};

enum class jpeg_error_kind {
    none,
    not_jpeg,          // missing SOI magic (FF D8)
    truncated,         // file too small / read error
    invalid_header,    // header parse failed
    dimension_limit,   // dimensions exceed hard limits
    allocation_limit,  // decoded size exceeds hard limit
    decode_failed,
};

struct jpeg_result {
    jpeg_error_kind error = jpeg_error_kind::none;
    std::string detail;  // internal detail; never shown to normal users
    jpeg_header header;
    std::vector<std::uint8_t> pixels;  // BGRA, alpha 255
    std::int32_t stride = 0;           // bytes per row
};

// True when the buffer begins with the JPEG SOI marker (FF D8).
bool has_jpeg_magic(const std::uint8_t* data, std::size_t size) noexcept;

// Validate that width*height*4 does not overflow and stays within the decoded
// byte limit and the per-dimension limit.
bool decoded_size_within_limits(std::int32_t w, std::int32_t h) noexcept;

// Parse the JPEG header (dimensions) from an in-memory buffer. Returns
// jpeg_error_kind::none on success; otherwise the specific failure kind.
jpeg_error_kind parse_jpeg_header(const std::uint8_t* data, std::size_t size,
                                  jpeg_header& out) noexcept;

// Decode a JPEG buffer to BGRA (alpha 255) at the given scaling denominator
// (1, 2, 4 or 8). The caller must have already validated magic/limits via
// parse_jpeg_header. On success the result carries the decoded pixels.
jpeg_result decode_jpeg(const std::uint8_t* data, std::size_t size,
                        int scaling_denominator) noexcept;

}  // namespace oz::viewer_core
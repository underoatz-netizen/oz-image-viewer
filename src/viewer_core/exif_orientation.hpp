// exif_orientation.hpp - shared bounded TIFF/EXIF orientation parser (Stage 3.4).
#pragma once

#include <cstddef>
#include <cstdint>

namespace oz::viewer_core {

// Parse bounded TIFF structure for orientation tag 0x0112 (SHORT/LONG, values 1-8).
// Returns 1-8 if found and valid, else 0.
int parse_tiff_orientation(const std::uint8_t* tiff, std::size_t len) noexcept;

// Bounded EXIF chunk orientation extractor: handles 4-byte header offset (ISO 23008-12)
// or standard Exif\0\0 prefix.
int parse_exif_orientation_payload(const std::uint8_t* exif, std::size_t len) noexcept;

// Convert irot (0..3 CCW 90-deg) and imir (axis 0=vertical/left-right, 1=horizontal/top-bottom)
// to standard 1..8 orientation transform representation.
int compose_irot_imir(bool has_irot, std::uint8_t irot_val, bool has_imir, std::uint8_t imir_mode) noexcept;

// Apply orientation geometry transform (1..8) to 4-byte pixel buffer (BGRA/RGBA).
void apply_transform_geometry(const std::uint8_t* src, std::int32_t sw, std::int32_t sh,
                              std::uint8_t* dst, int transform_op) noexcept;

}  // namespace oz::viewer_core

// isobmff.hpp - bounded ISO-BMFF box classifier and brand parser (Stage 3.4).
// Shared codec-neutral parser for ftyp box inspection ahead of TGA discrimination.
#pragma once

#include <cstddef>
#include <cstdint>

namespace oz::viewer_core::isobmff {

// Brand classification flags returned by parse_ftyp_brands
struct ftyp_classification {
    bool valid = false;          // True if ftyp syntax and bounds are valid
    bool has_avif = false;       // 'avif' brand present
    bool has_avis = false;       // 'avis' brand present
    bool has_heic = false;       // 'heic' or 'heix' brand present
    bool has_mif1 = false;       // 'mif1' brand present
    bool has_hevc_seq = false;   // 'hevc', 'hevx', or 'msf1' present (sequence/burst)
};

// Parse bounded ISO-BMFF ftyp box.
// Evaluates major_brand and all compatible_brands.
// Enforces 1 MiB box bound, checked size (normal/extended), header bounds.
ftyp_classification classify_ftyp(const std::uint8_t* data, std::size_t size) noexcept;

// True ONLY if explicit 'avif' brand is present, 'avis' is NOT present anywhere,
// and ftyp syntax is valid within bounds. 'mif1' alone is NOT avif.
bool has_avif_signature(const std::uint8_t* data, std::size_t size) noexcept;

// True ONLY if 'heic' or 'heix' brand is present, 'hevc', 'hevx', and 'msf1'
// are NOT present anywhere, and ftyp syntax is valid within bounds.
// 'mif1' alone is NOT heic.
bool has_heic_signature(const std::uint8_t* data, std::size_t size) noexcept;

}  // namespace oz::viewer_core::isobmff

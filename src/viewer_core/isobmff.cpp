// isobmff.cpp - bounded ISO-BMFF box classifier and brand parser (Stage 3.4).
#include "isobmff.hpp"

#include <cstring>

namespace oz::viewer_core::isobmff {
namespace {

using std::uint32_t;
using std::uint64_t;

uint32_t read_be32(const std::uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

uint64_t read_be64(const std::uint8_t* p) noexcept {
    return (static_cast<uint64_t>(read_be32(p)) << 32) |
           static_cast<uint64_t>(read_be32(p + 4));
}

void check_brand(const std::uint8_t* b, ftyp_classification& result) noexcept {
    if (std::memcmp(b, "avif", 4) == 0) {
        result.has_avif = true;
    } else if (std::memcmp(b, "avis", 4) == 0) {
        result.has_avis = true;
    } else if (std::memcmp(b, "heic", 4) == 0 || std::memcmp(b, "heix", 4) == 0) {
        result.has_heic = true;
    } else if (std::memcmp(b, "mif1", 4) == 0) {
        result.has_mif1 = true;
    } else if (std::memcmp(b, "hevc", 4) == 0 || std::memcmp(b, "hevx", 4) == 0 ||
               std::memcmp(b, "msf1", 4) == 0) {
        result.has_hevc_seq = true;
    }
}

}  // namespace

ftyp_classification classify_ftyp(const std::uint8_t* data, std::size_t size) noexcept {
    ftyp_classification result{};
    if (data == nullptr || size < 12) {
        return result;
    }
    // Must start with box size (4 bytes) and type 'ftyp'
    if (std::memcmp(data + 4, "ftyp", 4) != 0) {
        return result;
    }
    uint64_t box_size = read_be32(data);
    std::size_t header_len = 8;
    if (box_size == 1) {
        // Extended 64-bit size
        if (size < 16) {
            return result;
        }
        box_size = read_be64(data + 8);
        header_len = 16;
    } else if (box_size == 0) {
        box_size = size;
    }
    if (box_size < header_len + 8 || box_size > size) {
        return result;
    }
    if (box_size > 1024 * 1024) {  // Bound ftyp box size to 1 MiB
        return result;
    }

    const std::uint8_t* payload = data + header_len;
    const std::size_t payload_len = static_cast<std::size_t>(box_size - header_len);

    // Major brand: first 4 bytes of payload
    check_brand(payload, result);

    // Skip minor_version (payload + 4)
    // Following 4-byte multiples: compatible_brands list
    for (std::size_t o = 8; o + 4 <= payload_len; o += 4) {
        check_brand(payload + o, result);
    }

    result.valid = true;
    return result;
}

bool has_avif_signature(const std::uint8_t* data, std::size_t size) noexcept {
    const auto info = classify_ftyp(data, size);
    if (!info.valid) {
        return false;
    }
    // AVIF requires explicit 'avif' brand, and rejects 'avis' anywhere
    if (info.has_avis) {
        return false;
    }
    return info.has_avif;
}

bool has_heic_signature(const std::uint8_t* data, std::size_t size) noexcept {
    const auto info = classify_ftyp(data, size);
    if (!info.valid) {
        return false;
    }
    // HEIC routes for 'heic' or 'heix', rejecting 'hevc' / 'hevx' / 'msf1' sequence brands
    // Mixed sequence + still brand set -> reject.
    if (info.has_hevc_seq) {
        return false;
    }
    // Note: mif1 alone routes to neither.
    return info.has_heic;
}

}  // namespace oz::viewer_core::isobmff

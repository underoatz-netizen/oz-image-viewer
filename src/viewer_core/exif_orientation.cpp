// exif_orientation.cpp - shared bounded TIFF/EXIF orientation parser (Stage 3.4).
#include "exif_orientation.hpp"

#include <algorithm>
#include <cstring>

namespace oz::viewer_core {
namespace {

using std::uint32_t;

std::uint16_t read_le16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
}

std::uint16_t read_be16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) | p[1]);
}

uint32_t read_le32(const std::uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint32_t read_be32(const std::uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

}  // namespace

int parse_tiff_orientation(const std::uint8_t* tiff, std::size_t len) noexcept {
    if (tiff == nullptr || len < 8) {
        return 0;
    }
    const bool little = (tiff[0] == 'I' && tiff[1] == 'I');
    const bool big = (tiff[0] == 'M' && tiff[1] == 'M');
    if (!little && !big) {
        return 0;
    }
    auto u16 = [&](std::size_t o, bool& ok) -> std::uint16_t {
        if (o + 2 > len) {
            ok = false;
            return 0;
        }
        return little ? read_le16(tiff + o) : read_be16(tiff + o);
    };
    auto u32 = [&](std::size_t o, bool& ok) -> uint32_t {
        if (o + 4 > len) {
            ok = false;
            return 0;
        }
        return little ? read_le32(tiff + o) : read_be32(tiff + o);
    };
    bool ok = true;
    if (u16(2, ok) != 42 || !ok) {
        return 0;
    }
    const uint32_t ifd_off = u32(4, ok);
    if (!ok || ifd_off > len) {
        return 0;
    }
    const std::size_t count = u16(ifd_off, ok);
    if (!ok) {
        return 0;
    }
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t e = static_cast<std::size_t>(ifd_off) + 2 + i * 12;
        if (e + 12 > len || i > 256) {
            return 0;
        }
        if (u16(e, ok) != 0x0112 || !ok) {
            continue;
        }
        const std::uint16_t type = u16(e + 2, ok);
        const uint32_t n = u32(e + 4, ok);
        if (!ok || n < 1) {
            return 0;
        }
        uint32_t value = 0;
        if (type == 3) {
            value = little ? read_le16(tiff + e + 8) : read_be16(tiff + e + 8);
        } else if (type == 4) {
            value = u32(e + 8, ok);
        } else {
            return 0;
        }
        if (!ok || value < 1 || value > 8) {
            return 0;
        }
        return static_cast<int>(value);
    }
    return 0;
}

int parse_exif_orientation_payload(const std::uint8_t* exif, std::size_t len) noexcept {
    if (exif == nullptr || len == 0 || len > 1024 * 1024) {
        return 0;
    }
    if (len >= 4) {
        const uint32_t tiff_offset = read_be32(exif);
        if (tiff_offset < len - 4) {
            const int o = parse_tiff_orientation(exif + 4 + tiff_offset, len - 4 - tiff_offset);
            if (o >= 1 && o <= 8) {
                return o;
            }
        }
    }
    static constexpr std::uint8_t kExifPrefix[6] = {'E', 'x', 'i', 'f', 0, 0};
    if (len >= 6 && std::memcmp(exif, kExifPrefix, 6) == 0) {
        const int o = parse_tiff_orientation(exif + 6, len - 6);
        if (o >= 1 && o <= 8) {
            return o;
        }
    }
    return parse_tiff_orientation(exif, len);
}

int compose_irot_imir(bool has_irot, std::uint8_t irot_val, bool has_imir, std::uint8_t imir_mode) noexcept {
    int r = 1;
    if (has_irot) {
        switch (irot_val % 4) {
            case 0: r = 1; break;
            case 1: r = 8; break;
            case 2: r = 3; break;
            case 3: r = 6; break;
        }
    }
    if (has_imir) {
        if (imir_mode == 0) { // left-right mirror
            switch (r) {
                case 1: r = 2; break;
                case 8: r = 5; break;
                case 3: r = 4; break;
                case 6: r = 7; break;
            }
        } else if (imir_mode == 1) { // top-bottom mirror
            switch (r) {
                case 1: r = 4; break;
                case 8: r = 7; break;
                case 3: r = 2; break;
                case 6: r = 5; break;
            }
        }
    }
    return r;
}

void apply_transform_geometry(const std::uint8_t* src, std::int32_t sw, std::int32_t sh,
                              std::uint8_t* dst, int transform_op) noexcept {
    const std::size_t sstride = static_cast<std::size_t>(sw) * 4u;
    const bool swap = (transform_op >= 5 && transform_op <= 8);
    const std::int32_t dw = swap ? sh : sw;
    const std::int32_t dh = swap ? sw : sh;
    const std::size_t dstride = static_cast<std::size_t>(dw) * 4u;
    for (std::int32_t dy = 0; dy < dh; ++dy) {
        for (std::int32_t dx = 0; dx < dw; ++dx) {
            std::int32_t sx = dx;
            std::int32_t sy = dy;
            switch (transform_op) {
                case 2: sx = sw - 1 - dx; sy = dy; break;
                case 3: sx = sw - 1 - dx; sy = sh - 1 - dy; break;
                case 4: sx = dx; sy = sh - 1 - dy; break;
                case 5: sx = dy; sy = dx; break;
                case 6: sx = sy; sy = sh - 1 - dx; break;
                case 7: sx = sw - 1 - dy; sy = sh - 1 - dx; break;
                case 8: sx = sw - 1 - dy; sy = dx; break;
                default: break;
            }
            const std::uint8_t* sp = src + static_cast<std::size_t>(sy) * sstride +
                                     static_cast<std::size_t>(sx) * 4u;
            std::uint8_t* dp = dst + static_cast<std::size_t>(dy) * dstride +
                                static_cast<std::size_t>(dx) * 4u;
            dp[0] = sp[0];
            dp[1] = sp[1];
            dp[2] = sp[2];
            dp[3] = sp[3];
        }
    }
}

}  // namespace oz::viewer_core

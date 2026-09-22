// png_mini_writer.hpp - test-only minimal PNG encoder (Stage 3.1).
//
// Decoder-independent fixture generation: hand-builds PNG files with stored
// (uncompressed) zlib blocks, so fixtures never depend on WIC, libpng, or
// any other codec under test. Covers RGB, RGBA, grayscale, palette+tRNS,
// Adam7 interlaced, eXIf orientation, and iCCP chunks for the registry tests
// and the ozviewer_make_png fixture tool.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace oz::test_png {

struct rgba {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;
};

inline std::uint32_t crc32_ieee(const std::uint8_t* data, std::size_t len) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

inline std::uint32_t adler32(const std::uint8_t* data, std::size_t len) {
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (std::size_t i = 0; i < len; ++i) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

inline void append_be32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
}

inline void append_chunk(std::vector<std::uint8_t>& out, const char type[4],
                         const std::uint8_t* data, std::size_t len) {
    append_be32(out, static_cast<std::uint32_t>(len));
    const std::size_t type_pos = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data, data + len);
    const std::uint32_t crc =
        crc32_ieee(out.data() + type_pos, 4 + len);
    append_be32(out, crc);
}

// zlib wrapper around stored (uncompressed) deflate blocks.
inline std::vector<std::uint8_t> zlib_stored(const std::uint8_t* raw, std::size_t len) {
    std::vector<std::uint8_t> out;
    out.push_back(0x78);
    out.push_back(0x01);
    std::size_t pos = 0;
    if (len == 0) {
        out.push_back(0x01);  // BFINAL=1, BTYPE=00
        out.push_back(0x00);
        out.push_back(0x00);
        out.push_back(0xFF);
        out.push_back(0xFF);
    }
    while (pos < len) {
        const std::size_t n = (len - pos > 65535) ? 65535 : (len - pos);
        const bool last = (pos + n == len);
        out.push_back(last ? 0x01 : 0x00);
        out.push_back(static_cast<std::uint8_t>(n & 0xFF));
        out.push_back(static_cast<std::uint8_t>((n >> 8) & 0xFF));
        const std::uint16_t nlen = static_cast<std::uint16_t>(~static_cast<std::uint32_t>(n) & 0xFFFFu);
        out.push_back(static_cast<std::uint8_t>(nlen & 0xFF));
        out.push_back(static_cast<std::uint8_t>((nlen >> 8) & 0xFF));
        out.insert(out.end(), raw + pos, raw + pos + n);
        pos += n;
    }
    append_be32(out, adler32(raw, len));
    return out;
}

// Raw scanlines (filter byte 0 + packed samples per row) for non-interlaced.
inline std::vector<std::uint8_t> raw_rows_rgba(std::uint32_t w, std::uint32_t h,
                                               const std::vector<rgba>& px) {
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(h) * (1 + static_cast<std::size_t>(w) * 4));
    for (std::uint32_t y = 0; y < h; ++y) {
        raw.push_back(0);
        for (std::uint32_t x = 0; x < w; ++x) {
            const rgba& p = px[static_cast<std::size_t>(y) * w + x];
            raw.push_back(p.r);
            raw.push_back(p.g);
            raw.push_back(p.b);
            raw.push_back(p.a);
        }
    }
    return raw;
}

// Adam7 interlace parameters: xstart, ystart, xstep, ystep per pass.
struct adam7_pass {
    std::uint32_t xs, ys, dx, dy;
};

inline std::vector<adam7_pass> adam7_passes() {
    return {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4},
            {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}};
}

inline std::vector<std::uint8_t> raw_rows_rgba_interlaced(std::uint32_t w, std::uint32_t h,
                                                          const std::vector<rgba>& px) {
    std::vector<std::uint8_t> raw;
    for (const auto& p : adam7_passes()) {
        if (p.xs >= w || p.ys >= h) {
            continue;
        }
        const std::uint32_t pw = (w - p.xs + p.dx - 1) / p.dx;
        const std::uint32_t ph = (h - p.ys + p.dy - 1) / p.dy;
        for (std::uint32_t row = 0; row < ph; ++row) {
            raw.push_back(0);
            for (std::uint32_t col = 0; col < pw; ++col) {
                const std::uint32_t x = p.xs + col * p.dx;
                const std::uint32_t y = p.ys + row * p.dy;
                const rgba& pxv = px[static_cast<std::size_t>(y) * w + x];
                raw.push_back(pxv.r);
                raw.push_back(pxv.g);
                raw.push_back(pxv.b);
                raw.push_back(pxv.a);
            }
        }
    }
    return raw;
}

// Minimal eXIf chunk payload: little-endian TIFF with a single SHORT
// orientation tag (value 1-8 as given; callers may pass out-of-range values
// to test the safe-fallback contract). Layout (22 bytes): II, 42, IFD
// offset 8, 1 entry: tag 0x0112, type SHORT, count 1, inline value.
inline std::vector<std::uint8_t> exif_payload(int orientation) {
    std::vector<std::uint8_t> tiff = {'I', 'I', 42, 0, 8, 0, 0, 0,
                                      1, 0, 0x12, 0x01, 3, 0,
                                      1, 0, 0, 0, 0, 0, 0, 0};
    tiff[18] = static_cast<std::uint8_t>(orientation & 0xFF);
    tiff[19] = static_cast<std::uint8_t>((orientation >> 8) & 0xFF);
    return tiff;
}

inline void append_ihdr(std::vector<std::uint8_t>& out, std::uint32_t w, std::uint32_t h,
                        std::uint8_t bit_depth, std::uint8_t color_type,
                        std::uint8_t interlace) {
    std::uint8_t ihdr[13];
    ihdr[0] = static_cast<std::uint8_t>((w >> 24) & 0xFF);
    ihdr[1] = static_cast<std::uint8_t>((w >> 16) & 0xFF);
    ihdr[2] = static_cast<std::uint8_t>((w >> 8) & 0xFF);
    ihdr[3] = static_cast<std::uint8_t>(w & 0xFF);
    ihdr[4] = static_cast<std::uint8_t>((h >> 24) & 0xFF);
    ihdr[5] = static_cast<std::uint8_t>((h >> 16) & 0xFF);
    ihdr[6] = static_cast<std::uint8_t>((h >> 8) & 0xFF);
    ihdr[7] = static_cast<std::uint8_t>(h & 0xFF);
    ihdr[8] = bit_depth;
    ihdr[9] = color_type;
    ihdr[10] = 0;  // compression
    ihdr[11] = 0;  // filter
    ihdr[12] = interlace;
    append_chunk(out, "IHDR", ihdr, 13);
}

inline void append_idat(std::vector<std::uint8_t>& out, const std::vector<std::uint8_t>& raw) {
    const auto zlib = zlib_stored(raw.data(), raw.size());
    append_chunk(out, "IDAT", zlib.data(), zlib.size());
}

inline void append_iend(std::vector<std::uint8_t>& out) {
    append_chunk(out, "IEND", nullptr, 0);
}

inline std::vector<std::uint8_t> signature() {
    return {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
}

inline std::vector<std::uint8_t> make_rgba_png(std::uint32_t w, std::uint32_t h,
                                               const std::vector<rgba>& px,
                                               bool interlaced = false,
                                               int exif_orientation = 0,
                                               const std::vector<std::uint8_t>& icc = {}) {
    auto out = signature();
    append_ihdr(out, w, h, 8, 6, interlaced ? 1 : 0);
    if (!icc.empty()) {
        std::vector<std::uint8_t> iccp = {'t', 'e', 's', 't', 0, 0};
        const auto comp = zlib_stored(icc.data(), icc.size());
        iccp.insert(iccp.end(), comp.begin(), comp.end());
        append_chunk(out, "iCCP", iccp.data(), iccp.size());
    }
    if (exif_orientation != 0) {
        const auto exif = exif_payload(exif_orientation);
        append_chunk(out, "eXIf", exif.data(), exif.size());
    }
    const auto raw =
        interlaced ? raw_rows_rgba_interlaced(w, h, px) : raw_rows_rgba(w, h, px);
    append_idat(out, raw);
    append_iend(out);
    return out;
}

inline std::vector<std::uint8_t> make_rgb_png(std::uint32_t w, std::uint32_t h,
                                              const std::vector<rgba>& px) {
    auto out = signature();
    append_ihdr(out, w, h, 8, 2, 0);
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(h) * (1 + static_cast<std::size_t>(w) * 3));
    for (std::uint32_t y = 0; y < h; ++y) {
        raw.push_back(0);
        for (std::uint32_t x = 0; x < w; ++x) {
            const rgba& p = px[static_cast<std::size_t>(y) * w + x];
            raw.push_back(p.r);
            raw.push_back(p.g);
            raw.push_back(p.b);
        }
    }
    append_idat(out, raw);
    append_iend(out);
    return out;
}

inline std::vector<std::uint8_t> make_gray_png(std::uint32_t w, std::uint32_t h,
                                               const std::vector<std::uint8_t>& gray) {
    auto out = signature();
    append_ihdr(out, w, h, 8, 0, 0);
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(h) * (1 + w));
    for (std::uint32_t y = 0; y < h; ++y) {
        raw.push_back(0);
        for (std::uint32_t x = 0; x < w; ++x) {
            raw.push_back(gray[static_cast<std::size_t>(y) * w + x]);
        }
    }
    append_idat(out, raw);
    append_iend(out);
    return out;
}

inline std::vector<std::uint8_t> make_palette_png(std::uint32_t w, std::uint32_t h,
                                                  const std::vector<std::uint8_t>& indices,
                                                  const std::vector<rgba>& palette,
                                                  const std::vector<std::uint8_t>& trns) {
    auto out = signature();
    append_ihdr(out, w, h, 8, 3, 0);
    std::vector<std::uint8_t> plte;
    for (const auto& p : palette) {
        plte.push_back(p.r);
        plte.push_back(p.g);
        plte.push_back(p.b);
    }
    append_chunk(out, "PLTE", plte.data(), plte.size());
    if (!trns.empty()) {
        append_chunk(out, "tRNS", trns.data(), trns.size());
    }
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(h) * (1 + w));
    for (std::uint32_t y = 0; y < h; ++y) {
        raw.push_back(0);
        for (std::uint32_t x = 0; x < w; ++x) {
            raw.push_back(indices[static_cast<std::size_t>(y) * w + x]);
        }
    }
    append_idat(out, raw);
    append_iend(out);
    return out;
}

// Gradient test pattern (opaque unless alpha_fn says otherwise).
inline std::vector<rgba> gradient(std::uint32_t w, std::uint32_t h) {
    std::vector<rgba> px(static_cast<std::size_t>(w) * h);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            rgba& p = px[static_cast<std::size_t>(y) * w + x];
            p.r = static_cast<std::uint8_t>((x * 255) / (w > 1 ? w - 1 : 1));
            p.g = static_cast<std::uint8_t>((y * 255) / (h > 1 ? h - 1 : 1));
            p.b = 128;
            p.a = 255;
        }
    }
    return px;
}

}  // namespace oz::test_png

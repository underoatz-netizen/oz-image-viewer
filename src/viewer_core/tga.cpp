// tga.cpp - Common Raster Decode Profile v1 Phase B: bounded native TGA
// adapter. See tga.hpp for the contract.
//
// Supported (ordinary common raster TGA):
// - image types 1/2/3 (uncompressed color-mapped / true-color / grayscale)
//   and 9/10/11 (the RLE forms of the same), image type 0 rejected;
// - color-mapped index depth 8 or 16 with 15/16/24/32-bit color-map entries;
// - true-color pixel depth 15/16/24/32 (BGR(A), ARGB1555 packing);
// - grayscale pixel depth 8 or 16 (intensity, intensity+alpha);
// - image-descriptor alpha bits (0-8), left/right and bottom/top origin;
// - optional TGA 2.0 extension/developer areas + footer, preserved as
//   ignored metadata only (never decoded).
//
// Explicitly rejected (precision over corruption): image type 0/4-8/12+,
// unknown pixel depths, a color map on a non-color-mapped type (or none on a
// color-mapped type), palette entries outside the declared range, RLE
// streams that do not fill exactly width*height pixels, any container whose
// declared pixel/RLE extent does not fit the file exactly (or within a valid
// extension/developer/footer region), and any mismatch between declared and
// walked extents.
#include "tga.hpp"

#include <cstring>
#include <vector>

namespace oz::viewer_core {
namespace tga {
namespace {

std::uint16_t rd16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
}

std::uint32_t rd32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

constexpr std::size_t kHeaderSize = 18;
constexpr std::size_t kFooterSize = 26;
constexpr std::size_t kExtensionAreaSize = 495;
constexpr std::uint16_t kExtensionAreaSizeField = 495;

bool is_cmap_type(std::uint8_t t) noexcept { return t == 1 || t == 9; }
bool is_truecolor_type(std::uint8_t t) noexcept { return t == 2 || t == 10; }
bool is_gray_type(std::uint8_t t) noexcept { return t == 3 || t == 11; }

struct layout {
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::uint8_t image_type = 0;
    std::uint8_t pixel_depth = 0;
    std::uint8_t descriptor = 0;
    bool rle = false;
    bool has_cmap = false;
    std::size_t cmap_first = 0;
    std::size_t cmap_entries = 0;
    std::size_t cmap_entry_bytes = 0;
    std::size_t cmap_off = 0;
    std::size_t pixel_off = 0;
    std::size_t data_end = 0;
};

// Full bounded structural validation. Returns none only when the bytes form
// a supported TGA container whose complete pixel/RLE extent is accounted for.
codec_error parse_layout(const std::uint8_t* data, std::size_t size,
                         layout& out) noexcept {
    out = layout{};
    if (data == nullptr || size < kHeaderSize) {
        return codec_error::unsupported;
    }
    if (size > kMaxEncodedBytes) {
        return codec_error::allocation_limit;
    }
    const std::uint8_t id_len = data[0];
    const std::uint8_t cmap_type = data[1];
    const std::uint8_t image_type = data[2];
    const std::uint8_t pixel_depth = data[16];
    const std::uint8_t descriptor = data[17];
    if (cmap_type > 1) {
        return codec_error::unsupported;
    }
    if (!is_cmap_type(image_type) && !is_truecolor_type(image_type) &&
        !is_gray_type(image_type)) {
        return codec_error::unsupported;  // image type 0/4-8/12+ are not accepted
    }
    if ((descriptor & 0xC0u) != 0) {
        return codec_error::unsupported;  // reserved descriptor bits must be 0
    }
    const std::uint32_t width = rd16(data + 12);
    const std::uint32_t height = rd16(data + 14);
    if (width == 0 || height == 0) {
        return codec_error::unsupported;
    }

    std::uint64_t p = kHeaderSize + id_len;
    if (p > size) {
        return codec_error::truncated;  // ID field beyond the buffer
    }

    std::size_t cmap_first = 0;
    std::size_t cmap_entries = 0;
    std::size_t cmap_entry_bytes = 0;
    std::size_t cmap_off = 0;
    if (cmap_type == 1) {
        if (!is_cmap_type(image_type)) {
            return codec_error::unsupported;  // a color map is only valid for cmap types
        }
        cmap_first = rd16(data + 3);
        cmap_entries = rd16(data + 5);
        const std::uint8_t entry_bits = data[7];
        if (cmap_entries == 0) {
            return codec_error::unsupported;
        }
        if (entry_bits != 15 && entry_bits != 16 && entry_bits != 24 && entry_bits != 32) {
            return codec_error::unsupported;
        }
        cmap_entry_bytes = (static_cast<std::size_t>(entry_bits) + 7u) / 8u;
        cmap_off = static_cast<std::size_t>(p);
        const std::uint64_t cmap_bytes =
            static_cast<std::uint64_t>(cmap_entries) * cmap_entry_bytes;
        if (static_cast<std::uint64_t>(p) + cmap_bytes > size) {
            return codec_error::truncated;
        }
        p += cmap_bytes;
    } else if (is_cmap_type(image_type)) {
        return codec_error::unsupported;  // color-mapped type requires a color map
    }

    if (is_cmap_type(image_type)) {
        if (pixel_depth != 8 && pixel_depth != 16) {
            return codec_error::unsupported;
        }
    } else if (is_truecolor_type(image_type)) {
        if (pixel_depth != 15 && pixel_depth != 16 && pixel_depth != 24 &&
            pixel_depth != 32) {
            return codec_error::unsupported;
        }
    } else {
        if (pixel_depth != 8 && pixel_depth != 16) {
            return codec_error::unsupported;
        }
    }
    const std::uint32_t bpp = (static_cast<std::uint32_t>(pixel_depth) + 7u) / 8u;
    const std::uint64_t pixel_count =
        static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    if (pixel_count == 0) {
        return codec_error::unsupported;
    }
    if (pixel_count > kMaxDecodedBytes / 4u) {
        return codec_error::dimension_limit;  // w*h*4 would exceed the decoded cap
    }
    const std::size_t pixel_off = static_cast<std::size_t>(p);
    const bool rle = (image_type == 9 || image_type == 10 || image_type == 11);
    std::size_t data_end = 0;
    if (!rle) {
        const std::uint64_t pixel_bytes = pixel_count * bpp;
        if (pixel_bytes > kMaxDecodedBytes) {
            return codec_error::allocation_limit;
        }
        if (static_cast<std::uint64_t>(pixel_off) + pixel_bytes > size) {
            return codec_error::truncated;
        }
        data_end = pixel_off + static_cast<std::size_t>(pixel_bytes);
    } else {
        std::size_t q = pixel_off;
        std::uint64_t filled = 0;
        while (filled < pixel_count) {
            if (q >= size) {
                return codec_error::truncated;
            }
            const std::uint8_t packet = data[q++];
            const std::uint64_t run =
                static_cast<std::uint64_t>(packet & 0x7Fu) + 1u;
            if (run > pixel_count - filled) {
                return codec_error::invalid_header;  // RLE overfills the image
            }
            if ((packet & 0x80u) != 0) {
                if (static_cast<std::uint64_t>(q) + bpp > size) {
                    return codec_error::truncated;
                }
                q += bpp;
            } else {
                const std::uint64_t bytes = run * bpp;
                if (static_cast<std::uint64_t>(q) + bytes > size) {
                    return codec_error::truncated;
                }
                q += static_cast<std::size_t>(bytes);
            }
            filled += run;
        }
        data_end = q;
    }

    // Optional TGA 2.0 footer + extension/developer areas.
    bool footer_present = false;
    std::size_t footer_off = 0;
    if (size >= kFooterSize) {
        const std::uint8_t* f = data + size - kFooterSize;
        if (std::memcmp(f + 8, "TRUEVISION-XFILE.", 17) == 0 && f[25] == 0) {
            footer_present = true;
            footer_off = size - kFooterSize;
        }
    }
    if (footer_present) {
        if (data_end > footer_off) {
            return codec_error::invalid_header;
        }
        const std::uint32_t ext_off = rd32(data + footer_off);
        const std::uint32_t dev_off = rd32(data + footer_off + 4);
        if (ext_off != 0) {
            if (static_cast<std::size_t>(ext_off) != data_end) {
                return codec_error::invalid_header;  // extension must follow pixels
            }
            if (static_cast<std::uint64_t>(ext_off) + kExtensionAreaSize > footer_off) {
                return codec_error::invalid_header;
            }
            if (rd16(data + ext_off) != kExtensionAreaSizeField) {
                return codec_error::invalid_header;
            }
        } else if (dev_off == 0) {
            if (data_end != footer_off) {
                return codec_error::invalid_header;  // unaccounted bytes before footer
            }
        }
        if (dev_off != 0) {
            if (dev_off < data_end || dev_off >= footer_off) {
                return codec_error::invalid_header;
            }
            if (static_cast<std::uint64_t>(dev_off) + 4u > footer_off) {
                return codec_error::invalid_header;
            }
            const std::uint32_t tags = rd32(data + dev_off);
            if (static_cast<std::uint64_t>(dev_off) + 4u +
                    static_cast<std::uint64_t>(tags) * 10u >
                footer_off) {
                return codec_error::invalid_header;
            }
        }
    } else if (data_end != size) {
        // Legacy TGA: the file must end exactly with the pixel/RLE extent.
        // Anything else is ambiguous and stays unsupported (never guessed).
        return codec_error::unsupported;
    }

    if (width > static_cast<std::uint32_t>(kMaxImageDimension) ||
        height > static_cast<std::uint32_t>(kMaxImageDimension)) {
        return codec_error::dimension_limit;
    }
    if (!decoded_size_within_limits(static_cast<std::int32_t>(width),
                                    static_cast<std::int32_t>(height))) {
        return codec_error::dimension_limit;
    }

    out.width = static_cast<std::int32_t>(width);
    out.height = static_cast<std::int32_t>(height);
    out.image_type = image_type;
    out.pixel_depth = pixel_depth;
    out.descriptor = descriptor;
    out.rle = rle;
    out.has_cmap = (cmap_type == 1);
    out.cmap_first = cmap_first;
    out.cmap_entries = cmap_entries;
    out.cmap_entry_bytes = cmap_entry_bytes;
    out.cmap_off = cmap_off;
    out.pixel_off = pixel_off;
    out.data_end = data_end;
    return codec_error::none;
}

struct rgba {
    std::uint8_t b = 0;
    std::uint8_t g = 0;
    std::uint8_t r = 0;
    std::uint8_t a = 255;
};

std::uint8_t expand5(std::uint32_t v) noexcept {
    return static_cast<std::uint8_t>((v << 3) | (v >> 2));
}

rgba unpack_16(std::uint16_t v, bool use_attribute) noexcept {
    rgba p;
    p.a = use_attribute ? ((v & 0x8000u) != 0 ? 0xFFu : 0x00u) : 0xFFu;
    p.r = expand5((v >> 10) & 0x1Fu);
    p.g = expand5((v >> 5) & 0x1Fu);
    p.b = expand5(v & 0x1Fu);
    return p;
}

std::uint8_t premultiply(std::uint8_t c, std::uint8_t a) noexcept {
    return static_cast<std::uint8_t>((static_cast<unsigned>(c) * a + 127u) / 255u);
}

int normalize_denominator(int den) noexcept {
    return (den == 1 || den == 2 || den == 4 || den == 8) ? den : 1;
}

// Reads the stored pixel stream in file order (row-major stored order, which
// the RLE packets span continuously) and applies color-map lookup + the
// canonical premultiplied BGRA mapping exactly once.
class pixel_reader {
public:
    pixel_reader(const std::uint8_t* data, const layout& lay) noexcept
        : data_(data), lay_(lay), pos_(lay.pixel_off) {}

    bool next(rgba& out) noexcept {
        if (!lay_.rle) {
            return read_raw(out);
        }
        if (run_remaining_ == 0) {
            if (pos_ >= lay_.data_end) {
                return false;
            }
            const std::uint8_t packet = data_[pos_++];
            const std::uint32_t run = static_cast<std::uint32_t>(packet & 0x7Fu) + 1u;
            if ((packet & 0x80u) != 0) {
                if (!read_raw(run_pixel_)) {
                    return false;
                }
                run_rle_ = true;
            } else {
                run_rle_ = false;
            }
            run_remaining_ = run;
        }
        --run_remaining_;
        if (run_rle_) {
            out = run_pixel_;
            return true;
        }
        return read_raw(out);
    }

private:
    bool read_raw(rgba& out) noexcept {
        const std::uint32_t bpp = (static_cast<std::uint32_t>(lay_.pixel_depth) + 7u) / 8u;
        if (pos_ + bpp > lay_.data_end) {
            return false;
        }
        const std::uint8_t* p = data_ + pos_;
        pos_ += bpp;
        if (lay_.has_cmap) {
            std::uint32_t index = 0;
            if (lay_.pixel_depth == 8) {
                index = p[0];
            } else {
                index = rd16(p);
            }
            if (index < lay_.cmap_first) {
                return false;
            }
            const std::uint64_t rel =
                static_cast<std::uint64_t>(index) - lay_.cmap_first;
            if (rel >= lay_.cmap_entries) {
                return false;
            }
            const std::uint8_t* e =
                data_ + lay_.cmap_off + static_cast<std::size_t>(rel) * lay_.cmap_entry_bytes;
            const bool use_attr = (lay_.descriptor & 0x0Fu) != 0;
            switch (lay_.cmap_entry_bytes) {
                case 2:
                    out = unpack_16(rd16(e), use_attr);
                    return true;
                case 3:
                    out.b = e[0];
                    out.g = e[1];
                    out.r = e[2];
                    out.a = 0xFF;
                    return true;
                default:  // 4
                    out.b = e[0];
                    out.g = e[1];
                    out.r = e[2];
                    out.a = use_attr ? e[3] : 0xFF;
                    return true;
            }
        }
        if (is_truecolor_type(lay_.image_type)) {
            const bool use_attr = (lay_.descriptor & 0x0Fu) != 0;
            switch (lay_.pixel_depth) {
                case 15:
                case 16:
                    out = unpack_16(rd16(p), use_attr);
                    return true;
                case 24:
                    out.b = p[0];
                    out.g = p[1];
                    out.r = p[2];
                    out.a = 0xFF;
                    return true;
                default:  // 32
                    out.b = p[0];
                    out.g = p[1];
                    out.r = p[2];
                    out.a = use_attr ? p[3] : 0xFF;
                    return true;
            }
        }
        // Grayscale.
        const std::uint8_t i = p[0];
        out.b = out.g = out.r = i;
        if (lay_.pixel_depth == 16) {
            out.a = ((lay_.descriptor & 0x0Fu) != 0) ? p[1] : 0xFF;
        } else {
            out.a = 0xFF;
        }
        return true;
    }

    const std::uint8_t* data_;
    const layout& lay_;
    std::size_t pos_;
    std::uint32_t run_remaining_ = 0;
    bool run_rle_ = false;
    rgba run_pixel_{};
};

}  // namespace

bool has_signature(const std::uint8_t* data, std::size_t size) noexcept {
    layout lay;
    return parse_layout(data, size, lay) == codec_error::none;
}

codec_error parse_header(const std::uint8_t* data, std::size_t size,
                         std::int32_t& oriented_w_out,
                         std::int32_t& oriented_h_out) noexcept {
    oriented_w_out = 0;
    oriented_h_out = 0;
    layout lay;
    const codec_error err = parse_layout(data, size, lay);
    if (err != codec_error::none) {
        return err;
    }
    oriented_w_out = lay.width;
    oriented_h_out = lay.height;
    return codec_error::none;
}

decode_result decode(const std::uint8_t* data, std::size_t size,
                     int scaling_denominator) noexcept {
    decode_result out;
    layout lay;
    const codec_error err = parse_layout(data, size, lay);
    if (err != codec_error::none) {
        out.error = err;
        return out;
    }
    const int den = normalize_denominator(scaling_denominator);
    const std::int32_t w = lay.width;
    const std::int32_t h = lay.height;
    if (!decoded_size_within_limits(w, h)) {
        out.error = codec_error::dimension_limit;
        return out;
    }
    const bool top_down = (lay.descriptor & 0x20u) != 0;
    const bool right_left = (lay.descriptor & 0x10u) != 0;

    if (den == 1) {
        decoded_image img;
        img.codec = codec_id::tga;
        img.scale_den = 1;
        img.src_width = w;
        img.src_height = h;
        img.width = w;
        img.height = h;
        std::size_t bytes = 0;
        if (!checked_stride_and_size(w, h, img.stride, bytes)) {
            out.error = codec_error::allocation_limit;
            return out;
        }
        try {
            img.pixels.resize(bytes);
        } catch (...) {
            out.error = codec_error::allocation_limit;
            return out;
        }
        pixel_reader reader(data, lay);
        rgba px;
        for (std::int32_t sy = 0; sy < h; ++sy) {
            const std::int32_t dy = top_down ? sy : h - 1 - sy;
            for (std::int32_t sx = 0; sx < w; ++sx) {
                if (!reader.next(px)) {
                    out.error = codec_error::invalid_header;
                    out.detail = "tga pixel stream ended early";
                    return out;
                }
                const std::int32_t dx = right_left ? w - 1 - sx : sx;
                std::uint8_t* dst =
                    img.pixels.data() + static_cast<std::size_t>(dy) * img.stride +
                    static_cast<std::size_t>(dx) * 4u;
                dst[0] = premultiply(px.b, px.a);
                dst[1] = premultiply(px.g, px.a);
                dst[2] = premultiply(px.r, px.a);
                dst[3] = px.a;
            }
        }
        out.image = std::move(img);
        out.error = codec_error::none;
        return out;
    }

    // Streaming box downsample: accumulate premultiplied components per
    // scaled pixel so no full-size intermediate allocation is required.
    const std::uint64_t sw64 =
        (static_cast<std::uint64_t>(w) + static_cast<std::uint64_t>(den) - 1) /
        static_cast<std::uint64_t>(den);
    const std::uint64_t sh64 =
        (static_cast<std::uint64_t>(h) + static_cast<std::uint64_t>(den) - 1) /
        static_cast<std::uint64_t>(den);
    if (sw64 == 0 || sh64 == 0 ||
        sw64 > static_cast<std::uint64_t>(kMaxImageDimension) ||
        sh64 > static_cast<std::uint64_t>(kMaxImageDimension)) {
        out.error = codec_error::allocation_limit;
        return out;
    }
    const std::int32_t sw = static_cast<std::int32_t>(sw64);
    const std::int32_t sh = static_cast<std::int32_t>(sh64);
    if (!decoded_size_within_limits(sw, sh)) {
        out.error = codec_error::allocation_limit;
        return out;
    }
    const std::uint64_t cells = static_cast<std::uint64_t>(sw) * sh;
    std::vector<std::uint64_t> acc;
    std::vector<std::uint32_t> count;
    try {
        acc.assign(static_cast<std::size_t>(cells) * 4u, 0);
        count.assign(static_cast<std::size_t>(cells), 0);
    } catch (...) {
        out.error = codec_error::allocation_limit;
        return out;
    }

    pixel_reader reader(data, lay);
    rgba px;
    for (std::int32_t sy = 0; sy < h; ++sy) {
        const std::int32_t dy = top_down ? sy : h - 1 - sy;
        for (std::int32_t sx = 0; sx < w; ++sx) {
            if (!reader.next(px)) {
                out.error = codec_error::invalid_header;
                out.detail = "tga pixel stream ended early";
                return out;
            }
            const std::int32_t dx = right_left ? w - 1 - sx : sx;
            const std::size_t ox = static_cast<std::size_t>(dx / den);
            const std::size_t oy = static_cast<std::size_t>(dy / den);
            const std::size_t cell = oy * static_cast<std::size_t>(sw) + ox;
            acc[cell * 4u + 0] += premultiply(px.b, px.a);
            acc[cell * 4u + 1] += premultiply(px.g, px.a);
            acc[cell * 4u + 2] += premultiply(px.r, px.a);
            acc[cell * 4u + 3] += px.a;
            ++count[cell];
        }
    }

    decoded_image img;
    img.codec = codec_id::tga;
    img.scale_den = den;
    img.src_width = w;
    img.src_height = h;
    img.width = sw;
    img.height = sh;
    std::size_t bytes = 0;
    if (!checked_stride_and_size(sw, sh, img.stride, bytes)) {
        out.error = codec_error::allocation_limit;
        return out;
    }
    try {
        img.pixels.resize(bytes);
    } catch (...) {
        out.error = codec_error::allocation_limit;
        return out;
    }
    for (std::int32_t y = 0; y < sh; ++y) {
        for (std::int32_t x = 0; x < sw; ++x) {
            const std::size_t cell = static_cast<std::size_t>(y) *
                                         static_cast<std::size_t>(sw) +
                                     static_cast<std::size_t>(x);
            const std::uint32_t n = count[cell] == 0 ? 1u : count[cell];
            std::uint8_t* dst = img.pixels.data() +
                                static_cast<std::size_t>(y) * img.stride +
                                static_cast<std::size_t>(x) * 4u;
            dst[0] = static_cast<std::uint8_t>(acc[cell * 4u + 0] / n);
            dst[1] = static_cast<std::uint8_t>(acc[cell * 4u + 1] / n);
            dst[2] = static_cast<std::uint8_t>(acc[cell * 4u + 2] / n);
            dst[3] = static_cast<std::uint8_t>(acc[cell * 4u + 3] / n);
        }
    }
    out.image = std::move(img);
    out.error = codec_error::none;
    return out;
}

}  // namespace tga

bool has_tga_signature(const std::uint8_t* data, std::size_t size) noexcept {
    return tga::has_signature(data, size);
}

}  // namespace oz::viewer_core

// tga.hpp - Common Raster Decode Profile v1 Phase B bounded native TGA
// adapter. Internal to ozviewer_core: the registry in codec.cpp is the only
// caller. Not a public header.
//
// TGA has no universal leading magic, so this adapter provides a strict,
// bounded *structural discriminator* (never extension selection):
// - the optional TGA 2.0 footer `TRUEVISION-XFILE.\0` is recognized where
//   present, with extension/developer offset validation; or
// - a legacy (footerless) file is accepted only when the full header,
//   color-map, pixel-depth, image-type, dimension, origin and complete
//   pixel/RLE walk account for the file boundary exactly.
// Unknown/ambiguous bytes are never claimed.
//
// Contract (mirrors the Phase A raster adapters):
// - No COM/WIC is used at all: TGA is fully decoded by this bounded native
//   adapter (there is no WIC TGA container and no extension-only fallback).
// - parse_header() performs the full bounded structural pre-parse and never
//   allocates pixels; decode() repeats it before allocating output.
// - Canonical output is top-down SDR sRGB premultiplied BGRA8, with bottom/
//   top and left/right origins normalized and alpha applied exactly once.
// - All failures map to stable generic codec_error categories.
#pragma once

#include "oz/viewer_core/codec.hpp"

namespace oz::viewer_core::tga {

// Strict bounded structural discriminator. True only when the bytes form a
// supported, fully bounded TGA container (footer or exact legacy fit).
bool has_signature(const std::uint8_t* data, std::size_t size) noexcept;

// Bounded TGA header parse (no pixel allocation). Returns the stored,
// origin-normalized dimensions (width/height are the same either way; the
// origin bits are reflected in decode only).
codec_error parse_header(const std::uint8_t* data, std::size_t size,
                         std::int32_t& oriented_w_out,
                         std::int32_t& oriented_h_out) noexcept;

// Full bounded native TGA decode to canonical top-down PBGRA8 at the given
// denominator (1/2/4/8; other values fall back to 1). No COM/WIC, no global
// mutable state, no encoder.
decode_result decode(const std::uint8_t* data, std::size_t size,
                     int scaling_denominator) noexcept;

}  // namespace oz::viewer_core::tga

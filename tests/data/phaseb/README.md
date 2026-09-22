# Phase B test fixtures — provenance, hashes, license

All files in this directory are CHECKED-IN decoder-independent bitstreams for
Common Raster Decode Profile v1 Phase B (CUR deterministic selection, native
bounded TGA decode). Tests decode these bytes with NO test-time encoder
dependency.

## Generated fixtures (dependency-free writer, one-shot offline)

Produced once offline by a dependency-free Python writer (standard library
`struct`/`zlib`/`hashlib` only: TGA 1.0/2.0 headers + uncompressed/RLE
true-color, grayscale, color-map payloads; CUR `00 00 02 00` directories with
DIB and PNG payloads; a minimal PNG RGBA writer for the PNG-in-CUR payload).
The generator script was not committed and the build never links an encoder.
No third-party source, no redistributable license, no attribution required:
these bytes are original to this repository (provenance: generated
2026-09-13 for Phase B).

| File | SHA-256 | Bytes | Content |
| ---- | ------- | ----- | ------- |
| `opaque_4x3.tga` | `75f28a0a5c13ef70b576c18e7d5ac610421b38e7f0aeb79b8904be91c3d783c9` | 54 | 24-bit BI_RGB-equivalent true-color 4x3, bottom-left origin |
| `topdown_4x3.tga` | `f5f806b0dabe13f51001d09052dedfe701cdbd6f816764c9278358aa45c39eba` | 54 | same canonical image, top-down origin (descriptor 0x20) |
| `rle_4x3.tga` | `edf197c76b634bb17d3ff912649bfbbf07d5982cc4adad57b7958f7f2e22664d` | 55 | RLE true-color (image type 10), bottom-left origin |
| `cmap_2x2.tga` | `86693b97d497f524592e0fcc032d21b469c850904544cd7a2490589c57ad5f0b` | 34 | 8-bit color-mapped (type 1), 24-bit palette |
| `cmap_alpha_2x2.tga` | `16e00ccd9a6435ca9cd2c047494533cae4f51c64c3fa46a769d418182b04f6f5` | 38 | color-mapped, 32-bit palette, attribute bits 8 |
| `gray_4x3.tga` | `a9c50da4d9ae8660216a71d6e4d614287abedb38683421ba09bb1f8a56ea733b` | 30 | 8-bit grayscale (type 3), top-down |
| `gray16_2x2.tga` | `c37a39183b7a6bba373b7f1632d3db5cdb9edbd47b88109d22955f334cc3ec0d` | 26 | 16-bit grayscale intensity+alpha, attribute bits 8 |
| `alpha_2x2.tga` | `f09fa742f0eb9e4a2acccd6c8c537ce295c9ddebfd14c73909394f58c4b5b97f` | 34 | 32-bit true-color BGRA, attribute bits 8 |
| `packed16_2x2.tga` | `d411267d8fb2e5709388bb23f127eb9109d6172a2c46e4355affd73cf865bf33` | 26 | 16-bit true-color ARGB1555 packing, attribute bit |
| `footer_ext_4x3.tga` | `6a4b2efba72e18ef2f0e62ddce12186fdf4885f914cf0deccad019f40cdae702` | 575 | valid TGA 2.0 footer + 495-byte extension area |
| `bench_64x48.tga` | `4aaf43e30204778946cee1937da44d7da563162fc8514bba585a60d392af6bc2` | 9234 | 64x48 true-color (`--benchmark` marker fixture) |
| `png_2x2.cur` | `ec29d899e50b9305d07e13ae2bc73680f398b60084203c5b4ac0f373af4133da` | 99 | CUR with embedded PNG payload, nonzero hotspot |
| `dib_2x2.cur` | `858997f0c88a399f57d7da05b83ff6344dcfbec25a371e52ad86b8f9ceac414c` | 86 | CUR with 32bpp DIB + AND mask, nonzero hotspot |
| `multi_16_32.cur` | `9a86ab3309dc2dd5edba2c157f3243bdbe2439c3fb5fd68e81c72989121bb72f` | 5174 | 16x16 24bpp + 32x32 32bpp (selects 32x32) |
| `depth_tie_16.cur` | `5b61d8ca7092398c6d512916e3fcbe1d705740f7e41f732f44b64c31063a429f` | 2294 | two 16x16 entries, 8bpp vs 24bpp (higher derived depth wins) |
| `bench_32x32.cur` | `4fda9e4d77a3f1c0cb2ee70cd531ee77ad8a6a20551cf4c8f9c938d0af9a2f02` | 4286 | single 32x32 DIB entry (`--benchmark` marker fixture) |

The small-fixture bytes are embedded in `tests/phaseb_fixtures.hpp`
(arrays `k_<name>`) so unit tests are self-contained; the files on disk serve
the `--benchmark` marker tests and hash verification.

## Supported TGA features

- Image types 1/2/3 (uncompressed color-mapped / true-color / grayscale) and
  9/10/11 (their RLE forms).
- Index depth 8 or 16 (color-mapped); color-map entries 15/16/24/32-bit.
- True-color pixel depth 15/16/24/32 (BGR/BGRA, ARGB1555 packing).
- Grayscale pixel depth 8 or 16 (intensity, intensity+alpha).
- Image descriptor alpha bits (0-8), left/right origin (bit 4), bottom/top
  origin (bit 5). Alpha premultiplied exactly once into canonical PBGRA8.
- Optional TGA 2.0 footer `TRUEVISION-XFILE.\0` with validated 495-byte
  extension area and developer-directory offsets (kept as ignored metadata).

## Explicitly rejected TGA variants (precision over corruption)

- Image type 0 (no image) and types 4-8/12+ (compressed color-map, compressed
  true-color with different headers, etc.): rejected, not guessed.
- Unknown pixel depths, a color map on a non-color-mapped type, or a
  color-mapped type without a color map.
- RLE streams that do not fill exactly `width*height` pixels (overfill or
  underfill) or that run past the container.
- Legacy (footerless) files whose complete pixel/RLE extent does not end
  exactly at the file boundary; unaccounted bytes before a footer; corrupt
  extension/developer offsets. Such bytes stay `unsupported`.
- Palette indexes outside the declared color-map range: the structurally valid
  container is rejected as an invalid header rather than corrupting output.

## Scope notes

- CUR follows the same deterministic "largest safe image" policy as ICO
  (largest area, then derived/validated bit depth, then directory order); the
  cursor hotspot words are metadata only and never treated as ICO planes/bpp.
  Only the selected payload slice reaches an explicit in-box WIC decoder
  (PNG via `GUID_ContainerFormatPng`, DIB via a bounded BITMAPFILEHEADER
  wrapper + `GUID_ContainerFormatBmp` with the AND mask applied).
- TGA uses no COM/WIC at all: it is a bounded native adapter.
- No encoder, plugin, RAW, vector, or extension-based selection exists.

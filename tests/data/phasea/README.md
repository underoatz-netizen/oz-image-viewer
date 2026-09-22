# Phase A test fixtures — provenance, hashes, license

All files in this directory are CHECKED-IN decoder-independent bitstreams
for Common Raster Decode Profile v1 Phase A (BMP, static GIF, classic TIFF
page 0, ICO deterministic selection). Tests decode these bytes with NO
test-time encoder dependency.

## Generated fixtures (dependency-free writer, one-shot offline)

Produced once offline by a dependency-free Python writer (BITMAPFILEHEADER +
known DIB sizes, GIF89a + GCT + single image descriptor with table-driven
LZW, classic little-endian TIFF with contiguous uncompressed strips, ICO
directory + 32bpp DIB payloads), then checked in. The generator script was
not committed; the build never links an encoder. No third-party source,
no redistributable license, no attribution required: these bytes are
original to this repository (provenance: generated 2026-09-13 for Phase A).

| File | SHA-256 | Bytes | Content |
| ---- | ------- | ----- | ------- |
| `opaque_4x3.bmp` | `1ffd40735e42f98cbaf3bebbab1ec30aae7aa7f3c6f6ae37034357d1f520d41d` | 90 | 24-bit BI_RGB 4x3 gradient |
| `alpha_2x2.bmp` | `97efaba9ef4e965e3e4b0ad5b0307dbd8dcae575bd816a72fb53ece11394b43f` | 70 | 32-bit 2x2 with transparency (canonical PBGRA contract) |
| `bench_64x48.bmp` | `88654069118dd87e929c40af4d26b0dd412ea1e19a4e8e92651e66a394e731cd` | 9270 | 24-bit 64x48 gradient (`--benchmark` marker fixture) |
| `static_2x2.gif` | `36f0da6154c0668f11f2c4db85a90fac5d529550507a5fa2bea6391311290e81` | 50 | static GIF89a 2x2 + transparency extension, one image descriptor |
| `opaque_4x3.gif` | `0c3fcc7d3ba45bcdfdfbf12f4c05b86e1047d069403d7ed178c36059c2008d10` | 44 | static 4x3 gradient, one image descriptor |
| `bench_64x48.gif` | `2e480124ec6de797cecfcda3a20ee7b83224c1939aeb4759d75595135575648c` | 197 | static 64x48 gradient (`--benchmark` marker fixture) |
| `animated_2x2.gif` | `163fa2c6477bc6da5c0b87e9e297eb77d63612ef97987164c26c63909f78ba3b` | 58 | two image descriptors (decoder must reject, never frame 0) |
| `opaque_4x3.tif` | `a73ae0fee334436462fcfa64b3fcfd4aac16984cfba0f2c751cacdfa5a31de09` | 172 | classic little-endian 4x3, one IFD, uncompressed strips |
| `bench_64x48.tif` | `93d3275aa2965f7598919aca4bf06987637c1dfb4c60adbd604be0f28926261b` | 9352 | classic 64x48 (`--benchmark` marker fixture) |
| `multipage_4x3.tif` | `783232d7cb01926ee168261f54b0c982cbddb30067d6ea713c4b1db17781ed79` | 202 | two-IFD classic TIFF (page-0 policy: renders first IFD only) |
| `bigtiff_4x3.tif` | `ffd5e675646f929bf057d8e571692a44abbf4b56f7309ca120cb6ba991260074` | 24 | BigTIFF magic 43 (decoder must reject as unsupported) |
| `raw_dng_4x3.tif` | `5792a215984fac540fcb0f4fe3782c95a8c2ad4dfc71be7d96ea20c4775b67db` | 188 | classic TIFF + DNGVersion tag (RAW-gated: must reject, no preview) |
| `loop_4x3.tif` | `f1b3769880cb4a478e5d5fd1ede4f94e2d54ea356bad2b56ad18915178585ad5` | 172 | IFD next-offset loop (must reject as invalid header) |
| `orient_o6_4x3.tif` | `e71914a8445a4ec9f46a67e3ca761fef8720a7bdeab0d688ae69d2d5d48176fb` | 184 | classic 4x3 stored + EXIF orientation 6 (3x4 shown, applied on decode) |
| `multi_16_32.ico` | `8ba591200d7d6d203329a9b1d7b822c2bf177953a9b2256c1fbf0cd70c41e1c1` | 5430 | 16x16 + 32x32 entries (selection must pick 32x32) |
| `bench_32x32.ico` | `288711f8706a4afe74cb0de47367dfc29391eda460adb0833c90a4e5cb56acbb` | 4286 | single 32x32 entry (`--benchmark` marker fixture) |
| `tie_16x16.ico` | `3e876019b7267ed15599363a35c85fe5dfdd0c82b2e0ce53a1a776f5de4d31e8` | 2038 | two 16x16 entries, 24bpp vs 32bpp DIB (tie-break: higher bit depth wins) |

The same small-fixture bytes are embedded in `tests/phasea_fixtures.hpp`
(arrays `k_<name>`) so unit tests are self-contained; the files on disk
serve the `--benchmark` marker tests and hash verification.

## Scope notes

- CUR (type 2) and TGA are Phase B and have their own fixtures under
  `tests/data/phaseb/`; this directory is intentionally Phase A only.
- TIFF fixtures cover ordinary classic TIFF plus the RAW-gated DNGVersion
  indicator; unrecognized RAW variants remain `unsupported` without claiming
  exhaustive identification.
- WIC semantic reproducibility: pixels come from the in-box Windows WIC
  container decoders after the byte-level structural gate; identical bytes
  on the same Windows build decode identically. No third-party WIC codec
  is used.

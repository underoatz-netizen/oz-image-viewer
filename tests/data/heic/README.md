# HEIC / HEIF test fixtures (Stage 3.4) — provenance, hashes, license

All files in this directory are CHECKED-IN decoder-independent HEIC/HEIF bitstreams.
Tests decode these bytes with NO test-time encoder dependency.

## Fixture inventory and provenance

The fixtures below are selected from the attributable upstream public test suite of
libheif 1.23.2 (v1.23.2, strukturag/libheif, LGPL-3.0-only, verified identical
to the acquired vcpkg baseline `52d80838fb40c755b1615fbc9c7b994a33742a22` source tree
under `buildtrees/libheif/src/v1.23.2-5aa17603d3.clean/`).

| File | SHA-256 | Bytes | Content / Test purpose |
| ---- | ------- | ----- | --------------------- |
| `rainbow-451x461.heic` | `4b2ce727f093944975f143ba2b39c4c64511b766d94552f8d51a755916e7f983` | 7080 | 8-bit SDR sRGB 451x461 opaque still image (`--benchmark` integration fixture) |
| `with-alpha-512x512.heic` | `dac399d3bf1019baaf5f88eef8b277087d0643e735db947c42355237bb9d0221` | 8284 | 8-bit SDR sRGB 512x512 still image with alpha auxiliary item |
| `colors-no-alpha.heic` | `76f82ffc717a647b1c9c2551e5ea0545832a2d3216c7540f7e5b092282a04b63` | 499 | 8-bit SDR sRGB 32x32 minimal opaque still |
| `colors-with-alpha.heic` | `cbe33c0e3efe098570787a9dcded1061436f9d06b09cd5b2afd0d02eacce527b` | 817 | 8-bit SDR sRGB 32x32 minimal still with alpha |
| `colors-with-alpha-thumbnail.heic` | `adfe0a2782891979af1f1fc4a9b90c33fbec060e73f38697e3b79a2d52c9107c` | 2060 | 8-bit SDR sRGB still image with thumbnail |
| `example.heic` | `7f8b363e4936c0666a25f64f3a92fda10bd8e5453be4592530b65a55dd98f3f2` | 718114 | Full photo still image with EXIF orientation metadata |
| `hevc32.heif` | `7076e79a3a6f7ed9c09089fbe3a7608eb6c044fa34de900f3e3467704f36bcb7` | 523 | HEIF 32x32 still with `heic` brand (valid still decode) |
| `seq_seed.heif` | `a5abddf977aba0c950b5d512c173482f5df0d1d76c7cfe567114dcafc6ab9f1c` | 1064 | HEIF sequence track (`msf1` brand; must be rejected by brand routing) |
| `uncompressed_comp_B16R16G16.heic` | `bd73426262f9a6f6bac73cabc33d0f0bd438651d5c1925c8b245f4218c33c031` | 4004 | 16-bit uncompressed (must be rejected; 8-bit SDR contract only) |
| `uncompressed_pix_B16R16G16.heic` | `0de9b0d862e1f50627a6f80a00e5475ee0b04eab2e481f2ca2ce0c9582175c30` | 4004 | 16-bit uncompressed pixel (must be rejected; 8-bit SDR contract only) |

## Upstream copyright and license notice

Copyright 2017-2024 struktur AG, Dirk Farin.
SPDX-License-Identifier: LGPL-3.0-only
(See `THIRD-PARTY-NOTICES.md` for full license grant).

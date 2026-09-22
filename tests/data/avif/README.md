# AVIF test fixtures (Stage 3.3) — provenance, hashes, license

All files in this directory are CHECKED-IN decoder-independent AVIF bitstreams.
Tests decode these bytes with NO test-time encoder dependency.

## Fixture inventory and provenance

The fixtures below are selected from the attributable upstream public test suite of
libavif 1.4.2 (v1.4.2, AOMediaCodec/libavif, BSD-2-Clause AND Apache-2.0, verified identical
to the acquired vcpkg baseline `52d80838fb40c755b1615fbc9c7b994a33742a22` source tree
under `buildtrees/libavif/src/v1.4.2-ed6d068be2.clean/tests/data/`).

| File | SHA-256 | Bytes | Content / Test purpose |
| ---- | ------- | ----- | --------------------- |
| `white_1x1.avif` | `ea4e43d1f07e4c00de16c13afa32376111bb306e51f08212cf4c1b6064df3667` | 305 | 8-bit SDR sRGB 1x1 white still image (`--benchmark` integration fixture) |
| `colors_sdr_srgb.avif` | `24463b9b79e4624d06e247079efddf624f9062c516d43ddf6fe5e150343629d6` | 18845 | 8-bit SDR sRGB still image (standard SDR sRGB contract) |
| `circle_custom_properties.avif` | `6c57595c1b814392c6a0d0e1f60e34c6f7f09a8ce7e46885d85059b7205e82fb` | 1065 | 8-bit SDR still image with custom properties |
| `abc_color_irot_alpha_irot.avif` | `b371cc88244a873131e4d10ff9363d71ce4f41cf333bd4a491b38d970d9abd3b` | 10597 | 8-bit SDR with alpha and irot container transform |
| `paris_icc_exif_xmp.avif` | `961bc38b61e60b7651fa20efa24269ae2f35e4958822a81c908c9bbf9b3f66e1` | 21132 | 8-bit SDR with embedded ICC profile, EXIF, and XMP metadata |
| `sofa_grid1x5_420.avif` | `c9e04ff9d90d7093454750fa33b7543ee5479e0cfb151e2c3d2ce6a16c1651c1` | 25409 | Single composed primary item from 1x5 image grid |
| `colors-animated-8bpc.avif` | `2f8683d21725261f37f86e115f0c212cc52d0fefd3a2ddfcc4fa648c1859906d` | 1235 | Animated AVIF sequence (`avis` brand; must be rejected) |
| `colors_hdr_srgb.avif` | `1aecb78d6d363caae95a5dd198f347ce4b200073ced638647364a2ad04d9707d` | 28030 | HDR AVIF (must be rejected; no silent tone mapping) |
| `colors_text_wcg_hdr_rec2020.avif` | `26cfca294b9403133eba46fdbf15ab04f68e48855c835be8cbec24097aa8b846` | 33242 | Wide-gamut Rec.2020 / HDR image (must be rejected) |
| `seine_hdr_gainmap_srgb.avif` | `9bf9c6a7606951de07e4079cd63c2cfe379d95139cd99ab9142d8a6ee22d28c7` | 122961 | Gainmap auxiliary image (must be rejected) |
| `alpha_noispe.avif` | `8deb96e78c3e5d608a157b2de4c98eb1a30e0c85736b4230758400509c88d47e` | 1403 | Missing essential `ispe` property in alpha auxiliary item (must be rejected under strict policy) |

Small fixture bytes are also embedded in `tests/avif_fixtures.hpp` for self-contained unit tests.

## Upstream copyright and license notice

Copyright 2019 Joe Drago. All rights reserved.
SPDX-License-Identifier: BSD-2-Clause AND Apache-2.0
(See `THIRD-PARTY-NOTICES.md` for full license grant).

# WebP test fixtures (Stage 3.2) — provenance, hashes, license

All files in this directory are CHECKED-IN decoder-independent WebP
bitstreams. Tests decode these bytes with NO test-time encoder dependency.

## Generated fixtures (pinned libwebp 1.6.0 encoder/mux, one-shot offline)

Produced once with the pinned libwebp 1.6.0 encoder/mux from vcpkg baseline
`52d80838fb40c755b1615fbc9c7b994a33742a22`, then checked in. The generator
tool was deleted after use; the build never links an encoder.

| File | SHA-256 | Bytes | Content |
| ---- | ------- | ----- | ------- |
| `lossy_4x3.webp` | `eeaf302012b9b10d23c580ebdcd0fc59574f6267cb5c62f2870ff9f182d2d2cb` | 112 | lossy 4x3 gradient (quality 80) |
| `lossless_4x3.webp` | `95014f971a4d9369d688b2030de2204c4aaec41baecc2fb4d986eff86c05d14d` | 70 | lossless 4x3 gradient (exact) |
| `alpha_2x2.webp` | `5d00a52cd56fedbed8ee1947a7e17fac3375738fdae93444fc22fb4c32541ec0` | 50 | lossless 2x2: opaque red, half white, transparent green, opaque black |
| `icc_bogus_4x4.webp` | `493e7490766cae39613d3887041868f2bb66926d2f21613af4c563e22a350207` | 108 | lossless 4x4 + bogus 16-byte ICCP (safe sRGB fallback) |
| `exif_o6_3x2.webp` | `c464bf1954028cdab2cd82c420e4c0491be5e62f034d641710c3a1ee33adce88` | 102 | lossless 3x2 distinct pixels + EXIF orientation 6 (3x2 stored, 2x3 shown) |
| `animated_2x2.webp` | `ea2e975c5e4703ab7e4d7f32173f80b444b2077883665c20ebea549072995cae` | 140 | animated 2x2 two-frame loop (decoder must reject, never frame 0) |
| `bench_64x48.webp` | `a8b66f08f979dab885da91d350ba7a1b496a6ddb3a0a1de86fa0b73e39044dc7` | 82 | lossless 64x48 gradient (`--benchmark` marker fixture) |

The same bytes are embedded in `tests/webp_fixtures.hpp` (arrays `k_<name>`)
so unit tests are self-contained; the `.webp` files on disk serve the
`--benchmark` marker test and hash verification.

## Upstream fixture (verbatim, compatible attribution)

| File | SHA-256 | Bytes | Source |
| ---- | ------- | ----- | ------ |
| `upstream_test.webp` | `1bf9449d529b491c6eb22ec84d42217e43cf9af0ffe391fa7a30b29b037f098a` | 4880 | libwebp `examples/test.webp` at v1.6.0 (verified identical to the acquired vcpkg source `buildtrees/libwebp/src/v1.6.0-*/examples/test.webp`) |

Copyright (c) 2010, Google Inc. All rights reserved. BSD-3-Clause
(see `COPYING` in the acquired libwebp source and `THIRD-PARTY-NOTICES.md`
at the repo root). Patent grant: libwebp `PATENTS` ("Additional IP Rights
Grant (Patents)" for the WebM codecs) applies; see `THIRD-PARTY-NOTICES.md`.

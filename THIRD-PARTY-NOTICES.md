# Third-party notices — OZ image viewer

## libheif 1.23.2 (Stage 3.4 dynamic HEIC/HEIF decode)

- **Source**: strukturag/libheif at `v1.23.2`, acquired via the pinned vcpkg
  baseline `52d80838fb40c755b1615fbc9c7b994a33742a22` (port `libheif` `1.23.2`,
  `default-features: false`, no encoder/x265/AOM/JPEG/OpenJPEG features).
  Source code repository and exact release: https://github.com/strukturag/libheif/releases/tag/v1.23.2
  Verified after acquisition: source SHA-512 `d34ccd40d7f7cc48ff8ceddafd8f7c7004ab0df2a657237fcc715c4bcc453a41946d7db542d32b781875868ee116c451b4b63daaf073530245ec7b117ae60e27`
  per `ports/libheif/portfile.cmake`, version `1.23.2`, license `LGPL-3.0-only`.
- **Linkage**: dynamic linkage via standalone `heif.dll`. The OZ image viewer executable
  (`ozviewer.exe`) links NO import library for libheif (zero PE import table entries).
  `heif.dll` is dynamically loaded at runtime on demand via `LoadLibraryExW` with the
  `LOAD_LIBRARY_SEARCH_APPLICATION_DIRECTORY` flag only.
  **DLL replacement**: Users may freely modify, recompile, or replace `heif.dll` alongside
  `ozviewer.exe` with any ABI-compatible build under the LGPL v3 terms.
- **License**: GNU Lesser General Public License version 3.0 (LGPL-3.0-only).
  See full text at https://www.gnu.org/licenses/lgpl-3.0.txt
  Portions copyright (c) 2017-2024 struktur AG, Dirk Farin.
- **HEVC / Patent Notice**: HEIC still-image decode utilizes the HEVC video standard
  (ITU-T H.265 / ISO/IEC 23008-2). Distributed under freeware/donationware terms
  with open-source software decoder (`libde265`).

## libde265 1.1.1 (Stage 3.4 HEVC decoder dependency for libheif)

- **Source**: strukturag/libde265 at `v1.1.1`, acquired via the pinned vcpkg
  baseline `52d80838fb40c755b1615fbc9c7b994a33742a22` (port `libde265` `1.1.1`).
  Source code repository and exact release: https://github.com/strukturag/libde265/releases/tag/v1.1.1
  Verified after acquisition: source SHA-512 `fb2207f5a3ba901853f61f345c72130f000134918febbc4f3529c3d289fc79ee7457b3e61660110f698bb4ac15d62426e284034bf870bfbd1859ab3feaa52be8`
  per `ports/libde265/portfile.cmake`, version `1.1.1`, license `LGPL-3.0-only`.
- **Linkage**: dynamic linkage via standalone `libde265.dll` (dependency of `heif.dll`).
  `ozviewer.exe` links NO import library for libde265.
  **DLL replacement**: Users may freely modify, recompile, or replace `libde265.dll` alongside
  `ozviewer.exe` under the LGPL v3 terms.
- **License**: GNU Lesser General Public License version 3.0 (LGPL-3.0-only).
  See full text at https://www.gnu.org/licenses/lgpl-3.0.txt
  Portions copyright (c) 2013-2024 struktur AG, Dirk Farin.

- **Test-data attribution**:
  Bitstreams in `tests/data/heic/` are attributable to libheif 1.23.2 upstream test suite (`strukturag/libheif`),
  verified identical to the acquired vcpkg baseline source tree (`buildtrees/libheif/src/v1.23.2-5aa17603d3.clean/`).
  All fixtures are checked-in decoder-independent bitstreams with verified SHA-256 hashes (see `tests/data/heic/README.md`);
  no test links an encoder.

## libavif 1.4.2#1 (Stage 3.3 static AVIF decode)

- **Source**: AOMediaCodec/libavif at `v1.4.2`, acquired via the pinned vcpkg
  baseline `52d80838fb40c755b1615fbc9c7b994a33742a22` (port `libavif`
  `1.4.2#1`, `default-features: false`, features `["dav1d"]`).
  Verified after acquisition: source SHA-512 `cbf31827884058acc54d3b1fa0f2059f022691609a76e0f913981d5b8c1be60f52069a25b61c888c09e963161d1f4c9bf692c0716ab0ee067a4ccaa4e36d9ce1`
  per `ports/libavif/portfile.cmake`, version `1.4.2`, license `BSD-2-Clause AND Apache-2.0`.
- **Linkage**: static libraries, dynamic CRT
  (`triplets/oz-x64-windows-static-md.cmake`: x64, `VCPKG_CRT_LINKAGE
  dynamic`, `VCPKG_LIBRARY_LINKAGE static`). Linked target: `avif`
  (static library `avif.lib` backed statically by `dav1d.lib` and `yuv.lib`).
  No AVIF DLL, delay-load, `LoadLibrary`, encoder, or generic fallback.
- **License** (from the acquired source `LICENSE`):

  ```text
  Copyright 2019 Joe Drago. All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

  1. Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

  2. Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
  ```

- **dav1d 1.5.4 dependency**:
  - **Source**: VideoLAN/dav1d at `1.5.4` (`ports/dav1d/portfile.cmake`, SHA-512 `7ee5906640495919462b2242c44a8c3cc577fdda52fc25257792f6df919429d54e4a48c315a7a11759385f044f68d3d6573fd720853f447e2d8520a13693827f`).
  - **License**: BSD-2-Clause AND ISC (VideoLAN, FFmpeg developers). Static link.

- **libyuv 1916 dependency**:
  - **Source**: Chromium/libyuv at commit `d98915a654d3564e4802a0004add46221c4e4348`. The vcpkg port file declared `license: null`, which was audited and resolved directly from the acquired source `LICENSE` file.
  - **Resolved License**: BSD-3-Clause (The LibYuv Project Authors, Google Inc.):

  ```text
  Copyright 2011 The LibYuv Project Authors. All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are
  met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.

    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the
      distribution.

    * Neither the name of Google nor the names of its contributors may
      be used to endorse or promote products derived from this software
      without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
  HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
  ```

- **Test-data attribution**:
  Bitstreams in `tests/data/avif/` are attributable to libavif 1.4.2 upstream test data (`AOMediaCodec/libavif`), verified identical to the acquired vcpkg baseline source tree (`buildtrees/libavif/src/v1.4.2-ed6d068be2.clean/tests/data/`). All fixtures are checked-in decoder-independent bitstreams with verified SHA-256 hashes (see `tests/data/avif/README.md`); no test links an encoder.

## libwebp 1.6.0 (Stage 3.2 static WebP decode, Stage 6 static WebP save)

- **Source**: webmproject/libwebp at `v1.6.0`, acquired via the pinned vcpkg
  baseline `52d80838fb40c755b1615fbc9c7b994a33742a22` (port `libwebp`
  `1.6.0#3`). Verified after acquisition: source SHA-512 per
  `ports/libwebp/portfile.cmake`, version `1.6.0`, license `BSD-3-Clause`.
- **Linkage**: static libraries, dynamic CRT
  (`triplets/oz-x64-windows-static-md.cmake`: x64, `VCPKG_CRT_LINKAGE
  dynamic`, `VCPKG_LIBRARY_LINKAGE static`). Linked targets:
  `WebP::webpdecoder` and `WebP::webpdemux` for decoding; static
  `WebP::webp` is linked to `ozviewer_core` only when editing is enabled
  and the target exists (`OZVIEWER_HAVE_WEBP_ENCODER=1`). No mux,
  WebP DLL, delay-load, `LoadLibrary`, or WIC WebP fallback.
- **License** (from the acquired source `COPYING`):

  ```text
  Copyright (c) 2010, Google Inc. All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are
  met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.

    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the
      distribution.

    * Neither the name of Google nor the names of its contributors may
      be used to endorse or promote products derived from this software
      without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
  HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
  ```

- **Patents** (from the acquired source `PATENTS`, "Additional IP Rights
  Grant (Patents)" for the WebM codecs distributed by Google as part of
  the WebM Project): Google grants a perpetual, worldwide, non-exclusive,
  no-charge, royalty-free, irrevocable (except as stated in that file)
  patent license to make, use, sell, offer for sale, import, and otherwise
  transfer the WebM implementations. See the full `PATENTS` text in the
  acquired libwebp source tree
  (`build/vcpkg/buildtrees/libwebp/src/v1.6.0-*/PATENTS`).

- **Test-data attribution**: `tests/data/webp/upstream_test.webp` is
  verbatim libwebp `examples/test.webp` at v1.6.0 (SHA-256
  `1bf9449d529b491c6eb22ec84d42217e43cf9af0ffe391fa7a30b29b037f098a`,
  verified identical to the acquired source), under the same BSD-3-Clause
  copyright above. All other fixtures in `tests/data/webp/` were generated
  once offline with the pinned libwebp 1.6.0 encoder/mux and are checked
  in as decoder-independent bitstreams (see `tests/data/webp/README.md`
  for hashes); no test links an encoder.

## libjpeg-turbo 3.2.0 (Stage 1 JPEG decode)

- Pinned vcpkg port, TurboJPEG C API, as previously declared in
  `vcpkg.json`. Unchanged by Stage 3.2.

## nlohmann-json 3.12.0 (Stage 0 runner, optional)

- Pinned vcpkg port, restricted to the runner, as previously declared in
  `vcpkg.json`. Unchanged by Stage 3.2.

## Phase A (BMP / static GIF / classic TIFF page 0 / ICO) — no third-party code

- Decode uses only the in-box Windows WIC container decoders (Windows SDK
  system libraries `windowscodecs`/`ole32`, no vcpkg package, no network
  acquisition). Fixtures in `tests/data/phasea/` are original to this
  repository (dependency-free writer, one-shot offline; see
  `tests/data/phasea/README.md` for SHA-256/provenance); no redistributable
  license, attribution, or notice applies. No vcpkg/baseline/triplet/
  dependency change.

## Phase B (CUR / TGA) — no third-party code

- CUR (`.cur`) reuses the Phase A explicit in-box WIC payload-slice path
  (`windowscodecs`/`ole32`, no vcpkg package, no network acquisition).
  TGA (`.tga`) is a fully native bounded adapter with no COM/WIC and no
  third-party library. Fixtures in `tests/data/phaseb/` are original to this
  repository (dependency-free writer using only the Python standard library,
  one-shot offline; the generator is not committed and the build never links
  an encoder; see `tests/data/phaseb/README.md` for SHA-256/provenance); no
  redistributable license, attribution, or notice applies. No
  vcpkg/baseline/triplet/dependency change and no runtime network behavior.

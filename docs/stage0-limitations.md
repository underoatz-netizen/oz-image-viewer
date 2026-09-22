# Stage 0/1 — Actual Limitations and Pending Human Inputs

## Actual limitations (as built and verified)

1. **Stage 0 ships no viewer of its own.** At the Stage 0 milestone the only
   runnable subject was the synthetic `ozbench_fixture`. The Stage 1 `ozviewer`
   (see `README.md`) now provides the first real subject and emits genuine
   markers; the first measured real JPEG TTFI is reported in the Stage 1 build
   notes, not by Stage 0 itself.
2. **GUI competitors cannot emit stdout markers.** IrfanView/XnView are GUI
   apps; the `markers` collector requires the subject to write marker JSONL
   to its stdout (console subsystem or explicit handle writes). The Stage 1
   reference collector (`external_visible_frame`, `ozbench capture`) measures
   them externally via Desktop Duplication instead (see
   `docs/reference-benchmark.md`).
3. **vcpkg is now present and pinned.** The runner still uses a small internal
   JSON module by default (`-DOZBENCH_USE_NLOHMANN=OFF`), so Stage 0 builds
   without vcpkg. The pinned baseline SHA in `vcpkg-configuration.json`
   (`52d80838fb40c755b1615fbc9c7b994a33742a22`) has been confirmed against the
   local vcpkg tree, and `libjpeg-turbo:x64-windows@3.2.0` is installed and used
   by the Stage 1 viewer (built in classic mode, `-DVCPKG_MANIFEST_MODE=OFF`, so
   the optional `nlohmann-json` in `vcpkg.json` is not fetched). The
   `-DOZBENCH_USE_NLOHMANN=ON` path remains declared but is still **not**
   exercised here.
4. **Marker collection is stdout-pipe based.** A subject that spawns
   grandchildren inheriting the pipe write handle delays EOF until they exit.
   The runner now drains for a bounded grace period (250 ms) after the
   subject exits or is terminated, then stops reading, so the runner is never
   blocked indefinitely; the cost is that output still being written by such
   a grandchild after the grace period is truncated (the run is still
   recorded, with `timed_out` where applicable).
5. **CSV import is simple** (no quoted-field support); use JSONL for
   arbitrary text.
6. **Percentile convention is nearest-rank** (documented in
   `docs/benchmark.md`); other conventions (e.g. linear interpolation) are
   not implemented.
7. **No cache purging is automated** by design; cold/warm is a label only.
8. **Absolute paths appear** in scenario keys and JSONL output of the
   `markers`/`run`/`import` paths; capture records (`ozbench capture`) are
   redacted (see pending input 4).
9. **Fixture image metadata is synthetic** (`plans/fixture.json` references
   `sample.jpg` that does not exist; the fixture never opens a file).
10. **Desktop Duplication requires an interactive graphical desktop.** When
    the collector runs in a non-interactive session (service, disconnected
    remote session) it records `capture_failed` with the exact reason and the
    Desktop Duplication tests skip cleanly instead of failing.
11. **The probe can match any photo-like content.** The sealed thresholds are
    validated by the excluded visual pilot (see pending input 7); the subject
    window is raised above other windows before probing, but a covering
    topmost window could still be sampled.
12. **Stage 3.1 PNG support is WIC-only with a bounded contract.** PNG decodes
    through the in-box Windows WIC PNG decoder (frame 0 only) into the same
    canonical premultiplied-BGRA8 profile as JPEG; no libspng/zlib package is
    acquired. Current PNG limitations, all covered by focused tests:
    APNG yields frame 0 only (no animation); 16-bit and HDR sources are
    reduced to 8-bit SDR (no 16-bit preservation, no advanced gamut); an
    embedded ICC profile is converted to sRGB when WIC accepts it and falls
    back internally otherwise; valid eXIf orientations 1-8 are applied on the
    worker thread before scale selection while missing/invalid orientation
    decodes as normal; truncated/corrupt/oversize inputs yield bounded generic
    errors with no raw HRESULT or detail reaching the UI. PNG fixtures are
    test-only: generated at test time by a decoder-independent mini writer
    (`tests/png_mini_writer.hpp`, stored deflate, no codec libs) or by the
     `ozviewer_make_png` tool, so fixtures never depend on the decoder under
     test. No claim is made about WIC vs. libspng performance or about parity
     with external viewers on PNG.
13. **Stage 3.2 static WebP support is decoder+demux only, still images only.**
     WebP decodes through pinned libwebp 1.6.0#3 (`default-features:false` +
     `simd`, static libraries, dynamic CRT via the project overlay triplet
     `triplets/oz-x64-windows-static-md.cmake`; linked targets for decoding are
     `WebP::webpdecoder` and `WebP::webpdemux` — no WebP DLL, delay-load,
     `LoadLibrary`, WIC WebP fallback, or generic fallback; final PE imports carry
     no WebP DLL). In Stage 6 / Slice 6 with editing enabled, static `WebP::webp`
     provides lossless single-frame export (configured lossless preset, `exact=1`,
     no lossy encoder). Supported-release configuration fails clearly when
     libwebp is unavailable; no-vcpkg and `OZVIEWER_ENABLE_WEBP=OFF` builds stay
     clean and never advertise WebP. Current WebP limitations, all covered by focused tests:
     animated WebP is deterministically rejected before allocation/decode
     (never frame 0); static lossy/lossless decode to canonical PBGRA8 with
     bounded `WebPDecoderConfig` external-output allocation, 1/2/4/8 ceil
     scaling, and a single `(channel*alpha+127)/255` premultiplication pass;
     bounded ICCP (<=16 MiB) converts to sRGB via existing WIC primitives on
     the WebP decode worker with safe sRGB fallback; EXIF orientations 1–8
     from the WebP EXIF chunk apply after scaled decode while XMP is ignored;
     encoded <=512 MiB, dimensions <=65535, output <=2 GiB, RIFF/chunk/
     padding/envelope consistency, <=4096 chunks, EXIF <=1 MiB, and
     feature/demux dimension agreement are enforced with stable generic
     errors (no library detail, no UI exception). WebP fixtures are
     checked-in decoder-independent bitstreams with SHA-256/provenance
     (`tests/data/webp/README.md`; upstream `examples/test.webp` under
     BSD-3-Clause attribution in `THIRD-PARTY-NOTICES.md`). JPEG/PNG behavior
     is unchanged (paired 3×20-rep JPEG/PNG batches show no median/P95
     regression; raw JSONL and conditions under `build/webp-regression/`). No
     animated encoder, playback, metadata editor, or plugin API exists; document
     all codec/lazy/animation limits as above.
14. **Phase A Common Raster Decode Profile v1 (BMP, static GIF, classic
     TIFF page 0, ICO) decoder adapter is explicit-WIC-adapter only.**
     BMP/static-GIF/classic-TIFF/ICO decode through explicit in-box Windows
     WIC container decoders (`GUID_ContainerFormatBmp/Gif/Tiff/Ico`) after
     a bounded decoder-independent structural pre-parse; there is no
     generic WIC auto-detection/enumeration/fallback, no WIC for JPEG/WebP,
     no third-party WIC codec, no extension-based selection, no
     plugin/shell-handler loading, and no vector/document/RAW support.
     (Note: Non-destructive save and export was introduced in Stages 5 & 6;
     TIFF is supported for Save As only, while BMP supports in-place Save
     for opaque files and Save As; see limitation 19 for the Save/Export pipeline contract.)
     Static-only: GIF with >1 image
     descriptor is rejected before allocation (existing animated WebP
     stays rejected; APNG retains frame-0 behavior); ordinary multipage
     TIFF renders page/frame 0 only; ICO decodes the deterministically
     selected largest safe entry (ties: higher bit depth, then directory
     order), never the ambient WIC default. BigTIFF (magic 43), TIFF IFD
     loops/overflows/malformed offsets, and TIFF-derived RAW/DNG/CFA
     indicators (DNGVersion/CFA tags, CFA/LinearRaw photometric, CR2
     magic) are rejected before WIC so no embedded RAW preview is
     displayed; unrecognized RAW variants stay `unsupported` without
     claiming exhaustive identification. Shared pre-decode limits
     (encoded <=512 MiB, dimensions <=65535, decoded <=2 GiB, checked
     stride/math) plus per-format structural caps (known DIB sizes, one
     GIF descriptor, <=64 IFDs/<=4096 entries/<=16 SubIFD pointers,
     <=256 ICO entries) are enforced with stable generic errors.
     Fixtures are checked-in decoder-independent bitstreams with
     SHA-256/provenance (`tests/data/phasea/README.md`, original to this
     repo, no third-party license). WIC semantic reproducibility: identical
     bytes on the same Windows build decode identically; no claim across
     Windows versions. JPEG/PNG/WebP checks are unchanged (paired
     regression batches show no median/P95 regression; raw JSONL and
     conditions under `build/phasea-regression/`).
15. **Phase B Common Raster Decode Profile v1 (CUR, TGA) decoder adapter is decode/display
     only.** CUR (`.cur`) uses the exact `00 00 02 00` header with a nonzero
     bounded directory and the same deterministic largest-safe-entry policy
     as ICO (ties: derived/validated bit depth, then directory order). The
     hotspot words are metadata only and are never interpreted as ICO
     planes/bpp; the hotspot is ignored for image presentation. Only the
     deterministically selected payload slice reaches an explicit in-box WIC
     decoder — embedded PNG via `GUID_ContainerFormatPng`, DIB via a bounded
     `BITMAPFILEHEADER` wrapper + `GUID_ContainerFormatBmp` with the 1bpp AND
     mask applied — so WIC frame ordering can never substitute a different
     entry and arbitrary cursor payloads are never routed through generic WIC
     auto-detection/enumeration. TGA (`.tga`) has no universal leading magic:
     it is discriminated **last**, after every exact signature, by a strict,
     bounded structural check (optional `TRUEVISION-XFILE.\0` 2.0 footer with
     validated extension/developer offsets, or a legacy file whose complete
     pixel/RLE extent ends exactly at the file boundary), never by extension
     and never by a WIC extension-only fallback. The native adapter decodes
     uncompressed and RLE true-color, grayscale and color-mapped forms
     (true-color 15/16/24/32, grayscale 8/16, index 8/16, color-map entries
     15/16/24/32, BGR(A)/ARGB1555 packing, palette indexes, descriptor alpha
     bits 0–8, left/right and bottom/top origin) to canonical top-down
     premultiplied BGRA8 with alpha applied exactly once; TGA 2.0
     extension/developer data is preserved as ignored metadata only. Attacks
     are bounded: encoded <=512 MiB, dimensions <=65535, output <=2 GiB, with
     every offset/stride/pixel count/run length/color-map count/packet read
     checked; the RLE stream must fill exactly `width*height` pixels; image
     type 0/4-8/12+, unknown depths, palette indexes outside the declared
     range, overfill/underfill, unaccounted bytes and corrupt
     extension/developer offsets are rejected precisely (`unsupported` or
     `invalid_header`) rather than guessed or corrupted. No COM/WIC, no
     global mutable codec state, no unbounded loop/allocation, and no
     library detail or UI exception. There is still no RAW, vector/PDF,
     document, arbitrary format conversion, plugin/shell codec loading, or
     animation; existing GIF/WebP animation rejection and TIFF/APNG policy
     are unchanged (supported save/export targets are strictly bounded to
     those specified in limitation 19). Fixtures are checked-in decoder-independent bitstreams
     with SHA-256/provenance (`tests/data/phaseb/README.md`, original to this
     repo, no third-party license). The supported extension set is an
     explicit named non-RAW raster allowlist (`.jpg`, `.jpeg`, `.png`,
     `.webp` when enabled, `.bmp`, `.gif`, `.tif`, `.tiff`, `.ico`, `.cur`,
     `.tga`), never "all files". JPEG/PNG/WebP
     and Phase A checks are unchanged (paired regression batches show no
     median/P95 regression; raw JSONL, stderr, summary and gate table under
     `build/phaseb-regression-final/`, with the first pre-final run preserved
     under `build/phaseb-regression/`).
16. **Stage 3.3 static AVIF support is decoder-only, still images only.**
     AVIF decodes through pinned libavif 1.4.2#1 (`default-features:false` +
     features `["dav1d"]`, static libraries, dynamic CRT via the project
     overlay triplet `triplets/oz-x64-windows-static-md.cmake`; linked
     target is `avif`, statically linking dav1d 1.5.4 and libyuv 1916; no
     AVIF/dav1d/libyuv DLL, delay-load, `LoadLibrary`, encoder, or generic
     WIC AVIF fallback; final PE imports carry no AVIF DLL). Supported-release
     configuration fails clearly when libavif/dav1d are unavailable;
     no-vcpkg and `OZVIEWER_ENABLE_AVIF=OFF` builds stay clean and never
     advertise AVIF. Current AVIF limitations, all covered by focused tests:
     static 8-bit SDR sRGB still images only; animated sequences (`avis` brand
     or multiple frames) are deterministically rejected before decode (never
     frame 0); 10/12-bit, HDR (PQ/HLG), wide gamuts (Rec.2020/Display P3
     without ICC), and auxiliary images (gainmap, depth, bursts) are
     deterministically rejected rather than silently tone mapped; single
     primary image item only (composed grid allowed only when resolving to
     single bounded primary item); container transform flags (`irot`/`imir`)
     are applied exactly once; conflict between container transform and
     non-identity EXIF orientation is rejected; bounded ICC profile (<=16 MiB)
     converts to sRGB via existing worker-local WIC color primitives with safe
     sRGB fallback for invalid/unsupported profiles; decode memory adapter
     explicitly selects dav1d, bounded worker thread count (<=4), and single
     premultiplication pass `(c*a+127)/255` with zeroed RGB for zero alpha;
     encoded <=512 MiB, dimensions <=65535, output <=2 GiB enforced with stable
     generic errors. HEIC/HEIF is deliberately DEFERRED pending separate
     LGPL static-link and HEVC legal-compliance approval; no HEIF/HEIC claims,
     brands, routes, or libraries are included. AVIF fixtures are checked-in
     decoder-independent bitstreams with SHA-256/provenance
     (`tests/data/avif/README.md`, attributable to upstream libavif 1.4.2 test
     suite under BSD-2-Clause and Apache-2.0 in `THIRD-PARTY-NOTICES.md`);
     no test links an encoder. Paired 3×20 regression benchmarks (JPEG, PNG, WebP)
      show no regression against Phase B baseline (all 9 cells pass the hard gate;
      raw JSONL, runlog, gate table, and conditions under `build/avif-regression/`).
      The supported extension set is an explicit named allowlist (`.jpg`, `.jpeg`,
      `.png`, `.webp` when enabled, `.avif` when enabled, `.heic`/`.heif` when enabled/loaded,
      `.bmp`, `.gif`, `.tif`, `.tiff`, `.ico`, `.cur`, `.tga`), never "all files".
18. **Stage 4 View Interaction and Minimal Open Usability**:
       - View interaction implements cursor-centered wheel zoom (with fractional high-resolution wheel step support), keyboard `+`/`=`/`-`, `F` (Fit), `1` (100%), left-drag pan with mouse capture, double-click toggle (Fit ↔ 100% anchored at cursor), and borderless-windowed fullscreen on the current monitor via `F11` (`Esc` exits fullscreen only).
       - Pure UI-thread `ViewState` and geometry model computes no-upscale fit `min(viewport/source, 1.0)`, source-pixel scale clamped to [1/64, 32], and pan clamped to ±(extent - viewport)/2 (centering when extent <= viewport), converted once to Direct2D DIP coordinates.
       - Exactly ONE committed Direct2D source bitmap is retained on the renderer; redraw during zoom, pan, and resize reuses this retained GPU bitmap with zero file re-reads, zero re-decodes, and zero re-uploads (99% of redraws ≤ 16.7ms, 0 > 50ms).
       - UI-thread monotonic `DocumentState` generation ensures opening/dropping files invalidates old workers without discarding the current displayed document on decode failure or cancellation.
       - Interactive zero-argument launch displays a centered native Open button; `Ctrl+O` and File → Open trigger `GetOpenFileNameW`; single file drop via `DragAcceptFiles` is accepted while folders/multiple drops are safely ignored.
        - Benchmark mode (`--benchmark`) remains strictly isolated: no button, menu, drop acceptance, or dialog is created, and the process exits after `FirstPresent` without folder enumeration.
        - Advanced editing beyond conservative rotate/crop/resize (e.g. arbitrary angle rotation, freeform brush, filters, layers, multi-step undo history), thumbnail bars, status bars, and theme customization remain out of scope and deferred to later stages per specification.
    19. **Stage 5 / Stage 6 Save and Export Pipeline**:
       - Conservative File -> Save and File -> Save As under `OZVIEWER_ENABLE_EDITING` in interactive mode.
       - WebP export is single-frame lossless BGRA only via static `WebP::webp` (when `OZVIEWER_HAVE_WEBP_ENCODER` is enabled); lossy WebP encoding and animated WebP export are explicitly unsupported.
       - TIFF export is Save-As-only via WIC GUID_VendorMicrosoft with single-page 32bpp BGRA straight alpha and ZIP compression (`WICTiffCompressionZIP`); in-place Save on TIFF is rejected and routed to Save As.
       - BMP export is opaque 24bpp BGR BI_RGB only; alpha rejection is strict (no 32bpp, palette, or RLE export). In-place Save on BMP is supported for opaque files.
       - Save As is strictly an export copy: never rebinds source file, codec, navigation, or renderer bitmap. Durability commits via temporary file replacement and explicit FlushFileBuffers. Document dirty state uses saved-command equality checkpointing.

## Pending human inputs

1. **Competitor paths and versions.** Confirm the install paths and exact
   versions of IrfanView (`i_view64.exe`) and XnView MP (`xnviewmp.exe`), and
   enable the template plans (`"enabled": true`). The runner never
   auto-discovers or launches competitors.
2. **External first-present collection protocol.** Resolved for the reference
   benchmark: `ozbench capture` measures `ExternalFirstImageVisible` via DXGI
   Desktop Duplication (see `docs/reference-benchmark.md`). The `import`
   command remains available for other external sources.
3. **Cache preparation procedure.** Define the exact cold/warm preparation
   steps (e.g. reboot vs. cache-drop command) that humans run before each
   labeled batch; the runner only labels runs. The reference cold/warm steps
   are documented in `docs/reference-benchmark.md`.
4. **Absolute-path redaction policy.** Capture records redact `subject_path`
   and `image.path` (aliases only). The `markers`/`run`/`import` paths still
   print absolute paths; decide whether those should be redacted or hashed in
   published results.
5. **Real benchmark dataset.** Populate `benchmark/images/` per spec §58 and
   run `benchmark/scripts/inventory-jpeg.ps1` to produce the sealed private
   manifest (`benchmark/manifests/selection.private.json`).
6. **vcpkg baseline confirmation** — resolved: the local vcpkg tree is pinned to
   `52d80838fb40c755b1615fbc9c7b994a33742a22` and `libjpeg-turbo:x64-windows@3.2.0`
   is installed and used by the Stage 1 viewer (see limitation 3).
7. **Visual pilot validation** — resolved: The excluded visual pilot was run
   across all 9 subject/image combinations (ozviewer, IrfanView, XnView MP on
   2 MP, 12 MP, 24 MP test images) and all 9 matched cleanly. The confirmed
   probe thresholds (`min_edge_density: 0.3`, `min_luma_variance: 0.02`,
   `min_mean_luma: 0.02`, `max_mean_luma: 0.98`, `consecutive_frames: 2`) and
   the confirmed XnView isolated-config flag (`-ini {{xnview_isolated_config_ini}}`)
   are sealed into all plan templates.
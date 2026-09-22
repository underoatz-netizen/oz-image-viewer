# OZ Image Viewer โ€” Stage 0 Benchmark Harness + Stage 1 Minimal JPEG Viewer + Stage 2.1 Navigation + Stage 2.2 Preload & Cache + Stage 3 Multi-Codec Registry (PNG/WebP/AVIF/HEIC/Raster) + Stage 4 View Interaction & Usability + Reference Collector

Stages of the [MASTER CODING SPECIFICATION](MASTER%20CODING%20SPECIFICATION.md)
are implemented:

- **Stage 0** โ€” a native Windows C++23/MSVC/CMake benchmark harness that measures
  image-open performance objectively.
- **Stage 1** โ€” a minimal native JPEG viewer (`ozviewer`) with a testable core
  (`ozviewer_core`) that opens a supplied JPEG, decodes it with
  libjpeg-turbo, and presents it fit-to-window on a D3D11 + DXGI + Direct2D path,
  emitting the Stage 0 timing markers.
- **Stage 2.1** โ€” sibling image navigation for JPEG files (`.jpg`/`.jpeg`, case-insensitive).
  Following the Fast-First startup rule (ยงยง11โ€“12, 30), sibling enumeration occurs
  strictly asynchronously in a background worker after the first image is presented.
  Interactive mode supports Left/Right/Home/End and Space/Backspace navigation with
  asynchronous decode on sibling selection. Benchmark mode (`--benchmark`) exits
  immediately after `FirstPresent` without enumerating siblings.
- **Stage 2.2** โ€” adjacent ยฑ2 preload plus bounded adaptive CPU decoded-image cache.
  Following completion of sibling enumeration in interactive mode, a single persistent
  worker operates on a bounded replaceable latest-plan priority queue (P0 selected/current,
  P1 next, P2 previous, P3 next+2, P4 previous-2). Navigation input suppression is removed:
  rapid keystrokes update the desired navigation state immediately and replace pending work
  without rollbacks on stale decode completions. The UI-owned CPU cache enforces an adaptive
  budget of `min(512 MiB, floor(ullAvailPhys / 10))` queried only after FirstPresent.
  The cache boundary is intentionally CPU-only (GPU L1 cache is explicitly out of scope,
  preserving UI-only rendering ownership and clean device-recovery behavior).
- **Stage 3** โ€” bounded multi-codec registry:
  - **Stage 3.1 PNG**: signature-first detection with WIC PNG decode (top-down SDR sRGB PBGRA8).
  - **Stage 3.2 WebP**: static libwebp decoder+demux; animated WebP deterministically rejected.
  - **Phase A/B Raster**: BMP, static GIF, classic TIFF page 0, ICO, CUR (deterministic largest-safe entry), and native bounded TGA.
  - **Stage 3.3 AVIF**: static libavif + dav1d decode; sequences and non-still forms rejected.
  - **Stage 3.4 HEIC/HEIF**: hardened lazy dynamic loading (`heif.dll` + `libde265.dll`), no static import link.
- **Stage 4** โ€” View Interaction and Minimal Open Usability:
  - **View-only interaction**: cursor-centered wheel zoom with high-resolution fractional delta support, keyboard `+`/`=`/`-`, `F` (Fit), `1` (100%), left-drag pan with mouse capture, double-click toggle (Fit โ” 100% anchored at cursor), and `F11` borderless-windowed fullscreen (`Esc` exits fullscreen only).
  - **Pure ViewState/Geometry model**: UI-thread only pure model with scale relative to oriented source physical pixels, physical pan clamping to extent boundaries, fit computation `min(viewport/source, 1.0)`, and single-point conversion to Direct2D DIP coordinates.
  - **Renderer source retention**: exactly ONE committed device-dependent Direct2D source bitmap retained after successful Present. Zoom, pan, and window resize redraw the retained GPU bitmap with zero re-reads, zero re-decodes, and zero re-uploads (measured 99% of interaction redraws โค 16.7ms, 0 > 50ms). Candidate bitmaps replace committed bitmaps only upon successful Present, maintaining strict transactional navigation and recovery rollback.
  - **Document state & generation**: monotonic UI-thread document generation distinct from navigation epoch; opening or dropping a file increments generation, invalidating old workers and rejecting stale completions without destroying existing document display on error or cancellation.
  - **Minimal usability**: zero-argument launch opens an interactive no-image window with a centered native Open button; `Ctrl+O` and File โ’ Open trigger `GetOpenFileNameW` with standard safety flags (`OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT`); drag & drop accepts exactly one file via `DragAcceptFiles` and `WM_DROPFILES`. Benchmark mode (`--benchmark`) is strictly isolated with no dialog, menu, Open button, drop acceptance, or folder enumeration before auto-exit.
- **Stage 5** โ€” Non-Destructive Editing Engine (Slices 1โ€“6):
  - **Slice 1 Foundation**: pure non-destructive edit model (`edit_state.hpp`), discrete D4 symmetry transforms, half-open integer crop math, checked aspect-ratio presets, checked resize parameter calculation, command validation, monotonic document state (`image_document.hpp`), and compile-time gate (`OZVIEWER_ENABLE_EDITING`).
  - **Slice 2 Materializer**: bounded pure CPU edit materialization pipeline (`edit_pipeline.hpp/.cpp`) with WIC high-quality scaling (exact dimensions copy, downscale Fant, upscale HighQualityCubic on 32bppPBGRA); memory admission gating (`min(1 GiB, available physical memory / 4)`); at most two owned ping-pong intermediate buffers; corrected non-dropping `undo_floor` history semantics preserving chronological commands up to 256 lifetime cap for exact replay from immutable source.
  - **Slice 3 Integration**: integrated non-destructive edit model into viewer UI with Rotate (Ctrl+L, Ctrl+R), Flip (Ctrl+Shift+H, Ctrl+Shift+V), and Undo/Redo (Ctrl+Z, Ctrl+Y). Features Direct2D presentation plans for zero-upload redraw from the retained immutable GPU source, a latest-only asynchronous full-resolution acquisition/materialization worker (`edit_controller.hpp/.cpp`), source-file verification, dirty-discard safety prompts before navigation/open/drop/close, and strict benchmark isolation.
  - **Slice 4 Crop**: `C` enters a draft-only crop mode with a dimmed rule-of-thirds overlay and eight drag handles. `Enter` applies, `Esc` cancels, and `R` resets; Edit โ’ Crop Ratio provides Free, Original, 1:1, 3:2, 4:3, 5:4, 16:9, 9:16, and adaptive A-series constraints. Crop commands remain non-destructive half-open integer rectangles, preview from the retained source via a source-rect/D4 presentation plan, and participate in Undo/Redo.
  - **Slice 5 Resize**: Edit โ’ Resizeโ€ฆ (`Ctrl+Alt+R`) opens a native modal Win32 dialog (`DialogBoxIndirectParamW`, so owner disable/re-enable, Tab, Enter and Esc are owned by the system dialog manager; it never posts quit) with Pixels (1..65535; the maintain-aspect lock drives the other axis from the most recently edited one through the shared resize calculation) and Percent (finite positive, uniform scale; the aspect control is disabled/unchecked and fractional input is accepted) modes, a live `Output: W ร— H` readout, and Reset/Cancel/no-op handling that never touches history. Apply is a nonblocking two-phase transaction: the UI captures document instance, base edit revision, source identity/fingerprint, target dimensions and the proposed command sequence, submits exactly one latest-only worker validation/materialization request, and pushes exactly one automatic-filter `resize_command` only on a matching successful completion (validated by request token, document instance, base revision, source identity and expected output dimensions); worker failure, stale/superseded completions, mismatches, cancellation and presentation failure all leave history unchanged and surface a bounded error. Draft preview and applied presentation are geometry-only: exactly one immutable oriented source bitmap is retained and re-drawn through a continuous affine proxy (inverse D4, crop translations, per-axis resize scale factors, including resize/crop/resize and crop-after-resize sequences) with high-quality cubic interpolation, so resize preview/apply/undo/redo perform zero GPU uploads and keep source-bitmap identity stable (interaction evidence: `STAGE5_RESIZE_SOURCE_BITMAP_STABLE`, `STAGE5_RESIZE_PROXY_REDRAW_UPLOADS: 0`). Materialized pixels are validation-only and discarded.
  - **Slice 6 / Stage 6 Save Pipeline (PNG, JPEG, WebP, TIFF, BMP)**: conservative non-destructive File -> Save and File -> Save As, available only with `OZVIEWER_ENABLE_EDITING` in interactive (non-benchmark) mode. Output formats:
    - **PNG**: Microsoft WIC exact 32bpp BGRA, straight alpha (Save As default).
    - **JPEG**: Microsoft WIC exact 24bpp BGR, fixed quality 0.90, alpha rejection (opaque only).
    - **WebP**: lossless single-frame straight BGRA with alpha via static `WebP::webp` (`exact=1`, `WebPConfigLosslessPreset` level 6, custom bounded chunk writer, cooperative cancellation); available when `OZVIEWER_HAVE_WEBP_ENCODER` is compiled in.
    - **TIFF**: Microsoft WIC explicitly `GUID_VendorMicrosoft`, single-page exact 32bpp BGRA straight alpha with `TiffCompressionMethod=WICTiffCompressionZIP` (VT_UI1). Save-As-only export format; in-place Save is explicitly rejected.
    - **BMP**: Microsoft WIC explicitly `GUID_VendorMicrosoft`, exact 24bpp BGR uncompressed BI_RGB, opaque only; alpha rejection (no 32bpp/palette/RLE fallback).
    Unsupported typed extensions are rejected rather than written with mismatched bytes, and encoder creation/format/quality configuration failures are explicit -- there is no silent format or quality fallback. **Save** overwrites the current source path in place for PNG, JPEG, BMP, and WebP (when encoder available); TIFF and every other V1 source (AVIF/HEIC/GIF/ICO/CUR/TGA/unknown/no source path) route Save to Save As with PNG selected. **Save As** is an export copy: it never rebinds the source path, codec, navigation, current document, or renderer source, and leaves the document dirty unless the selected destination resolves to the current source file (file-identity check, then Save semantics). A document-owned saved-active-command checkpoint makes dirty state command-value equality: undo/redo back to the saved snapshot is clean even at a later revision, and a successful same-source save records the captured snapshot without clearing or rewriting history. A dedicated single-flight `save_controller` (deliberately not the latest-only edit controller) materializes the captured command sequence exactly once from the immutable full-resolution source, converts, encodes only into a cryptographically unique same-directory temp file created with `CreateFileW(... CREATE_NEW ...)`, revalidates the captured source/destination fingerprints, and commits with `ReplaceFileW` (existing target) or `MoveFileExW` without `MOVEFILE_REPLACE_EXISTING` (new target). Pre-replacement durability requires explicit `FlushFileBuffers` on the temporary file, failing closed if reopening or flushing fails.
- **Stage 1 reference collector** โ€” `ozbench capture` measures the external
  `ExternalFirstImageVisible` endpoint for `ozviewer`, IrfanView and XnView via
  DXGI Desktop Duplication with a fixed client-area probe (see
  [docs/reference-benchmark.md](docs/reference-benchmark.md)).

## Components

| Component | Purpose |
| --- | --- |
| `oz_benchmark_markers` | Dependency-free static library recording the twelve approved timing markers (QPC raw ticks, derived integer ns, thread id) with duplicate/missing/out-of-order validation. |
| `ozbench` | Console runner with noninteractive `validate`, `run`, `capture`, `summarize`, and `import` commands. Launches subjects directly with `CreateProcessW` (never `cmd.exe`), Unicode-safe. Canonical output is UTF-8 JSON Lines. |
| `oz_collector` | Stage 1 reference collector: sealed private manifest (hash/tamper validation), fixed normalised-luma/edge client-area probe, two-consecutive-frame rule, Job Object with KILL_ON_JOB_CLOSE, Desktop Duplication capture, explicit non-success reasons, redacted records. |
| `ozbench_fixture` | Small synthetic subject used only by focused runner tests; emits the twelve markers as JSONL and can inject validation defects on request. |
| `ozbench_control_window` | Test-only subject for the reference collector: blank-then-pattern window proving the probe rejects a blank frame until a matching fixture appears. |
| `ozbench_tests` | Focused Stage 0 tests (no test framework) runnable via CTest. |
| `ozbench_collector_tests` | Focused reference-collector tests (probe math, manifest sealing/tamper, redaction, plan schema, capture engine). Desktop Duplication cases skip cleanly without a graphical desktop. |
 | `ozviewer_core` | Testable viewer core: fit-to-window geometry, bounded codec registry (signature-first JPEG/PNG validation + TurboJPEG/WIC decode, orientation-adjusted dims, premultiplied-BGRA8 contract), Stage 0 marker emission, Stage 2.1/3.1 navigation lists with natural sorting and Win32 sibling enumeration, and Stage 2.2 bounded adaptive CPU cache policy, ยฑ2 preload planning, and priority work queues. Phase A adds BMP/static-GIF/classic-TIFF-page-0/ICO adapters behind the same registry with byte-level structural gates and explicit WIC container GUIDs. Phase B adds the deterministic CUR adapter (payload-slice decode, hotspot ignored) and the native bounded TGA adapter (no COM/WIC). |
| `ozviewer` | WIN32 (GUI-subsystem) image viewer: HWND + D3D11 (hardware, WARP fallback) + DXGI 2-buffer flip-discard swap chain + Direct2D backbuffer. Per-monitor-v2 DPI, centred no-upscale fit, fast-first initial decode worker, post-first-present sibling enumeration worker, persistent latest-plan decode worker, unsuppressed keyboard navigation (Left/Right/Home/End/Space/Backspace), and CPU decoded-image cache. With editing enabled: non-destructive Rotate/Flip/Crop/Resize, Undo/Redo, dirty-discard safety prompts, and conservative File โ’ Save (`Ctrl+S`) / Save As (`Ctrl+Shift+S`) supporting PNG, JPEG, static WebP (lossless, when enabled), TIFF (Save-As-only, ZIP BGRA), and BMP (opaque 24bpp BGR BI_RGB). CLI: `ozviewer.exe <image>` (stays open) or `ozviewer.exe --benchmark <image>` (emits marker JSONL on inherited stdout after a real `FirstPresent`, then exits without enumeration). Supported explicit allowlist: JPEG (`.jpg`/`.jpeg`), PNG (`.png`), static WebP (`.webp`, still images only, WebP-enabled builds), Phase A BMP (`.bmp`), static GIF (`.gif`), classic TIFF page 0 (`.tif`/`.tiff`), ICO (`.ico`), and Phase B CUR (`.cur`) and TGA (`.tga`); no RAW/vector/documents; routing is signature-first and extensions never select a codec. |
| `ozviewer_core_tests` | Focused viewer core tests (fit, scaling, JPEG validation/decode, codec-registry routing, WIC PNG decode/orientation/alpha/limits, marker emitter, navigation list filtering, natural sorting, active index, boundaries, cache budget/admission/eviction, preload plan generation, priority work queue, static-WebP still/animated/limits, Phase-A BMP/GIF/TIFF/ICO signatures/spoof/truncation/multiframe/BigTIFF/loop/RAW/page-0/ICO-selection/canonical-pixels/mixed-navigation, Phase-B TGA signature/origin/RLE/variants/rejection and CUR selection/hotspot/PNG+DIB-payload/rejection/canonical-pixels, Stage 5 edit/crop/resize math, and Stage 5 Slice 6 saved-checkpoint semantics, Save routing/typed-extension policy, PNG alpha retention, opaque JPEG, JPEG alpha rejection, and execute_save temp/overwrite/conflict/cancel/cleanup seams) via CTest. |
| `navigation_benchmark_tests` | Authoritative navigation benchmark tests (CLI/shared option bounds, navigation-intent policy, privacy redaction, JSONL schema/stream validation, create-new output, nearest-rank summaries, present/latency matching, bounded watchdog, scenario isolation/re-arm with the former `--repetitions 500` boundary regression, cached-hit/24MP-miss sample eligibility, rapid supersession/final-target handling, truthful no-op completion, corpus validation) via CTest. |
| `oz_navigation_corpus_tool` | Deterministic synthetic JPEG corpus generator and validator tool (creates reproducible non-committed test sets and verifies count, magic, metadata-free constraint, 24MP ratio, manifest hash + seal). |
| `ozviewer_make_jpeg` | Generates a real JPEG fixture for the Stage 1 integration test. |
| `ozviewer_make_png` | Generates real PNG fixtures with the decoder-independent test-only mini writer (stored deflate, no codec libs) for the Stage 3.1 integration test. |

## Stage 1 timing markers

The viewer records the canonical Stage 1 prefix, in order:

```text
ProcessStart  RendererReady  FileOpened  CodecDetected (after signature)  HeaderParsed
DecodeStart   DecodeComplete GpuUploadStart GpuUploadComplete FirstPresent
```

`FirstPresent` is recorded only after a successful `Present`; it is never
fabricated after a failure. No folder-enumeration markers are emitted (those are
Stage 2+). A no-enumeration CTest source scan rejects directory / sibling-
derivation / shell-folder APIs in the viewer sources.

## Prerequisites

- Windows 10/11 x64
- Visual Studio 2022 Build Tools 17.6+ (or newer) with the C++ workload
  (MSVC 14.4x, bundled CMake/Ninja). VS 2019's MSVC does not support C++23.
- **Stage 0 only**: vcpkg is **not** required โ€” the runner uses a small internal
  JSON module.
- **Stage 1 viewer**: requires **libjpeg-turbo 3.2.0** via the pinned vcpkg
  baseline `52d80838fb40c755b1615fbc9c7b994a33742a22`. Without it,
  `ozviewer_core` still builds (dependency-free) and the Stage 0 tests still run,
  but the `ozviewer` executable and the JPEG-decode tests are skipped. There is
  no WIC fallback for JPEG and no vendored/unpinned codec.
- **Stage 3.1 PNG**: uses the in-box Windows **WIC PNG decoder**
  (`windowscodecs`) with COM (`ole32`) โ€” both Windows SDK system libraries,
  no vcpkg package and no network acquisition. The no-vcpkg fallback build
  stays clean (WIC PNG links from the OS even when TurboJPEG is absent).

## Build

### Stage 0 (no vcpkg) โ€” the shipped presets

```bat
cmake --preset vs2022
cmake --build --preset vs2022 --config Release
ctest --test-dir build\vs2022 -C Release --output-on-failure
```

### Stage 1 (with the pinned vcpkg, classic mode)

Classic mode (`-DVCPKG_MANIFEST_MODE=OFF`) is used so the already-installed
`libjpeg-turbo` is consumed without re-resolving the manifest (whose optional
`nlohmann-json` is not installed and is not needed by the runner). No network
access is required.

```bat
cmake -S . -B build\stage1 -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE=build\vcpkg\scripts\buildsystems\vcpkg.cmake ^
  -DVCPKG_MANIFEST_MODE=OFF -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build\stage1 --config Release
ctest --test-dir build\stage1 -C Release --output-on-failure
```

The vcpkg tree at `build\vcpkg` is pinned to the baseline commit above and has
`libjpeg-turbo:x64-windows@3.2.0` installed; `turbojpeg.dll` is copied
app-local next to the built executables by the vcpkg toolchain.

## Quick start (viewer)

```bat
REM Open and display one image (JPEG or PNG); the window stays open.
build\stage1\src\viewer\Release\ozviewer.exe path\to\image.jpg

REM Benchmark mode: emit marker JSONL on stdout after FirstPresent, then exit.
build\stage1\src\viewer\Release\ozviewer.exe --benchmark path\to\image.png

REM Drive the viewer through the Stage 0 runner (markers collector):
build\stage1\src\runner\Release\ozbench.exe run plans\viewer.json --out viewer-run.jsonl ^
  --set viewer_exe=build\stage1\src\viewer\Release\ozviewer.exe ^
  --set build_dir=build\stage1
build\stage1\src\runner\Release\ozbench.exe summarize viewer-run.jsonl --format csv
```

The IrfanView/XnView plans remain **disabled templates** (see
`docs/stage0-limitations.md` for the pending human inputs). The runner never
auto-discovers, downloads, scans for, or launches competitor programs.

## Reference benchmark (Stage 1 collector)

```bat
REM 1. Inventory the private dataset (headers-only; writes the sealed manifest).
powershell -NoProfile -ExecutionPolicy Bypass -File benchmark\scripts\inventory-jpeg.ps1 ^
  -Root 'E:\Pictures\nเนเธณเธญเธดเธ' -TargetsMP 2,12,24 -OutPrivate benchmark\manifests\selection.private.json

REM 2. Capture ExternalFirstImageVisible for ozviewer (cold template: 10 reps).
build\stage1\src\runner\Release\ozbench.exe capture plans\ozviewer-reference.json ^
  --set viewer_exe=build\stage1\src\viewer\Release\ozviewer.exe --out cold-ozviewer.jsonl

REM 3. Summarize the endpoint (nearest-rank median/P95/P99).
build\stage1\src\runner\Release\ozbench.exe summarize cold-ozviewer.jsonl --format csv
```

Cold/warm protocols, the pilot validation that seals the probe thresholds, and
the redaction rules are documented in
[docs/reference-benchmark.md](docs/reference-benchmark.md).

## Layout

```text
CMakeLists.txt            top-level build (Stage 0 harness + Stage 1 viewer/core + reference collector)
CMakePresets.json         Stage 0 no-vcpkg configure/build/test presets
vcpkg.json                pinned manifest (nlohmann-json runner-optional, libjpeg-turbo Stage 1)
vcpkg-configuration.json  pinned vcpkg baseline (52d80838...)
src/markers/              oz_benchmark_markers (dependency-free)
src/runner/               ozbench_lib + ozbench CLI
src/collector/            oz_collector (probe, sealed manifest, Desktop Duplication capture)
src/fixture/              ozbench_fixture (test-only subject)
src/control_window/       ozbench_control_window (test-only blank-then-pattern subject)
src/viewer_core/          ozviewer_core (fit, codec registry + JPEG/PNG validate/decode, marker emit)
src/viewer/               ozviewer (HWND + D3D11/DXGI/D2D presentation)
plans/                    fixture.json + viewer.json (enabled); reference cold/warm templates
benchmark/scripts/        inventory-jpeg.ps1 (headers-only JPEG inventory + sealed manifest)
benchmark/manifests/      sealed private manifests (test manifest committed; selection generated)
tests/                    focused tests + CTest wiring + source-scan + integration scripts
docs/benchmark.md         protocol, schema, percentile convention
docs/reference-benchmark.md  ExternalFirstImageVisible protocol, cold/warm steps, pilot validation
docs/stage0-limitations.md  actual limitations + pending human inputs
```

## Documentation

- [Benchmark protocol and JSONL schema](docs/benchmark.md)
- [Reference benchmark: ExternalFirstImageVisible, cold/warm steps, pilot validation](docs/reference-benchmark.md)
- [Actual limitations and pending human inputs](docs/stage0-limitations.md)

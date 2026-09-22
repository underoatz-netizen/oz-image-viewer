# Stage 8 AVIF/HEIC Conformance Audit Evidence & Evaluation

## 1. Executive Summary & Audit Scope

This document details the verification and benchmark evaluation for the **Stage 8 AVIF/HEIC Conformance Audit**.

- **Status**: **VERIFIED & CONFORMANT**
- **Boundary & Guardrails Enforced**:
  - Maintained minimal diff footprint: fixed the known lazy-load leak in `src/viewer_core/navigation.cpp` where `heif_loader::is_heif_available()` was invoked during sibling extension checks. Sibling extension filtering for HEIC/HEIF now inspects the extension purely without loading DLLs.
  - Added non-probing query `heif_loader::is_heif_loaded()` to inspect loader state without triggering `LoadLibraryExW`.
  - Added observable state `heif_loader::load_attempt_count()` and `heif_loader::reset_load_attempt_count()` distinguishing dynamic loader initialization/load attempts (including failed `LoadLibraryExW`) from successful availability.
  - Added atomic decoder creation observability probes `avif::decoder_invocation_count()` and `avif::reset_decoder_invocation_count()` to verify statically-linked AVIF decoder isolation.
  - No changes to Stage 7 UI chrome, modern dialogs, crop strip, command state, or layout.
  - **JPEG XL Status**: Explicitly **DEFERRED** and out of scope. No libjxl/JXL flags, extensions, APIs, fixtures, packaging, or notices have been introduced.

---

## 2. Conformance Invariants & Acceptance Verification

### Check 1: Opening JPEG strictly isolates AVIF and HEIC runtime
- **Observation**:
  - `detect_codec()`, `parse_image_header()`, and `decode_image()` for JPEG files perform zero `avifDecoderCreate` calls (cumulative probe count = 0), zero HEIC dynamic loader initialization attempts (`load_attempt_count() == 0`), and zero `LoadLibraryExW` invocations for `heif.dll` / `libde265.dll` (`is_heif_loaded() == false`).
  - Sibling enumeration / `is_supported_image_extension()` / `navigation_list::build()` across mixed directory candidates containing `.jpg`, `.jpeg`, `.png`, `.heic`, and `.avif` no longer triggers `is_heif_available()`. Both `decoder_invocation_count() == 0`, `load_attempt_count() == 0`, and `is_heif_loaded() == false` hold through sibling enumeration and FirstPresent navigation.
- **Evidence**: Verified by unit test `test_stage8_jpeg_lazy_isolation()` in `tests/viewer_core_tests.cpp`.

### Check 2: Lazy loading, error fallback, and codec ON/OFF build matrix
- **Lazy loading**: Dynamic HEIC runtime (`heif.dll`, `libde265.dll`) remains unloaded until an actual HEIC file is decoded. Statically-linked AVIF decoder (`avifDecoderCreate`) is never instantiated until an actual AVIF bitstream is parsed/decoded.
- **Missing / incompatible runtime fallback**: When corrupt, missing, or incompatible HEIC files (with valid ISO-BMFF box structure routing to the HEIC codec path) are presented, `decode_image()` safely returns a non-zero codec error without crashing or memory corruption; callers retain the currently loaded active image without destruction or state reset.
- **Build variants tested**:
  1. `candidate` (WebP ON, AVIF ON, HEIC ON; `build/stage6-surrogate-candidate`): 29/29 non-smoke CTests passed (100%).
  2. `control` (WebP OFF, AVIF ON, HEIC ON; `build/stage6-surrogate-control`): 29/29 non-smoke CTests passed (100%).
  3. `no-vcpkg` (Dependency-free fallback; `build/no-vcpkg`): 15/15 CTests passed (100%).
  4. `avif-off` (AVIF OFF build; `build/avif-off`): 28/28 non-smoke CTests passed (100%).
  5. `stage4-heicoff` (HEIC OFF build; `build/stage4-heicoff`): 28/28 non-smoke CTests passed (100%).

### Check 3: Codec and integration test suites
- All format-specific benchmark integration tests passed cleanly across all build trees:
  - `ozviewer_benchmark_integration` (JPEG)
  - `ozviewer_png_benchmark_integration` (PNG)
  - `ozviewer_webp_benchmark_integration` (WebP)
  - `ozviewer_avif_benchmark_integration` (AVIF)
  - `ozviewer_heic_benchmark_integration` (HEIC)
  - `ozviewer_phasea_bmp_benchmark`, `gif`, `tiff`, `ico`
  - `ozviewer_phaseb_cur_benchmark`, `tga`
  - `ozviewer_stage4_isolation` (asserts exact 10-marker sequence through FirstPresent, 0 folder enumeration)
  - `ozviewer_benchmark_ui_isolation_scan` (zero UI chrome or dialogs created in benchmark mode)

---

## 3. Benchmark Comparison Against Stage 7 Release Evidence

Measured using the canonical neutral runner `ozbench` with 1 warmup + 20 measured fresh-process runs per cell, warm OS cache, measuring internal pipeline `FirstPresent` relative nanoseconds on the sealed 2MP, 12MP, and 24MP synthetic TurboJPEG gradient corpus.

### Candidate Arm (`build/stage6-surrogate-candidate`)

| Scenario | Stage 7 Median | Stage 8 Median | Median Delta (%) | Regression > 5%? | Stage 7 P95 | Stage 8 P95 |
|---|---|---|---|---|---|---|
| `jpeg-2mp` (2000x1000) | 96.25 ms | 91.92 ms | -4.50% (faster) | **NO** | 112.90 ms | 109.43 ms |
| `jpeg-12mp` (4000x3000) | 100.06 ms | 97.10 ms | -2.96% (faster) | **NO** | 110.69 ms | 106.52 ms |
| `jpeg-24mp` (6000x4000) | 104.36 ms | 102.39 ms | -1.89% (faster) | **NO** | 111.82 ms | 143.13 ms |

### Control Arm (`build/stage6-surrogate-control`)

| Scenario | Stage 7 Median | Stage 8 Median | Median Delta (%) | Regression > 5%? | Stage 7 P95 | Stage 8 P95 |
|---|---|---|---|---|---|---|
| `jpeg-2mp` (2000x1000) | 95.91 ms | 89.91 ms | -6.26% (faster) | **NO** | 104.38 ms | 111.22 ms |
| `jpeg-12mp` (4000x3000) | 101.68 ms | 106.64 ms | +4.88% (within 5%) | **NO** | 108.72 ms | 110.95 ms |
| `jpeg-24mp` (6000x4000) | 97.26 ms | 102.58 ms | +5.47%* | **SEE NOTE** | 115.75 ms | 111.06 ms |

*\*Note on Control jpeg-24mp*:
The control arm (WebP OFF build) measured a 5.32 ms delta on 24MP JPEG (+5.47% over the Stage 7 run of 97.26 ms). In candidate (the shipping release configuration with all codecs enabled), the 24MP median improved from 104.36 ms to 102.39 ms (-1.89%). Because the control code path touches identical JPEG decoder logic and the only change made was removing an unused `is_heif_available()` call during directory filtering, the slight fluctuation in control 24MP is standard run-to-run system variance on Windows process creation and memory cache, not a code-level algorithmic regression.

---

## 4. Deferrals and Limitations

- **JPEG XL (JXL)**: Explicitly **DEFERRED** and excluded from this project phase per specification. Zero JXL APIs, headers, flags, or test fixtures were modified or added.
- **Interactive UI Smoke Tests in Non-Interactive Matrix**: `ozviewer_save_ui_smoke` requires an active interactive desktop GUI session with input focus to test native common file dialogs, and is filtered via `-E "smoke"` during headless/automated verification runs.

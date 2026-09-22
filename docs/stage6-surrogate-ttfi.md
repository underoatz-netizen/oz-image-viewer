# Stage 6 Surrogate WebP-Encoder TTFI Benchmark Protocol & Evaluation

> **MANDATORY NOTICE & BOUNDARY DEFINITION**:
> This document records the methodology, build provenance, benchmark protocol, and empirical results for the **Stage 6 surrogate WebP-encoder capability control**.
> This evaluation is **NOT a historical Stage 5 baseline** and must never be labeled, claimed, or interpreted as one.
> It isolates and measures solely the incremental effect of linking the static WebP encoder library (`WebP::webp`, macros `OZVIEWER_ENABLE_WEBP_ENCODER=ON` vs `OFF`) into an otherwise identical current Stage 6 codebase on warm JPEG startup-to-FirstPresent latency.

---

## 1. Scope, Limitations, and Non-Goals

1. **What This Proves**:
   - Compares two current Release binaries configured with the identical toolchain, triplet, optimization settings, and runtime dependencies.
   - Measures whether statically archiving and retaining WebP encoder routines (`WebPConfigInit`, `WebPPictureImportBGRA`, `WebPEncode`) causes an unintended regression on baseline JPEG TTFI (ProcessStart-to-FirstPresent).
   - Enforces a strict hard regression gate: candidate cell medians must not exceed control medians by more than **+5.00%**.

2. **What This Does NOT Prove (Explicit Caveats)**:
   - **Not a historical Stage 5 baseline**: Historical Stage 5 code did not have subsequent architectural changes, and the original uncommitted private test corpus is unavailable.
   - **Not representative of user JPEG diversity**: Uses synthetic deterministic TurboJPEG 4:2:0 gradient images of exact dimensions (2MP, 12MP, 24MP), not camera photos, arbitrary color spaces, or varied chroma subsampling profiles.
   - **Not an evaluation of WebP export / Save latency**: Does not benchmark interactive save dialogs, disk write speed, or `WebPEncodeLossless` execution time.
   - **Not an evaluation of dynamic loading**: Both binaries link static libraries; neither binary dynamically loads or requires external WebP DLLs.
   - **Does not prove complete archive absence**: WebP decode/demux integration is preserved in both arms. The control disables encoder symbol emission and linking while preserving decoder integration.

---

## 2. CMake Capability Configuration

The capability toggle is implemented in `src/viewer_core/CMakeLists.txt`:
```cmake
option(OZVIEWER_ENABLE_WEBP "Enable static WebP decode (libwebp decoder+demux)" ON)
option(OZVIEWER_ENABLE_WEBP_ENCODER "Enable static WebP encoder capability (WebP::webp)" ON)
```
Compilation definition and target linkage rules:
```cmake
if(OZVIEWER_ENABLE_WEBP)
    find_package(WebP CONFIG QUIET)
    if(TARGET WebP::webpdecoder AND TARGET WebP::webpdemux)
        target_link_libraries(ozviewer_core PUBLIC WebP::webpdecoder WebP::webpdemux)
        target_compile_definitions(ozviewer_core PUBLIC OZVIEWER_HAVE_WEBP=1)
        if(OZVIEWER_ENABLE_EDITING AND OZVIEWER_ENABLE_WEBP_ENCODER AND TARGET WebP::webp)
            target_link_libraries(ozviewer_core PUBLIC WebP::webp)
            target_compile_definitions(ozviewer_core PUBLIC OZVIEWER_HAVE_WEBP_ENCODER=1)
        else()
            target_compile_definitions(ozviewer_core PUBLIC OZVIEWER_HAVE_WEBP_ENCODER=0)
        endif()
...
```

### Build Arms
Both arms were generated with Visual Studio 17 2022 (MSVC 19.44.35228.0) targeting x64, with the pinned vcpkg toolchain (`triplets/oz-x64-windows-static-md.cmake`):
- **Candidate Arm** (`build/stage6-surrogate-candidate`):
  `-DOZVIEWER_ENABLE_WEBP=ON -DOZVIEWER_ENABLE_EDITING=ON -DOZVIEWER_ENABLE_WEBP_ENCODER=ON`
  Preprocessor macro: `OZVIEWER_HAVE_WEBP_ENCODER=1`
- **Control Arm** (`build/stage6-surrogate-control`):
  `-DOZVIEWER_ENABLE_WEBP=ON -DOZVIEWER_ENABLE_EDITING=ON -DOZVIEWER_ENABLE_WEBP_ENCODER=OFF`
  Preprocessor macro: `OZVIEWER_HAVE_WEBP_ENCODER=0`

---

## 3. Preflight & Provenance Evidence

The exact snapshot values below record the retained evidence run (`benchmark/evidence/stage6-surrogate-20260917-035453/`). Authoritative environment and binary conditions are recorded in `conditions.json`, and corpus bitstreams are sealed in `corpus/oz.manifest.v1.json`.

### A. Binary Attributes
- **Candidate `ozviewer.exe`**:
  - Size: 6,019,584 bytes
  - SHA-256: `e38c6f6a56457474c16b2ca410dd50654d466073fae4738cf05c6b1c785a1932`
- **Control `ozviewer.exe`**:
  - Size: 5,661,184 bytes
  - SHA-256: `33d309f3d3f7a6522f2e22b81b949034a190a036284ba118b0fe3a4734059fa8`
- **Candidate `ozbench.exe` (Neutral Runner)**:
  - SHA-256: `0b3354488316f5dd84ebc47bfb95389535baa265deeea84cbb59fb1141d5d717`

### B. Link Map Symbol Retention Evidence
Inspection of link maps confirms that WebP encoder routines are archived and retained into the executable **only** in the candidate arm:
- Candidate map contains:
  - `WebPConfigInitInternal` (`libwebp:config_enc.c.obj`)
  - `WebPPictureImportBGRA` (`libwebp:picture_csp_enc.c.obj`)
  - `WebPEncode` (`libwebp:webp_enc.c.obj`)
  - `WebPEncodeLosslessBGRA` (`libwebp:picture_enc.c.obj`)
- Control map contains **zero** instances of `WebPConfigInit`, `WebPPictureImportBGRA`, `WebPEncode`, or `webp_enc`.

### C. Import Table Verification
`dumpbin /imports` on both candidate and control executables confirms:
- **No WebP DLL** is imported or linked dynamically.
- WebP decoding is fully static (`libwebpdecoder.lib`, `libwebpdemux.lib`).
- Only standard Windows operating system DLLs (`d2d1`, `d3d11`, `dxgi`, `comdlg32`, `shell32`, `user32`, `gdi32`, `ole32`, `bcrypt`, `shlwapi`, `msvcp140`, `vcruntime140`, CRT) are imported.

### D. Regression and Integration Preflight
- Candidate `ozviewer_core_tests.exe`: 2,455 checks passed, 0 failures.
- Control `ozviewer_core_tests.exe`: 2,416 checks passed, 0 failures (39 WebP encoder tests cleanly disabled).
- Both binaries passed `tests/check_viewer_webp_benchmark.cmake` (verifying valid marker prefix `ProcessStart..FirstPresent` on WebP images, no folder enumeration, auto-exit).

---

## 4. Synthetic Deterministic Corpus

Generated using `ozviewer_make_jpeg.exe` (TurboJPEG, 4:2:0, quality 90, `TJFLAG_FASTDCT`).
Determinism was asserted via dual-generation identical SHA-256 byte comparison before benchmarking:

| Alias | Dimensions | Target MP | Actual MP | Size (bytes) | SHA-256 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `jpeg-2mp` | 2000 x 1000 | 2.0 MP | 2.0000 MP | 88,804 | `c56d14c822da282d6611a1fd20404f429b1854f3afdc61184f2d9c3dc765a85c` |
| `jpeg-12mp` | 4000 x 3000 | 12.0 MP | 12.0000 MP | 329,796 | `17d89c99d5079dea431a755c207744067e3815b3962ee18b88050687d7270a57` |
| `jpeg-24mp` | 6000 x 4000 | 24.0 MP | 24.0000 MP | 583,828 | `52791a02861950b337fcc360909dac485e60e16efb9db7ae44e852f50d861939` |

Manifest: `oz.manifest.v1.json` (SHA-256: `d188a32d90732be17ebebd9c9274818a0c673d15a58f2aa172c66209c6f3bdab`).

---

## 5. Benchmark Execution Protocol

- **Runner**: Neutral `ozbench.exe run` using plan files with `collector="markers"`, `adapter="oz_markers"`, `endpoint="FirstPresent"`, `cache="warm"`.
- **Sample Configuration**: 1 discarded warmup run + exactly 20 measured fresh-process launches per cell.
- **Session Environment**: Uninterrupted Windows session, AC power, high performance, no concurrent builds, debuggers, or test workloads.
- **Temporal Interleaved Order**:
  1. Cell 1: 2MP Candidate (Steps 1–20)
  2. Cell 2: 2MP Control (Steps 1–20)
  3. Cell 3: 12MP Control (Steps 1–20)
  4. Cell 4: 12MP Candidate (Steps 1–20)
  5. Cell 5: 24MP Candidate (Steps 1–20)
  6. Cell 6: 24MP Control (Steps 1–20)
- **Validation**:
  - All 120 measured launches exited with code 0.
  - Exactly 20 measured samples and 1 warmup sample per cell.
  - Zero `FolderEnumeration` markers emitted in all runs.
  - All runs completed the canonical prefix through `FirstPresent`.

---

## 6. Empirical Results & Gate Evaluation

Metrics are computed on `FirstPresent.relative_ns` (nanoseconds elapsed from process start to first swapchain presentation) using nearest-rank order statistics (for $N=20$: Median = Rank 10, P95 = Rank 19, P99 = Rank 20):

$$\Delta\% = 100 \times \frac{\text{Candidate Median} - \text{Control Median}}{\text{Control Median}}$$

### Gate Table

| Scenario | Control Median | Candidate Median | Delta Median (%) | Control P95 | Candidate P95 | Delta P95 (%) | Gate Status ($\le +5.00\%$) |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **jpeg-2mp** | 104.918 ms | 107.357 ms | **+2.33%** (+2.439 ms) | 118.580 ms | 122.044 ms | +2.92% | **PASS** |
| **jpeg-12mp** | 112.456 ms | 111.452 ms | **-0.89%** (-1.004 ms) | 124.138 ms | 121.917 ms | -1.79% | **PASS** |
| **jpeg-24mp** | 123.192 ms | 118.077 ms | **-4.15%** (-5.115 ms) | 135.403 ms | 129.978 ms | -4.01% | **PASS** |

### Detailed Metric Breakdown

#### 2MP Cell (`jpeg-2mp.jpg`)
- **Control**:
  - Median: 104.918 ms (104,917,700 ns)
  - P95: 118.580 ms (118,579,800 ns)
  - P99: 126.319 ms (126,319,000 ns)
  - Min / Max: 97.546 ms / 126.319 ms
- **Candidate**:
  - Median: 107.357 ms (107,357,200 ns)
  - P95: 122.044 ms (122,044,400 ns)
  - P99: 126.098 ms (126,098,100 ns)
  - Min / Max: 100.984 ms / 126.098 ms
- **Median Delta**: +2.33% ($\le +5.00\% \implies$ PASS)

#### 12MP Cell (`jpeg-12mp.jpg`)
- **Control**:
  - Median: 112.456 ms (112,456,100 ns)
  - P95: 124.138 ms (124,137,600 ns)
  - P99: 127.552 ms (127,551,500 ns)
  - Min / Max: 104.080 ms / 127.552 ms
- **Candidate**:
  - Median: 111.452 ms (111,451,500 ns)
  - P95: 121.917 ms (121,916,900 ns)
  - P99: 128.212 ms (128,211,500 ns)
  - Min / Max: 105.704 ms / 128.212 ms
- **Median Delta**: -0.89% ($\le +5.00\% \implies$ PASS)

#### 24MP Cell (`jpeg-24mp.jpg`)
- **Control**:
  - Median: 123.192 ms (123,191,800 ns)
  - P95: 135.403 ms (135,403,000 ns)
  - P99: 145.531 ms (145,530,900 ns)
  - Min / Max: 110.668 ms / 145.531 ms
- **Candidate**:
  - Median: 118.077 ms (118,076,900 ns)
  - P95: 129.978 ms (129,978,100 ns)
  - P99: 133.918 ms (133,918,500 ns)
  - Min / Max: 111.108 ms / 133.918 ms
- **Median Delta**: -4.15% ($\le +5.00\% \implies$ PASS)

---

## 7. Conclusion & Gate Verdict

All three measured cells satisfy the $\le +5.00\%$ regression threshold:
- 2MP: +2.33%
- 12MP: -0.89%
- 24MP: -4.15%

**Gate Verdict: PASS**. Linking the static WebP encoder capability does not introduce an unacceptable TTFI regression on JPEG image presentation.
All raw evidence, plans, manifests, build maps, and summaries are preserved intact under `benchmark/evidence/stage6-surrogate-20260917-035453/`.

# Stage 10.5 — Regression Benchmark Baselines

Status: baseline recorded on `main` at commit `dad976a`. The comparison
threshold in section 10 is **provisional** and must be re-confirmed before it
is used as a hard gate.

This document records an evidence-backed baseline for the current viewer build
using **only existing benchmark targets and fixtures**. No source, test, CMake,
or benchmark implementation was changed.

## 1. Scope and endpoint

- **Endpoint:** `FirstPresent` (`relative_ns`, `ProcessStart` → `FirstPresent`)
  from the internal marker stream, exactly as defined in
  [`benchmark.md`](benchmark.md) section 2.
- **Harness:** `ozbench` `markers` collector, which launches
  `ozviewer.exe --benchmark <image>` as a fresh process and captures its stdout
  marker JSONL. This is the supported invocation; a bare PowerShell
  `& ozviewer.exe` does not wait for the GUI-subsystem process.
- **Fixture source:** deterministic synthetic TurboJPEG gradient JPEGs produced
  by the existing `ozviewer_make_jpeg` test target.

`FirstPresent` is an **internal presentation-submission marker**. It is not
desktop-composed visibility (`ExternalFirstImageVisible`) and no interactive UI
latency, key handling, or GPU-visible behavior was measured. Nothing in this
document should be read as a claim about interactive or desktop-visible
behavior.

## 2. Build and commit identity

| Item | Value |
| :--- | :--- |
| Git branch | `main` |
| Git commit | `dad976a7d66130ab613b6db15edd76d5b2c44cac` |
| Commit subject | `feat(viewer): externalize UI strings for localization readiness` |
| Commit time | 2026-09-25 14:27:07 +0700 |
| Tracking | ahead of `origin/main` by 11 (no push performed) |
| Tracked working tree | clean |
| Build directory | `build/off-link-verify` |
| Generator / config | Visual Studio 17 2022, x64, `Release` |
| vcpkg triplet | `oz-x64-windows-static-md` |
| Feature flags | `OZVIEWER_ENABLE_EDITING=OFF`, `WEBP=ON`, `WEBP_ENCODER=ON`, `AVIF=ON`, `HEIC=ON`, `OZBENCH_USE_NLOHMANN=OFF` |
| Subject `ozviewer.exe` | `e449259fdcb6bf743e2ef7ca1e05a5997a196b6f46ee38f05386ef3cb526c6ac` (3,392,512 bytes) |
| Runner `ozbench.exe` | `071b0a0ad1365f251c38e7cd5a6f91aa7f5027fceb49b2da904a52978c792b9e` (286,720 bytes) |
| Fixture gen `ozviewer_make_jpeg.exe` | `53822634553bc9667b48dd97cf4c55279dac7a39caba9a76dbbceeaac5f13226` (495,616 bytes) |

The selected binaries were linked after the last tracked source modification
and the tracked tree equals `HEAD`, so they reflect this commit. The config has
editing **OFF**; the benchmark path is non-interactive and does not create the
edit/save controllers, but the binary layout and link set differ from an
editing-ON build, so this baseline is only directly comparable to builds with
the same configuration.

## 3. Host, OS, and display context

| Item | Value |
| :--- | :--- |
| Host | HP Victus by HP Laptop 16-e1xxx, x64 |
| OS | Microsoft Windows 11 Home Single Language, version 10.0.22631, build 22631 |
| Timezone | SE Asia Standard Time |
| CPU | AMD Ryzen 5 6600H with Radeon Graphics — 6 cores / 12 threads, 3301 MHz |
| Memory | 15.19 GB physical |
| GPU 0 | NVIDIA GeForce RTX 3050 Ti Laptop GPU — driver `32.0.16.1088` (2026-07-22) |
| GPU 1 | AMD Radeon(TM) Graphics — driver `31.0.12044.3` (2022-10-25), 1920×1080 @ 144 Hz |
| Display | `\\.\DISPLAY1`, 1920×1080, primary (single monitor reported) |
| Power | AC connected, battery 100% (`Win32_Battery` status 2) |
| Measurement window | 2026-09-25T08:00:47Z → 2026-09-25T08:01:06Z |

**Hybrid-GPU caveat:** the machine has two adapters. The benchmark creates a
D3D11 renderer, but the adapter actually selected per run was **not directly
measured**. Results must not be attributed to a specific GPU. GPU clocks,
thermal state, and DPI scaling were not recorded.

## 4. Fixtures

Each JPEG was generated **twice** by `ozviewer_make_jpeg` and the two SHA-256
digests matched, confirming deterministic generation. These bytes are identical
to the Stage 6 surrogate corpus.

| Alias | Dimensions | Pixels | Bytes | SHA-256 |
| :--- | :--- | ---: | ---: | :--- |
| `jpeg-2mp` | 2000×1000 | 2,000,000 | 88,804 | `c56d14c822da282d6611a1fd20404f429b1854f3afdc61184f2d9c3dc765a85c` |
| `jpeg-12mp` | 4000×3000 | 12,000,000 | 329,796 | `17d89c99d5079dea431a755c207744067e3815b3962ee18b88050687d7270a57` |
| `jpeg-24mp` | 6000×4000 | 24,000,000 | 583,828 | `52791a02861950b337fcc360909dac485e60e16efb9db7ae44e852f50d861939` |

## 5. Method and exact commands

Protocol per scenario: **3 warmups + 30 measured fresh-process launches**, run
in the order `jpeg-2mp`, `jpeg-12mp`, `jpeg-24mp` (90 measured launches total).
`cache` is the protocol label `warm`; no OS or application cache was purged.
Warmups are excluded from every statistic. Percentiles use the nearest-rank
convention of [`benchmark.md`](benchmark.md) section 6 (`sorted[ceil(p·N/100)−1]`,
median = P50).

```bat
REM 1. Deterministic fixtures (existing ozviewer_make_jpeg target)
build\off-link-verify\tests\Release\ozviewer_make_jpeg.exe <EVIDENCE>\corpus\jpeg-2mp.jpg  2000 1000
build\off-link-verify\tests\Release\ozviewer_make_jpeg.exe <EVIDENCE>\corpus\jpeg-12mp.jpg 4000 3000
build\off-link-verify\tests\Release\ozviewer_make_jpeg.exe <EVIDENCE>\corpus\jpeg-24mp.jpg 6000 4000

REM 2. Validate the plan
build\off-link-verify\src\runner\Release\ozbench.exe validate <EVIDENCE>\plans\plan-stage10.5-baseline.json

REM 3. Measure (3 warmups + 30 measured per scenario)
build\off-link-verify\src\runner\Release\ozbench.exe run <EVIDENCE>\plans\plan-stage10.5-baseline.json --scenario jpeg-2mp  --warmups 3 --repetitions 30 --out <EVIDENCE>\raw\jpeg-2mp.jsonl
build\off-link-verify\src\runner\Release\ozbench.exe run <EVIDENCE>\plans\plan-stage10.5-baseline.json --scenario jpeg-12mp --warmups 3 --repetitions 30 --out <EVIDENCE>\raw\jpeg-12mp.jsonl
build\off-link-verify\src\runner\Release\ozbench.exe run <EVIDENCE>\plans\plan-stage10.5-baseline.json --scenario jpeg-24mp --warmups 3 --repetitions 30 --out <EVIDENCE>\raw\jpeg-24mp.jsonl

REM 4. Summarize (nearest-rank)
build\off-link-verify\src\runner\Release\ozbench.exe summarize <EVIDENCE>\raw\<scenario>.jsonl --format json --out <EVIDENCE>\summaries\<scenario>-summary.json
build\off-link-verify\src\runner\Release\ozbench.exe summarize <EVIDENCE>\raw\<scenario>.jsonl --format csv  --out <EVIDENCE>\summaries\<scenario>-summary.csv
```

`<EVIDENCE>` = `benchmark\evidence\stage10.5-baseline-20260925-080011`.

Validation results: every `run_end` reported `launched=true`, `timed_out=false`,
`exit_code=0`, and empty `issues` / `validation` arrays; no
`FolderEnumeration` marker was emitted in any run. Each scenario produced 396
raw JSONL lines (33 launches × [1 `run_start` + 10 markers + 1 `run_end`]).

## 6. Baseline — `FirstPresent`

| Scenario | Samples | Median (ms) | P95 (ms) | P99 (ms) | Min (ms) | Max (ms) | IQR/median | MAD/median |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| jpeg-2mp | 30 | 126.868 | 144.616 | 153.122 | 112.237 | 153.122 | 0.100 | 0.043 |
| jpeg-12mp | 30 | 136.660 | 142.477 | 145.899 | 121.516 | 145.899 | 0.044 | 0.020 |
| jpeg-24mp | 30 | 138.016 | 147.561 | 150.268 | 118.350 | 150.268 | 0.068 | 0.033 |

## 7. Baseline — all markers (median / P95 / P99, ms)

### jpeg-2mp

| Marker | Median | P95 | P99 |
| :--- | ---: | ---: | ---: |
| ProcessStart | 0.002 | 0.002 | 0.002 |
| RendererReady | 43.598 | 51.615 | 63.379 |
| FileOpened | 89.044 | 101.922 | 114.995 |
| CodecDetected | 89.075 | 101.935 | 115.005 |
| HeaderParsed | 89.144 | 101.981 | 115.049 |
| DecodeStart | 89.150 | 101.985 | 115.053 |
| DecodeComplete | 96.228 | 109.230 | 121.038 |
| GpuUploadStart | 96.338 | 110.197 | 121.589 |
| GpuUploadComplete | 126.839 | 144.588 | 153.110 |
| FirstPresent | 126.868 | 144.616 | 153.122 |

### jpeg-12mp

| Marker | Median | P95 | P99 |
| :--- | ---: | ---: | ---: |
| ProcessStart | 0.002 | 0.002 | 0.002 |
| RendererReady | 44.517 | 51.446 | 51.911 |
| FileOpened | 91.227 | 97.391 | 98.193 |
| CodecDetected | 91.238 | 97.404 | 98.206 |
| HeaderParsed | 91.283 | 97.457 | 98.250 |
| DecodeStart | 91.286 | 97.461 | 98.253 |
| DecodeComplete | 106.421 | 112.546 | 112.833 |
| GpuUploadStart | 106.538 | 112.659 | 112.944 |
| GpuUploadComplete | 136.651 | 142.468 | 145.886 |
| FirstPresent | 136.660 | 142.477 | 145.899 |

### jpeg-24mp

| Marker | Median | P95 | P99 |
| :--- | ---: | ---: | ---: |
| ProcessStart | 0.002 | 0.002 | 0.002 |
| RendererReady | 44.251 | 51.865 | 52.099 |
| FileOpened | 90.907 | 97.599 | 99.302 |
| CodecDetected | 90.917 | 97.610 | 99.314 |
| HeaderParsed | 90.965 | 97.661 | 99.369 |
| DecodeStart | 90.968 | 97.666 | 99.372 |
| DecodeComplete | 106.634 | 116.020 | 120.696 |
| GpuUploadStart | 106.736 | 116.123 | 120.814 |
| GpuUploadComplete | 138.004 | 147.552 | 150.258 |
| FirstPresent | 138.016 | 147.561 | 150.268 |

## 8. Raw `FirstPresent` samples (chronological order, ns)

Full raw records for every marker and run are preserved in
`benchmark/evidence/stage10.5-baseline-20260925-080011/raw/*.jsonl`; the
per-scenario `summaries/*.json` contain the nearest-rank summaries. The
chronological measured `FirstPresent` samples are repeated here for audit:

```text
jpeg-2mp (30):
 144615800, 134382900, 130303500, 135071000, 118987400, 112237400, 138715700,
 135488900, 153122200, 120893100, 116875300, 130756500, 112516200, 126358800,
 139672500, 131893100, 128209300, 125120500, 121346900, 124812500, 124625000,
 117451800, 125223500, 121681700, 129977300, 127842200, 132286800, 126812000,
 126868000, 136169700

jpeg-12mp (30):
 127211500, 140494200, 141311200, 127042000, 140753000, 134399400, 139116200,
 137058200, 133877300, 132776900, 145898800, 136685000, 136660200, 136454700,
 140452000, 136018900, 137939300, 136328200, 130933500, 137992200, 130033500,
 139600600, 135665200, 135701300, 140861600, 142477100, 142270000, 137452000,
 121516400, 134625800

jpeg-24mp (30):
 138333100, 142506200, 131412200, 141767500, 126859200, 140573400, 138015800,
 136453000, 136256700, 142569800, 139159100, 143433500, 118349900, 119778800,
 136976700, 127665500, 138218100, 147561000, 144153400, 150268500, 144829900,
 133486500, 132996000, 142213600, 141991700, 135746900, 133152200, 132867100,
 134927500, 143053200
```

## 9. Variability and stability

The median is stable within the session; individual runs are not.

| Scenario | Split-half median drift | Robust SE of median | (P95−median)/median | Range/median |
| :--- | ---: | ---: | ---: | ---: |
| jpeg-2mp | −3.02% | 1.45% | 0.140 | 0.322 |
| jpeg-12mp | −0.26% | 0.69% | 0.043 | 0.178 |
| jpeg-24mp | +0.15% | 1.11% | 0.069 | 0.231 |

- Split-half drift compares the median of measured samples 1–15 against 16–30
  in chronological order; the largest absolute drift was **3.02%**.
- Robust SE of the median is `1.2533 × 1.4826 × MAD / √N` with N = 30; the
  largest was **1.45%**.
- Per-run spread is much larger: range/median reached **0.32** and the
  `(P95 − median)/median` gap reached **0.14**. This is why single runs are not
  a usable comparison unit.

## 10. Future comparison rule (provisional)

**Status: PROVISIONAL — not a durable gate yet.**

For a candidate build measured on the **same host, power state, and build
configuration** with the same protocol (≥30 measured runs, warmups discarded):

1. **Primary:** flag a regression only when the candidate `FirstPresent`
   **median** exceeds the baseline median by **more than +10%**.
2. **Secondary (lower confidence):** flag when the candidate `FirstPresent`
   **P95** exceeds the baseline P95 by **more than +25%** (requires ≥30
   samples; P95 at N = 30 is rank-coarse).
3. Do **not** gate on a single run, a single sample, or a mean; do **not**
   compare across hosts, power states, build configurations, or GPU adapters.

Rationale: a +10% median band is more than 3× the largest observed split-half
drift (3.02%) and more than 4× the largest robust standard error of the median
(1.45%). The per-run tail spread (up to 14% P95−median) makes single-run tail
gating indefensible.

Confirmation requirement: re-run this protocol in at least two independent
baseline sessions before treating the threshold as a hard gate. If baseline
medians drift by more than ~5% between sessions, no single threshold is
defensible and the observed drift should be reported instead.

## 11. Limitations and explicit non-claims

- **Single session, single host, single build configuration.** This is a
  reference point, not a cross-machine or historical baseline.
- **Adapter selection, GPU clocks, thermal state, and DPI scaling were not
  measured.** Do not attribute results to a specific GPU.
- **No interactive or desktop-visible timing was measured.** `FirstPresent` is
  an internal marker; no key handling, transition latency, or composed-frame
  behavior is covered.
- **Background activity was not isolated or monitored.** A 15.19 GB laptop was
  used with AC power connected.
- **Competitor comparison was not performed.** IrfanView / XnView templates
  exist but are disabled by default and were not run.
- **No leak-free or memory claim is made here.** Stage 10.4 private-byte soak
  results are separate evidence.

## 12. Artifacts and preservation note

Evidence directory:
`benchmark/evidence/stage10.5-baseline-20260925-080011/`

| Path | Contents |
| :--- | :--- |
| `conditions.json` | Environment, git + binary hashes, corpus hashes, exact commands, limitations |
| `plans/plan-stage10.5-baseline.json` | `oz.plan.v1` plan used verbatim |
| `corpus/*.jpg` | Deterministic fixtures |
| `raw/*.jsonl` | Canonical `oz.benchmark.v1` raw records (3 × 396 lines) |
| `summaries/*-summary.json`, `*.csv` | `ozbench summarize` output |
| `analysis.json` | Statistics, chronological raw samples, validation, stability, provisional threshold |

No source, test, CMake, or benchmark implementation files were modified. No
commit or push was performed. Pre-existing unrelated untracked workspace
artifacts were left untouched.

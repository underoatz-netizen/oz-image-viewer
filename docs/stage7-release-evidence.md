# Stage 7 Release Evidence & Benchmark Evaluation

## 1. Executive Summary & Release Recommendation

This document compiles the release and benchmark verification for **Stage 7 (Native Modern Chrome, SVG Icon Cache, Modern Dialogs, Contextual Crop Controls, Multi-DPI / Accessibility Hardening, and Benchmark UI Isolation)**.

- **Status**: **VERIFIED WITH DOCUMENTED OBSERVATIONAL WARNINGS**
- **Architecture & Isolation Integrity**:
  - All interactive UI chrome (toolbar, status bar, crop strip, open button, interactive menus, drag/drop acceptance, and modern dialogs) is strictly isolated to interactive non-benchmark mode.
  - Zero menus, open buttons, drag/drop registration, UI chrome, window controls, dialog infrastructure, or focus handling are instantiated in `--benchmark` or `--navigation-benchmark` paths.
  - Zero disk I/O, icon rasterization, or per-frame polling occurs in input or redraw loops.
  - 100% of interaction steps maintain 60 FPS presentation budgets (median 0.916 ms, P95 6.184 ms, max 8.174 ms; 0 steps > 16.7 ms; 0 redundant GPU uploads).
- **Automated Test Matrix**:
  - `candidate` (WebP ON, `build/stage6-surrogate-candidate`): 31 registered CTests; 29 executed and passed (100% of executed); 1,116/1,116 seam checks passed (0 failures). 2 registered tests unexecuted in non-interactive run: `oz_navigation_corpus_tool_smoke` (smoke-filtered) and `ozviewer_save_ui_smoke` (requires interactive desktop session).
  - `control` (WebP OFF, `build/stage6-surrogate-control`): 31 registered CTests; 29 executed and passed (100% of executed); 1,116/1,116 seam checks passed (0 failures). 2 registered tests unexecuted in non-interactive run: `oz_navigation_corpus_tool_smoke` (smoke-filtered) and `ozviewer_save_ui_smoke` (requires interactive desktop session).
  - `no-vcpkg` (dependency-free, `build/no-vcpkg`): 15 registered CTests; 15 executed and passed (100% of executed); 1,112/1,112 seam checks passed (0 failures). 0 unexecuted tests.
- **Release Recommendation**: **PROCEED TO FINAL RELEASE REVIEW**

---

## 2. Baseline Provenance & Benchmark Measurement Statement

### A. Statement on Pre-Stage-7 Baseline
- **Determination**: **`NO_COMPARABLE_BASELINE`**
- **Provenance Analysis**:
  - The repository contains prior benchmark evidence in `benchmark/evidence/stage6-surrogate-20260917-035453/`. As explicitly stated in `docs/stage6-surrogate-ttfi.md` and the Master Specification, that dataset was a **surrogate encoder-capability control** (comparing encoder enabled vs disabled within Stage 6), **not a historical pre-Stage-7 baseline**.
  - No historical pre-Stage-7 baseline exists for this toolchain, OS build, and environment that isolates Stage 7 changes without mixing codebases or using surrogate data.
  - In strict compliance with instructions and the Master Specification, we report `NO_COMPARABLE_BASELINE`. We do not claim "no regression relative to historical baseline", nor do we compare Stage 6 surrogate figures to Stage 7. We do not claim that the Master Specification's 5% regression gate has passed against a historical baseline since no valid pre-Stage-7 baseline is present.

### B. Current Absolute Post-Stage-7 Benchmark Results (Empirical Evidence)
Measured using the canonical neutral runner `ozbench` with 1 warmup + 20 measured fresh-process runs per cell, warm OS cache, measuring internal pipeline `FirstPresent` relative nanoseconds on the sealed 2MP, 12MP, and 24MP synthetic TurboJPEG gradient corpus. Note: These are non-interactive `--benchmark` pipeline measurements, not external interactive-launch or desktop-composed measurements.

#### Candidate Arm (`build/stage6-surrogate-candidate/src/viewer/Release/ozviewer.exe`)
- `jpeg-2mp` (2000×1000):
  - Median TTFI: **96.25 ms** (`96,248,700 ns`)
  - P95 TTFI: **112.90 ms** (`112,902,600 ns`)
  - P99 TTFI: **113.10 ms** (`113,095,000 ns`)
- `jpeg-12mp` (4000×3000):
  - Median TTFI: **100.06 ms** (`100,064,800 ns`)
  - P95 TTFI: **110.69 ms** (`110,692,500 ns`)
  - P99 TTFI: **114.28 ms** (`114,277,400 ns`)
- `jpeg-24mp` (6000×4000):
  - Median TTFI: **104.36 ms** (`104,360,900 ns`)
  - P95 TTFI: **111.82 ms** (`111,821,100 ns`)
  - P99 TTFI: **113.67 ms** (`113,672,100 ns`)

#### Control Arm (`build/stage6-surrogate-control/src/viewer/Release/ozviewer.exe`)
- `jpeg-2mp` (2000×1000):
  - Median TTFI: **95.91 ms** (`95,905,700 ns`)
  - P95 TTFI: **104.38 ms** (`104,379,600 ns`)
  - P99 TTFI: **108.94 ms** (`108,935,700 ns`)
- `jpeg-12mp` (4000×3000):
  - Median TTFI: **101.68 ms** (`101,682,800 ns`)
  - P95 TTFI: **108.72 ms** (`108,721,000 ns`)
  - P99 TTFI: **111.04 ms** (`111,042,100 ns`)
- `jpeg-24mp` (6000×4000):
  - Median TTFI: **97.26 ms** (`97,262,200 ns`)
  - P95 TTFI: **115.75 ms** (`115,746,400 ns`)
  - P99 TTFI: **117.50 ms** (`117,503,800 ns`)

#### Observation
Only medians are below 110 ms in both arms: all measured medians are below 110 ms (candidate medians: 96.25 / 100.06 / 104.36 ms; control medians: 95.91 / 101.68 / 97.26 ms). P95 and P99 values are NOT below 110 ms (candidate P95: 112.90 / 110.69 / 111.82 ms; control P95: 104.38 / 108.72 / 115.75 ms); therefore, no claim is made that TTFI is sub-110 ms in general or at P95/tail. External interactive-launch or desktop-composed startup was NOT measured (only non-interactive `--benchmark` internal pipeline FirstPresent was measured). No regression is proven, and no pre-Stage-7 historical baseline exists for this environment (see NO_COMPARABLE_BASELINE above). Absolute values and all documented caveats are strictly preserved.

---

## 3. Test & Verification Matrix

| Preset / Target | Seam / Core Checks | Registered CTests | Executed / Passed CTests | Unexecuted / Excluded Tests & Exact Reason | Interaction / Latency Evidence | Status |
|---|---|---|---|---|---|---|
| **Candidate** (`build/stage6-surrogate-candidate`) | 1,116 checks, 0 failures | 31 registered | 29/29 passed (100% of executed) | 2 unexecuted in automated non-interactive matrix:<br>1. `oz_navigation_corpus_tool_smoke` (excluded by `-E "smoke"`; smoke fixture generator check)<br>2. `ozviewer_save_ui_smoke` (excluded by `-E "smoke"`; requires interactive desktop session with foreground window focus) | 100/100 ≤ 16.7ms (100%), median 0.916ms | **PASS** |
| **Control** (`build/stage6-surrogate-control`) | 1,116 checks, 0 failures | 31 registered | 29/29 passed (100% of executed) | 2 unexecuted in automated non-interactive matrix:<br>1. `oz_navigation_corpus_tool_smoke` (excluded by `-E "smoke"`; smoke fixture generator check)<br>2. `ozviewer_save_ui_smoke` (excluded by `-E "smoke"`; requires interactive desktop session with foreground window focus) | 100/100 ≤ 16.7ms (100%), median 0.916ms | **PASS** |
| **No-vcpkg** (`build/no-vcpkg`) | 1,112 checks, 0 failures | 15 registered | 15/15 passed (100% of executed) | 0 unexecuted (all 15 registered tests executed and passed) | N/A (viewer skipped in no-vcpkg) | **PASS** |

*\**Detailed note on registered but unexecuted tests in Candidate and Control:***
- `ozviewer_save_ui_smoke`: Excluded from automated non-interactive CTest runs by `-E "smoke"`. Requires an interactive graphical Windows desktop session with active desktop window manager and input focus to interact with Windows shell dialogs. In headless/agent sessions, it cannot run and is never counted as passed.
- `oz_navigation_corpus_tool_smoke`: Excluded from the non-interactive matrix by `-E "smoke"`. Verified separately via direct execution (`ctest -R oz_navigation_corpus_tool_smoke` passes in 0.50s on candidate and 3.36s on control).*

---

## 4. Architectural & Quality Invariants Verified

1. **Benchmark UI Isolation**:
   - Verified via `check_benchmark_ui_isolation.cmake` (integrated as CTest `ozviewer_benchmark_ui_isolation_scan` across all presets).
   - In both `--benchmark` and `--navigation-benchmark` modes, zero menus, zero DragAcceptFiles registration, zero Open buttons, zero toolbar/status chrome, zero crop strips, zero dialog infrastructure, and zero edit/save controllers are created.
   - Decode-failure and present-failure branches in `kMsgDecodeComplete` use the shared `noninteractive_mode()` predicate (`benchmark || nav_benchmark`), so no modal error prompt can surface in either benchmark mode; interactive mode keeps its error prompts and recovery paths unchanged.
2. **Deterministic Multi-DPI Layout**:
   - Programmatically validated across 96, 120, 144, 168, and 192 DPI in client widths from 1400px down to 480px.
   - Proved mathematically that zero control overlap occurs between Ratio ComboBox, Dimension Summary, and Reset/Apply/Cancel buttons.
3. **Keyboard & Focus Contract**:
   - `Enter` applies crop, `Esc` cancels, `R` resets even when child controls inside crop strip or toolbar have keyboard focus.
   - `F6` cycles focus cleanly across top-level UI panes.
   - `Tab` / `Shift+Tab` traverses crop strip controls using native dialog message rules.
   - Global shortcuts (`Ctrl+O`, `Ctrl+S`, `Ctrl+Shift+S`, `Ctrl+Z`, `Ctrl+Y`, `Ctrl+L`, `Ctrl+R`, `Ctrl+Alt+R`, `F11`) function consistently regardless of child focus.
4. **Screen Reader & Accessible Names**:
   - Toolbar buttons expose complete accessible names (e.g., "Rotate left 90 degrees", "Rotate right 90 degrees") in both SVG icon mode and text-fallback mode.
   - High-contrast mode maps directly to Windows system colors (`COLOR_WINDOW`, `COLOR_WINDOWTEXT`, `COLOR_HIGHLIGHT`, etc.).
5. **Dialog Focus Resilience**:
   - Asynchronous validation failure in Resize dialog immediately restores input controls and focuses the active numeric edit box with full text selection.

---

## 5. Documented Limitations & Honest Testing Caveats

1. **Headless Execution Boundary**:
   - Physical multi-monitor drag-and-drop between differing physical monitor DPIs was verified via mathematical unit/seam tests against the Win32 layout algorithms, but cannot be physically observed on hardware displays in this headless agent container.
   - Screen reader (Windows Narrator) audio narration cannot be heard in a headless environment; verification is based on inspecting Win32 `TBIF_TEXT` and accessibility descriptor data structures.
2. **Interactive UI Smoke Test**:
   - `ozviewer_save_ui_smoke.ps1` requires an interactive Windows desktop with active desktop window manager and input focus. In headless execution, automated tests rely on isolated unit and seam fixtures.
3. **Absence of Historical Pre-Stage-7 Baseline**:
   - As established in Section 2, no historical pre-Stage-7 baseline exists for this environment, so regression deltas relative to prior stages cannot be calculated and are explicitly not claimed.

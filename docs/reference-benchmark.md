# Reference Benchmark — ExternalFirstImageVisible (Stage 1)

This document describes the reference-benchmark collector added in Stage 1:
`ozbench capture` with the `external_visible_frame` collector. It measures the
**ExternalFirstImageVisible** endpoint for `ozviewer`, IrfanView and XnView on
the same machine, same images, same warm/cold conditions.

## 1. Endpoint definition

**ExternalFirstImageVisible** is the QPC elapsed time from *immediately before
a direct `CreateProcessW`* until DXGI Desktop Duplication detects the **first
of two consecutive desktop-composed frames** where a fixed client-area image
probe matches the expected selected-JPEG appearance. The endpoint is
timestamped at that **first** matching frame; the second consecutive match only
*confirms* it (recorded as `confirm_ticks` provenance) and never replaces the
first-frame timestamp.

- The endpoint is **external**: it is observed from outside the subject, so it
  applies identically to `ozviewer`, IrfanView and XnView.
- It is **never** labelled `FirstPresent`, window-ready, or native TTFI.
  `ozviewer`'s internal `FirstPresent` marker is retained **only as a
  diagnostic** (the `markers` collector / `ozbench run`); the reference
  comparison uses `ExternalFirstImageVisible` exclusively.
- PresentMon / WPR are **not** the gate endpoint.

## 2. How the collector works

`ozbench capture <plan.json>`:

1. **Preflight** — refuses to start when the subject executable is already
   running. Unrelated processes are never killed.
2. **Launch** — the subject is launched directly with `CreateProcessW` inside
   a **Job Object with `KILL_ON_JOB_CLOSE`** (the whole process tree is
   cleaned up when the job handle closes). QPC is timestamped immediately
   before the launch call.
3. **Window association** — the subject's visible top-level window is found by
   PID (`EnumWindows`), raised above other windows, and its client area is
   resolved in physical pixels. The collector process sets **per-monitor-v2
   DPI awareness** before any window/client coordinate query so
   `GetClientRect` / `ClientToScreen` return physical pixels that map 1:1 onto
   the Desktop Duplication buffer (no manual DPI scaling). A runtime fallback
   scales logical coordinates by the monitor DPI when that awareness cannot be
   set; the active desktop is never assumed to be 100%.
4. **Desktop Duplication** — the desktop output of the window's monitor is
   duplicated (`IDXGIOutput1::DuplicateOutput`, D3D11 hardware with WARP
   fallback). A configured `monitor_index` that does not match the window's
   monitor is an explicit `wrong_monitor` failure.
5. **Probe** — each frame's client-area region is sampled on a fixed grid and
   scored with normalised luma statistics (mean, variance) and an edge
   density. A frame matches when the scores clear the sealed thresholds.
6. **Two consecutive frames** — detection (`detect_ticks`, the endpoint) is
   recorded at the **first** frame of a run of `consecutive_frames` matching
   frames; the run-completing frame records `confirm_ticks` as provenance only.
   `relative_ns` is always derived from `detect_ticks` (the first frame).
7. **Cleanup** — quiet wait (500 ms), `WM_CLOSE` request, allow 5 s, then Job
   cleanup (`TerminateJobObject`) if still running. The outcome is recorded
   (`exited` / `forced` / `failed`).

Explicit non-success reasons are recorded for: `missing_window`,
`wrong_monitor`, `blank_or_nonmatching`, `timeout`, `launch_failed`,
`capture_failed`, `cleanup_failed`, `preflight_failed`.

## 3. Private manifest and redaction

Source image paths are **private**. They live only in a sealed private
manifest (schema `oz.manifest.v1`) produced by
`benchmark/scripts/inventory-jpeg.ps1`:

```bat
powershell -NoProfile -ExecutionPolicy Bypass -File benchmark\scripts\inventory-jpeg.ps1 ^
  -Root 'E:\Pictures\n้ำอิง' -TargetsMP 2,12,24 -OutPrivate benchmark\manifests\selection.private.json
```

The script scans JPEG **headers only** (no decode, no copy, no writes to
source images), selects three distinct files closest to 2/12/24 MP (ties
broken by the lexicographically smallest lowercase SHA-256), and writes:

- `benchmark/manifests/selection.private.json` — the manifest (alias, private
  path, sha256, width, height, size_bytes, pixels, target_mp, deviation_mp)
- `benchmark/manifests/selection.private.json.sha256` — the **seal**: the
  lowercase hex SHA-256 of the manifest file bytes

`ozbench capture` rejects the manifest on any seal mismatch (**tamper
detection**). Public results carry only aliases (`jpeg-2mp`, `jpeg-12mp`,
`jpeg-24mp`), hashes and dimensions — never source paths. `subject_path` and
`image.path` are serialized as `null` in capture records.

## 4. Probe thresholds and pilot validation

The probe thresholds (`min_edge_density`, `min_luma_variance`,
`min_mean_luma`, `max_mean_luma`, `consecutive_frames`) are **sealed by an
excluded visual pilot validation** — they are never tuned per measurement.

Pilot validation procedure (excluded from all measurements):

1. For each of the **9 subject/image combinations** (3 subjects × 3 images),
   launch the subject once and visually confirm the image is displayed.
2. Record the probe metrics (`edge_density`, `luma_variance`, `mean_luma`)
   from the settled display.
3. Set thresholds that every settled display clears and that reject blank
   windows, the DWM window-appearance animation, and non-image content.
4. Seal the thresholds into the plan templates and re-run the pilot to
   confirm all 9 combinations still match.

The shipped templates carry the pilot-validated, sealed probe thresholds:
`min_edge_density: 0.3`, `min_luma_variance: 0.02`, `min_mean_luma: 0.02`,
`max_mean_luma: 0.98`, `consecutive_frames: 2`, which clear all settled test
images across all three subjects and reject blank windows and the DWM
window-appearance animation.

## 5. Cold protocol (user-friendly steps)

Goal: one measurement per fresh boot, before the tested app/image has ever
been opened.

1. **Restart Windows** and sign in.
2. Wait for the desktop to settle (quiet interval: no other apps, no
   background activity, screen unlocked and visible).
3. Run **exactly one** measurement before opening the tested app or image:
   ```bat
   ozbench capture plans\ozviewer-reference.json --set viewer_exe=build\stage1\src\viewer\Release\ozviewer.exe
   ```
4. **10 repetitions per subject/image** (the cold templates already set
   `repetitions: 10`), then **restart Windows** before the next subject or
   image.
5. Never combine cold and warm results.

## 6. Warm protocol (user-friendly steps)

Goal: 30 measured fresh launches in a fixed session, after the OS file cache
and application components are warm.

1. Sign in and open the desktop once.
2. Run the warm template (3 excluded warmups + 30 measured launches):
   ```bat
   ozbench capture plans\ozviewer-reference-warm.json --set viewer_exe=build\stage1\src\viewer\Release\ozviewer.exe
   ```
3. Between launches the collector waits 500 ms, requests close, allows 5 s,
   then performs Job cleanup; a forced cleanup is recorded in the result.
4. **No cache purging** is performed or required — warm means "as the OS and
   app leave it".

## 7. Templates

| Plan | Subject | Cache | Reps | Warmups |
| --- | --- | --- | --- | --- |
| `plans/ozviewer-reference.json` | ozviewer | cold | 10 | 0 |
| `plans/ozviewer-reference-warm.json` | ozviewer | warm | 30 | 3 |
| `plans/irfanview.json` | IrfanView (disabled) | cold | 10 | 0 |
| `plans/irfanview-warm.json` | IrfanView (disabled) | warm | 30 | 3 |
| `plans/xnview.json` | XnView MP (disabled) | cold | 10 | 0 |
| `plans/xnview-warm.json` | XnView MP (disabled) | warm | 30 | 3 |

The IrfanView template carries the isolated-config command field
`/ini="<dir>"`; the XnView MP template carries the confirmed isolated-config
command field `"-ini", "{{xnview_isolated_config_ini}}"`. Competitor templates
are **disabled by default** and are never auto-discovered or launched by the
harness or the automated tests.

## 8. Result records

`ozbench capture` emits three canonical records per repetition (schema
`oz.benchmark.v1`, all redacted):

- `run_start` — scenario (alias-based), subject, `endpoint:
  ExternalFirstImageVisible`, `manifest_id`, `run_id`, `pid`, image alias /
  hash / dimensions / bytes
- `external_visible` — `status`, `reason`, `relative_ns` (the measurement),
  `launch_ticks` / `detect_ticks` (first matching frame) / `confirm_ticks`
  (run-completing frame), monitor data, probe metrics + thresholds,
  cleanup outcome
- `run_end` — `status`, `reason`, `exit_code`, cleanup outcome

`ozbench summarize` groups matched `external_visible` records by
`(scenario, ExternalFirstImageVisible)` with the nearest-rank convention.
Non-matched captures are recorded explicitly and never summarized as
measurements.

Exit codes: `0` all matched · `3` any capture did not match · `4` capture
environment unavailable (no graphical desktop).
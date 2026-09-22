# Benchmark Protocol (Stage 0)

## 1. Timing markers

Exactly twelve marker names are approved, in canonical order:

| # | Marker | Meaning |
| --- | --- | --- |
| 0 | `ProcessStart` | Process entry; recorded at session construction |
| 1 | `RendererReady` | Renderer/device initialization complete |
| 2 | `FileOpened` | Requested file handle opened |
| 3 | `CodecDetected` | Codec identified (magic bytes, not extension) |
| 4 | `HeaderParsed` | Image header parsed |
| 5 | `DecodeStart` | Decode of the display-ready image begins |
| 6 | `DecodeComplete` | Decode finished |
| 7 | `GpuUploadStart` | Upload of image data to GPU begins |
| 8 | `GpuUploadComplete` | Upload finished |
| 9 | `FirstPresent` | First image-present call issued |
| 10 | `FolderEnumerationStart` | Async sibling enumeration begins (after FirstPresent) |
| 11 | `FolderEnumerationComplete` | Enumeration finished |

Each record captures:

- `raw_ticks` — the `QueryPerformanceCounter` value at record time
- `relative_ns` — integer nanoseconds since session start, derived as
  `(now - start) * 1e9 / frequency` without overflow for realistic sessions
- `thread_id` — the OS thread that recorded the marker

### Validation rules

- **duplicate** — a marker recorded more than once
- **out_of_order** — a marker recorded after a later-canonical marker
- **missing** — a marker that canonically precedes the last recorded marker
  but was never recorded (e.g. `DecodeComplete` without `DecodeStart`)

Multi-error behavior is non-duplicative: a duplicated marker is reported once
as `duplicate` and is **not** additionally reported as `out_of_order`;
`out_of_order` is only reported for the first occurrence of a marker that
appears after a later-canonical marker. `missing` is reported for every
canonical marker before the last recorded marker that was never recorded, so
a stream can legitimately carry several failures at once.

## 2. TTFI definition

TTFI (Time To First Image) is the `relative_ns` of the **`FirstPresent`**
marker. **Window creation / renderer readiness / app idle is NOT an
image-visible TTFI.** For GUI competitors that cannot emit markers, first
present must be collected by the external protocol (pending human input) and
imported with `ozbench import`; the runner never fabricates a FirstPresent.

### 2a. ExternalFirstImageVisible (reference endpoint)

The Stage 1 reference benchmark adds a second, **external** endpoint:
`ExternalFirstImageVisible`, measured by the `external_visible_frame`
collector via `ozbench capture` (see
[reference-benchmark.md](reference-benchmark.md)). It is the QPC elapsed time
from immediately before a direct `CreateProcessW` until DXGI Desktop
Duplication detects the first of two consecutive desktop-composed frames where
a fixed client-area image probe matches the expected selected-JPEG appearance.

- Used for `ozviewer`, IrfanView and XnView; never labelled `FirstPresent`,
  window-ready, or native TTFI.
- `ozviewer`'s internal `FirstPresent` remains a diagnostic only.
- PresentMon / WPR are not the gate endpoint.

## 3. Scenario identity

A scenario key is `subject|cache|image path`. "Identical scenario" means the
same key; summaries group by `(scenario, marker)`.

## 4. Cold / warm protocol

`cache` is a **protocol label only** (`cold` | `warm`). The runner never
purges the OS file cache or any application cache. The cache preparation
procedure is a pending human input (see `stage0-limitations.md`). Cold and
warm results are never combined.

## 5. Canonical output — UTF-8 JSON Lines (schema `oz.benchmark.v1`)

One JSON object per line. Record kinds:

- `run_start` — scenario, cache, repetition, warmup flag, subject,
  `subject_path` (the **resolved** executable path after placeholder
  substitution), `subject_version` (the plan's `version_hint`, a
  human-provided label — never auto-detected), `subject_sha256` (lowercase
  hex SHA-256 of the resolved executable file; `null` when the file cannot be
  read), image metadata/provenance, collector/adapter, `started_utc`
- `marker` — scenario, cache, repetition, warmup, marker name, `raw_ticks`,
  `relative_ns`, `thread_id`
- `run_end` — `launched` (false when `CreateProcessW` failed), and on launch
  failure `error` (GetLastError) / `error_message`; otherwise exit code,
  `duration_ns`, `timed_out`, `validation` array
  (duplicate/missing/out_of_order), `issues` array (runner-level: launch
  failure, unknown marker, malformed line, no markers)
- `external_visible` — the `ExternalFirstImageVisible` measurement: `status`
  (`matched` or an explicit non-success reason), `reason`, `relative_ns`,
  `launch_ticks`/`detect_ticks`/`confirm_ticks` (`detect_ticks` is the first
  matching frame and is the endpoint; `confirm_ticks` is the run-completing
  frame used only as provenance), `monitor` (index/name/size/refresh rate),
  `probe` (measured mean luma / variance / edge density plus the sealed
  thresholds and consecutive-frame rule), `cleanup` (outcome/forced/wait_ms)
- `summary` — scenario, marker, metric (`median`/`p95`/`p99`), `value_ns`,
  `count`, `convention`
- `validation` — plan path, `valid`, `subject_available`, errors, warnings
- `import` — externally collected samples bridged into the canonical stream

Capture records (`run_start`/`external_visible`/`run_end` from `ozbench
capture`) are **redacted**: `subject_path` and `image.path` are `null`, and
the image is identified by its public `alias` plus hash/dimensions/bytes.
`manifest_id` and `run_id` link every record to the sealed private manifest
and the capture invocation.

## 6. Summarization

- Group marker records by `(scenario, marker)`.
- Warmup repetitions are excluded unless `--include-warmups`.
- **Nearest-rank convention**: for N sorted samples, the p-th percentile is
  `sorted[ceil(p * N / 100) - 1]` (1-based rank). Median is P50.
- Output formats: `json` (array), `csv`
  (`scenario,marker,count,median_ns,p95_ns,p99_ns,convention`), `jsonl`
  (canonical `summary` records). CSV fields are RFC 4180-quoted: a field
  containing a comma, double quote, CR or LF is wrapped in double quotes with
  internal quotes doubled.

## 7. Plans (schema `oz.plan.v1`)

A plan models: program (`subject.executable`), `args`, cache condition,
`repetitions`/`warmups`, `timeout_ms`, `working_directory`, image
metadata/provenance (`images[]`), and `collector`/`adapter` selection.

- `collector: "markers"` — subject writes marker JSONL to stdout; the runner
  parses and validates it.
- `collector: "external_first_present"` — subject is a GUI app; the runner
  only launches and waits. First-present samples come from the external
  protocol via `ozbench import`. A failed launch is recorded on the `run_end`
  (`launched:false`, `error`, `error_message`, an `issues` entry) and the
  `run` command exits non-zero; it never appears as clean success.
- `collector: "external_visible_frame"` — the Stage 1 reference collector.
  Requires `ozbench capture` (never `ozbench run`), a sealed private manifest
  (`plan.manifest`), and an `endpoint` of `ExternalFirstImageVisible`.
  Optional `collector_config` carries `monitor_index` (-1 = the subject
  window's monitor), `window_wait_ms`, `frame_timeout_ms`, and the sealed
  `probe` thresholds (`min_edge_density`, `min_luma_variance`,
  `min_mean_luma`, `max_mean_luma`, `consecutive_frames`).
- Placeholders: `{{image_path}}` (per image) and `{{key}}` from `--set
  KEY=VALUE` or the `OZBENCH_FIXTURE_EXE` environment variable.
- Numeric bounds: `timeout_ms` must be in `[1, 86400000]` (24 h),
  `repetitions` in `[1, 1000000]`, `warmups` in `[0, 1000000]`. Negative
  values are rejected before narrowing (a negative value would otherwise wrap
  to a huge unsigned count and cause a pathological run loop); the same
  bounds apply to the `--repetitions`/`--warmups` CLI overrides.

## 8. Process launch

Subjects are launched directly with `CreateProcessW`; arguments are quoted
with the `CommandLineToArgvW`-compatible rule, so Unicode paths and spaces
are safe. `cmd.exe` is never used. On timeout the child is terminated and the
run is marked `timed_out`.

Stdout capture uses a non-blocking pipe reader. After the subject exits (or
is terminated on timeout), the runner drains remaining buffered output for a
bounded grace period (250 ms) and then stops reading, so a timed-out subject
whose grandchildren inherited the pipe write handle cannot leave the runner
blocked indefinitely. If such a grandchild keeps writing, output after the
grace period is truncated (see `stage0-limitations.md`).

## 9. Exit codes

`0` ok · `1` usage/IO error · `2` invalid plan · `3` subject
unavailable/disabled or launch failure (skipped/failed with a `validation`
record).
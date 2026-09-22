# Navigation Benchmark Protocol & Human Manual Validation Checklist

## 1. Architecture and Scope

The navigation benchmark measures image-to-image transition latency within the OZ image viewer (`ozviewer.exe`).

### Isolation Contract
- **Normal usage** (`ozviewer.exe <jpeg>`) and **startup benchmark** (`ozviewer.exe --benchmark <jpeg>`) remain strictly byte/schema compatible with Stage 0 and Stage 1/2.2. No cache probing, enumeration, persistent decode worker, synthetic corpus, or navigation instrumentation runs before `FirstPresent` exits.
- **Navigation benchmark** is an explicitly test-only CLI mode:
  ```bat
  ozviewer.exe --navigation-benchmark --corpus <corpus_dir> --out <out.jsonl> [--overwrite] [--os-cache uncontrolled|cold_protocol|warm_protocol] [--repetitions 5] [--rapid-burst 10] [--timeout-ms 10000]
  ```
- Uses the **exact same UI navigation request execution path** as interactive keyboard navigation (`WM_KEYDOWN`), ensuring fidelity without code duplication. Both paths decode navigation intent through the shared `expected_target_index` policy (no-wrap Stage 2.1 boundary rule).
- **Bounded options**: parser and core share one set of limits (`kNavBench*` constants in `navigation_benchmark_policy.hpp`). `--repetitions` accepts `[1, 1000]`, `--rapid-burst` accepts `[1, 100]`, `--timeout-ms` accepts `[1, 86400000]`. Every value accepted by the parser is accepted by validation and vice versa.
- **Watchdog**: `--timeout-ms` bounds every outstanding request, every quiesced step wait, and every scenario setup/re-arm wait. The first expired request or setup wait is recorded as a categorized `timed_out` task event and the run ends with `run_end.status = "timed_out"` instead of hanging.
- **Scenario isolation and re-arm**: every scenario begins with an explicit, bounded test-only setup/re-arm phase and never relies on the cursor the previous scenario left behind. Re-arm transitions are labeled `setup_rearm` `step` task events and are excluded from latency summaries (no `setup_rearm` scenario summary is emitted). When a measured step would otherwise fall outside the list (for example the 500th `cached_next` step on the 500-image corpus), the scenario re-arms inside itself instead of issuing a boundary request.
- **No-op steps**: when the navigation list is empty or a measured step is genuinely at a boundary, no request exists, so the step is recorded as a `no_op` task event (auditable, never a latency sample) and the state machine advances. A single-item corpus therefore completes. For the standard 500-image corpus the measured scenarios complete with zero no-ops.
- **No in-session directory switching**: the corpus directory is enumerated once at launch. Relocation/cold-OS-cache protocols are performed externally between launches (see section 7); there is no `--relocation-dir` option.

### Scenario protocols (`--repetitions N`, `--rapid-burst B`)

- `cached_next` (N measured steps): re-arm to a range with forward runway when needed; before every measured request the next target is verified resident for the current viewport (`await_cache`, no purging), then the ordinary shared Next path is issued. Only request-time-hit presents are eligible samples.
- `uncached_24mp_next` (at most 10 measured steps, `min(N, 10)`): before every measured request the scenario re-arms to a corpus index whose successor is a 24MP (6000x4000) image inside indices `0..49`, waits for decode work to drain, clears the app cache except the pinned current image, verifies a request-time miss, then issues the ordinary shared Next path. Only request-time-miss presents whose committed target is a 24MP image are eligible samples.
- `rapid_replacement` (up to `min(B, list_size - 1)` requests): re-arms to the start of the list, issues genuine ordinary navigation requests without quiescing so replacement occurs, then settles the newest outstanding request and requires at least one final successful present.
- `forward_back_sweep` (N steps): re-arms to a safe mid-range index (or index 0 for two-item lists) so the alternating Right/Left requests are valid for the whole sweep.
- `cache_memory_sweep`: re-arms to the last index, then issues Home and End through the ordinary shared path.

## 2. Metric Definition: `NavigationSuccessfulPresent`

- **Start timestamp**: `QueryPerformanceCounter` (QPC) tick taken immediately when the navigation request is accepted on the UI thread before worker plan scheduling or cache admission.
- **End timestamp**: `QueryPerformanceCounter` (QPC) tick taken immediately after successful swap chain `Present(0, 0)` returns on the UI thread (stamped before any cache bookkeeping, hashing or record serialization).
- **Visibility notice**: This measures the presentation submission endpoint (`NavigationSuccessfulPresent`), not desktop-composed visibility. It is never reported as desktop-visible timing.
- **Omission rule**: Latency percentiles and scenario summaries include ONLY presents matched to a real, still-current outstanding request. Unmatched presents, duplicate presents for an already-resolved request, superseded requests (e.g. during rapid keystrokes), failed requests, timed-out requests and no-op steps are recorded as individual task events for auditing but are strictly excluded from latency aggregations. No record ever receives a session-start placeholder latency.
- **Scenario sample eligibility**: a still-current successful present contributes to its scenario's percentiles only when it satisfies the scenario contract: `cached_next` requires `hit_at_request` and a committed index equal to the requested target; `uncached_24mp_next` requires `miss_at_request`, a committed 24MP target, and a committed index equal to the requested target. An ineligible successful present is still emitted as `successful_present` (it really presented) and is additionally audited as a `step`/`excluded` task event with a `sample_ineligible:*` reason; it never enters latency statistics. `scenario_summary.successful_count` counts eligible latency samples, while `run_end.total_presents` counts every accepted present, so ineligible samples are visible by comparison.
- **Cache state labelling**: `app_cache_state` on a `successful_present` is the state observed when the request was accepted (stored on the outstanding request), never re-derived at present time.

## 3. Cache Condition Distinctions

- `app_cache_state`: `hit_at_request` | `miss_at_request`. Describes whether the destination JPEG was already present in the viewer's CPU decoded cache at the exact instant the navigation request occurred.
- `os_cache_condition`: `uncontrolled` | `cold_protocol` | `warm_protocol`. Describes the system filesystem cache environment. Purging or evicting the app cache NEVER changes `os_cache_condition` to cold.
- `cache_entry_count` (cached `(path, decode-profile)` variants) and `resident_decoded_count` (distinct decoded source images resident) are separate metrics and are never reported as the same number.

## 4. Deterministic 500-JPEG Synthetic Corpus Tool

The helper executable `oz_navigation_corpus_tool.exe` handles corpus generation and validation without committing any images to git:

```bat
REM Generate synthetic 500-JPEG corpus in a local scratch directory:
build\stage1\tests\Release\oz_navigation_corpus_tool.exe generate benchmark\navigation_corpus

REM Validate corpus compliance (exact 500 count, magic, metadata-free, 24MP ratio, manifest hash + seal):
build\stage1\tests\Release\oz_navigation_corpus_tool.exe validate benchmark\navigation_corpus
```

Both commands accept an optional trailing count (`generate <dir> [count]`, `validate <dir> [expected_count]`; default 500). Small counts exist only for isolated smoke checks of the tool and are never final performance data; the shipped CTest check `oz_navigation_corpus_tool_smoke` uses this path with 4 images.

Corpus contract enforced by the validator:
- exactly the requested count of `.jpg`/`.jpeg` files and no foreign files (a `manifest.json` / `manifest.json.sha256` pair is allowed)
- JPEG SOI magic, parseable positive dimensions, and no EXIF/XMP/comment (`APP1..APP15`, `COM`) metadata segments
- sufficient 24MP images (`min(10, count)`; `0..49` are 24MP)
- when `manifest.json` exists, it must describe every file with matching sha256/width/height/bytes, and its `.sha256` seal must exist and match the manifest bytes

Corpus composition:
- `0..49`: 24 MP (6000x4000) for uncached heavy decode stress
- `50..149`: 12 MP (4240x2832)
- `150..299`: 6 MP (3000x2000)
- `300..449`: 2 MP (1920x1080)
- `450..499`: 0.5 MP (800x600)

## 5. Output Schema (`oz.navigation-benchmark.v1`)

The output is written as UTF-8 JSON Lines. Records:
- `run_start`: `run_id`, `corpus_manifest_hash`, `corpus_image_count`, `viewport_width`, `viewport_height`, `dpi`, `warp`
- `corpus`: image aliases, ordinals, sha256, width, height, file size (no absolute paths or user folder names)
- `navigation_request`: `request_id`, `epoch`, `direction` (`forward` | `backward` | `home` | `end`), `requested_target_index`, `requested_target_alias`, `from_index`, `from_alias`, `app_cache_state`, `os_cache_condition`, `request_ticks`, `relative_ns`
- `successful_present`: `request_id`, `epoch`, `committed_index`, `committed_alias`, `committed_sha256`, `app_cache_state`, `request_ticks`, `present_ticks`, `latency_ns`, cache memory footprint and entry counts (`cache_entry_count` vs `resident_decoded_count` distinct)
- `task_event`: `event_type` (`superseded` | `failed` | `timed_out` | `no_op` | `unmatched_present` | `duplicate_present` | `step`), `status`, `reason`, `relative_ns`. `step` events carry labeled setup transitions (`status: "setup"`, reason `rearm_to_index:<n>`) and excluded samples (`status: "excluded"`, reason `sample_ineligible:*`); they are audit records only and never latency samples.
- `memory_sample`: `phase`, process `private_bytes`, `working_set_bytes`, `peak_working_set_bytes`, cache budget/used/entry/resident counts
- `scenario_summary`: per-scenario counts (requested, successful, superseded, failed, timed_out, no_op) and nearest-rank median/P95/P99 latency
- `run_end`: overall duration, total requests, presents, superseded, failed, timed_out, no_ops, final status (`completed` | `timed_out`)

---

## 6. Human Manual Validation Checklist

Automation and unit tests MUST NOT claim human manual validation has occurred. A qualified human reviewer must execute the following checklist in an interactive desktop session:

### Pre-Run Checks
- [ ] Verify test environment display resolution (e.g. 1920x1080 or 4K) and Windows Display Scaling DPI.
- [ ] Confirm no concurrent background applications (video decoders, antivirus scans, game launchers) are active.
- [ ] Confirm `ozviewer.exe --benchmark <jpeg>` still emits the standard Stage 1 JSONL prefix and exits without folder markers.

### Corpus Setup
- [ ] Generate the 500-JPEG corpus into a local directory:
      `oz_navigation_corpus_tool.exe generate C:\temp\oz_corpus`
- [ ] Validate the generated corpus:
      `oz_navigation_corpus_tool.exe validate C:\temp\oz_corpus`
      Confirm output reports: `Corpus valid: exactly 500 JPEGs`.

### Execution
- [ ] Run the navigation benchmark in uncontrolled mode:
      `ozviewer.exe --navigation-benchmark --corpus C:\temp\oz_corpus --out C:\temp\nav_run_01.jsonl --overwrite`
- [ ] Observe visually:
  - Window opens with initial image displayed.
  - Sibling navigation starts automatically across cached and uncached steps.
  - Rapid replacement phase bursts without UI freeze or crash.
  - Forward/back and Home/End transitions complete smoothly.
  - Process exits cleanly with exit code `0`.

### Evidence Audit
- [ ] Open `C:\temp\nav_run_01.jsonl` and verify:
  - Header schema is `oz.navigation-benchmark.v1`.
  - Privacy check: grep for user names, drive letters, or absolute paths (must find NONE in public records).
  - Check `successful_present` latency numbers are non-negative and plausible.
  - Check `scenario_summary` for `cached_next`: `successful_count` equals the requested repetitions, `no_op_count` is `0`, and every `successful_present` for this scenario carries `app_cache_state: "hit_at_request"`.
  - Check `scenario_summary` for `uncached_24mp_next`: `successful_count` equals `min(repetitions, 10)` (5 with the default repetitions; pass `--repetitions 10` or more to require the full 10-sample uncached set), `no_op_count` is `0`, and every `successful_present` carries `app_cache_state: "miss_at_request"` with `committed_index < 50`.
  - Check `setup_rearm` `task_event` records exist for the labeled re-arm transitions, and that no `scenario_summary` is emitted for `setup_rearm`.
  - Check `scenario_summary` for `rapid_replacement`: verify `superseded_count > 0`, `successful_count >= 1`, `no_op_count == 0`, and latency percentiles reflect ONLY eligible successful presents.
  - Check no `step`/`excluded` events are present in a clean run (an ineligible sample means readiness/purge verification failed).
  - Check `run_end` record has `status: "completed"`.

### Directory Relocation / Post-Cleanup
- [ ] Delete temporary corpus `C:\temp\oz_corpus` and run artifacts after review.
- [ ] Sign and date the review evidence.

---

## 7. Cross-Launch Directory Relocation (External Procedure)

The benchmark never switches the corpus directory inside a run: the corpus is enumerated once at launch from the initial image's directory. OS-cache/cold-path provenance is therefore a **cross-launch** procedure performed by the operator between two separate `ozviewer.exe` invocations:

1. Run A (`--os-cache warm_protocol`): launch against the corpus in place, e.g. `C:\temp\oz_run_a\oz_corpus`.
2. Close the viewer completely (no benchmark process may remain).
3. Relocate the corpus on disk to a different directory/volume (for example copy `C:\temp\oz_run_a\oz_corpus` to `C:\temp\oz_run_b\oz_corpus` where `oz_run_b` has not been read recently), then validate it again with `oz_navigation_corpus_tool validate <new_dir>`.
4. Run B (`--os-cache cold_protocol`): launch against the relocated directory.

Rules:
- The two runs are separate `run_start` records; results must never be merged into a single scenario when the `os_cache_condition` differs.
- Evicting the app CPU cache (`--navigation-benchmark` does this internally for the uncached phase) does **not** make the OS cache cold and does not change `os_cache_condition`.
- `app_cache_state` labels remain per-request observations and are independent of relocation provenance.

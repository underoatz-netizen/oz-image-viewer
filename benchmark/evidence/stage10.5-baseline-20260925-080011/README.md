# Stage 10.5 Regression Benchmark Baseline Evidence

> Raw evidence for the Stage 10.5 baseline. The human-readable report is
> [`docs/stage10.5-benchmark-baselines.md`](../../../docs/stage10.5-benchmark-baselines.md).

## What this is

A same-host, same-build-config `FirstPresent` baseline for the current `main`
build (`dad976a`), measured through the existing Stage 0 marker protocol
(`ozviewer --benchmark` via the `ozbench` `markers` collector) on the existing
deterministic synthetic JPEG fixture generator.

It is **not** a desktop-visible, interactive-UI, GPU-adapter-specific, or
cross-machine baseline.

## Contents

| Path | Description |
| :--- | :--- |
| `conditions.json` | Host/OS/CPU/GPU/display/power context, git + binary hashes, corpus hashes, exact commands, limitations |
| `plan-stage10.5-baseline.json` (`plans/`) | `oz.plan.v1` plan used for all runs |
| `corpus/*.jpg` | Deterministic synthetic TurboJPEG gradient fixtures (2 MP, 12 MP, 24 MP) |
| `raw/*.jsonl` | Canonical `oz.benchmark.v1` raw output, one file per scenario (3 warmups + 30 measured launches each) |
| `summaries/*-summary.json` / `*.csv` | `ozbench summarize` nearest-rank median/P95/P99 per marker |
| `analysis.json` | Baseline statistics, per-run chronological raw samples, validation, stability, provisional threshold |

## Protocol

- 3 warmups + 30 measured fresh-process launches per scenario.
- Scenarios: `jpeg-2mp`, `jpeg-12mp`, `jpeg-24mp` (run in that order).
- `cache` is the protocol label `warm`; no cache purging was performed.
- Warmups are excluded from all statistics.
- Percentiles use the nearest-rank convention from `docs/benchmark.md` section 6.

## Headline result (FirstPresent median, 30 measured runs)

| Scenario | Median (ms) | P95 (ms) | P99 (ms) | Min (ms) | Max (ms) |
| :--- | ---: | ---: | ---: | ---: | ---: |
| jpeg-2mp | 126.868 | 144.616 | 153.122 | 112.237 | 153.122 |
| jpeg-12mp | 136.660 | 142.477 | 145.899 | 121.516 | 145.899 |
| jpeg-24mp | 138.016 | 147.561 | 150.268 | 118.350 | 150.268 |

Validation: all run records `launched=true`, `exit_code=0`, no timeouts, no
folder-enumeration markers. Subject SHA-256
`e449259fdcb6bf743e2ef7ca1e05a5997a196b6f46ee38f05386ef3cb526c6ac`.

The comparison threshold is **provisional** — see the report and
`analysis.json` → `threshold_recommendation`.

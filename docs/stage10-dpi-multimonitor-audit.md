# Stage 10.2 — DPI Awareness and Multi-Monitor Audit

Audit target: local `main` @ `1998b23384a490ea6936e8f319b200a52254d1bb` (post Stage 10.1).
Scope: DPI awareness, window placement, DPI-aware layout, renderer DPI, message handling,
fullscreen/restore, crop/resize/dialog layout, and reopen behavior. No UI redesign, no string
localization, no Stage 5/6/9 behavioral change, no commit/push.

## 1. Audit environment (actual hardware)

| Fact | Observed value |
|---|---|
| Monitors present | 1 active (`\\.\DISPLAY1`, primary) |
| Monitor bounds | 1920x1080, work area 1920x1032 (taskbar 48 px) |
| Effective DPI / scale | 96 DPI (100%) |
| Monitor count for mixed-DPI testing | **None** — no second monitor and no differing scale |

Because only a single 96-DPI monitor is available, the live mixed-DPI and real
`WM_DPICHANGED` transitions could not be physically exercised. Those rows are marked
`NOT EXERCISED` below and rely on deterministic pure/layout coverage plus code inspection.

## 2. Code audit summary

| Area | Implementation | Assessment |
|---|---|---|
| Process DPI awareness | `SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)` in `wWinMain` before `RegisterClassExW`/`CreateWindowExW` (`src/viewer/main.cpp`). No app manifest DPI entry. | Correct; live-confirmed PMv2. Return value unchecked: no fallback for pre-Win10-1703. Acceptable for a D3D11/D2D target; noted as compatibility note only. |
| Renderer DPI | `create_renderer` seeds `renderer::dpi` from `GetDpiForWindow`; `resize_renderer` re-applies `SetDpi(r.dpi, r.dpi)` after every `SetTarget`; `set_renderer_dpi` updates it on `WM_DPICHANGED`. Candidate bitmaps are non-target and inherit the device-context DPI, so DIP draw rects stay correct. | Correct; no target re-creation path loses DPI. |
| Layout | Pure `compute_ui_layout` (`ui_layout.cpp`) converts DIP→physical with `std::round`, snaps canvas top/bottom to chrome/status physical edges, and centers/clamps the Open button to the canvas physical rect. All partitions tile the client with no gaps at 96/120/144/168/192. | Correct and deterministically covered. |
| `WM_DPICHANGED` | Updates renderer DPI, `chrome->set_dpi` + `update_icons`, `crop_strip->set_dpi`, then `SetWindowPos` to the OS-suggested `RECT` (`SWP_NOZORDER|SWP_NOACTIVATE`). The resulting `WM_SIZE` recomputes layout, `resize_renderer`, viewport resize, and redraw. | Matches the documented per-monitor-v2 pattern. Live handler execution not exercised (single DPI). |
| Mouse/crop coordinates | `GET_X/Y_LPARAM` client coordinates are physical under PMv2; canvas rect, `view_geometry`/presentation plan, pan/zoom, and `crop_canvas_point_for_client` all consume physical pixels consistently. Crop overlay converts to DIP once via `view_geometry_to_dip`. | Correct; spec §97 "mouse coordinates / crop box" satisfied by construction. |
| Window placement / fullscreen | `toggle_fullscreen` saves style/exstyle plus `GetWindowPlacement` (properly `length`-initialized), uses `MonitorFromWindow`/`rcMonitor` for borderless fullscreen, and restores style then `SetWindowPlacement`. | Correct; live-confirmed exact restore. Saved placement is on the original monitor by design. |
| Crop strip layout | `crop_strip::update_layout` derives every metric from DIP via `dip_to_physical_round(dpi)`, right-aligns action buttons first, then collapses label/dimension safely. | Correct; live Win32 control test covers 96–192 DPI and 480–1400 px widths. |
| Resize dialog | In-memory template placed with dialog units; each control created through `MapDialogRect` (`resize_dialog.cpp`), so it is font/DPI-relative. Uses `DS_CENTER`. | DPI-correct by DLU mapping. Cross-monitor centering behavior not live-tested (see gaps). |
| Modern dialogs | OS common dialogs / `IFileDialog` / `TaskDialogIndirect` with the owner HWND supplied; DPI handled by the OS. | Correct. |
| Noninteractive/benchmark | `current_ui_layout_config` forces all chrome off and no chrome/crop HWNDs are created in `--benchmark` / `--navigation-benchmark`; only renderer DPI + window resize can occur. | Invariant retained. |

## 3. Validation matrix

`Method` legend: **Seam** = deterministic `ozviewer_stage7_seam_tests`; **Core** = `ozviewer_core_tests`;
**Live** = direct GUI observation of `ozviewer.exe` on the single 96-DPI monitor.

| # | Scenario | Method | Result |
|---|---|---|---|
| 1 | PMv2 awareness before HWND creation | Live | `GetWindowDpiAwarenessContext` = `PER_MONITOR_AWARE_V2`; window DPI 96. PASS |
| 2 | Layout at 96/120/144/168/192 DPI (100/125/150/175/200%) | Seam | Exact physical heights, edge tiling, canvas 0-height, Open-button centering/clamping. PASS |
| 3 | Chrome partitions tile client without 1-px gaps | Seam | `canvas.y == crop.bottom`, `canvas.bottom == status.y`, `canvas.height == status.y - crop.bottom`. PASS |
| 4 | Constrained/wide crop strip (480–1400 px) at 96–192 DPI | Seam (real controls) | No overlap/clipping; buttons always visible. PASS |
| 5 | Canvas DIP↔physical round-trip (100–200%) | Core | `fit_centered_dip` round-trips to physical fit within 1e-3. PASS |
| 6 | Empty/fullscreen layout (toolbar+status 0, crop strip retained) | Seam | Fullscreen crop strip stays at top, canvas consumes remainder. PASS |
| 7 | Renderer live path under PMv2 at 96 DPI | Live (`ozviewer_interaction_evidence`) | 1 initial upload, 0 redraw uploads, 96% ≤16.7 ms, no >50 ms, HW. PASS |
| 8 | Fullscreen enter (F11) → monitor rect, chrome hidden | Live | `0,0 1920x1080`, toolbar+status hidden, borderless style. PASS |
| 9 | Fullscreen restore → exact prior rect/style/chrome | Live | Restored to saved rect and style bit-for-bit; toolbar+status visible. PASS |
| 10 | Reopen behavior | Live | Fresh launch recomputes DPI (96) and re-creates chrome; default geometry cascades via `CW_USEDEFAULT` (no placement persistence). Observed, not a defect. |
| 11 | Move across monitors with differing DPI (real `WM_DPICHANGED`) | NOT EXERCISED | No second monitor / differing scale available. Deterministic DPI layout + handler code path only. |
| 12 | Fullscreen window moved to a different-DPI monitor | NOT EXERCISED | Requires mixed-DPI hardware; see gap G2. |
| 13 | Resize dialog on a secondary-DPI monitor (`DS_CENTER`) | NOT EXERCISED | Requires mixed-DPI hardware; see gap G3. |

## 4. Test evidence (this audit run)

- `ozviewer_stage7_seam_tests.exe` — **1658 checks, 0 failures** (includes multi-DPI metric scaling and real-crop-strip responsive layout at 96/120/144/168/192 DPI).
- `ozviewer_core_tests.exe` — **2898 checks, 0 failures** (includes `fit_centered_dip` 100–200% round-trip and UI-layout seams).
- `ozviewer_interaction_evidence.exe` — exit 0; live D3D hardware renderer: `INITIAL_UPLOADS: 1`, `REDRAW_UPLOADS: 0`, `STEPS_LE_16_7MS: 96 (96.0%)`, `STEPS_GT_50MS: 0`.
- Live single-monitor probe: PMv2 confirmed; F11 enter/restore exact; `RESTORE_EQUALS_SAVED: True`.

## 5. Gaps and open items

- **G1 — Live mixed-DPI / multi-monitor not exercised.** Only one 96-DPI monitor is present. The cross-process synthetic `WM_DPICHANGED` cannot be used because its `lParam` must be a valid in-process `RECT*`; sending a null/garbage pointer would crash the target. No automated substitute was attempted. Deterministic pure/layout tests at all five supported scales cover the math; the OS message plumbing remains code-inspected only.
- **G2 — Fullscreen + monitor DPI change (architect decision).** If a borderless fullscreen window is moved to a different-DPI monitor, the standard `WM_DPICHANGED` handler unconditionally applies the OS-suggested `RECT`, which preserves DIP size rather than `rcMonitor`. The window could shrink while `is_fullscreen` remains true. Resolving this is a Windows DPI policy choice (honor suggested rect vs. re-assert `rcMonitor` while fullscreen); it is **not** fixed here and is referred to the architect rather than guessed.
- **G3 — `DS_CENTER` resize dialog on a non-owner monitor (open question).** Whether the modal Resize dialog always centers on the owner's monitor under mixed DPI was not observable here. If Windows centers `DS_CENTER` on a different working area, an owner-relative placement would be required. No change made pending evidence/decision.
- **G4 — Compatibility note.** `SetProcessDpiAwarenessContext` failure (pre-Win10-1703, or a manifest that already set awareness) is not handled/verified; the process would fall back to system/unaware scaling. Not a defect for the current Windows 10+ D3D11 target.

## 6. Conclusion

- **Defect demonstrated:** none. Every covered path behaves correctly and deterministically; no source or test change was made.
- **Noninteractive invariants retained:** `--benchmark`/`--navigation-benchmark` still create zero chrome, zero dialogs, and zero crop controls; DPI awareness and layout math are unchanged.
- **Stage 5/6/9 scope untouched:** no edits to editing, navigation, save/copy/print/recycle, packaging, installer, accessibility, or benchmark code.
- Open items G2/G3 are Windows DPI policy/compatibility questions and are referred to the architect; G1/G4 are documented gaps, not defects.

Evidence snapshot date: 2026-09-25. Reviewer gate: not required (no source/test change); a read-only review of this audit artifact is optional.

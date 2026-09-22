# Stage 7 Contextual Crop Controls

## 1. Overview and Scope
This slice adds an interactive-only native contextual crop strip (`src/viewer/crop_strip.{hpp,cpp}`) that appears directly beneath the top toolbar whenever the existing Crop mode is active.
- **Contextual Strip UX (32 DIP)**:
  - **Aspect Ratio Selector**: Win32 combo box wired directly to existing Crop Ratio command IDs and presets (Free, Original, 1:1 Square, 3:2, 4:3, 5:4, 16:9, 9:16, and adaptive A-series).
  - **Read-Only Dimension Summary**: Live display of current crop width and height (`W × H`) sourced authoritatively from `crop_interaction_state::selection` without duplicating crop math.
  - **Actions**: Native buttons for Reset (`R`), Apply (`Enter`), and Cancel (`Esc`) triggering existing crop handler pathways.
- **Strict Precedence & Keyboard Safety**:
  - `C` enters crop mode, `Enter` applies, `Esc` cancels, `R` resets.
  - Mouse drags and eight-handle hit testing (`crop_canvas_point_for_client`) continue to operate on logical canvas coordinates.
  - Controls inside the crop strip safely route standard `Enter` (apply) and `Esc` (cancel), with Tab navigation between controls.
- **Layout & Fullscreen Integration**:
  - Integrated into `ui_layout_config` with `crop_strip_height_dip = 32.0f`.
  - When crop is active, canvas top snaps directly to `crop_strip_physical.bottom()`.
  - In Fullscreen mode (`F11`), normal toolbar and status bars hide (0 DIP), but an active crop strip remains visible at the top (32 DIP) with canvas adjusted accordingly. Outside crop mode, the crop strip consumes 0 DIP and has zero layout impact.
- **Benchmark Isolation**:
  - In `--benchmark` and `--navigation-benchmark` modes, `crop_strip_ui` is never instantiated and the crop strip consumes 0 DIP.

---

## 2. Component Reference

### Contextual Crop Strip (`src/viewer/crop_strip.{hpp,cpp}`)
- `create(parent, instance, dpi)`: Creates child controls (`STATIC` label, `COMBOBOX` for ratios, `STATIC` for dimensions, and `BUTTON`s for Reset, Apply, Cancel).
- `update_layout(layout, crop_active)`: Repositions controls to match physical pixel layout, or hides the container when crop mode is inactive.
- `sync_state(crop_state)`: Updates ratio selection and dimension string from authoritative `crop_interaction_state`.
- `sync_dimension_summary(crop_state)`: Lightweight text-only refresh of the `W × H` dimension summary (or `-` when no valid selection); called after every successful crop-state mutation (drag, reset, ratio change) so the summary never goes stale.
- `handle_ctl_color(hdc, control)`: Applies theme background and text colors.

### Layout Partitioning (`src/viewer/ui_layout.{hpp,cpp}`)
- `ui_layout_config`: Augmented with `crop_strip_height_dip` and `show_crop_strip`.
- `ui_layout_result`: Augmented with `crop_strip_logical` and `crop_strip_physical`.
- Canvas positioning: physical canvas top is calculated by snapping to `crop_strip_physical.bottom()` when visible, or `toolbar_physical.bottom()` otherwise.

---

## 3. Automated Tests & Headless Evidence
Tests are registered in `tests/viewer_core_tests.cpp` and standalone `tests/stage7_seam_tests.cpp`:
- `test_modern_dialogs_mapping_and_fallback()`:
  - Verifies crop strip layout across 96, 144, 192, and 240 DPI.
  - Verifies zero layout consumption when crop mode is inactive.
  - Verifies fullscreen mode layout: toolbar/status hide, crop strip remains visible at 32 DIP, and canvas spans remainder.
- `tests/interaction_evidence_test.cpp`:
  - `STAGE5_CROP_OVERLAY_OK` and `STAGE7_CROP_OVERLAY_OFFSET_OK` verify crop overlay drawing and nonzero canvas offset alignment without re-uploads.
- Historical slice evidence: all 14 CTests passed in `no-vcpkg` during slice development (prior to the addition of `ozviewer_benchmark_ui_isolation_scan`).
- Historical slice evidence: all 29 CTests passed in candidate (WebP ON) and control (WebP OFF) configurations during slice development (2,717 and 2,678 core checks; 253 seam checks; 0 failures).
- Crop-strip sync repair (final-review MUST_FIX): `sync_state`/`sync_dimension_summary` invoked from `reset_crop_mode`, `set_crop_ratio`, and the crop-drag `WM_MOUSEMOVE` path; seam/core tests cover ratio change, drag, and reset dimension updates (reset restores the active preset's centered crop, and switching back to Free restores full canvas).

*Authoritative Whole-Stage Verification*: For the current whole-stage status and verified counts across the complete Stage 7 matrix, see [docs/stage7-release-evidence.md](stage7-release-evidence.md) (Candidate/Control 31 registered CTests with 29 passed non-interactive, `oz_navigation_corpus_tool_smoke` passed directly, and `ozviewer_save_ui_smoke` unexecuted in headless sessions; 1,116 seam checks; No-vcpkg 15/15 passed CTests, 1,112 seam checks).

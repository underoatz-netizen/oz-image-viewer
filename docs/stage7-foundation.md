# Stage 7 Foundation — Command State, DPI Layout, and Canvas Seam

## 1. Overview and Architectural Invariants
Stage 7 introduces an interactive-only chrome layer (toolbars, status presentation, buttons) on top of the OZ Image Viewer without compromising startup latency or editing performance.

The Stage 7 foundation slice establishes two strict seams:
1. **Presentation-Only UI Command-State Seam** (`src/viewer/ui_command_state.{hpp,cpp}`):
   - Derives presentation states (`enabled`, `checked`, `visible`) for existing commands based purely on an immutable snapshot of authoritative application, document, crop, resize, and save state (`ui_command_context`).
   - Does **not** mutate domain state, duplicate save/crop/resize policies, or introduce new commands/shortcuts.
   - Respects benchmark mode: under `--benchmark` or `--navigation-benchmark`, command items are hidden/disabled to preserve benchmark isolation.
2. **DPI-Aware UI Layout / Canvas-Rectangle Seam** (`src/viewer/ui_layout.{hpp,cpp}`):
   - Computes toolbar, status bar, image canvas, and centered Open button rectangles in both logical DIP units and physical pixels.
   - Accepts arbitrary DPI scaling (including per-monitor v2 high-DPI factors).
   - In this foundation slice, default chrome heights remain `0.0f` DIP (chrome absent), preserving exact existing visual canvas bounds (matching client rect), while fully routing view math, zoom anchors, pan tracking, crop hit testing, and Direct2D drawing through the canonical canvas rectangle.
   - When chrome is introduced in subsequent slices, fit, zoom anchor, pan, crop hit testing, and presentation plans will automatically offset into `canvas_physical` rather than assuming client `(0, 0)`.

---

## 2. Component Reference

### UI Command State (`ui_command_state.hpp`)
- `enum class ui_command_id`: Identifiers for existing menu commands and actions (File Open/Save/Save As, Edit Rotate/Flip/Undo/Redo/Crop/Resize, Crop Ratios, View Fit/100%/Zoom, Navigation Prev/Next).
- `struct ui_item_state`: Value type representing presentation flags:
  - `enabled`: Command can be executed.
  - `checked`: Menu/toolbar toggle indicator is active.
  - `visible`: Element should be shown in UI.
- `struct ui_command_context`: Snapshot of authoritative states (document presence, source baseline validity, dirty state, undo/redo availability, crop interaction draft, async save/resize busy flags, view mode, fullscreen).
- `ui_item_state derive_command_state(ui_command_id, const ui_command_context&)`: Pure, side-effect-free derivation function.

### UI Layout (`ui_layout.hpp`)
- `struct ui_rect_logical`: Rectangle in DIPs (`x, y, width, height`).
- `struct ui_rect_physical`: Rectangle in physical pixels.
- `struct ui_layout_config`: Configuration options (`show_toolbar`, `toolbar_height_dip`, `show_status`, `status_height_dip`, `open_btn_width_dip`, `open_btn_height_dip`).
- `struct ui_layout_result`: Output containing partitions for client, toolbar, status bar, image canvas, and centered open button.
- `ui_layout_result compute_ui_layout(client_width, client_height, dpi, config)`: Deterministic layout calculation.

### Presentation Integration (`renderer.hpp`, `main.cpp`)
- `presentation_plan`: Extended with `canvas_offset_x` and `canvas_offset_y`.
- `apply_presentation_plan_drawing`: Direct2D rendering maps image draw and rotation centers by DIP-scaled canvas offsets.
- `draw_crop_overlay`: Direct2D crop overlay rendering maps image and selection rectangles by DIP-scaled canvas offsets, ensuring the crop frame and rule-of-thirds overlay remain locked in alignment with the presented image for nonzero canvas offsets.
- Interaction handlers (`WM_SIZE`, `WM_MOUSEWHEEL`, `WM_LBUTTONDOWN`, `WM_MOUSEMOVE`, `WM_LBUTTONDBLCLK`, `update_open_button_layout`, `crop_canvas_point_for_client`) derive coordinates and extents relative to `canvas_physical`.

---

## 3. Automated Verification and Tests
Automated tests are registered under `tests/viewer_core_tests.cpp` and standalone `tests/stage7_seam_tests.cpp`:
- `test_stage7_ui_command_state()` / `test_ui_command_state()`:
  - Validates benchmark isolation: all commands invisible and disabled.
  - Validates empty/zero-arg state: Open button visible/enabled; editing/saving disabled.
  - Validates clean loaded image: Save disabled (not dirty); Save As enabled; undo disabled; fit checked.
  - Validates dirty document: Save enabled; undo enabled; redo disabled.
  - Validates async save/resize modal collision: editing/saving blocked during in-flight operations.
  - Validates active crop mode: non-crop edits disabled; crop apply/cancel and active aspect ratio checked.
- `test_stage7_ui_layout()` / `test_ui_layout()`:
  - Validates baseline identity (no chrome): canvas physical exactly matches client area.
  - Validates High-DPI scaling (192 DPI): correct DIP-to-physical conversion and centered button scaling.
  - Validates synthetic chrome partitioning: toolbar (40 DIP) top, status bar (24 DIP) bottom, canvas spanning remainder without 1-pixel seams.
  - Validates physical clamping on small positive canvas sizes across fractional/high DPI (100x30 @ 96 DPI, 50x20 @ 144 DPI, 120x500 @ 240 DPI, 1x1 @ 192 DPI).
  - Validates degenerate dimensions: handles zero and negative inputs cleanly.
- `interaction_evidence_test.cpp`:
  - Includes `STAGE7_CROP_OVERLAY_OFFSET_OK` verifying crop overlay drawing alignment with nonzero canvas offsets (`canvas_offset_x = 32`, `canvas_offset_y = 48`).

All tests compiled and passed during slice development (historical slice evidence; see authoritative whole-stage verification in [docs/stage7-release-evidence.md](stage7-release-evidence.md)):
- Historical slice results: `build/stage6-surrogate-candidate` (WebP encoder ON): `ozviewer_core_tests` (2,558 checks / 0 failures), `ozviewer_stage7_seam_tests` (103 checks / 0 failures), 29 CTests passing.
- Historical slice results: `build/stage6-surrogate-control` (WebP encoder OFF): `ozviewer_core_tests` (2,519 checks / 0 failures), `ozviewer_stage7_seam_tests` (103 checks / 0 failures), 29 CTests passing.
- Historical slice results: `build/no-vcpkg`: `ozviewer_stage7_seam_tests` (103 checks / 0 failures), 14 CTests passing (prior to the addition of `ozviewer_benchmark_ui_isolation_scan`).

*Authoritative Whole-Stage Verification*: See [docs/stage7-release-evidence.md](stage7-release-evidence.md) for final verified counts across the full Stage 7 test matrix: Candidate/Control 31 registered CTests (29 passed in automated non-interactive run, `oz_navigation_corpus_tool_smoke` passed directly, `ozviewer_save_ui_smoke` unexecuted in headless sessions), 1,116 seam checks (0 failures); No-vcpkg 15/15 registered CTests passed, 1,112 seam checks (0 failures).

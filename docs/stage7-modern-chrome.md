# Stage 7 Modern Chrome — Architecture, Seams, and Verification

## 1. Overview and Scope
Stage 7 Modern Chrome implements an interactive-only, performance-first visual layer for OZ Image Viewer on Windows. It delivers:
- **System-Aware Non-Client Titlebar**: Follows Windows light/dark mode and High Contrast themes via `DwmSetWindowAttribute` (`DWMWA_USE_IMMERSIVE_DARK_MODE`) with runtime fallbacks. The standard non-client frame and caption buttons are preserved; no custom non-client hit testing is used.
- **Native Compact Toolbar (32 DIP)**: Built from standard Win32 common controls (`ToolbarWindow32`) with flat, nodivider styling. Contains 11 existing commands (Open, Save, Undo, Redo, Rotate Left, Rotate Right, Crop, Resize, Fit, 1:1, Fullscreen). Presentation states (enabled, checked) are derived strictly from the authoritative `ui_command_state` snapshot.
- **Embedded SVG Icon Cache & Transactional Toolbar Integration**: Encapsulates monochrome SVG assets for the 11 commands. Uses a lazy Direct2D cache (`ID2D1DeviceContext5` / `ID2D1SvgDocument`) keyed by icon ID, size, and theme color. Connected directly to the Win32 `ToolbarWindow32` control:
  - On supported systems with Direct2D SVG support, icons are rendered into an intermediate `D2D1_BITMAP_OPTIONS_TARGET` surface and copied to a separate `D2D1_BITMAP_OPTIONS_CANNOT_DRAW | D2D1_BITMAP_OPTIONS_CPU_READ` staging bitmap for CPU readback (strictly conforming to Direct2D bitmap option constraints).
  - Rasterization is strictly transactional: if SVG support, bitmap allocation, rendering, readback, or any individual icon creation fails, the partial `HIMAGELIST` is destroyed, and the toolbar falls back immediately to explicit text-labelled buttons (`Open`, `Save`, `Undo`, `Redo`, `RotL`, `RotR`, `Crop`, `Resize`, `Fit`, `1:1`, `Full`) via `TB_SETBUTTONINFO` (`TBIF_TEXT`) with `I_IMAGENONE`. No blank icon-only buttons are ever displayed or retained.
  - No eager icon rasterization occurs during startup or benchmark runs; the icon cache is rebuilt/invalidated strictly on DPI changes and theme changes.
- **Actionable Tooltips**: Native toolbar tooltips (`TTN_GETDISPINFOW`) display command names and keyboard shortcuts (e.g. `Save (Ctrl+S)`, `Fit to Window (F)`).
- **Canvas-Consistent Decode and Navigation Planning**:
  - `schedule_navigation_plan` and `open_new_document` derive viewport extents strictly from `current_ui_layout(app).canvas_physical` rather than the raw window client area `(rc.right, rc.bottom)`.
  - Guarantees exact parity between planned decode requests (e.g. TurboJPEG scaling denominator selection) and completion validation (`comp_queue` / cache key matching), preventing spurious request requeues or candidate rejections when chrome is active.
- **Event-Driven Status Bar (24 DIP)**: Four proportional partitions displaying document filename/format, pixel dimensions, zoom percentage/fit state, and navigation index. Updated purely on document/interaction state changes without per-frame polling or filesystem access.
- **Fullscreen Integration**: Fullscreen mode (`F11`) cleanly hides the toolbar and status bar and expands the image canvas to fill the display; exiting fullscreen restores the toolbar, status bar, and canvas offsets.
- **Benchmark Isolation**: Zero chrome controls, tooltip windows, GDI fonts, or icon caches are instantiated in `--benchmark` or `--navigation-benchmark` modes.

---

## 2. Component Reference

### UI Theme Seam (`src/viewer/ui_theme.{hpp,cpp}`)
- `enum class theme_mode`: `light`, `dark`, `high_contrast`.
- `query_system_theme_mode()`: Detects High Contrast via `SPI_GETHIGHCONTRAST` and Dark Mode via registry (`AppsUseLightTheme`).
- `get_theme_palette(theme_mode)`: Computes neutral chrome colors (background, text, button states, borders).
- `apply_dwm_caption_theme(HWND, theme_mode)`: Dynamically loads `dwmapi.dll` and applies caption dark mode attributes (attributes 20 and 19).

### SVG Icon Cache (`src/viewer/svg_icon_cache.{hpp,cpp}`)
- `enum class icon_id`: `open`, `save`, `undo`, `redo`, `rotate_left`, `rotate_right`, `crop`, `resize`, `fit`, `one_to_one`, `fullscreen`.
- `get_svg_icon_data(icon_id)`: Canonical monochrome SVG markup strings.
- `get_icon_fallback_label(icon_id)`: Short text fallback labels (`Open`, `Save`, `RotL`, `Fit`, etc.).
- `svg_icon_cache`: Lazy cache mapping `(icon_id, size_px, color)` to `ID2D1Bitmap`. Invalidate on DPI/theme changes.

### UI Chrome Manager (`src/viewer/ui_chrome.{hpp,cpp}`)
- Encapsulates toolbar, status bar, and tooltip HWNDs.
- `create(parent, instance, dpi)`: Creates child controls with Segoe UI fonts scaled to the active DPI.
- `update_layout(layout, is_fullscreen)`: Positions controls using `ui_layout_result` physical boundaries or hides them in fullscreen.
- `update_command_states(ctx)`: Synchronizes button enabled and checked states with `derive_command_state()`.
- `update_status(info)`: Updates status bar partitions event-driven.
- `handle_notify(lparam)`: Supplies tooltip text (`TTN_GETDISPINFOW`) and custom draw theming.
- `handle_ctl_color(hdc, control)`: Paints chrome background brushes for child controls.

---

## 3. Automated Tests & Headless Evidence
Tests are registered in `tests/viewer_core_tests.cpp` and standalone `tests/stage7_seam_tests.cpp`:
- `test_stage7_ui_command_state()`: Verifies command enabled/checked/visible states across all document, crop, modal, and benchmark scenarios.
- `test_stage7_ui_layout()`:
  - Verifies baseline 0-DIP chrome layout.
  - Verifies 32-DIP toolbar and 24-DIP status bar layout partitioning.
  - Verifies physical button clamping on small positive canvases (100x30, 50x20, 120x500, 1x1) at 96, 144, and 240 DPI.
  - Verifies fullscreen mode toggles chrome heights to 0 and restores full canvas extents.
  - Verifies decode planning canvas vs client dimension threshold consistency: proves that when 56px of chrome is active, an image scaling boundary selects `scale_den = 2` against the canvas viewport rather than defaulting to `scale_den = 1` against the client area, matching completion validation.
- `test_svg_icon_cache_and_fallback()`:
  - Verifies fallback labels and valid SVG markup exist for all 11 command icons.
  - Verifies non-poisoning capability semantics: querying `is_svg_supported(nullptr)` immediately returns `false` without locking `checked_svg_support_`.
  - Exercises Direct2D WARP headless rasterization using valid `TARGET` to `CPU_READ | CANNOT_DRAW` staging copy on systems with `ID2D1DeviceContext5`, asserting non-empty alpha pixels (`[stage7_seam_tests] Direct2D WARP SVG rasterization: VERIFIED`).
  - Reports an explicit skip condition if the runtime platform lacks `ID2D1DeviceContext5` while asserting clean text fallback activation.
- `tests/interaction_evidence_test.cpp`:
  - `STAGE5_CROP_OVERLAY_OK`: Proves crop overlay drawing.
  - `STAGE7_CROP_OVERLAY_OFFSET_OK`: Proves crop overlay rendering with non-zero canvas offsets (`offset_x = 32`, `offset_y = 48`).
- Historical slice evidence: all 14 CTests passed in `no-vcpkg` without external dependencies during slice development (prior to the addition of `ozviewer_benchmark_ui_isolation_scan`).
- Historical slice evidence: all 29 CTests passed in candidate (WebP ON) and control (WebP OFF) configurations (2,610 and 2,571 core checks; 155 seam checks; 0 failures).

*Authoritative Whole-Stage Verification*: For the current whole-stage status and verified counts across the complete Stage 7 matrix, see [docs/stage7-release-evidence.md](stage7-release-evidence.md) (Candidate/Control 31 registered CTests with 29 passed non-interactive, `oz_navigation_corpus_tool_smoke` passed directly, and `ozviewer_save_ui_smoke` unexecuted in headless sessions; 1,116 seam checks; No-vcpkg 15/15 passed CTests, 1,112 seam checks).

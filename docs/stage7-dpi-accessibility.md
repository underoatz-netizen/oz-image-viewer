# Stage 7 DPI and Accessibility Hardening

## 1. Overview and Scope
This slice solidifies per-monitor mixed-DPI transitions and keyboard/focus accessibility across all native UI controls introduced in Stage 7, preserving existing editing shortcuts, canvas panning/zooming, and benchmark isolation contracts.

- **Mixed-DPI Hardening (100% to 200% / 96 to 192 DPI)**:
  - Top-level `WM_DPICHANGED`: atomically queries suggested target rectangle `RECT`, updates renderer DPI via `set_renderer_dpi()`, updates chrome fonts/metrics via `ui_chrome::set_dpi()`, re-renders toolbar icons to physical pixels without caching stale sizes, updates crop strip fonts/metrics via `crop_strip::set_dpi()`, re-creates the zero-arg Open button font at proportional Segoe UI physical height, and triggers layout recomputation via `SetWindowPos(SWP_NOZORDER | SWP_NOACTIVATE)` followed by canvas resize.
  - Coordinate consistency: all layout partitions (toolbar 32 DIP, crop strip 32 DIP, status bar 24 DIP, open button 160×44 DIP) compute physical extents strictly through `dip_to_physical_round(dip, dpi)` without double-scaling or coordinate drift.
  - Fullscreen restore: fullscreen transition hides toolbar and status bar while preserving crop strip visibility when crop mode is active; restoration cleanly reparents and re-lays out controls to the restored monitor DIP scale.

- **Keyboard & Focus Accessibility**:
  - **Pane Cycling (`F6`)**: Iterates focus smoothly across active top-level regions:
    `Canvas Viewport (app.hwnd)` -> `Toolbar (hwnd_toolbar_)` -> `Crop Strip (first tab stop: Ratio ComboBox)` -> `Centered Open Button (if visible)`.
  - **Tab Traversal (`Tab` / `Shift+Tab`)**: Inside contextual crop strip, cycles through interactive controls (`Ratio ComboBox` -> `Reset Button` -> `Apply Button` -> `Cancel Button`) using `GetNextDlgTabItem`.
  - **Accelerators & Global Shortcuts**: Global shortcuts (`Ctrl+O`, `Ctrl+S`, `Ctrl+Shift+S`, `Ctrl+Z`, `Ctrl+Y`, `Ctrl+L`, `Ctrl+R`, `Ctrl+Alt+R`, `F11`) remain functional regardless of whether a child toolbar or crop strip control has keyboard focus.
  - **Strict Crop Modal Contract**: `C` begins crop mode; `Enter` applies crop; `Esc` cancels crop; `R` resets crop, even when a child control inside the crop strip or toolbar has focus.

- **Accessible Names & High Contrast**:
  - Toolbar buttons configure `TBIF_TEXT` with complete, concise accessible names (`Open file`, `Save image`, `Undo edit`, `Redo edit`, `Rotate left 90 degrees`, `Rotate right 90 degrees`, `Crop mode`, `Resize image`, `Fit image to window`, `Zoom 100 percent`, `Toggle fullscreen`) in both SVG icon mode and text fallback mode. Screen readers receive full command semantics rather than abbreviations (e.g. "RotL").
  - Tooltips retain keyboard shortcuts (`swprintf_s(..., L"%s (%s)", desc.tooltip, desc.shortcut)`).
  - Responsive crop strip layout: Action buttons (Reset, Apply, Cancel) and ratio combo are guaranteed visible and never clipped or overlapped at any DPI (96 to 192 DPI) or client width (from wide 1400px down to constrained 480px). Label and dimension summary reflow deterministically without overlapping action buttons.
  - High contrast theme uses system colors (`COLOR_WINDOW`, `COLOR_WINDOWTEXT`, `COLOR_HIGHLIGHT`, `COLOR_BTNFACE`, `COLOR_BTNTEXT`, `COLOR_WINDOWFRAME`) with visible focus rectangles.
  - Status updates and crop dimension summary refreshes do not steal focus or poll.

- **Resize Dialog Focus Resilience**:
  - Two-phase Apply validation failure or cancellation cleanly restores controls and returns keyboard focus to the active numeric input field (`edit_w` or `edit_pct`) with all text selected for rapid correction.

---

## 2. Manual Desktop and Screen Reader Validation Checklist

| Item | Step | Expected Result | Verified in Session |
|---|---|---|---|
| **1. Mixed-DPI Drag** | Drag viewer between 100% (96 DPI) and 150% (144 DPI) monitors. | Toolbar, status bar, crop strip, and Open button rescale sharply without blurry bitmap stretching or layout overlap. Canvas presentation plan adjusts without drift. | ⚠️ Automated seam tested (96, 120, 144, 168, 192 DPI); real multi-monitor drag requires interactive desktop. |
| **2. F6 Pane Navigation** | Launch empty app, press `F6`. | Focus moves from Canvas to Toolbar to Open button and back. Visible focus cues appear on controls. | Automated message-pump pre-filter verified; real screen reader test requires interactive desktop. |
| **3. Crop Strip Tab Traversal** | Load image, press `C`, then press `Tab`. | Focus enters Crop Strip at Ratio ComboBox, moves to Reset, Apply, Cancel, then wraps back. | Automated Win32 control test verified. |
| **4. Strict Crop Keyboard Contract** | With focus inside Crop Ratio ComboBox, press `Enter`. | Crop commits immediately to domain document; focus returns to main window. | Automated pre-filter verified. |
| **5. High Contrast Mode** | Enable Windows High Contrast (Left Alt + Left Shift + PrintScreen). | Application switches to system palette; custom draw items respect `COLOR_WINDOW` / `COLOR_WINDOWTEXT`. | Automated palette check verified (274 seam checks); real desktop visual requires interactive session. |
| **6. Screen Reader (Narrator)** | Turn on Windows Narrator (`Win + Ctrl + Enter`), tab through toolbar buttons. | Narrator announces button names ("Open file, button", "Crop mode, button") rather than empty labels. | Verified `TBIF_TEXT` with accessible names populated on toolbar items. |

*Honest execution note: In this headless CI/agent environment, Windows graphical desktop interactions and screen reader audio output cannot be observed directly. Deterministic programmatic verification covers all layouts, Win32 window classes, message filtering, and control trees. (Historical slice seam check count: 274 checks; see authoritative whole-stage verification in [docs/stage7-release-evidence.md](stage7-release-evidence.md) for final verified counts: Candidate/Control 1,116 seam checks / 31 registered CTests, No-vcpkg 1,112 seam checks / 15 registered CTests).*

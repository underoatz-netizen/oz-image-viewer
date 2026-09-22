# Stage 7 Modern Dialogs and Resize Presentation

## 1. Overview and Scope
This slice introduces an interactive-only modern dialog adapter seam (`src/viewer/modern_dialogs.{hpp,cpp}`) and modernizes the native modal Resize dialog (`src/viewer/resize_dialog.cpp`) for OZ Image Viewer on Windows:
- **`IFileOpenDialog` Adapter with Safe Fallback**: Uses modern COM-based `IFileOpenDialog` with `FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT` when available. Automatically and safely falls back to standard `GetOpenFileNameW` if COM or interface initialization fails. Preserves exact path/document routing, single-image launch, cancellation, and error handling.
- **`IFileSaveDialog` Adapter with Safe Fallback**: Uses modern COM-based `IFileSaveDialog` with `FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT`. Automatically falls back to standard `GetSaveFileNameW`. Returns user selection strictly to the existing authoritative save routing and save pipeline. Exact Stage 6 policies are fully preserved: typed-extension rejection, default format/filter behavior, TIFF Save-As-only constraint, source identity verification, non-rebinding of Save As copies, and atomic disk writes.
- **`TaskDialogIndirect` Prompt Adapter**: Adapts confirmation and error prompts via `TaskDialogIndirect` with safe `MessageBoxW` fallback, returning a choice code (`prompt_result`). Existing dirty-discard, in-flight save conflict, close/navigation/open/drop control flows remain authoritative.
- **Modernized `resize_dialog` Presentation**: Integrates system-aware theme colors (`ui_theme`) and DWM caption styling into the modal Win32 `DialogBoxIndirectParamW` dialog. Supports light, dark, and high-contrast styling across all dialog controls and edit fields (`WM_CTLCOLORDLG`, `WM_CTLCOLORSTATIC`, `WM_CTLCOLORBTN`, `WM_CTLCOLOREDIT`). Preserves exact modal ownership, two-phase asynchronous worker validation, live canvas preview callback, aspect ratio lock, and single-flight commit protocol.
- **Strict Benchmark Isolation**: `modern_dialogs` and COM initialization (`CoInitializeEx` / `CoUninitialize`) are scoped exclusively to interactive user invocations. Zero dialog, COM, or task dialog objects are created under `--benchmark` or `--navigation-benchmark` modes.

---

## 2. Component Reference

### Modern Dialogs Adapter (`src/viewer/modern_dialogs.{hpp,cpp}`)
- `enum class prompt_result`: `ok`, `cancel`, `yes`, `no`, `retry`.
- `struct save_as_dialog_result`: `accepted`, `destination`, `format`, `explicit_extension`.
- `show_open_image_dialog(parent)`: Invokes `IFileOpenDialog` (or `GetOpenFileNameW` fallback), returning selected path or nullopt on cancel.
- `show_save_as_dialog(parent, default_path, source_codec, webp_encoder_available)`: Invokes `IFileSaveDialog` (or `GetSaveFileNameW` fallback), returning destination path and format.
- `show_prompt(parent, title, main_instruction, content, buttons, icon)`: Invokes `TaskDialogIndirect` (or `MessageBoxW` fallback).

### Modernized Resize Dialog (`src/viewer/resize_dialog.cpp`)
- Retains native `DialogBoxIndirectParamW` with `MapDialogRect` for DPI-aware sizing.
- Applies `apply_dwm_caption_theme(hwnd, theme_mode)`.
- Implements theme-aware brush handling for dialog background (`WM_CTLCOLORDLG`), static labels (`WM_CTLCOLORSTATIC`), buttons (`WM_CTLCOLORBTN`), and edit controls (`WM_CTLCOLOREDIT`).
- Preserves all Stage 5 Slice 5 callbacks: `preview_cb`, `apply_cb`, and `notify_resize_dialog_result()`.

---

## 3. Automated Tests & Headless Evidence
Tests are registered in `tests/viewer_core_tests.cpp` and standalone `tests/stage7_seam_tests.cpp`:
- `test_modern_dialogs_mapping_and_fallback()`:
  - Verifies `prompt_result` return code mappings.
  - Verifies `save_as_dialog_result` initial values.
  - Verifies format extension parity with save pipeline contracts (PNG, JPEG, WebP, TIFF, BMP; rejects unsupported formats).
  - Verifies exact filter parity between `IFileOpenDialog` and legacy `GetOpenFileNameW`: derives from single authoritative `get_supported_open_filters()`, asserting `All Supported Images` contains all supported extensions (`*.jpg;*.jpeg;*.png;*.webp;*.avif;*.heic;*.heif;*.bmp;*.gif;*.tif;*.tiff;*.ico;*.cur;*.tga`), individual format categories (including GIF, ICO, CUR, TGA), and double-null terminated legacy filter string encoding.
- Historical slice evidence: all 14 CTests passed in `no-vcpkg` without external dependencies during slice development (prior to the addition of `ozviewer_benchmark_ui_isolation_scan`).
- Historical slice evidence: all 29 CTests passed in candidate (WebP ON) and control (WebP OFF) configurations (2,654 and 2,615 core checks; 190 seam checks; 0 failures).
- Scan and isolation tests (`ozviewer_save_isolation_scan`, `ozviewer_no_enumeration_scan`, `ozviewer_navigation_containment_scan`, `ozviewer_stage4_isolation`) all pass cleanly.

*Authoritative Whole-Stage Verification*: For the current whole-stage status and verified counts across the complete Stage 7 matrix, see [docs/stage7-release-evidence.md](stage7-release-evidence.md) (Candidate/Control 31 registered CTests with 29 passed non-interactive, `oz_navigation_corpus_tool_smoke` passed directly, and `ozviewer_save_ui_smoke` unexecuted in headless sessions; 1,116 seam checks; No-vcpkg 15/15 passed CTests, 1,112 seam checks).

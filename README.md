![OZ Image Viewer](assets/icons/ozviewer.svg)

# OZ Image Viewer

A fast, lightweight image viewer for Windows. OZ Image Viewer shows the first
image quickly, stays responsive while you zoom, pan and browse a folder, and
offers simple non-destructive editing — all in a small native application.

It is built with C++23, Win32, Direct3D 11, DXGI and Direct2D.

---

## Project status

> **In development.** OZ Image Viewer is at version 0.1.0. There is no public
> release and no public download.

- Source repository (public): <https://github.com/underoatz-netizen/oz-image-viewer>
- Product page (once the OZ Lab site is published):
  <https://underoatz-netizen.github.io/software/oz-image-viewer/>

## Platform

- Windows 10 (22H2) and Windows 11
- x64 only (32-bit and ARM64 are out of scope for now)

## Supported formats

Formats are detected by the file's own signature, never by the file extension.

| Kind | Formats |
| --- | --- |
| Open / view | JPEG (`.jpg`, `.jpeg`), PNG (`.png`), static WebP (`.webp`), BMP (`.bmp`), static GIF (`.gif`), TIFF page 0 (`.tif`, `.tiff`), ICO (`.ico`), CUR (`.cur`), TGA (`.tga`), AVIF (`.avif`) |
| Open / view (optional codecs) | HEIC/HEIF still images (`.heic`, `.heif`) via a dynamically loaded codec |
| Save / Save As | PNG, JPEG, WebP (lossless), BMP; TIFF (Save As only) |

WebP, AVIF and HEIC/HEIF support depend on the optional codecs being compiled in.

## What it does

**Viewing**

- Opens one image at a time and presents it fit to the window without enlarging
  images smaller than the window.
- Fast-first startup: the requested image appears before the folder is scanned;
  sibling enumeration and neighbour preloading run in the background.
- Navigates other images in the same folder with the keyboard, decoding each
  one as it is selected.
- Cursor-centred mouse-wheel zoom, keyboard zoom, drag-to-pan, fit-to-window and
  100% views, and borderless-windowed fullscreen.
- Opens from the command line, from an Open button, or by dragging a single file
  onto the window.

**Editing (non-destructive)**

- Rotate, flip, crop and resize, with undo and redo.
- Edits change the on-screen document only; the original file is not modified
  until you save.
- Save and Save As with the formats listed above.

## Keyboard and mouse

| Input | Action |
| --- | --- |
| `Ctrl`+`O` | Open an image |
| `Left` / `Right` | Previous / next image |
| `Home` / `End` | First / last image in the folder |
| `Space` / `Backspace` | Next / previous image |
| Mouse wheel | Zoom in or out at the cursor |
| `+` / `=` and `-` | Zoom in / zoom out |
| `F` | Fit to window |
| `1` | Actual size (100%) |
| Left-drag | Pan the image |
| Double-click | Toggle between fit and 100% |
| `F11` | Enter or leave fullscreen |
| `Esc` | Leave fullscreen, or cancel a crop |
| `C` | Enter crop mode |
| `Enter` / `R` | Apply the crop / reset the crop |
| `Ctrl`+`L` / `Ctrl`+`R` | Rotate left / rotate right |
| `Ctrl`+`Shift`+`H` / `Ctrl`+`Shift`+`V` | Flip horizontally / vertically |
| `Ctrl`+`Z` / `Ctrl`+`Y` | Undo / redo |
| `Ctrl`+`Alt`+`R` | Open the Resize dialog |
| `Ctrl`+`S` / `Ctrl`+`Shift`+`S` | Save / Save As |

## Prerequisites

- Windows 10/11 x64.
- Visual Studio 2022 Build Tools 17.6 or newer with the C++ workload
  (MSVC 14.4x and C++23; Visual Studio 2019's compiler is too old).
- CMake and Ninja, which ship with the Visual Studio Build Tools.
- vcpkg for the codec dependencies, pinned by `vcpkg-configuration.json`. The
  dependency-free benchmark harness (Stage 0) builds without vcpkg.

## Build

Stage 0 (no vcpkg), using the shipped presets:

```bat
cmake --preset vs2022
cmake --build --preset vs2022 --config Release
ctest --test-dir build\vs2022 -C Release --output-on-failure
```

Viewer and core (with the pinned vcpkg baseline, classic mode — no network):

```bat
cmake -S . -B build\stage1 -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE=build\vcpkg\scripts\buildsystems\vcpkg.cmake ^
  -DVCPKG_MANIFEST_MODE=OFF -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build\stage1 --config Release
ctest --test-dir build\stage1 -C Release --output-on-failure
```

## Quick start

```bat
REM Open one image; the window stays open.
build\stage1\src\viewer\Release\ozviewer.exe path\to\image.jpg

REM Benchmark mode: emit marker JSONL on stdout after FirstPresent, then exit.
build\stage1\src\viewer\Release\ozviewer.exe --benchmark path\to\image.png
```

## Documentation

- [Master coding specification](MASTER%20CODING%20SPECIFICATION.md)
- [Benchmark protocol and JSONL schema](docs/benchmark.md)
- [Reference benchmark: ExternalFirstImageVisible](docs/reference-benchmark.md)
- [Navigation benchmark](docs/navigation-benchmark.md)
- [Actual limitations and pending human inputs](docs/stage0-limitations.md)
- [Stage 8 AVIF/HEIC conformance audit](docs/stage8-conformance-audit.md)
- [Stage 10.6 release package validation](docs/stage10.6-release-package-validation.md)
- [Third-party notices](THIRD-PARTY-NOTICES.md)

## Limitations

- No RAW support; RAW and camera-raw containers are deliberately rejected.
- No animation support. Animated WebP and multi-frame GIF files are rejected
  rather than played, and animated PNG (APNG) shows only the first frame.
- No vector graphics, PDF or document formats.
- Multi-page or multi-frame files such as TIFF and ICO show a single page or
  frame only.
- WebP export is lossless single-frame only; lossy and animated WebP export are
  not supported.
- No 16-bit or HDR preservation; sources are reduced to 8-bit SDR.
- No plugins or third-party shell codecs; the supported extension set is a fixed
  allowlist.

## Licenses and notices

First-party source code in this repository is not covered by a top-level
`LICENSE` file at this time.

Third-party components and their licences are listed in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md). These include libjpeg-turbo,
libwebp, libavif with dav1d, and libheif with libde265. libheif and libde265 are
LGPL-3.0-only and are loaded dynamically as standalone DLLs that can be replaced
alongside `ozviewer.exe`.

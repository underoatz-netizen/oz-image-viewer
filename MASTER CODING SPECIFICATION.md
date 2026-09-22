# MASTER CODING SPECIFICATION
## Ultra-Fast Modern Image Viewer for Windows

**Document Status:** Development Baseline  
**Target Version:** v1.0  
**Primary Platform:** Windows x64  
**Primary Goal:** Build a modern, lightweight image viewer that prioritizes image-open speed, navigation responsiveness, low memory usage, and minimal UI overhead.

---

# 1. PRODUCT VISION

Build a native Windows image viewer with the following priorities, in this exact order:

1. Fast image opening
2. Fast next/previous navigation
3. Low memory and CPU overhead
4. Smooth zoom and pan
5. Reliable common image format support
6. Minimal modern UI
7. Essential image editing only
8. Maintainable modular architecture

The application should feel visually modern, similar in cleanliness to ImageGlass, while targeting performance competitive with or better than IrfanView for common image viewing workloads.

This is NOT intended to become:

- photo management software
- DAM software
- image catalog software
- RAW photo editor
- Lightroom alternative
- Photoshop alternative
- AI image editor
- batch image management suite

The application is first and foremost:

> A very fast image viewer with essential lightweight editing tools.

---

# 2. CORE PRODUCT PRINCIPLE

The application must follow:

## FAST-FIRST DESIGN

The first visible image is more important than:

- scanning the directory
- loading metadata
- creating thumbnails
- preloading other images
- initializing optional codecs
- loading print infrastructure
- loading settings not immediately required
- performing animation
- preparing editing tools

The requested image must appear first.

Everything else should be deferred or executed asynchronously.

---

# 3. SUPPORTED PLATFORM

## v1.0

Target:

- Windows 10 22H2
- Windows 11
- x64 only

ARM64 may be considered after v1.0.

32-bit Windows is out of scope.

---

# 4. PRIMARY TECHNOLOGY STACK

Use the following baseline unless profiling proves a component to be objectively unsuitable.

## Language

C++23

## Compiler

MSVC

Current supported Visual Studio toolchain.

## Build System

CMake

## Dependency Management

vcpkg with pinned dependency versions.

Avoid dependency versions that silently change during builds.

---

# 5. WINDOWS APPLICATION STACK

Use:

- Win32
- HWND
- Direct3D 11
- DXGI
- Direct2D
- DirectWrite
- DWM

Do NOT use the following for the main application shell:

- Electron
- Chromium Embedded Framework
- WebView
- Qt
- .NET MAUI
- WPF
- WinForms
- WinUI 3/XAML

unless a future architectural review explicitly changes this requirement.

The application must remain fundamentally native.

---

# 6. UI RENDERING ARCHITECTURE

Primary rendering path:

```text
Win32 HWND
    ↓
D3D11 Device
    ↓
DXGI Flip-Model Swap Chain
    ↓
Direct2D Device Context
    ↓
Image + UI Overlay
```

Direct2D is responsible for:

- image presentation
- toolbar drawing
- crop overlays
- selection borders
- UI surfaces
- SVG icons

DirectWrite is responsible for text.

DWM is responsible for appropriate native Windows window treatment.

---

# 7. UI DESIGN PHILOSOPHY

Use a custom lightweight modern UI.

Visual direction:

- minimal
- modern
- professional
- low visual noise
- Fluent-inspired
- compact controls
- image-first

Avoid:

- ribbons
- oversized menus
- multi-row toolbars
- heavy side panels
- permanent property inspectors
- nested configuration surfaces

Default viewing surface should prioritize the image.

---

# 8. DEFAULT WINDOW STRUCTURE

Conceptual layout:

```text
┌──────────────────────────────────────────────────┐
│                  Custom Title Bar                │
│                                                  │
│ ‹ › | ↶ ↷ | Flip | Crop Resize | − Fit + | ⋯   │
├──────────────────────────────────────────────────┤
│                                                  │
│                                                  │
│                     IMAGE                        │
│                                                  │
│                                                  │
├──────────────────────────────────────────────────┤
│ filename.jpg   6000×4000   8.4 MB   24 / 512   │
└──────────────────────────────────────────────────┘
```

Toolbar should use SVG icons.

Text labels should normally be hidden unless required.

Tooltips must identify:

- function
- keyboard shortcut

Example:

```text
Rotate Right
Ctrl+R
```

---

# 9. THEMES

Support:

- Dark
- Light
- System

Default:

System.

Dark mode image background should remain neutral and not overly bright.

---

# 10. ICONS

Use modern SVG icons.

Prefer Microsoft Fluent System Icons or internally authored equivalent SVG icons.

Only bundle icons actually used by the application.

Do not load a large external icon framework at runtime.

---

# 11. APPLICATION STARTUP RULE

Startup flow:

```text
Process Start
    ↓
Initialize minimum application state
    ↓
Create HWND
    ↓
Initialize renderer
    ↓
Open requested file
    ↓
Identify codec
    ↓
Decode display-ready image
    ↓
Upload required image data
    ↓
Present first image
```

Only after first image presentation may the application start:

- folder enumeration
- sorting
- metadata extraction
- preload
- optional codec loading
- image information analysis
- non-essential UI initialization

---

# 12. ABSOLUTE STARTUP RESTRICTIONS

Before First Image Present, do NOT:

- recursively scan directories
- enumerate an entire large image directory unnecessarily
- generate thumbnails
- inspect every file
- initialize all codec libraries
- initialize print infrastructure
- preload multiple unrelated images
- read all EXIF metadata
- initialize optional services
- perform update checks
- contact the internet

No network access should be required for normal image viewing.

---

# 13. IMAGE FORMAT SUPPORT — v1.0

Required:

- JPEG
- JPG
- PNG
- BMP
- GIF
- WebP
- TIFF
- ICO
- SVG
- HEIF
- HEIC
- AVIF

JPEG XL may be included if integration remains lightweight and stable.

---

# 14. RAW SUPPORT

RAW is explicitly NOT required in v1.0.

Do not include LibRaw or any RAW decoder in the main v1.0 binary.

Future support may include:

- CR2
- CR3
- NEF
- ARW
- RAF
- ORF
- RW2
- DNG

The architecture must allow future RAW support through a codec module without rewriting Viewer Core.

Example:

```text
codec_raw.dll
    ↓
LibRaw
```

---

# 15. CODEC STACK

Recommended primary codecs:

## JPEG

libjpeg-turbo

## PNG

libspng

WIC may be benchmarked against libspng.

Use whichever implementation produces the best real-world performance and reliability.

## WebP

libwebp

## AVIF

libavif + dav1d

Use libyuv where useful.

## HEIF / HEIC

libheif

Preferred decoder backend may include libde265.

## JPEG XL

libjxl, optional/lazy-loaded.

## BMP

WIC

## TIFF

WIC initially.

## GIF

WIC initially.

Dedicated animation handling may be added if required.

## ICO

WIC

## SVG

Direct2D SVG APIs.

---

# 16. CODEC MODULARITY

Create a common codec abstraction.

Conceptual interface:

```cpp
class IImageCodec
{
public:
    virtual bool Probe(...) = 0;
    virtual ImageMetadata ReadMetadata(...) = 0;

    virtual DecodeResult DecodePreview(...) = 0;
    virtual DecodeResult DecodeFull(...) = 0;

    virtual bool CanEncode(...) const = 0;
    virtual EncodeResult Encode(...) = 0;

    virtual bool IsAnimated(...) const = 0;
    virtual FrameResult DecodeFrame(...) = 0;

    virtual ~IImageCodec() = default;
};
```

Exact signatures may differ.

Core concept must remain.

---

# 17. CODEC REGISTRY

Use:

```text
CodecRegistry
│
├─ JPEGCodec
├─ PNGCodec
├─ WebPCodec
├─ WICCodec
│
└─ Lazy Modules
    ├─ AVIF
    ├─ HEIF
    └─ JXL
```

Codec detection should primarily use:

- file signature
- magic bytes
- actual container data

File extension is only a hint.

A PNG renamed to `.jpg` should still be identifiable where practical.

---

# 18. LAZY CODEC LOADING

Heavy optional codecs must not load during normal JPEG startup.

Example:

Opening:

```text
photo.jpg
```

must not initialize:

- libavif
- libheif
- libjxl

They should be loaded only when required.

---

# 19. IMAGE DOCUMENT MODEL

Create a clear `ImageDocument` abstraction.

Conceptual state:

```text
ImageDocument
│
├─ Source File
├─ Metadata
├─ Pixel Dimensions
├─ Orientation
├─ Decode State
├─ Render State
├─ Edit State
└─ Dirty State
```

The UI must not directly own decoded image state.

---

# 20. VIEW STATE

Maintain separate view state:

```text
ViewState
│
├─ zoom
├─ panX
├─ panY
├─ fitMode
├─ rotation
├─ flipHorizontal
└─ flipVertical
```

View-only changes must not modify source image pixels.

---

# 21. EDIT STATE

Editing must be non-destructive until save.

Conceptual model:

```text
Original
    ↓
Edit Operations
    ├─ Rotate
    ├─ Flip
    ├─ Crop
    └─ Resize
```

Only Save / Save As commits final output.

---

# 22. REQUIRED EDITING FUNCTIONS

v1.0 must include:

- Crop
- Resize
- Rotate Left 90°
- Rotate Right 90°
- Flip Horizontal
- Flip Vertical
- Save
- Save As

---

# 23. UNDO / REDO

Required.

Keyboard:

```text
Ctrl+Z
Ctrl+Y
```

Editing operations should use an edit-command history or equivalent reversible model.

View operations such as zoom do not necessarily need Undo.

---

# 24. CROP MODE

Crop must support:

- Free
- Original ratio
- 1:1
- 3:2
- 4:3
- 5:4
- 16:9
- 9:16
- standard A-series ratio

Crop rectangle must support drag handles:

```text
●────●────●
│         │
●         ●
│         │
●────●────●
```

Support:

- Apply
- Cancel
- Reset

Crop must not modify the original file until Save.

---

# 25. RESIZE MODE

Required parameters:

- Width
- Height
- Maintain Aspect Ratio
- Pixels
- Percent

Basic UI should show an automatic high-quality resize method.

Advanced algorithms may be hidden.

Potential algorithms:

- Lanczos
- Bicubic
- Bilinear
- Nearest Neighbor

Default:

High Quality Automatic.

---

# 26. JPEG ROTATE / FLIP

Where technically possible, JPEG rotation and flipping should support lossless transformations.

Avoid unnecessary JPEG decode/re-encode cycles.

This improves:

- speed
- quality preservation

Fallback to normal decode/encode where lossless transform is not applicable.

---

# 27. SAVE BEHAVIOR

## Save

Keyboard:

```text
Ctrl+S
```

Save modifies the existing file.

If there is risk of quality loss, the implementation must handle this safely.

Do not silently degrade an image through unnecessary repeated lossy encoding.

---

# 28. SAVE AS

Keyboard:

```text
Ctrl+Shift+S
```

Required initial output formats:

- JPEG
- PNG
- WebP
- BMP
- TIFF

AVIF / HEIC output can be supported where codec stability allows.

---

# 29. IMAGE VIEW NAVIGATION

Required:

```text
Left Arrow
Previous Image

Right Arrow
Next Image

Home
First Image

End
Last Image
```

Optional secondary shortcuts:

```text
Space
Next Image

Backspace
Previous Image
```

---

# 30. FOLDER ENUMERATION

When opening a file:

1. Show requested image first.
2. Enumerate sibling images asynchronously.
3. Create navigation list.
4. Sort.
5. Preload adjacent images.

Opening a file must NOT wait for full directory enumeration.

This requirement is critical for directories containing thousands of files.

---

# 31. THREADING MODEL

Recommended:

```text
UI Thread
│
├─ Win32 messages
├─ user input
├─ command dispatch
└─ render scheduling

Decode Worker Pool
│
├─ image decoding
└─ format conversion

I/O Worker
│
├─ file enumeration
├─ file reads
└─ filesystem operations

Background Tasks
│
├─ metadata
└─ optional preloading
```

Never decode large images synchronously on the UI thread.

---

# 32. TASK PRIORITY MODEL

Implement prioritized image tasks.

Suggested:

```text
P0 Current image
P1 Next image
P2 Previous image
P3 Next +2
P4 Previous -2
P5 Metadata and directory work
```

If the user rapidly changes image selection, obsolete decode tasks must be cancellable.

Do not waste CPU decoding images the user has already skipped.

---

# 33. PRELOAD STRATEGY

Do not use a fixed number of cached full-size images.

Use an adaptive memory budget.

Recommended initial policy:

```text
CacheBudget =
min(512 MB, approximately 10% of available system RAM)
```

This may be tuned through profiling.

Do not aggressively consume RAM merely because RAM exists.

---

# 34. CACHE LEVELS

Recommended:

```text
L1 GPU Cache
├─ Current
├─ Next
└─ Previous

L2 CPU Decode Cache
├─ Next + N
└─ Previous + N
```

Avoid uploading large numbers of images into VRAM.

---

# 35. LARGE IMAGE HANDLING

Do not assume every image should be fully decoded at native resolution immediately.

Example:

```text
Image:
8000 × 6000

Viewport:
1600 × 1000
```

If codec supports scaled or reduced-resolution decode:

decode close to required display resolution first.

Full-resolution decode may happen when needed for:

- 100% zoom
- detailed zoom
- crop
- resize
- save

---

# 36. JPEG FAST PATH

JPEG is the most important initial format.

The JPEG pipeline should aggressively optimize for:

- scaled decode
- SIMD decode
- minimal allocation
- low copying
- direct upload path where practical

Stage 1 development should focus heavily on JPEG performance.

---

# 37. ZOOM

Required:

- Zoom In
- Zoom Out
- Fit to Window
- 100%
- mouse wheel zoom

When zooming with the mouse wheel:

> Zoom toward the point under the cursor.

Do not always zoom toward image center.

---

# 38. ZOOM IMPLEMENTATION

After an image is available for rendering, normal zoom changes must use GPU transforms.

Do not re-decode image pixels every time zoom changes.

Conceptual transform:

```text
Translate
× Rotate
× Scale
× Flip
```

---

# 39. PAN

When displayed image exceeds viewport:

Left Mouse Drag = Pan.

Cursor:

```text
Hover:
Open Hand

Dragging:
Closed Hand
```

Pan must remain smooth and must not trigger image re-decoding.

---

# 40. DOUBLE CLICK

Default:

```text
Fit → 100%
100% or larger → Fit
```

Settings may include:

```text
Double Click:
○ Fit ↔ 100%
○ Always Fit
```

---

# 41. FULLSCREEN

Keyboard:

```text
F11
```

Fullscreen mode should:

- hide unnecessary chrome
- maximize image area
- optionally auto-hide toolbar

Moving pointer toward top may reveal controls.

---

# 42. COPY FUNCTIONS

Required:

```text
Ctrl+C
Copy Image
```

Additional contextual commands:

- Copy Image
- Copy File
- Copy File Path

---

# 43. CLIPBOARD INPUT

Support opening an image from clipboard where practical.

Potential shortcut:

```text
Ctrl+V
```

If clipboard contains a valid image, create a temporary document.

---

# 44. DELETE

Delete should:

- move file to Recycle Bin
- NOT permanently delete by default

After deletion:

display next valid image.

Do not crash if current directory contents change externally.

---

# 45. DRAG AND DROP

Support:

- single image
- multiple images
- folder

Multiple images may create a temporary navigation list.

Do not require importing files.

---

# 46. CONTEXT MENU

Right-click menu may include:

- Copy Image
- Copy File
- Copy Path
- Open File Location
- Rotate
- Flip
- Crop
- Resize
- Save As
- Print
- Set as Wallpaper
- Properties

Keep context menu concise.

---

# 47. STATUS BAR

Optional status bar should display:

- filename
- dimensions
- file size
- current image index
- image count

Example:

```text
DSC01234.jpg
6000×4000
8.4 MB
24 / 512
```

Status bar should be hideable.

---

# 48. IMAGE METADATA

Use WIC or appropriate parser for common metadata.

Minimum requirement:

- EXIF orientation
- pixel dimensions
- basic image metadata
- ICC profile awareness

Correct EXIF orientation is mandatory.

---

# 49. COLOR MANAGEMENT — v1.0

Required:

- sRGB
- basic embedded ICC handling
- SDR display

Optional:

- limited 10-bit source decoding downconverted for SDR display

Future:

- full HDR viewer
- advanced HDR display management
- professional wide gamut workflow
- 16/32-bit editing

Full HDR is explicitly outside v1.0 scope.

---

# 50. PRINTING

Print is required.

However print infrastructure must be lazy-initialized.

Do not initialize print subsystem during application startup.

Print options should remain simple:

- Fit to Page
- Actual Size
- Orientation
- Margins
- Printer selection

No advanced print-layout engine is required.

---

# 51. SETTINGS

v1.0 settings should remain small.

Required settings:

- Theme
- Mouse wheel behavior
- Double-click behavior
- Status bar show/hide
- Toolbar behavior
- basic image background
- default JPEG save quality

Avoid a large settings system.

---

# 52. DEFAULT MOUSE BEHAVIOR

Recommended default:

```text
Mouse Wheel
Zoom

Ctrl + Wheel
Optional alternate behavior if needed
```

Future configurable mode may support:

```text
Wheel
Next/Previous

Ctrl+Wheel
Zoom
```

---

# 53. KEYBOARD SHORTCUT BASELINE

Recommended:

```text
Left
Previous

Right
Next

Home
First

End
Last

Ctrl+S
Save

Ctrl+Shift+S
Save As

Ctrl+Z
Undo

Ctrl+Y
Redo

F11
Fullscreen

Delete
Recycle Current File

Esc
Exit current mode / cancel

Ctrl+C
Copy Image
```

Additional shortcuts may be assigned later.

---

# 54. PERFORMANCE PHILOSOPHY

Performance claims must be measured.

Do NOT describe the application as faster than another viewer based on subjective feeling.

Use reproducible benchmarking.

---

# 55. PRIMARY PERFORMANCE METRIC

Use:

## TTFI

Time To First Image

Definition:

```text
Application launch requested
        ↓
Process starts
        ↓
Window is created
        ↓
File opens
        ↓
Image decodes
        ↓
GPU presentation
        ↓
Image visibly appears
```

TTFI is the most important startup metric.

---

# 56. INTERNAL TIMING MARKERS

Debug and benchmark builds should record:

```text
ProcessStart
RendererReady
FileOpened
CodecDetected
HeaderParsed
DecodeStart
DecodeComplete
GpuUploadStart
GpuUploadComplete
FirstPresent
FolderEnumerationStart
FolderEnumerationComplete
```

Example output:

```text
Process Start      0.0 ms
HWND Ready        11.8 ms
File Opened       14.2 ms
Codec Detected    14.5 ms
Header Parsed     15.1 ms
Decode Start      15.3 ms
Decode Complete   41.7 ms
GPU Upload        46.8 ms
First Present     50.3 ms
```

---

# 57. BENCHMARK TOOLING

Use:

- ETW
- Windows Performance Recorder
- Windows Performance Analyzer
- internal high-resolution timing
- repeatable benchmark CLI

Optional:

```text
Viewer.exe --benchmark <file>
```

Benchmark tool must be able to run without manual interaction.

---

# 58. BENCHMARK DATASET

Create:

```text
benchmark/
└─ images/
```

Recommended dataset:

```text
JPEG 2 MP            100
JPEG 12 MP           100
JPEG 24 MP           100
JPEG 48 MP            30
PNG screenshots       50
PNG transparency      30
WebP                   50
AVIF                   30
HEIC                   30
TIFF                   20
Animated GIF           20
Huge image             10
```

Huge images should include approximately:

```text
80–150 MP
```

Do not change benchmark images between performance comparisons.

---

# 59. BENCHMARK HARDWARE

Use at least two hardware classes.

## Low / Mid System

Approximately:

- 4–6 core CPU
- 8 GB RAM
- SSD
- integrated or entry GPU

## Main Test System

Approximately:

- modern Ryzen 5 / Core i5 or better
- 16 GB RAM
- NVMe SSD
- mainstream GPU or modern integrated graphics

Performance tuning must not target only a high-end machine.

---

# 60. COLD VS WARM BENCHMARK

Always report separately:

## Cold Launch

Application and image data not intentionally warmed.

## Warm Launch

Operating system file cache and application components may already be warm.

Never combine these results.

---

# 61. PERFORMANCE TARGETS

These are engineering goals, not assumed guaranteed values.

On Main Test System:

```text
Warm empty window startup:
≤ 50 ms target

2 MP JPEG TTFI:
≤ 50 ms target

12 MP JPEG Fit TTFI:
≤ 70 ms target

24 MP JPEG Fit TTFI:
≤ 90 ms target

Cached Next:
≤ 16.7 ms target

Uncached 24 MP JPEG Next:
≤ 80 ms target

UI input response:
≤ 16.7 ms target

Zoom/Pan P95:
≤ 16.7 ms frame

Zoom/Pan P99:
≤ 25 ms

Idle RAM:
≤ 40 MB target

Base application:
target < 20 MB excluding optional codecs
```

Do not fake these results by disabling required functionality.

---

# 62. PERFORMANCE COMPARISON

Primary competitors:

- IrfanView
- XnView MP
- ImageGlass

Use:

- same machine
- same image
- same folder
- same storage
- same warm/cold conditions
- same number of repetitions

---

# 63. IRFANVIEW PERFORMANCE GATE

For JPEG, PNG, and WebP:

Calculate:

```text
Our Median TTFI
÷
IrfanView Median TTFI
```

Preferred target:

```text
≤ 0.90
```

Equivalent to approximately 10% faster median performance.

However do not optimize one benchmark at the cost of real-world usability.

---

# 64. RELEASE PERFORMANCE GATE

Before v1.0 release:

For core JPEG tests:

- application should beat IrfanView in at least 3 of 4 major scenarios if achievable
- application should not regress by more than roughly 10% in common image scenarios without documented reason

More important:

Navigation and interaction must feel consistently immediate.

---

# 65. LATENCY STATISTICS

Do not report only averages.

Report:

- Median
- P95
- P99

Example:

```text
Median
12 ms

P95
18 ms

P99
31 ms
```

Outliers matter because they create visible stutter.

---

# 66. NAVIGATION BENCHMARK

Test a directory containing at least:

```text
500 images
```

Measure rapid next/previous navigation.

Test:

- cached next
- uncached next
- repeated rapid arrow presses
- task cancellation
- directory changes

No long UI stalls are acceptable.

---

# 67. MEMORY BENCHMARK

Measure:

- process startup memory
- one JPEG open
- 100 navigation operations
- large PNG
- large AVIF
- giant image
- after cache eviction

Application memory should stabilize.

Avoid uncontrolled cache growth.

---

# 68. GPU MEMORY

GPU image caching must also obey budget limits.

Do not preload full-resolution textures for a large image directory.

---

# 69. PERFORMANCE REGRESSION RULE

Every major stage must run benchmark suite.

If median JPEG TTFI regresses by more than:

```text
5%
```

the change must be investigated before merge.

UI features are not allowed to silently degrade viewer-core performance.

---

# 70. PROJECT STRUCTURE

Recommended repository:

```text
/
├─ CMakeLists.txt
├─ README.md
├─ LICENSE
├─ docs/
│  ├─ architecture.md
│  ├─ benchmark.md
│  └─ codec.md
│
├─ src/
│  ├─ app/
│  │  ├─ Application.cpp
│  │  └─ Window.cpp
│  │
│  ├─ core/
│  │  ├─ ImageDocument.cpp
│  │  ├─ ViewState.cpp
│  │  ├─ EditHistory.cpp
│  │  └─ NavigationModel.cpp
│  │
│  ├─ render/
│  │  ├─ RenderDevice.cpp
│  │  ├─ SwapChain.cpp
│  │  ├─ ImageRenderer.cpp
│  │  └─ UiRenderer.cpp
│  │
│  ├─ codecs/
│  │  ├─ CodecRegistry.cpp
│  │  ├─ IImageCodec.hpp
│  │  ├─ JpegCodec.cpp
│  │  ├─ PngCodec.cpp
│  │  ├─ WebPCodec.cpp
│  │  └─ WicCodec.cpp
│  │
│  ├─ image/
│  │  ├─ DecodeQueue.cpp
│  │  ├─ DecodeTask.cpp
│  │  ├─ ImageCache.cpp
│  │  └─ PixelBuffer.cpp
│  │
│  ├─ edit/
│  │  ├─ Crop.cpp
│  │  ├─ Resize.cpp
│  │  ├─ Rotate.cpp
│  │  ├─ Flip.cpp
│  │  └─ SavePipeline.cpp
│  │
│  ├─ ui/
│  │  ├─ Toolbar.cpp
│  │  ├─ StatusBar.cpp
│  │  ├─ CropOverlay.cpp
│  │  └─ Icons.cpp
│  │
│  ├─ platform/
│  │  ├─ FileDialog.cpp
│  │  ├─ Clipboard.cpp
│  │  ├─ RecycleBin.cpp
│  │  ├─ ShellIntegration.cpp
│  │  └─ PrintService.cpp
│  │
│  ├─ metadata/
│  │  └─ MetadataReader.cpp
│  │
│  └─ util/
│     ├─ Timing.cpp
│     ├─ ThreadPool.cpp
│     └─ Logging.cpp
│
├─ plugins/
│  ├─ avif/
│  ├─ heif/
│  └─ jxl/
│
├─ benchmark/
│  ├─ runner/
│  ├─ scripts/
│  └─ results/
│
├─ tests/
│  ├─ codecs/
│  ├─ image/
│  ├─ edit/
│  └─ performance/
│
└─ assets/
   └─ icons/
```

Exact structure can evolve, but module boundaries should remain clear.

---

# 71. CODE QUALITY RULES

Required:

- RAII
- smart pointers
- no unexplained global mutable state
- clear ownership
- clear lifetime management
- cancellation-safe async work
- exception boundaries
- deterministic resource cleanup

Avoid unnecessary template complexity.

Avoid architectural overengineering.

---

# 72. MEMORY RULES

Do not repeatedly allocate large image buffers if reusable buffers are available.

Minimize:

- copies
- temporary image buffers
- format conversions
- full-resolution duplication

Prefer move semantics and clear ownership.

---

# 73. ERROR HANDLING

Application must not crash because:

- image is corrupted
- image is truncated
- metadata is invalid
- file disappears
- directory is changed
- codec fails
- unsupported image is opened
- file permission is denied

Display a compact error state.

---

# 74. UNSUPPORTED FILE

Example:

```text
Unable to open image.

Unsupported or corrupted image format.
```

Do not expose raw C++ exceptions to users.

---

# 75. CORRUPTED IMAGE SECURITY

Treat image input as untrusted.

Codec interfaces should validate:

- dimensions
- integer arithmetic
- buffer sizes
- frame count
- allocation sizes

Place hard limits where required to avoid malicious allocation requests.

---

# 76. MAXIMUM DIMENSION SAFETY

Do not allocate memory purely based on unchecked values from file headers.

Validate:

```text
width × height × bytesPerPixel
```

for overflow before allocation.

---

# 77. INTERNET ACCESS

v1.0 normal operation should require zero internet access.

No telemetry should be sent automatically.

Local performance telemetry is allowed.

---

# 78. LOGGING

Release build:

minimal logging.

Debug build:

detailed logging.

Performance build:

timing-focused logs.

Do not log sensitive user file contents.

---

# 79. CRASH DIAGNOSTICS

Future optional feature.

Do not add large crash-reporting SDKs in v1.0 if they affect startup size or performance.

---

# 80. FILE ASSOCIATION

Installer should optionally associate supported image extensions.

User must be able to decline.

Do not forcibly replace Windows defaults.

---

# 81. PORTABLE BUILD

Preferred:

Provide a portable `.exe` / folder build in addition to installer.

Application should work without requiring a background service.

---

# 82. INSTALLER

Use a lightweight installer.

Installer is separate from runtime application architecture.

No always-running updater service.

---

# 83. UPDATE SYSTEM

Out of scope for initial development.

Do not add update-check networking until core application is stable.

---

# 84. UNIT TESTS

Required areas:

- codec probing
- metadata parsing
- crop math
- resize dimensions
- rotations
- flips
- view transforms
- EXIF orientation
- cache eviction
- decode cancellation
- save path logic

---

# 85. IMAGE GOLDEN TESTS

Maintain known input/output samples.

Example:

```text
test.jpg
rotate-right.expected.png
crop.expected.png
resize.expected.png
```

Use image comparison with reasonable tolerance where lossy encoding is involved.

---

# 86. PERFORMANCE TESTS

Do not run all heavy benchmarks as normal unit tests.

Create dedicated benchmark configuration.

Example:

```text
ViewerBench.exe
```

---

# 87. DEVELOPMENT STAGES

Development must follow the stages below.

Do not jump directly to polished UI.

---

# STAGE 0 — BENCHMARK HARNESS

Build benchmark infrastructure first.

Deliver:

- benchmark runner
- timing markers
- repeatable test format
- results export
- reference IrfanView tests
- reference XnView tests

Definition of Done:

Performance can be measured objectively before viewer development continues.

---

# STAGE 1 — MINIMAL JPEG VIEWER

Implement only:

- native window
- renderer
- JPEG file opening
- fit to window
- first image display

No polished toolbar.

No crop.

No resize.

No HEIC.

No AVIF.

Goal:

prove fast image presentation architecture.

Definition of Done:

- reliable JPEG display
- measurable TTFI
- benchmark against IrfanView
- no directory scan before first image
- no UI thread blocking during large decode where avoidable

If Stage 1 performance is poor:

STOP.

Optimize architecture before continuing.

---

# STAGE 2 — NAVIGATION AND CACHE

Add:

- folder enumeration
- next
- previous
- decode queue
- preloading
- task priority
- cancellation
- adaptive cache

Definition of Done:

Navigation benchmark passes without major stalls.

---

# STAGE 3 — CORE FORMATS

Add:

- PNG
- WebP
- BMP
- TIFF
- GIF
- ICO

Benchmark each major codec.

Definition of Done:

Formats open correctly without significantly degrading startup of JPEG.

---

# STAGE 4 — VIEW INTERACTION

Add:

- Zoom
- Fit
- 100%
- cursor-centered zoom
- Pan
- double click behavior
- fullscreen

Definition of Done:

Smooth GPU transform interaction.

No decode during ordinary zoom/pan.

---

# STAGE 5 — BASIC EDITING

Add:

- Rotate
- Flip
- Crop
- Resize
- Undo
- Redo

Definition of Done:

Editing remains non-destructive until Save.

---

# STAGE 6 — SAVE PIPELINE

Add:

- Save
- Save As
- JPEG output
- PNG output
- WebP output
- TIFF output
- BMP output

Definition of Done:

Correct file output, error recovery, no silent corruption.

---

# STAGE 7 — MODERN UI

Only now add polished UI.

Add:

- titlebar treatment
- toolbar
- SVG icons
- tooltips
- theme
- status bar
- modern dialogs
- crop UI
- resize popup/dialog

Definition of Done:

Modern appearance without meaningful performance regression.

Run benchmark again.

---

# STAGE 8 — MODERN CODECS

Add as lazy modules:

- AVIF
- HEIC / HEIF
- JPEG XL if approved

Definition of Done:

Opening JPEG must not initialize these codecs.

---

# STAGE 9 — SYSTEM FEATURES

Add:

- Print
- Clipboard enhancements
- Drag & Drop
- Delete to Recycle Bin
- context menu
- Open File Location
- file association
- portable build
- installer

---

# STAGE 10 — POLISH AND RELEASE HARDENING

Add:

- error states
- corruption testing
- DPI testing
- multi-monitor testing
- localization readiness
- accessibility basics
- memory profiling
- regression benchmark
- installer validation

---

# 88. DO NOT DO DURING EARLY STAGES

AI Coding Agent must NOT:

- design a complicated UI first
- add a database
- build thumbnail browsing
- add tagging
- add ratings
- add albums
- add cloud support
- add AI features
- add RAW support
- add HDR editing
- build a file manager
- build a batch processor
- use Electron
- replace native renderer with browser technology
- initialize every codec at startup
- create a background service
- scan entire folder before first image
- preload unlimited images
- block UI thread with image decoding

---

# 89. DEFERRED FEATURES

Possible v1.1+:

- Instant Compare
- slideshow
- custom shortcuts
- extended metadata panel
- more resize algorithms
- wallpaper commands
- advanced color management
- JPEG XL full support
- animation optimization

---

# 90. FUTURE FEATURES

Potential future:

- RAW codec pack
- HDR
- 16-bit image pipeline
- ARM64
- comparison mode
- plugin SDK
- batch conversion

These must not influence v1.0 scope unless explicitly approved.

---

# 91. PERFORMANCE OVER FEATURES RULE

If there is a conflict between:

- adding a feature

and

- maintaining image-open speed

performance wins unless the feature is essential.

---

# 92. UI OVER PERFORMANCE RULE

A UI effect must be removed or changed if profiling proves that it meaningfully hurts:

- startup
- navigation
- memory
- interaction latency

Modern appearance must not come at the expense of the core objective.

---

# 93. ANIMATION POLICY

Default animation should be minimal.

Recommended:

```text
80–120 ms
```

Avoid image crossfade by default.

The user should see the next image immediately.

---

# 94. DEFINITION OF “LIGHTWEIGHT”

Lightweight means:

- small runtime footprint
- no background service
- no browser runtime
- low idle CPU
- reasonable RAM usage
- minimal startup dependency initialization
- no database requirement
- minimal disk writes
- no automatic network dependency

---

# 95. DEFINITION OF “MODERN”

Modern does NOT mean heavy.

Modern means:

- clean typography
- good spacing
- SVG icons
- dark/light support
- fluent-like window treatment
- smooth input
- clear hover state
- concise controls
- high-DPI correctness

---

# 96. DPI SUPPORT

Must support:

- 100%
- 125%
- 150%
- 175%
- 200%
- mixed-DPI multi-monitor configurations

Icons and text must remain sharp.

---

# 97. MULTI-MONITOR

Moving the window between monitors with different DPI must not break:

- toolbar dimensions
- image fit
- mouse coordinates
- crop box
- rendering scale

---

# 98. ACCESSIBILITY BASELINE

At minimum:

- keyboard-accessible toolbar functions
- meaningful tooltips
- clear focus state
- scalable text
- high contrast compatibility where practical

Do not compromise startup significantly for advanced accessibility systems in v1.0.

---

# 99. DEVELOPMENT WORKFLOW FOR AI CODING AGENT

For each stage:

1. Read this specification.
2. Inspect existing project structure.
3. Preserve architecture.
4. Implement only current stage scope.
5. Build project.
6. Run unit tests.
7. Run relevant benchmarks.
8. Report benchmark delta.
9. Fix regressions before progressing.
10. Update architecture documentation if design changed.

---

# 100. AI CODING REPORT FORMAT

After each development stage, report:

```text
Stage:
Completed:

Files Added:
Files Modified:

Features Implemented:

Architecture Changes:

Tests:
PASS / FAIL

Benchmarks:
Before:
After:

Performance Regressions:
None / Details

Known Issues:

Next Recommended Stage:
```

---

# 101. CHANGE CONTROL

Do not alter major architectural decisions silently.

Major changes requiring explicit justification include:

- changing GUI framework
- changing graphics API
- changing codec stack
- adding runtime dependency
- adding database
- adding background service
- changing threading model
- changing cache strategy
- changing minimum Windows version

AI must explain:

1. Why current approach fails.
2. Profiling evidence.
3. Proposed replacement.
4. Performance impact.
5. Binary-size impact.
6. Maintenance impact.

---

# 102. PROFILING RULE

Do not optimize based only on assumption.

Use profiling for:

- slow startup
- slow decode
- navigation stalls
- excessive memory
- UI stutter
- unexpected disk access
- excessive CPU

Optimization without measurement should be avoided.

---

# 103. PERFORMANCE REGRESSION BUDGET

Any feature causing more than approximately:

```text
5% median TTFI regression
```

must be reviewed.

For changes affecting only optional codecs, JPEG performance should remain effectively unchanged.

---

# 104. ACCEPTANCE CRITERIA — v1.0

v1.0 is acceptable when:

## Viewing

- common formats open reliably
- image shows quickly
- next/previous responsive
- zoom smooth
- pan smooth
- Fit works
- 100% works
- fullscreen works

## Editing

- crop works
- resize works
- rotate works
- flip works
- undo/redo works
- save works
- save as works

## System

- print works
- drag/drop works
- clipboard works
- delete goes to Recycle Bin
- file association works
- portable build works

## UI

- modern
- light/dark/system theme
- high-DPI correct
- icons modern
- not cluttered

## Performance

- no background service
- no browser runtime
- low idle CPU
- controlled memory
- strong JPEG TTFI
- fast image navigation
- no obvious UI stalls

---

# 105. PRIMARY SUCCESS METRIC

The user should be able to:

1. Double-click an image.
2. See it almost immediately.
3. Rapidly browse hundreds of images.
4. Zoom and inspect details smoothly.
5. Crop/resize/rotate quickly.
6. Save and continue.

Without feeling that the software is:

- heavy
- slow
- cluttered
- complicated

---

# 106. PRODUCT IDENTITY

The application should feel like:

> “A modern image viewer built around instant response.”

Not:

> “A photo management program with a viewer added to it.”

---

# 107. FINAL DEVELOPMENT PRIORITY

When uncertain, use this decision order:

```text
Correctness
    ↓
Image-open performance
    ↓
Navigation performance
    ↓
Stability
    ↓
Memory efficiency
    ↓
Interaction quality
    ↓
Essential features
    ↓
Visual polish
    ↓
Optional features
```

---

# 108. FINAL COMMAND TO AI CODING AGENT

Do not attempt to build the entire application in one pass.

Begin with:

## STAGE 0 — Benchmark Harness

Then proceed to:

## STAGE 1 — Minimal JPEG Viewer

Do not continue beyond Stage 1 until JPEG startup performance has been measured against the reference applications.

The architecture must earn the right to progress by meeting its performance goals.

---

END OF MASTER CODING SPECIFICATION

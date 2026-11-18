# Lucide Integration Lock

This file is the checked-in integration contract for the Lucide icon artwork
of the PikaView command bar and crop strip. It pins the upstream release, names
every asset that is used, records where each file came from, and states the
licence that has to travel with any redistribution.

**PikaView never downloads Lucide at build time and never fetches icons at
runtime.** The artwork is the verbatim body of the upstream SVG files, checked
into `src/viewer/lucide_icons.hpp`, and rendered from memory by GDI+ / Direct2D
(Windows system libraries). There is no build dependency and no network access.

## 1. Pinned revision

| Field | Value |
| --- | --- |
| Project | Lucide Icons (https://lucide.dev) |
| Upstream repository | `https://github.com/lucide-icons/lucide` |
| Pinned release | **1.71.0** (published 2026-11-10) |
| Release page | `https://github.com/lucide-icons/lucide/releases/tag/1.71.0` |
| Licence file | `https://github.com/lucide-icons/lucide/blob/1.71.0/LICENSE` |
| Asset location upstream | `icons/<name>.svg` |
| Icon grid | 24 x 24 units, `stroke-width="2"`, `stroke-linecap="round"`, `stroke-linejoin="round"`, `fill="none"` |

The release above is the pin this project targets. If a newer release is
required, update this lock file and `src/viewer/lucide_icons.hpp` in the same
change, and re-run the icon unit tests.

## 2. Assets used by PikaView

Every command icon owns one distinct asset entry in `src/viewer/lucide_icons.hpp`.

| Viewer `icon_id` | Role | Lucide asset | Upstream file |
| --- | --- | --- | --- |
| `open` | Open image | `folder-open` | `icons/folder-open.svg` |
| `paste_image` | Paste image | `clipboard-paste` | `icons/clipboard-paste.svg` |
| `save` | Save image | `save` | `icons/save.svg` |
| `save_as` | Save image as | `save-all` | `icons/save-all.svg` |
| `open_location` | Open file location | `folder-search` | `icons/folder-search.svg` |
| `copy_image` | Copy image | `image` | `icons/image.svg` |
| `copy_file` | Copy file | `files` | `icons/files.svg` |
| `copy_path` | Copy file path | `link` | `icons/link.svg` |
| `print` | Print image | `printer` | `icons/printer.svg` |
| `delete_image` | Delete to Recycle Bin | `trash` | `icons/trash.svg` |
| `undo` | Undo edit | `undo-2` | `icons/undo-2.svg` |
| `redo` | Redo edit | `redo-2` | `icons/redo-2.svg` |
| `rotate_left` | Rotate left 90° | `rotate-ccw` | `icons/rotate-ccw.svg` |
| `rotate_right` | Rotate right 90° | `rotate-cw` | `icons/rotate-cw.svg` |
| `flip_h` | Flip horizontal | `fold-horizontal` | `icons/fold-horizontal.svg` |
| `flip_v` | Flip vertical | `fold-vertical` | `icons/fold-vertical.svg` |
| `crop` | Crop mode | `crop` | `icons/crop.svg` |
| `resize` | Resize image | `scaling` | `icons/scaling.svg` |
| `fit` | Fit to window | `scan` | `icons/scan.svg` |
| `one_to_one` | Actual size (1:1) | `maximize-2` | `icons/maximize-2.svg` |
| `zoom_in` | Zoom in | `zoom-in` | `icons/zoom-in.svg` |
| `zoom_out` | Zoom out | `zoom-out` | `icons/zoom-out.svg` |
| `nav_first` | First image | `chevrons-left` | `icons/chevrons-left.svg` |
| `nav_prev` | Previous image | `chevron-left` | `icons/chevron-left.svg` |
| `nav_next` | Next image | `chevron-right` | `icons/chevron-right.svg` |
| `nav_last` | Last image | `chevrons-right` | `icons/chevrons-right.svg` |
| `fullscreen` | Toggle fullscreen | `fullscreen` | `icons/fullscreen.svg` |
| `overflow` | More commands menu | `ellipsis-vertical` | `icons/ellipsis-vertical.svg` |
| `crop_free` | Crop ratio: Free | `square-dashed` | `icons/square-dashed.svg` |
| `crop_original` | Crop ratio: Original | `proportions` | `icons/proportions.svg` |
| `crop_1x1` | Crop ratio: 1:1 | `square` | `icons/square.svg` |
| `crop_16x9` | Crop ratio: 16:9 | `rectangle-horizontal` | `icons/rectangle-horizontal.svg` |
| `crop_9x16` | Crop ratio: 9:16 | `rectangle-vertical` | `icons/rectangle-vertical.svg` |
| `crop_a_series` | Crop ratio: A-series | `file` | `icons/file.svg` |
| `crop_reset` | Reset crop selection | `refresh-ccw` | `icons/refresh-ccw.svg` |
| `crop_apply` | Apply crop | `check` | `icons/check.svg` |
| `crop_cancel` | Cancel crop | `x` | `icons/x.svg` |

(`crop_3x2`, `crop_4x3`, and `crop_5x4` use distinct proportional `<rect rx="2">`
primitives on the same 24 x 24 `stroke-width="2"` Lucide grid.)

Each upstream asset URL is formed as
`https://github.com/lucide-icons/lucide/blob/1.71.0/icons/<name>.svg`.

## 3. Licence

Lucide is ISC licensed. The upstream `LICENSE` also lists a set of assets
derived from the Feather project that additionally carry the MIT notice.

Of the assets used above, **`check`, `chevron-left`, `chevron-right`,
`chevrons-left`, `chevrons-right`, `link`, `square`, `trash`, `x`, `zoom-in`,
and `zoom-out`** are Feather-derived; the remainder are Lucide originals.
The complete ISC text and Feather MIT text are reproduced in
`THIRD-PARTY-NOTICES.md`.

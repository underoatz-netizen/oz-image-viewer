# OZ Image Viewer 0.1.0 Release Notes

OZ Image Viewer 0.1.0 is the initial public release of OZ Image Viewer, a fast, lightweight native image viewer built with modern C++23, Win32, Direct3D 11, DXGI, and Direct2D for 64-bit Windows (x64).

## Features

- **Fast First-Image Display:** Presents the requested image immediately fit-to-window while asynchronously enumerating sibling folder images and preloading neighbours in the background.
- **Multi-Format Image Decoding:** Signature-based detection and decoding for JPEG, PNG, static WebP, BMP, static GIF, TIFF (first page), ICO, CUR, TGA, and AVIF.
- **Dynamic HEIC/HEIF Decoding:** Dynamically loads standalone codec DLLs (`heif.dll` and `libde265.dll`) under LGPLv3 for still HEIC/HEIF viewing without static linkage.
- **Smooth Navigation & Zoom:** Arrow-key and spacebar navigation across images in the same folder; cursor-centred mouse wheel zoom, 100% actual size toggle (`1` or double-click), fit to window (`F`), drag-to-pan, and borderless fullscreen mode (`F11`).
- **Non-Destructive Adjustments:** Rotate 90° (`Ctrl+L`, `Ctrl+R`), flip (`Ctrl+Shift+H`, `Ctrl+Shift+V`), interactive crop (`C`), and pixel-dimension resize (`Ctrl+Alt+R`), with undo (`Ctrl+Z`) and redo (`Ctrl+Y`). Edits change in-memory display state only; original files remain unaltered until saved.
- **File Export:** Save and Save As to PNG, JPEG, WebP (lossless single-frame), BMP, and TIFF (Save As only).

## Distribution Packages

- `ozviewer-0.1.0-win-x64-installer.exe`: Guided Windows setup installer installing per-user to `%LOCALAPPDATA%\Programs\OZ Image Viewer`. Requires no administrator privileges. File associations are strictly opt-in during setup and do not hijack system defaults. Cleanly uninstalls via Windows Settings > Installed Apps.
- `ozviewer-0.1.0-win-x64-portable.zip`: Portable ZIP archive for extraction to any folder. Requires no installation or registry modifications. Companion DLLs must remain alongside `ozviewer.exe`.
- `ozviewer-0.1.0-lgpl-corresponding-source.zip`: Complete corresponding source code, build scripts, and build instructions for dynamically loaded LGPL components (libheif 1.23.2 and libde265 1.1.1) in compliance with LGPLv3 §4 and GPLv3 §6(d).
- `SHA256SUMS`: Official cryptographic checksum file for distribution assets.

## Cryptographic Checksums (SHA-256)

```
25637adf86ab1eeac9a0e5992918bc492c957c7568116f452de8c6a5e2d3b633 *ozviewer-0.1.0-win-x64-installer.exe
7c014784fc6a8aa0a9c466d22ae33b838c4f76521c91af2a68e01e445e312792 *ozviewer-0.1.0-win-x64-portable.zip
6d6aadebe2ecf2b4f96183de3860b5f24aabac2429673ab300c86ed1ff4cba3f *ozviewer-0.1.0-lgpl-corresponding-source.zip
```

Verify your downloaded files using PowerShell (`Get-FileHash -Algorithm SHA256 filename`) or Command Prompt (`certutil -hashfile filename SHA256`) before running them.

## Security & Platform Verification

- **Unsigned Binaries:** Release binaries in version 0.1.0 are currently not digitally code-signed. Windows SmartScreen or browser warnings ("Unknown Publisher") may appear when launching the installer or executable. Verify the official repository source and compare the SHA-256 hash against the values above.
- **Platform Scope & Testing:** Built for 64-bit Windows (x64). Disposable standard-user automated installation, launch, FirstPresent frame benchmark, association handling, and clean uninstallation have been verified in clean-room CI on Windows Server 2022 (x64). 32-bit x86 and native ARM64 are not supported.
- **Licensing:** First-party code is licensed under the MIT License (Copyright (c) 2026 OZ Lab). Third-party components retain their upstream licenses as documented in `THIRD-PARTY-NOTICES.md` and `THIRD-PARTY-LICENSES.txt`.

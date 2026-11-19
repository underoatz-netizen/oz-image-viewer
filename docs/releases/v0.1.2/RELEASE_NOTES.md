# PikaView 0.1.2 (formerly OZ Image Viewer) Release Notes

PikaView 0.1.2 is a 64-bit Windows (x64) release of PikaView (formerly OZ Image Viewer) featuring clean-from-source, bit-for-bit reproducible compiled PE binaries (`ozviewer.exe` 2-way; `heif.dll` and `libde265.dll` 3-way), while the distribution containers (`pikaview-0.1.2-win-x64-installer.exe` and `pikaview-0.1.2-win-x64-portable.zip`) are packaged once from the verified Build A payload and pinned in `SHA256SUMS`. Built with modern C++23, Win32, Direct3D 11, DXGI, and Direct2D, existing viewing, folder navigation, non-destructive editing (rotate, flip, crop, resize, undo/redo), interactive single-image printing (`PrintDlgExW`), save/export, and dynamic HEIC/HEIF codec support remain available, alongside a new PikaView application icon, a single-row 40-DIP Lucide 1.71.0 command bar, save cache/preload invalidation fixes, and localized storage error guidance.

The executable file name remains `ozviewer.exe`. Internal identity used for upgrades and association registration is retained: the stable AppId, the `OZImageViewer.Image` ProgID, the `HKCU\Software\OZImageViewer` registry layout, and the per-user install path `%LOCALAPPDATA%\Programs\OZ Image Viewer` are preserved across upgrades from both `0.1.0` and `0.1.1`. Historical `v0.1.0` and `v0.1.1` assets, git tags, and provenance records remain unchanged.

## Source & Build Identity

- **Source commit:** `4541ffedc4fde9d4095d92f3f7a0c5397bbb501b` (clean working tree, tag `v0.1.2`)
- **Git tree:** `0c464561de3d418b8d2a8b3689ca163a1cbad7bd`
- **Build configuration:** `Release` x64 with `OZVIEWER_PRINT_DIAGNOSTICS=OFF`
- **Deterministic flags:** MSVC `/experimental:deterministic /Brepro /PDBALTPATH:%_PDB%`, triplet `oz-x64-windows-static-md` (`VCPKG_PLATFORM_TOOLSET=v143`, `/experimental:deterministic`, `libheif`/`libde265` `/Brepro /PDBALTPATH:heif.pdb` & `libde265.pdb`), and `file-local` named namespace `oz_viewer_main_internal` in `src/viewer/main.cpp`
- **Toolchain & triplet:** `MSVC 14.44.35228.0`, `CMake 3.31.6-msvc6`, `vcpkg` baseline `52d80838fb40c755b1615fbc9c7b994a33742a22`, triplet `oz-x64-windows-static-md`

## What's Changed in 0.1.2

- **New PikaView application icon:** Applied consistently across `ozviewer.exe` executable resources, the main window, dialogs, empty/welcome view, and the Windows Setup installer.
- **Single-row 40-DIP Lucide 1.71.0 command bar & clean dark-charcoal UI:** Replaces the legacy menu bar with a 40-DIP single-row command bar featuring `32` DIP buttons, subtle separators, a deterministic right-side **More** overflow menu, crisp crop strip icons, `F10`/`Alt` keyboard menu activation, preserved keyboard shortcuts, and automatic canvas focus restore, styled cleanly in dark charcoal (`THIRD-PARTY-NOTICES.md` includes Lucide ISC and Feather MIT notices).
- **Save cache/preload invalidation & localized storage guidance:** Fixes stale cache/preload state on Save so navigating `Save -> Next -> Previous -> Save` reloads and re-saves cleanly without false "source file modified" errors; coordinates save completion with active document checkpoints and pending navigation rescheduling; and adds localized EN/TH guidance for `win32 19` write-protected storage and `win32 5` permission denials.
- **In-place upgrade continuity across 0.1.0, 0.1.1, and 0.1.2:** Installing PikaView 0.1.2 over an owned OZ Image Viewer 0.1.0 or PikaView 0.1.1 installation upgrades in place at `%LOCALAPPDATA%\Programs\OZ Image Viewer` while keeping `ozviewer.exe`, the stable AppId, `OZImageViewer.Image` ProgID, and `HKCU\Software\OZImageViewer`, leaving all existing Windows default file associations (`UserChoice`) untouched.
- **Deterministic MSVC `/Brepro` / `/experimental:deterministic` builds:** Uses the pinned `v143` `oz-x64-windows-static-md` triplet and clean from-source dependency rebuilds (`VCPKG_BINARY_SOURCES=clear`) to produce bit-for-bit reproducible PE binaries.

## Distribution Packages

- `pikaview-0.1.2-win-x64-installer.exe` (4,276,637 bytes, ~4.1 MB): Guided per-user Windows Setup installer targeting `%LOCALAPPDATA%\Programs\OZ Image Viewer` without requiring Administrator privileges. File associations and desktop shortcut creation are strictly opt-in (initially unchecked) and never hijack system defaults. Cleanly uninstalls via Windows Settings > Installed Apps.
- `pikaview-0.1.2-win-x64-portable.zip` (2,744,297 bytes, ~2.6 MB): Portable ZIP archive for extraction to any folder. Contains `ozviewer.exe` alongside its app-local runtime DLLs (`heif.dll`, `libde265.dll`, and VC143 CRT DLLs) plus `BUILD-PROVENANCE.json` and license notices.
- `pikaview-0.1.2-lgpl-corresponding-source.zip` (2,421,056 bytes, ~2.3 MB): Complete corresponding source archives, effective vcpkg recipes and patches, `triplets/oz-x64-windows-static-md.cmake`, full LGPL-3.0 and GPL-3.0 license texts, `README-BUILD-LGPL.txt`, and `build-lgpl-dlls.ps1` for dynamically loaded LGPL components (`libheif` 1.23.2 and `libde265` 1.1.1) under LGPLv3 §4 and GPLv3 §6(d).
- `SHA256SUMS` (316 bytes): Official cryptographic checksum file for the distribution assets.

## Cryptographic Checksums (SHA-256)

```
a36d9c8fff84a7374d8a5836b067398c0ab74a8ed578943d56b0b09eb9354bac *pikaview-0.1.2-win-x64-installer.exe
aa572e5eeab05c89fdcd5d92c734f75a21358254b9e4c4b32020df05ca8bd6d4 *pikaview-0.1.2-win-x64-portable.zip
f28505ee05f9dc9468e2da6ed8d4fc56d21c227ef6ef354a053ebbd551d32753 *pikaview-0.1.2-lgpl-corresponding-source.zip
```

`SHA256SUMS` itself has SHA-256 `bc3eff3d48b9fc6cda8dc01d55cdfec49ad05f6aee5d1276b0c5872c3d73fd27` (316 bytes).

Verify downloaded files using PowerShell (`Get-FileHash -Algorithm SHA256 filename`) or Command Prompt (`certutil -hashfile filename SHA256`) before running them.

## Observed Authenticode Signatures & SmartScreen Guidance

- **Unsigned project binaries (`NotSigned`):**
  - `pikaview-0.1.2-win-x64-installer.exe`: 4,276,637 bytes, SHA-256 `a36d9c8fff84a7374d8a5836b067398c0ab74a8ed578943d56b0b09eb9354bac`
  - `ozviewer.exe`: 3,853,824 bytes, SHA-256 `04dfd470ae8ab01f4e911d15dd4f38a85b4e18ebc0fd0442dfcfda10ff036c92`
  - `heif.dll`: 1,423,872 bytes, SHA-256 `9bf0954c60cfb0e16ba55c329f169657364e88ca0643d5cdc978bc4253a8d019`
  - `libde265.dll`: 366,592 bytes, SHA-256 `15536336f0ef6ca8b5249a853685e90b1414f5996080525b52e7c9d268faadcb`
- **Signed Microsoft CRT DLLs (`Valid`):**
  - `msvcp140.dll`: 557,728 bytes, SHA-256 `0f885b509a685d2bbfa652fed26b5fb31d88fbdab0a978c641d1c7b8aa460aa9`
  - `msvcp140_atomic_wait.dll`: 50,304 bytes, SHA-256 `640b2aefced484d0368eea5bdd06addd0658a3a70a49256e560d6923b404a479`
  - `vcruntime140.dll`: 124,544 bytes, SHA-256 `d5e4d9a3e835fa679450145d6a7d94e36573a509317111904d9b3712c30d9066`
  - `vcruntime140_1.dll`: 49,792 bytes, SHA-256 `1f2d41c4aa5db0bc33ebf7b66d72943a817d7ce6cbe880502a9403823633093f`
- **SmartScreen & browser guidance:** Because the installer and application binaries are unsigned (`NotSigned`), Windows SmartScreen or web browsers may display an "Unknown Publisher" warning before opening them. Always confirm the file was downloaded from the official GitHub repository (`https://github.com/underoatz-netizen/oz-image-viewer`) and verify that its SHA-256 hash matches `SHA256SUMS` and the values above before running anything.

## Reproducible-Build Proof & Clean From-Source Verification

- **Dual full-app & three-way LGPL clean-from-source verification:** Two independent clean full-application builds (`repro-a` and `repro-b`, with distinct directories and timestamps) were configured and built cleanly from source with `VCPKG_BINARY_SOURCES=clear` and zero cached binary restores (verifying 2-way `ozviewer.exe` reproducibility), alongside a 3rd standalone LGPL-only rebuild executed via `build-lgpl-dlls.ps1` extracted from `pikaview-0.1.2-lgpl-corresponding-source.zip` (also with `VCPKG_BINARY_SOURCES=clear`, verifying 3-way `heif.dll` and `libde265.dll` reproducibility).
- **Bit-for-bit PE match (`docs/releases/v0.1.2/REPRODUCIBLE-BUILD-PROOF.json`):**
  - `ozviewer.exe` (3,853,824 bytes): `04dfd470ae8ab01f4e911d15dd4f38a85b4e18ebc0fd0442dfcfda10ff036c92` (`repro-a` == `repro-b`, **100% bit-for-bit match**)
  - `heif.dll` (1,423,872 bytes): `9bf0954c60cfb0e16ba55c329f169657364e88ca0643d5cdc978bc4253a8d019` (`repro-a` == `repro-b` == Standalone LGPL rebuild, **100% bit-for-bit match**)
  - `libde265.dll` (366,592 bytes): `15536336f0ef6ca8b5249a853685e90b1414f5996080525b52e7c9d268faadcb` (`repro-a` == `repro-b` == Standalone LGPL rebuild, **100% bit-for-bit match**)
  - All compiled PE binaries contain `repro` debug directories and zero host PDB paths (`heif.pdb` / `libde265.pdb`).
- **Companion provenance relationship:** Static `claimLimits` line 39 in `docs/releases/v0.1.2/BUILD-PROVENANCE.json` describes the scope of `BUILD-PROVENANCE.json` alone; those two companion requirements (a reproducible-build proof and a real clean isolated dependency rebuild in a fresh workspace) are fulfilled for v0.1.2 in `docs/releases/v0.1.2/REPRODUCIBLE-BUILD-PROOF.json`. Distribution containers (`pikaview-0.1.2-win-x64-portable.zip` entry timestamps and the Inno Setup `pikaview-0.1.2-win-x64-installer.exe` wrapper) are packaged once from the verified Build A payload and pinned by SHA-256 in `SHA256SUMS` (while `BUILD-PROVENANCE.json` records the underlying staged binaries, Microsoft-signed VC143 CRT DLLs, and `pikaview-0.1.2-lgpl-corresponding-source.zip`).

## Observed Verification & Evidence Limits

- **Clean Build A targeted CTest & packaging/provenance suites (`OZVIEWER_PRINT_DIAGNOSTICS=OFF`):**
  - Clean Build A targeted CTest passed `11/11` checks (`ozviewer_core_tests` 2987/0, `ozviewer_stage7_seam_tests` 5366/0, `ozviewer_localization_tests` 1249/0, `ozviewer_recycle_delete_tests` 27/0, `ozviewer_icon_asset_tests`, `release_build_wrapper_tests`, and 5 isolation/catalog scans; note the full 52-test suite was not re-run post-`4541ffe`).
  - Packaging, provenance, and migration suites all passed with exit code `0`: `validate-portable-payload.ps1`, `smoke-portable.ps1` (`FirstPresent` PASS), `test-installer-migration.ps1` (mock `0.1.0`/`0.1.1 -> 0.1.2` + `ISCC.exe` compile), `test-dependency-provenance.ps1`, and `test-envelope-provenance.ps1`.
- **Clean-room GitHub Actions CI (run `44501854397`, `windows-2022` x64):**
  - Passed `fetch-candidate` (47s) and `validate-candidate` (1m26s): disposable lifecycle harness passed 48 checks (47 `pass`, 1 expected `non_admin_integrity_check` warning on the elevated runner, and 0 `fail`), and `standard-user-lifecycle` passed 12/12 parent + 48/48 non-elevated standard-user child checks.
  - Covered fresh per-user install, `FirstPresent` benchmark smoke, opt-in file associations and desktop shortcut handling, `v0.1.0 -> v0.1.2` in-place upgrade at the original install directory, runtime closure hashes, `UserChoice` preservation across all 14 extensions, and clean uninstall.
- **Truthful evidence limits:**
  - The `0.1.1 -> 0.1.2` upgrade path is verified via mock migration tests and `ISCC` compile, while hosted CI clean-room upgrade execution ran `v0.1.0 -> v0.1.2`.
  - Physical printer and `failed-network-first` print limits from v0.1.1 remain unchanged (not re-run against physical printer hardware, `failed-network-first` untested, and no claim that all print-dialog hangs are fixed).
  - Project binaries (`pikaview-0.1.2-win-x64-installer.exe`, `ozviewer.exe`, `heif.dll`, and `libde265.dll`) are unsigned (`NotSigned`).

## Licensing & Corresponding Source

First-party code is licensed under the MIT License (Copyright (c) 2026 OZ Lab); see `LICENSE`. Third-party components retain their upstream licenses as documented in `THIRD-PARTY-NOTICES.md` (including Lucide Icons 1.71.0 under ISC and Feather-derived icons under MIT), `THIRD-PARTY-LICENSES.txt`, and `THIRD-PARTY-SOURCE.txt`.

The dynamically loaded LGPL components remain `libheif` 1.23.2 and `libde265` 1.1.1 (`LGPL-3.0-only`). Their complete corresponding source, patches, build scripts, and rebuild instructions are packaged in `pikaview-0.1.2-lgpl-corresponding-source.zip`, subject to LGPLv3 §4 and GPLv3 §6(d). Users may rebuild and replace `heif.dll` and `libde265.dll` alongside `ozviewer.exe`; the application loads them dynamically.

## Historical v0.1.0 & v0.1.1 Continuity

OZ Image Viewer 0.1.0 and PikaView 0.1.1, along with their published release assets and git tags, remain historical and are not re-versioned or replaced by v0.1.2. Release provenance and reproducibility records are preserved at:
- `docs/releases/v0.1.2/BUILD-PROVENANCE.json`
- `docs/releases/v0.1.2/REPRODUCIBLE-BUILD-PROOF.json`
- `docs/releases/v0.1.1/RELEASE_NOTES.md` (`docs/releases/v0.1.1/BUILD-PROVENANCE.json`)
- `docs/releases/v0.1.0/BUILD-PROVENANCE.json`

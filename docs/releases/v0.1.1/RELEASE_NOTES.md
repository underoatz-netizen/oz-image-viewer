# PikaView 0.1.1 (formerly OZ Image Viewer) Release Notes

PikaView 0.1.1 is the PikaView-named successor release to OZ Image Viewer 0.1.0 for 64-bit Windows (x64), built with modern C++23, Win32, Direct3D 11, DXGI, and Direct2D. Existing viewing, folder navigation, non-destructive editing (rotate, flip, crop, resize, undo/redo), save/export, and dynamic HEIC/HEIF codec support remain available, alongside interactive single-image printing.

The executable file name remains `ozviewer.exe`. Internal identity used for upgrades and association registration is retained: the stable AppId, the `OZImageViewer.Image` ProgID, the `HKCU\Software\OZImageViewer` registry layout, and the per-user install path `%LOCALAPPDATA%\Programs\OZ Image Viewer` are unchanged. Historical v0.1.0 assets, git tag, and provenance records remain unchanged.

## Source & Build Identity

- **Source commit:** `b2904e5aa8f9d46d40f7cd37af6f7f7f9d2e00a8` (clean working tree)
- **Git tree:** `76376d3de2dce4b351243fd6f483994e22bef2df`
- **Build configuration:** `Release` x64 with `OZVIEWER_PRINT_DIAGNOSTICS=OFF`
- **Toolchain & triplet:** MSVC 14.44.35228.0, CMake 3.31.6, vcpkg triplet `oz-x64-windows-static-md`

## What's Changed in 0.1.1

- **PikaView product naming:** User-facing packaging, installer titles, shortcuts, and release identity move to **PikaView** (formerly OZ Image Viewer). The executable file name (`ozviewer.exe`) and internal upgrade identity stay unchanged so existing shortcuts and per-user installations continue cleanly.
- **Interactive Print path (`PrintDlgExW`):** The interactive Print command (`File -> Print...` or `Ctrl+P`) uses a dedicated `PrintDlgExW` helper with a strictly scoped apartment-threaded COM lifecycle, explicit cancel/apply/failure outcomes, complete `DevMode`/`DevNames`/`HDC` cleanup, and a reentrancy guard around the print attempt. The single-flight print controller prepares one fit-to-page copy from the committed image without modifying the file.
- **Private print diagnostics (`OFF` in Release):** The CMake option `OZVIEWER_PRINT_DIAGNOSTICS` defaults to `OFF` and is built `OFF` in this Release build; it is not a user-facing feature.
- **Dependency provenance & packaging:** `BUILD-PROVENANCE.json` binds the staged `heif.dll` and `libde265.dll` bytes to the effective vcpkg `oz-x64-windows-static-md` build inputs, pinned recipes/patches, and `pikaview-0.1.1-lgpl-corresponding-source.zip` (failing closed on missing or mismatched evidence).
- **In-place upgrade over v0.1.0:** Installing PikaView 0.1.1 over an owned OZ Image Viewer 0.1.0 installation upgrades in place at the original install directory, updates owned display/registration labels to PikaView, and preserves all existing Windows default file associations (`UserChoice`).

## Distribution Packages

- `pikaview-0.1.1-win-x64-installer.exe` (4,260,442 bytes): Guided per-user Windows Setup installer targeting `%LOCALAPPDATA%\Programs\OZ Image Viewer` without requiring Administrator privileges. File associations and desktop shortcut creation are strictly opt-in (initially unchecked) and never hijack system defaults. Cleanly uninstalls via Windows Settings > Installed Apps.
- `pikaview-0.1.1-win-x64-portable.zip` (2,714,302 bytes): Portable ZIP archive for extraction to any folder. Contains `ozviewer.exe` alongside its app-local runtime DLLs (`heif.dll`, `libde265.dll`, and VC143 CRT DLLs) plus `BUILD-PROVENANCE.json` and license notices.
- `pikaview-0.1.1-lgpl-corresponding-source.zip` (2,420,966 bytes): Complete corresponding source archives, effective vcpkg recipes and patches, `triplets/oz-x64-windows-static-md.cmake`, full LGPL-3.0 and GPL-3.0 license texts, `README-BUILD-LGPL.txt`, and `build-lgpl-dlls.ps1` for dynamically loaded LGPL components (libheif 1.23.2 and libde265 1.1.1) under LGPLv3 §4 and GPLv3 §6(d).
- `SHA256SUMS` (316 bytes): Official cryptographic checksum file for the distribution assets.

## Cryptographic Checksums (SHA-256)

```
2abc046cc5e20187bca5343fa9f73f4adb527b9f5bfd98ac35f380b42391c076 *pikaview-0.1.1-win-x64-installer.exe
ade4a2602a50c0949e9f6ddab2c004dbbdee3ea5ffed21d9b9348c20a97bf36b *pikaview-0.1.1-win-x64-portable.zip
5b35334749d600173bf97a2e6897f993611770d89d7f3fee99c885f131de144f *pikaview-0.1.1-lgpl-corresponding-source.zip
```

`SHA256SUMS` itself has SHA-256 `c40ea7e91909695b848f8236c88e1334293702e9c893964333ba37720babcb99` (316 bytes).

Verify downloaded files using PowerShell (`Get-FileHash -Algorithm SHA256 filename`) or Command Prompt (`certutil -hashfile filename SHA256`) before running them.

## Observed Authenticode Signatures & SmartScreen Guidance

- **Unsigned project binaries (`NotSigned`):** `pikaview-0.1.1-win-x64-installer.exe` (`2abc046cc5e20187bca5343fa9f73f4adb527b9f5bfd98ac35f380b42391c076`, 4,260,442 bytes), `ozviewer.exe` (`a31978e505a13fcfe61c0019a2a122407dd37c4e66099ae2ec5bc4f4ad373cae`, 3,866,624 bytes), `heif.dll` (`33e19aa93b8433ba059eacb7479d6132cea12759d4c2554ca03c21b0fd6f2b7e`, 1,394,688 bytes), and `libde265.dll` (`0780062929bb6b130306ef74cf1c522c598626c2e1090696155fa9e7d43e4515`, 360,960 bytes) are **unsigned (`NotSigned`)**.
- **Signed Microsoft CRT DLLs (`Valid`):** Bundled Microsoft VC143 CRT DLLs (`MSVCP140.dll`, `MSVCP140_ATOMIC_WAIT.dll`, `VCRUNTIME140.dll`, `VCRUNTIME140_1.dll`) carry valid Microsoft Authenticode signatures (`Valid`).
- **SmartScreen & browser guidance:** Because the installer and application binaries are unsigned, Windows SmartScreen or web browsers may display an "Unknown Publisher" warning before opening them. Always confirm the file was downloaded from the official GitHub repository (`https://github.com/underoatz-netizen/oz-image-viewer`) and verify that its SHA-256 hash matches `SHA256SUMS` and the values above before running anything.

## Observed Verification & Evidence Limits

- **Local Release build & tests (`OZVIEWER_PRINT_DIAGNOSTICS=OFF`):**
  - Clean Release x64 build completed with 0 errors.
  - CTest passed 48/51 checks (all 10/10 targeted print/UI tests passed; the 3 non-targeted pre-package failures were `ozbench_cli_capture_control_window`, `ozviewer_save_ui_smoke`, and `package_portable_smoke` before provenance staging).
  - Packaged portable smoke (`smoke-portable.ps1`): `PASS` (`FirstPresent` benchmark).
  - Local `test-lifecycle-harness.ps1` and `test-installer-migration.ps1`: passed.
- **Clean-room GitHub Actions CI (run `44229309225`, Windows Server 2022 x64):**
  - Passed `fetch-candidate` and `validate-candidate` (disposable lifecycle harness: 48 checks with 47 `pass`, 1 expected `non_admin_integrity_check` warning on the elevated runner, and 0 `fail`; plus `standard-user-lifecycle` gate: 12/12 parent checks + 48/48 non-elevated standard-user child checks `pass`).
  - Covered fresh per-user install, `FirstPresent` benchmark smoke, opt-in file associations, desktop shortcut opt-in/reinstall/user-modified preservation, `v0.1.0 -> v0.1.1` in-place upgrade at the original install directory, runtime closure hashes, `InstallLocation`/shell-open command target, complete `UserChoice` preservation across all 14 declared extensions, and clean uninstall.
- **Truthful print evidence limits:**
  - Reported physical page print and second-Cancel (`second-Cancel`) behavior are historical observations from an earlier candidate and environment, not re-run physical printer evidence for this v0.1.1 build.
  - The `failed-network-first` scenario (where the first print attempt targets a failed or unavailable network printer) was not tested.
  - No claim is made that all print-dialog hangs are fixed.

## Licensing & Corresponding Source

First-party code is licensed under the MIT License (Copyright (c) 2026 OZ Lab); see `LICENSE`. Third-party components retain their upstream licenses as documented in `THIRD-PARTY-NOTICES.md`, `THIRD-PARTY-LICENSES.txt`, and `THIRD-PARTY-SOURCE.txt`.

The dynamically loaded LGPL components remain libheif 1.23.2 and libde265 1.1.1 (LGPL-3.0-only). Their complete corresponding source, patches, build scripts, and rebuild instructions are packaged in `pikaview-0.1.1-lgpl-corresponding-source.zip`, subject to LGPLv3 §4 and GPLv3 §6(d). Users may rebuild and replace `heif.dll` and `libde265.dll` alongside `ozviewer.exe`; the application loads them dynamically.

## Historical v0.1.0 Continuity

OZ Image Viewer 0.1.0 and its published release assets and git tag remain historical and are not re-versioned or replaced by v0.1.1. The original v0.1.0 provenance archive is preserved byte-for-byte at `docs/releases/v0.1.0/BUILD-PROVENANCE.json`.

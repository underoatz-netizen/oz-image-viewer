# PikaView 0.1.1 (formerly OZ Image Viewer)

> **Reconstructed candidate — not a verified release.** These notes were
> reconstructed from recovered survivor files because the prior v0.1.1 notes
> were lost. This candidate has not been built, signed, tested, installed, or
> published. Nothing below is a claim that a real v0.1.1 build, signature, test
> run, installer lifecycle, or physical print has been observed.

PikaView 0.1.1 is the PikaView-named successor candidate to OZ Image Viewer
0.1.0 for 64-bit Windows (x64). Viewing, navigation, non-destructive editing,
save/export, and optional HEIC/HEIF codec behavior are intended to remain
available, but this reconstruction did not re-verify them.

The executable file name remains `ozviewer.exe`. Internal identity used for
upgrades and association registration is also retained: the stable AppId, the
`OZImageViewer.Image` ProgID, the `HKCU\Software\OZImageViewer` registry layout,
and the per-user install path `%LOCALAPPDATA%\Programs\OZ Image Viewer` are
unchanged. Historical v0.1.0 assets and notes remain unchanged.

## Changes in this candidate

- **PikaView product naming.** User-facing packaging and release identity move
  to PikaView (formerly OZ Image Viewer). The executable name and internal
  upgrade identity stay as described above.
- **Print path.** The interactive Print command uses a dedicated
  `PrintDlgExW` helper with a strictly scoped apartment-threaded COM lifecycle,
  explicit cancel/apply/failure outcomes, complete DevMode/DevNames/HDC
  cleanup, and a reentrancy guard around the print attempt. The existing
  single-flight print controller prepares one fit-to-page page from the
  committed image without changing the document.
- **Private print diagnostics.** The CMake option
  `OZVIEWER_PRINT_DIAGNOSTICS` exists and defaults to `OFF`. Release builds are
  expected to keep it `OFF`; it is not a user-facing feature.
- **Release packaging and provenance.** Packaging targets the new asset names
  below and requires a final `SHA256SUMS`. The release-provenance scripts bind
  staged `heif.dll` and `libde265.dll` bytes to effective dependency build
  evidence and fail closed on missing or mismatched evidence. These scripts were
  recovered but were not executed for this reconstruction.
- **Upgrade intent.** The installer is intended to upgrade an owned v0.1.0
  installation in place, migrating owned display/registration labels to
  PikaView while preserving the install directory and never changing a Windows
  default file association automatically. This behavior is covered by recovered
  synthetic/mock contract tests only; no real install or upgrade was performed
  here.

## Packaging assets

- `pikaview-0.1.1-win-x64-installer.exe`
- `pikaview-0.1.1-win-x64-portable.zip`
- `pikaview-0.1.1-lgpl-corresponding-source.zip`
- `SHA256SUMS`

The portable ZIP still contains `ozviewer.exe` plus its app-local runtime DLLs.
The installer remains a per-user installation and offers file associations only
as an initially unchecked option.

No v0.1.1 SHA-256 values are published in these reconstructed notes because no
final assets exist. `SHA256SUMS` must be generated from the final built assets;
the v0.1.0 hashes in the historical notes apply only to the published v0.1.0
assets and must not be reused.

## Verification status and evidence limits

- This is a **reconstructed candidate** assembled from recovered survivor files
  on base commit `e299e34`. Verification during reconstruction was limited to
  JSON parsing, exact metadata checks, file-hash checks, and static text/source
  checks that do not require the concurrent viewer-print recovery work.
- No integrated CMake configure/build, CTest run, installer compile, signature
  verification, install/uninstall lifecycle run, physical print, failed-network
  printer test, staging, commit, tag, push, or publication was performed for
  this candidate.
- **Print evidence is historical only.** The prior old-candidate's reported
  page success and its reported second Cancel (second-Cancel) behavior are
  observations of that earlier candidate and environment, not evidence for
  this candidate. The failed-network-first case (the first print attempt
  targets a failed or unavailable network printer) was not tested. No claim is
  made that all print-dialog hangs are fixed.
- Packaging scripts and their synthetic contract tests are recovered artifacts,
  not observed release evidence. A final release still requires a clean build,
  real dependency provenance, an unsigned/signed decision based on the actual
  final artifact, and an independent review.

## Unsigned-build warning (conditional)

No final Authenticode signature has been observed for this candidate. **If** the
final installer or executables are distributed unsigned, Windows SmartScreen or
browser warnings such as "Unknown Publisher" may appear. In that case, verify
the published SHA-256 in `SHA256SUMS` and the official repository before
running anything. If signing is performed and observed for the final artifacts,
this conditional warning must be replaced with the observed signature and
publisher facts before release.

## Licensing and corresponding source

First-party code is licensed under the MIT License
(Copyright (c) 2026 OZ Lab); see `LICENSE`. Third-party components retain their
upstream licenses as documented in `THIRD-PARTY-NOTICES.md`,
`THIRD-PARTY-LICENSES.txt`, and `THIRD-PARTY-SOURCE.txt`.

The dynamically loaded LGPL components remain libheif 1.23.2 and libde265 1.1.1
(LGPL-3.0-only). Their complete corresponding source, patches, build scripts, and
rebuild instructions are packaged in
`pikaview-0.1.1-lgpl-corresponding-source.zip`, subject to LGPLv3 §4 and GPLv3
§6(d). Users may rebuild and replace `heif.dll` and `libde265.dll` alongside
`ozviewer.exe`; the application loads them dynamically.

## Historical v0.1.0 material

OZ Image Viewer 0.1.0 and its release assets are historical and are not
re-versioned or replaced by this candidate. The original v0.1.0
`BUILD-PROVENANCE.json` is preserved byte-for-byte at
`docs/releases/v0.1.0/BUILD-PROVENANCE.json`. That historical archive is not
candidate provenance and must not be used as evidence for v0.1.1.

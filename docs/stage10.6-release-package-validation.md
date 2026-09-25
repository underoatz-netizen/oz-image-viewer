# Stage 10.6 — Final release package / installer validation

Status: validation recorded on `main` at commit
`45f8daaeeeb2bda05b905831365526ccecdc1d99`
(`docs(benchmark): record Stage 10.5 regression baselines`), ahead of
`origin/main` by 12 commits, no push performed.

This document records an evidence-backed validation of the committed Stage
9.9/9.10 portable package and installer deliverables using only the existing
packaging scripts, registered tests, and committed build outputs. No source,
test, CMake, or packaging script was changed. No installer was executed, no
association or registry state was modified, and no Windows account/profile was
created or changed.

## 1. Scope and non-goals

Validated:

- committed portable ZIP contents and integrity against its manifest;
- committed installer executable identity (hash, signature, version metadata);
- the three registered packaging/installer tests;
- a fresh, unique temporary extraction of the committed release ZIP, followed
  by an isolated launch of the packaged `ozviewer.exe` against a disposable
  existing fixture;
- a real Inno Setup compile of the Stage 9.10 script from a test-staged
  payload (in a test work directory, not the release artifact).

Explicitly **not** validated in this stage (see section 6):

- a real install / upgrade / uninstall on a clean machine or fresh Windows
  profile;
- association default behavior, Start Menu shortcut creation, UAC/elevation
  absence, or uninstall cleanup on a fresh profile;
- interactive (windowed, non-benchmark) GUI open/close behavior.

## 2. Identity and environment

| Item | Value |
| :--- | :--- |
| Git branch | `main` |
| Git commit | `45f8daaeeeb2bda05b905831365526ccecdc1d99` |
| Commit subject | `docs(benchmark): record Stage 10.5 regression baselines` |
| Tracking | ahead of `origin/main` by 12 (no push) |
| Tracked working tree | clean (only pre-existing untracked artifacts) |
| Build directory | `build/stage9-release` |
| Generator / config | Visual Studio 17 2022, x64, `Release` |
| CMake / CTest | 3.31.6-msvc6 (Visual Studio Build Tools) |
| Inno Setup | 6.7.3, `%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe` (auto-detected via the uninstall registry, not on `PATH`) |

## 3. Committed release deliverable integrity

The committed Stage 9.9/9.10 artifacts under `build/stage9-release` were hashed
and compared with the recorded values in
[`stage9-installer.md`](stage9-installer.md) and with their own `.sha256`
sidecars.

| Artifact | Path | Size (bytes) | SHA-256 | Sidecar agrees |
| :--- | :--- | ---: | :--- | :--- |
| Portable ZIP | `build/stage9-release/package/ozviewer-0.1.0-win-x64-portable.zip` | 2,526,299 | `bda44db5991ad371141828800285f1722e2203ccf4edb54e25d41cb0f5e2b908` | yes |
| Installer EXE | `build/stage9-release/installer/ozviewer-0.1.0-win-x64-installer.exe` | 3,945,903 | `94d6de96c48287f936f63bf302087bd323a0f8ef2bb67ecf494bdd3c194a6f4b` | yes |

Both hashes match the values recorded in `docs/stage9-installer.md` section
"Local validation evidence (authorized run)", so the committed release
deliverables are unchanged since that stage.

Installer executable metadata (read-only inspection):

- Authenticode signature: **NotSigned** (consistent with the documented
  unsigned-distribution statement).
- `ProductName`: `OZ Image Viewer`; `ProductVersion`: `0.1.0`.

Portable ZIP entry set (10 entries, flat — no top-level folder):

```text
heif.dll
libde265.dll
MSVCP140.dll
MSVCP140_ATOMIC_WAIT.dll
ozviewer.exe
package-manifest.json
RUNNING.txt
THIRD-PARTY-NOTICES.md
VCRUNTIME140.dll
VCRUNTIME140_1.dll
```

No excluded build/debug artifacts (`.pdb`, `.lib`, `.obj`, `.tlog`, etc.) are
present in the ZIP.

## 4. Registered test results

All three packaging/installer tests were run with the exact registered CTest
commands from `build/stage9-release`:

```powershell
ctest --test-dir build/stage9-release -C Release `
  -R "package_portable_script_tests|package_portable_smoke|installer_script_tests" `
  --output-on-failure -V
```

Result: **3/3 passed, 0 failed** (total 46.96 s).

| Test | Result | Time | Key evidence |
| :--- | :--- | ---: | :--- |
| `package_portable_script_tests` (#42) | Passed | 15.74 s | happy path 10 entries / 14 extensions; deterministic ZIP `7CC91C1B618025943A73FAC4EAC27442D240426ECD5E3DBF096D87E384AC8B6C`; fail-closed on missing and ambiguous dependency, missing entrypoint, x86 direct and transitive dependency; stray build artifacts excluded |
| `package_portable_smoke` (#43) | Passed | 2.96 s | staged package launched in benchmark mode from an isolated copy with PATH scrubbed to `%SystemRoot%\System32;%SystemRoot%`; full marker prefix through `FirstPresent`; auto-exit |
| `installer_script_tests` (#44) | Passed | 28.24 s | validator happy path (10 files, 14 extensions) and all fail-closed mutations; payload-only `[Files]` and exact extension mapping; 46 HKCU-rooted registry calls audited with prohibited keys absent; mock ownership collision/upgrade/uninstall policy; Unicode/space paths preserved; **real ISCC 6.7.3 compile succeeded** |

The two tests listed as failing in the stale
`build/stage9-release/Testing/Temporary/LastTestsFailed.log` were re-run and now
pass; that log predates this validation.

## 5. Fresh isolated portable extraction and launch

A unique, disposable extraction directory was created outside the repository:

```text
%TEMP%\opencode\stage10.6-20260925-151125-65fa6d42
```

1. The committed release ZIP was extracted there with `Expand-Archive`.
2. `packaging/validate-portable-payload.ps1` was run against the extraction with
   `-ZipPath` set to the committed release ZIP. Result: `VALIDATE_EXIT=0`,
   message `validate-portable: OK: ozviewer.exe 0.1.0 win-x64, 9 files, sha256
   bda44db5...e2b908`. This re-verified manifest schema/version/platform/
   entrypoint, extension allowlist/tokens, exact per-file byte length and
   SHA-256, exact on-disk file set, and the expected payload ZIP hash. The
   validation evidence file is
   `<extract-root>\validated-payload.json` (disposable, outside the repo).
3. `ozviewer.exe --benchmark <disposable fixture>` was launched from the
   extraction with `PATH` scrubbed to `%SystemRoot%\System32;%SystemRoot%`, a
   clean working directory, and a copied `tests/data/webp/bench_64x48.webp`
   fixture.

Observed launch/open result:

```text
EXIT=0
STDERR=(empty)
MARKERS=ProcessStart,RendererReady,FileOpened,CodecDetected,HeaderParsed,
        DecodeStart,DecodeComplete,GpuUploadStart,GpuUploadComplete,FirstPresent
FIRST_PRESENT_INDEX=9
MARKER_COUNT=10
```

The packaged application launched from the extracted release ZIP, opened the
disposable image, completed the decode/upload pipeline through `FirstPresent`,
and exited cleanly — all with the MSVC runtime resolved from the package
directory (no vcpkg/build directory on `PATH`). `FirstPresent` is an internal
presentation-submission marker, not desktop-composed visibility.

## 6. Unexercised checks and remaining risk (NOT EXERCISED)

No isolated VM, sandbox, or fresh disposable Windows profile was positively
confirmed, so the following were **NOT EXERCISED** on this host, which is the
current live developer profile:

- real installer execution (`ozviewer-0.1.0-win-x64-installer.exe` was
  inspected and hashed only, never run);
- clean-machine install, idempotent upgrade, and uninstall;
- Start Menu shortcut creation and removal;
- opt-in association registration and the Windows Default Apps launch on a
  fresh profile;
- association collision / foreign-value fail-closed handling and owned-value
  preservation against the real registry (only a mock-registry model ran);
- absence of UAC/elevation on a machine without the developer toolchain
  present;
- interactive windowed open/close (only `--benchmark`, which auto-exits, was
  exercised; a windowed launch would create a persistent process/window and was
  not driven).

Remaining risk: the `.iss` policy is covered statically (46 audited registry
calls, all HKCU-rooted) and by a mock model, but the real registry lifecycle on
a clean profile remains unverified. The installer is unsigned, so SmartScreen /
Smart App Control first-run warnings are expected. These are the same gaps
already recorded in `docs/stage9-installer.md` section "Remaining limitations"
and are not evidence of a defect.

Current-profile safety was preserved: `HKCU\Software\OZImageViewer` does not
exist after this validation (checked read-only), and no installer, uninstaller,
association, account, or system-state change was performed.

## 7. Working-tree hygiene

- No tracked source, test, CMake, or packaging file was edited.
- No commit or push was performed.
- The only repository addition is this report document.
- Pre-existing untracked artifacts (`dimprint.obj`, `gen_webp_fixtures.obj`,
  `pngprobe.obj`, `test viewer/`) were detected and left untouched; they were
  not used as test input.
- All generated package/test outputs went to `build/stage9-release/*` work
  directories or the disposable temp extraction directory.

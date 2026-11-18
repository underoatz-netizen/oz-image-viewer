# Stage 9.10 เนโฌโ€ per-user installer and opt-in handler registration

This document describes the Stage 9.10 installer packaging boundary for the OZ
image viewer: how the installer is produced, what it does and does not change,
and the local validation evidence recorded so far.

## Scope and non-goals

- The installer is an **unsigned** Inno Setup executable built exclusively from
  a validated Stage 9.9 portable payload. It is not code-signed and no signing
  step exists in this stage.
- It is a **per-user x64** install: `PrivilegesRequired=lowest`, default
  directory `{localappdata}\Programs\OZ Image Viewer`, and
  `ArchitecturesAllowed=x64compatible`. There is no machine-wide mode, no
  elevation override, no service, and no updater.
- Association support is **opt-in** and only registers OZ as an *available*
  handler. The installer **never changes a default automatically**, never
  writes `UserChoice`, never writes `OpenWithProgids`, never writes extension
  default values under `HKCU\Software\Classes\.<ext>`, and never writes `HKLM`
  or another application's keys.
- The installer is separate from the runtime application architecture; no
  application source or runtime behavior changes in this stage.

## Build pipeline

```text
Release x64 payload (ozviewer.exe + DLLs)
        |
        v
packaging/package-portable.ps1           (Stage 9.9, unchanged)
        |  -> staged dir + deterministic ZIP + <zip>.sha256
        v
packaging/validate-portable-payload.ps1  (Stage 9.10, fail closed)
        |  manifest schema / version / platform / entrypoint /
        |  extension allowlist + tokens / exact file hashes /
        |  exact on-disk set / expected payload ZIP hash / ZIP entry hashes /
        |  externally anchored commit/tree/LGPL-source bindings
        v
packaging/build-installer.ps1            (Stage 9.10)
        |  -> ozviewer-installer-generated.iss (payload-only [Files] + facts)
        |  -> copy of ozviewer-installer.iss
        v
ISCC.exe (Inno Setup 6) -> ozviewer-<version>-win-x64-installer.exe
```

`build-installer.ps1` performs **no binary rebuild, signing, or discovery**. It
may only consume a staged payload whose manifest and hashes check out and whose
expected payload ZIP SHA-256 is supplied (`-ExpectedSha256`) or read from the
`<zip>.sha256` sidecar. A build without an expected payload hash fails closed.
The installer `[Files]` list is exactly the validated manifest files plus
`package-manifest.json`; nothing else is staged.

The builder also requires three externally anchored provenance values:

- `-ExpectedCommitSha` (40-character lowercase hex SHA-1),
- `-ExpectedTreeSha` (40-character lowercase hex SHA-1),
- `-ExpectedLgplSourceSha256` (64-character lowercase hex SHA-256).

These values must come from the separately controlled release dispatcher or
clean-build record. **Never derive them by reading the payload's
`BUILD-PROVENANCE.json`**; that would make the untrusted payload vouch for
itself. Missing or wrong values fail closed.

Current v0.1.1 compile invocation (environment variables below are illustrative
dispatch inputs, not values read from the payload):

```powershell
$expectedCommitSha       = $env:EXPECTED_SOURCE_COMMIT_SHA
$expectedTreeSha         = $env:EXPECTED_SOURCE_TREE_SHA
$expectedLgplSourceSha256 = $env:EXPECTED_LGPL_SOURCE_SHA256

powershell -NoProfile -ExecutionPolicy Bypass -File packaging\build-installer.ps1 `
  -PackageDir build\<...>\pikaview-0.1.1-win-x64-portable `
  -ZipPath    build\<...>\pikaview-0.1.1-win-x64-portable.zip `
  -ExpectedCommitSha $expectedCommitSha `
  -ExpectedTreeSha $expectedTreeSha `
  -ExpectedLgplSourceSha256 $expectedLgplSourceSha256 `
  -OutDir     build\installer
```

The `ozviewer-0.1.0-...` identifiers later in this document are historical
v0.1.0 validation evidence, not the current v0.1.1 builder invocation.

When the Inno Setup compiler (`ISCC.exe`) is not installed, the script still
validates and generates everything, records
`INSTALLER_SKIP=INNO_SETUP_NOT_FOUND` with a precise limitation, and exits 77.

## Controlled Release-build wrapper precondition (v0.1.1)

`packaging/invoke-release-build.ps1` invokes
`cmake --build <BuildDir> --config Release` and writes `build-evidence.json`.
Before invoking CMake it requires all of the following:

- a clean Git worktree with stable observed HEAD/tree values;
- `CMakeCache.txt` `CMAKE_HOME_DIRECTORY` (or `ozbench_SOURCE_DIR`) equal to the
  resolved clean worktree;
- `OZVIEWER_PRINT_DIAGNOSTICS=OFF`;
- no candidate `ozviewer.exe` already present in `BuildDir`.

It fails before the build on any mismatch and never deletes user files. This
checks a controlled fresh-configure precondition: it is retained execution
evidence, **not tamper-proof attestation**, and it does not prove that every
object file in `BuildDir` was rebuilt. Use a freshly configured `BuildDir` for
release evidence.

## LGPL dependency provenance boundary (v0.1.1)

`packaging/generate-build-provenance.ps1` now binds the staged `heif.dll` and
`libde265.dll` bytes to the effective dependency build instead of trusting the
caller-selected source package or a self-written provenance file:

- the effective vcpkg root is derived from `CMakeCache.txt`
  `CMAKE_TOOLCHAIN_FILE` (`scripts/buildsystems/vcpkg.cmake`); a supplied
  `-VcpkgRoot` may only assert equality with that root;
- `VCPKG_MANIFEST_MODE`, `VCPKG_MANIFEST_DIR`, `VCPKG_INSTALLED_DIR`, and
  `VCPKG_TARGET_TRIPLET=oz-x64-windows-static-md` are observed from the cache;
- the effective overlay triplet must be byte-identical to the repository
  triplet in `triplets/`;
- registry recipes are selected from the `vcpkg-configuration.json` baseline
  commit and version database, and the local clean `ports/<port>` Git tree must
  match the selected registry `git-tree`; an explicit overlay port is accepted
  only when it is the unique effective selection and its bytes exactly match the
  source ZIP recipe;
- the installed package status must record the expected version, port-version,
  custom triplet, and ABI; the installed file list must contain the output, and
  the installed `heif.dll`/`libde265.dll` SHA-256 must equal the staged files;
- the source ZIP must contain exactly the effective recipe file set, the exact
  effective triplet bytes, and the pinned upstream archives by SHA-256 and
  SHA-512. Expectations come from the committed
  `packaging/lgpl-dependency-policy.json`, never from `BUILD-PROVENANCE.json`.

The trust anchor remains a separately controlled clean configure/build. Byte
equality is consistency evidence, not cryptographic proof of compilation,
custody, or bit-for-bit reproducibility, and a real clean dependency rebuild in
an isolated fresh workspace is still required release evidence. Missing,
ambiguous, mismatched, unrelated-root, substituted-DLL, wrong
version/triplet/overlay, and unobserved dependency evidence all fail closed.

## Opt-in association support

The `[Tasks]` page contains one **initially unchecked** task:

> Register OZ Image Viewer as an available image handler (opens Windows Default
> Apps afterward; your defaults are never changed automatically)

When the user selects it, the installer:

1. writes an application-specific ProgID `OZImageViewer.Image` under
   `HKCU\Software\Classes`;
2. writes a quoted icon and open command
   (`"<install dir>\ozviewer.exe" "%1"`, `DefaultIcon` = `"<exe>",0`);
3. writes `HKCU\Software\OZImageViewer\Capabilities` with `ApplicationName`,
   `ApplicationDescription`, and a `FileAssociations` subkey whose extension
   tokens come **exactly** from the validated manifest;
4. adds one `HKCU\Software\RegisteredApplications` value (`OZ Image Viewer` ->
   `Software\OZImageViewer\Capabilities`);
5. after installation completes, opens Windows Default Apps
   (`ms-settings:defaultapps`, falling back to generic `ms-settings:`) so the
   user can choose a default. It does not choose one.

All registry work happens in the bounded `[Code]` section of
`packaging\ozviewer-installer.iss`; there is no declarative `[Registry]`
section.

## Ownership, collision, upgrade, and uninstall policy

The installer writes an ownership marker
(`HKCU\Software\OZImageViewer\Install\AppId` = the stable AppId GUID).

- **Collision / unowned values fail closed.** Before copying any file
  (`PrepareToInstall`), the installer refuses when:
  - `HKCU\Software\OZImageViewer` exists without a matching ownership marker;
  - the `OZImageViewer.Image` ProgID already exists and is not owned;
  - the `RegisteredApplications` name `OZ Image Viewer` exists but does not
    point at our capabilities key;
  - a `FileAssociations` extension value points at a different ProgID.
  The user sees the reason; nothing is modified.
- **Upgrade reconciles only owned registration.** With a matching marker it
  rewrites its own ProgID/capabilities values, adds/updates the manifest's
  extension tokens, and reaps only stale extension values that both (a) are no
  longer in the manifest and (b) still point at our own ProgID.
- **Uninstall removes only matching owned values/keys.** It requires the
  ownership marker, deletes `RegisteredApplications` only when the value equals
  our capabilities path, deletes `FileAssociations` values only when the data
  equals our ProgID, and deletes ProgID values only when they still match what
  was written. Foreign or user-modified values are preserved; empty keys are
  removed with `RegDeleteKeyIfEmpty` (never recursively). Windows defaults are
  never inferred, changed, or restored.

## Test coverage

`packaging/test-installer-script.ps1` (registered as `installer_script_tests`)
uses an explicitly synthetic contract-test fixture repository and payload; it
does not require a real Release payload. It covers:

- manifest/hash validation happy path and fail-closed mutations (schema,
  version, platform, entrypoint, extension token rules, unknown/duplicate
  extensions, corrupted file hash, missing file, undeclared file, declared
  debug artifact, wrong/missing expected payload hash);
- missing and wrong externally anchored commit/tree/LGPL-source bindings in
  both `validate-portable-payload.ps1` and `build-installer.ps1`;
- installer generation: exact payload-only `[Files]` coverage, quoted source
  paths, and extension mapping identical to the validated manifest;
- static Inno policy assertions: stable AppId, `PrivilegesRequired=lowest`,
  `{localappdata}\Programs\`, `x64compatible`, unchecked association task,
  quoted open command, ownership marker, `RegDeleteKeyIfEmpty`, and absence of
  `[Registry]`, `HKLM`/`HKCR`/`HKEY_CLASSES_ROOT`, `UserChoice`,
  `OpenWithProgids`, extension default keys, and
  `RegDeleteKeyIncludingSubkeys`;
- Unicode/space payload paths preserved through validation and generation;
- a **mock-registry** model of the ownership collision/upgrade/uninstall policy
  (fresh register/clean uninstall, foreign ProgID, foreign
  `RegisteredApplications` value, owned stale-extension reap, modified
  extension fail-closed, user-modified value preservation, unowned no-op);
- the unsigned distribution statement;
- an ISCC compile of the synthetic contract payload only when Inno Setup is
  locally available.

`packaging/test-release-build-wrapper.ps1` (registered as
`release_build_wrapper_tests`) uses a mock CMake script and isolated clean
fixtures to demonstrate that a wrong cache source directory, Git command
failure, and a preexisting `ozviewer.exe` are rejected before a mock build,
and that ambiguous post-build executables or a failed build do not write
`build-evidence.json`. Its generated binaries and receipts are synthetic
contract-test evidence only, not real build evidence.

The mock registry is a model of the `[Code]` policy, not the runtime code. The
static assertions cover the real `.iss` source. Neither replaces a real
clean-machine install run.

## Unsigned distribution statement

The installer executable produced by this stage is **unsigned**: it has no
Authenticode signature and no timestamp. Windows SmartScreen or Smart App
Control may warn on first run. Users should verify the published SHA-256
(written next to the installer as `<name>.exe.sha256`) before running it.
Unsigned distribution is intentional for this stage; code signing, publisher
identity, and update delivery are out of scope.

## Historical local validation evidence (v0.1.0, not current v0.1.1 evidence)

An authorized local validation run was performed on the build host with Inno
Setup **6.7.3** (`ISCC.exe` available). The identifiers and hashes below are
historical **v0.1.0** evidence, not a current v0.1.1 builder run. The per-user
installer compiled successfully and the resulting artifacts were hashed:

- installer `ozviewer-0.1.0-win-x64-installer.exe` SHA-256:
  `94d6de96c48287f936f63bf302087bd323a0f8ef2bb67ecf494bdd3c194a6f4b`;
- validated portable payload ZIP SHA-256:
  `bda44db5991ad371141828800285f1722e2203ccf4edb54e25d41cb0f5e2b908`.

The run exercised the following lifecycle checks and all passed:

- clean install with the association task **unchecked** (no registry writes);
- opt-in association registration, including the Windows **Default Apps**
  launch (`ms-settings:defaultapps`);
- idempotent rerun over an existing owned install;
- collision/unowned-value fail-closed handling;
- preservation of foreign/user-modified values on upgrade and uninstall;
- Unicode/space install and payload paths.

## Remaining limitations

- The installer remains **unsigned** (see [Unsigned distribution
  statement](#unsigned-distribution-statement)); SmartScreen/Smart App Control
  warnings and publisher identity are still unaddressed by design for this
  stage.
- Validation ran on a single developer build host, not a clean machine or a
  clean Windows user profile. UAC/elevation absence, Start Menu shortcut
  creation, and uninstall cleanup on a fresh profile remain unverified there.
- The `INSTALLER_SKIP=INNO_SETUP_NOT_FOUND` exit-77 path is still the behavior
  on hosts without the Inno Setup compiler; it was not the path taken here.

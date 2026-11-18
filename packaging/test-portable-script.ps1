<#
.SYNOPSIS
  Focused tests for the Stage 9.9 packaging boundary (package-portable.ps1).

.DESCRIPTION
  Exercises the real production package-portable.ps1 inside an isolated,
  explicitly synthetic contract-test fixture rooted in a clean throwaway git
  repository. The fixture repository is what package-portable.ps1's clean-source
  release gate evaluates; the production gate itself is not weakened, bypassed,
  or replaced.

  The payload is a deterministic hand-authored contract-test payload (minimal
  x64 PE images with real import tables) and the BUILD-PROVENANCE.json is a
  clearly labelled synthetic contract fixture. Neither is a real build or a
  real dependency record. A fresh clean-commit Release build and wrapper
  integration remains a separately required release step.

  Covers:
    * happy-path staging + ZIP contents and exclusion of build artifacts,
    * deterministic ZIP output for identical inputs,
    * fail-closed on a missing app-local dependency,
    * fail-closed on an ambiguous dependency,
    * fail-closed when the entrypoint is absent,
    * fail-closed when a staged dependency is not x64,
    * fail-closed when a non-seeded transitive dependency (reached only through
      dependency-closure traversal from a seeded DLL) is not x64.

  The tests never modify the source repository or the real build output;
  broken inputs are made in temporary copies under -WorkDir.

.PARAMETER WorkDir
  Scratch directory for the synthetic fixture repository, payload copies, and
  packages.

.NOTES
  Exit codes: 0 = pass, 77 = skipped, 1 = failure.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$WorkDir
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = (Resolve-Path -LiteralPath (Split-Path -Parent $ScriptDir)).Path
$FixtureHelper = Join-Path $ScriptDir 'test-contract-fixtures.ps1'
if (-not (Test-Path -LiteralPath $FixtureHelper -PathType Leaf)) {
    Write-Host "test-portable: SKIP: contract fixture helper not found: $FixtureHelper" -ForegroundColor Yellow
    exit 77
}
. $FixtureHelper

function Skip([string]$Message) {
    Write-Host "test-portable: SKIP: $Message" -ForegroundColor Yellow
    exit 77
}
function Pass([string]$Message) { Write-Host "test-portable: PASS: $Message" -ForegroundColor Green }
function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "ASSERT FAILED: $Message" }
}

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Skip "git is required to construct the clean contract-test fixture repository"
}

if (-not (Test-Path -LiteralPath $WorkDir -PathType Container)) {
    New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null
}
$WorkDir = (Resolve-Path -LiteralPath $WorkDir).Path
# Unique run scope so repeated CTest invocations never collide with or delete
# a previous test run's artifacts.
$WorkDir = Join-Path $WorkDir ('run-' + [System.Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null

# ---------------------------------------------------------------------------
# Synthetic contract-test fixture setup. Never real build evidence.
# ---------------------------------------------------------------------------
$fixture = New-TestPackagingFixture -Root (Join-Path $WorkDir 'contract-fixture') `
    -SourceScriptDir $ScriptDir -SourceRepoRoot $RepoRoot
$PayloadDir = Join-Path $WorkDir 'contract-payload'
New-TestPayload -Dir $PayloadDir
$LgplAsset = Join-Path $WorkDir 'pikaview-0.1.1-lgpl-corresponding-source.zip'
$null = New-TestLgplFixtureAsset -Path $LgplAsset
$Provenance = New-TestBuildProvenance -Path (Join-Path $WorkDir 'BUILD-PROVENANCE.json') `
    -CommitSha $fixture.Head -TreeSha $fixture.Tree -PayloadDir $PayloadDir -LgplAssetPath $LgplAsset
$PackageScript = Join-Path $fixture.PackagingDir 'package-portable.ps1'
Write-Host "test-portable: NOTE: using synthetic contract-test fixture repo $($fixture.RepoRoot) at $($fixture.Head) (not a real build)" -ForegroundColor Yellow

function Invoke-Packaging {
    param(
        [string]$Payload,
        [string]$Out,
        [string[]]$ExtraSearchDirs = @(),
        [string]$RuntimeDir = "",
        [string]$MetadataPath = "",
        [string]$ProvenancePath = ""
    )
    $args = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PackageScript,
              '-PayloadDir', $Payload, '-OutDir', $Out)
    if (-not [string]::IsNullOrWhiteSpace($RuntimeDir)) { $args += @('-RuntimeDir', $RuntimeDir) }
    if (-not [string]::IsNullOrWhiteSpace($MetadataPath)) { $args += @('-MetadataPath', $MetadataPath) }
    if (-not [string]::IsNullOrWhiteSpace($ProvenancePath)) { $args += @('-ProvenancePath', $ProvenancePath) }
    foreach ($e in $ExtraSearchDirs) { $args += @('-ExtraSearchDirs', $e) }
    $out = & powershell @args 2>&1
    return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Output = ($out -join "`n") }
}

function Get-ZipEntries {
    param([string]$ZipPath)
    Add-Type -AssemblyName System.IO.Compression.FileSystem | Out-Null
    $zip = [System.IO.Compression.ZipFile]::OpenRead($ZipPath)
    try { return @($zip.Entries | ForEach-Object { $_.FullName }) }
    finally { $zip.Dispose() }
}

try {
    # --- Case 1: happy path --------------------------------------------------
    $out1 = Join-Path $WorkDir 'out-happy'
    $r1 = Invoke-Packaging -Payload $PayloadDir -Out $out1 -ProvenancePath $Provenance.Path
    Assert-True ($r1.ExitCode -eq 0) "happy path failed (exit $($r1.ExitCode)):`n$($r1.Output)"
    $zip1 = Join-Path $out1 'pikaview-0.1.1-win-x64-portable.zip'
    Assert-True (Test-Path -LiteralPath $zip1) "expected ZIP was not produced"
    $entries1 = Get-ZipEntries -ZipPath $zip1
    foreach ($required in @('ozviewer.exe', 'heif.dll', 'libde265.dll', 'localdep.dll', 'LICENSE', 'THIRD-PARTY-NOTICES.md', 'THIRD-PARTY-LICENSES.txt', 'THIRD-PARTY-SOURCE.txt', 'BUILD-PROVENANCE.json', 'RUNNING.txt', 'package-manifest.json')) {
        Assert-True ($entries1 -contains $required) "happy path ZIP is missing $required (got: $($entries1 -join ', '))"
    }
    foreach ($entry in $entries1) {
        Assert-True ($entry -notmatch '\.(pdb|lib|obj|exp|ilk|iobj|ipdb|map|idb|pch|tlog|log|res|tmp|xml|manifest)$') "excluded artifact staged: $entry"
    }
    $manifest1 = Get-Content -LiteralPath (Join-Path (Join-Path $out1 'pikaview-0.1.1-win-x64-portable') 'package-manifest.json') -Raw | ConvertFrom-Json
    Assert-True ($manifest1.supportedExtensions -contains '.jpg') "derived extension list missing .jpg"
    Assert-True ($manifest1.supportedExtensions -contains '.png') "derived extension list missing .png"
    foreach ($ext in $manifest1.supportedExtensions) {
        Assert-True ($ext -match '^\.[a-z0-9]+$') "unexpected derived extension token: $ext"
    }
    Pass "happy path: $(($entries1 | Measure-Object).Count) entries, extensions: $($manifest1.supportedExtensions -join ' ')"

    # --- Case 2: deterministic ZIP ------------------------------------------
    $out2 = Join-Path $WorkDir 'out-deterministic'
    $r2 = Invoke-Packaging -Payload $PayloadDir -Out $out2 -ProvenancePath $Provenance.Path
    Assert-True ($r2.ExitCode -eq 0) "second packaging run failed:`n$($r2.Output)"
    $zip2 = Join-Path $out2 'pikaview-0.1.1-win-x64-portable.zip'
    $h1 = (Get-FileHash -LiteralPath $zip1 -Algorithm SHA256).Hash
    $h2 = (Get-FileHash -LiteralPath $zip2 -Algorithm SHA256).Hash
    Assert-True ($h1 -eq $h2) "ZIP is not deterministic: $h1 vs $h2"
    Pass "deterministic ZIP: SHA256 $h1"

    # --- Case 3: missing app-local dependency (fail closed) ------------------
    $brokenPayload = Join-Path $WorkDir 'payload-missing-dll'
    if (Test-Path -LiteralPath $brokenPayload) { Remove-Item -LiteralPath $brokenPayload -Recurse -Force }
    New-Item -ItemType Directory -Path $brokenPayload -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $PayloadDir 'ozviewer.exe') -Destination $brokenPayload -Force
    Copy-Item -LiteralPath (Join-Path $PayloadDir 'localdep.dll') -Destination $brokenPayload -Force
    Copy-Item -LiteralPath (Join-Path $PayloadDir 'libde265.dll') -Destination $brokenPayload -Force
    $r3 = Invoke-Packaging -Payload $brokenPayload -Out (Join-Path $WorkDir 'out-missing') -ProvenancePath $Provenance.Path
    Assert-True ($r3.ExitCode -ne 0) "missing dependency did not fail closed"
    Assert-True ($r3.Output -match 'heif\.dll') "missing-dependency error did not name heif.dll:`n$($r3.Output)"
    Pass "missing dependency fails closed"

    # --- Case 4: ambiguous dependency (fail closed) --------------------------
    $ambiguousDir = Join-Path $WorkDir 'ambiguous'
    if (Test-Path -LiteralPath $ambiguousDir) { Remove-Item -LiteralPath $ambiguousDir -Recurse -Force }
    New-Item -ItemType Directory -Path $ambiguousDir -Force | Out-Null
    # A valid but different x64 PE with the same file name as the app-local DLL.
    New-TestPe64 -Path (Join-Path $ambiguousDir 'localdep.dll') -ExtraText 'ambiguous-duplicate'
    $r4 = Invoke-Packaging -Payload $PayloadDir -Out (Join-Path $WorkDir 'out-ambiguous') -ExtraSearchDirs @($ambiguousDir) -ProvenancePath $Provenance.Path
    Assert-True ($r4.ExitCode -ne 0) "ambiguous dependency did not fail closed"
    Assert-True ($r4.Output -match 'ambiguous') "ambiguity error not reported:`n$($r4.Output)"
    Pass "ambiguous dependency fails closed"

    # --- Case 5: entrypoint missing (fail closed) ----------------------------
    $emptyPayload = Join-Path $WorkDir 'payload-empty'
    if (Test-Path -LiteralPath $emptyPayload) { Remove-Item -LiteralPath $emptyPayload -Recurse -Force }
    New-Item -ItemType Directory -Path $emptyPayload -Force | Out-Null
    $r5 = Invoke-Packaging -Payload $emptyPayload -Out (Join-Path $WorkDir 'out-noentry') -ProvenancePath $Provenance.Path
    Assert-True ($r5.ExitCode -ne 0) "missing entrypoint did not fail closed"
    Assert-True ($r5.Output -match 'entrypoint') "missing-entrypoint error not reported:`n$($r5.Output)"
    Pass "missing entrypoint fails closed"

    # --- Case 6: stray build artifacts in payload are never staged -----------
    $dirtyPayload = Join-Path $WorkDir 'payload-dirty'
    if (Test-Path -LiteralPath $dirtyPayload) { Remove-Item -LiteralPath $dirtyPayload -Recurse -Force }
    New-Item -ItemType Directory -Path $dirtyPayload -Force | Out-Null
    foreach ($f in (Get-ChildItem -LiteralPath $PayloadDir -File)) { Copy-Item -LiteralPath $f.FullName -Destination $dirtyPayload -Force }
    foreach ($stray in @('ozviewer.pdb', 'ozviewer.lib', 'main.obj', 'bench_fixture.bmp', 'devtool.exe', 'cmake_install.cmake')) {
        [System.IO.File]::WriteAllText((Join-Path $dirtyPayload $stray), 'not a real artifact')
    }
    $out6 = Join-Path $WorkDir 'out-dirty'
    $r6 = Invoke-Packaging -Payload $dirtyPayload -Out $out6 -ProvenancePath $Provenance.Path
    Assert-True ($r6.ExitCode -eq 0) "packaging a dirty payload failed:`n$($r6.Output)"
    $entries6 = Get-ZipEntries -ZipPath (Join-Path $out6 'pikaview-0.1.1-win-x64-portable.zip')
    foreach ($stray in @('ozviewer.pdb', 'ozviewer.lib', 'main.obj', 'bench_fixture.bmp', 'devtool.exe', 'cmake_install.cmake')) {
        Assert-True ($entries6 -notcontains $stray) "stray artifact was staged: $stray"
    }
    Pass "stray build artifacts excluded from package"

    # --- Case 7: x86 dependency (fail closed) -------------------------------
    # Sub-case 7a: required direct runtime dependency
    $x86Payload = Join-Path $WorkDir 'payload-x86-dep'
    if (Test-Path -LiteralPath $x86Payload) { Remove-Item -LiteralPath $x86Payload -Recurse -Force }
    New-Item -ItemType Directory -Path $x86Payload -Force | Out-Null
    foreach ($f in (Get-ChildItem -LiteralPath $PayloadDir -File)) { Copy-Item -LiteralPath $f.FullName -Destination $x86Payload -Force }
    $patchTarget = Join-Path $x86Payload 'heif.dll'
    Assert-True (Test-Path -LiteralPath $patchTarget -PathType Leaf) "payload has no heif.dll to patch for the x86 test"
    $patchBytes = [System.IO.File]::ReadAllBytes($patchTarget)
    $patchPeOff = [BitConverter]::ToUInt32($patchBytes, 0x3C)
    $patchBytes[$patchPeOff + 4] = 0x4C
    $patchBytes[$patchPeOff + 5] = 0x01
    [System.IO.File]::WriteAllBytes($patchTarget, $patchBytes)
    $r7 = Invoke-Packaging -Payload $x86Payload -Out (Join-Path $WorkDir 'out-x86') -ProvenancePath $Provenance.Path
    Assert-True ($r7.ExitCode -ne 0) "x86 dependency did not fail closed"
    Assert-True (($r7.Output -match 'heif\.dll') -and ($r7.Output -match '0x8664')) "x86-dependency error did not name heif.dll with expected 0x8664:`n$($r7.Output)"
    Pass "x86 required dependency fails closed with clear diagnostic"

    # Sub-case 7b: genuine transitive dependency, reached only by dependency
    # closure traversal from a seeded DLL.
    #
    # libde265.dll is imported by heif.dll, but it is ALSO an explicit
    # requiredRuntimeDlls seed in the production metadata. Patching it there
    # would be caught by the direct-seed resolution path, not by traversal, so
    # that variant cannot prove the transitive case. Here libde265.dll is
    # removed from the seed declarations, leaving heif.dll's import table as the
    # only route into the closure.
    $transMetadataPath = Join-Path $WorkDir 'portable-package-transitive.json'
    $transMeta = Get-Content -LiteralPath $fixture.MetadataPath -Raw | ConvertFrom-Json
    $transMeta.requiredRuntimeDlls = @($transMeta.requiredRuntimeDlls | Where-Object { $_ -ne 'libde265.dll' })
    # Guard against a masked direct path: the candidate must not be seeded while
    # the traversal root (heif.dll) must remain seeded.
    Assert-True ($transMeta.requiredRuntimeDlls -notcontains 'libde265.dll') "transitive metadata still seeds libde265.dll (a direct path would mask traversal)"
    Assert-True ($transMeta.requiredRuntimeDlls -contains 'heif.dll') "transitive metadata must keep heif.dll seeded as the closure root"
    [System.IO.File]::WriteAllText($transMetadataPath, ($transMeta | ConvertTo-Json -Depth 8), (New-Object System.Text.UTF8Encoding($false)))

    # 7b-i (positive control): the healthy x64 payload with libde265.dll
    # unseeded must still package, and libde265.dll must appear in the ZIP only
    # because the closure follows heif.dll's imports to it.
    $out7bControl = Join-Path $WorkDir 'out-x86-trans-control'
    $r7bControl = Invoke-Packaging -Payload $PayloadDir -Out $out7bControl -MetadataPath $transMetadataPath -ProvenancePath $Provenance.Path
    Assert-True ($r7bControl.ExitCode -eq 0) "transitive control packaging failed:`n$($r7bControl.Output)"
    $entries7bControl = Get-ZipEntries -ZipPath (Join-Path $out7bControl 'pikaview-0.1.1-win-x64-portable.zip')
    Assert-True ($entries7bControl -contains 'libde265.dll') "libde265.dll was not staged through the dependency closure (control invalid)"
    Pass "transitive control: unseeded libde265.dll staged via heif.dll import closure"

    # 7b-ii (negative): patch only libde265.dll to x86. It is not a seed, so a
    # rejection can only come from closure traversal rooted at seeded heif.dll.
    $x86TransPayload = Join-Path $WorkDir 'payload-x86-trans-dep'
    if (Test-Path -LiteralPath $x86TransPayload) { Remove-Item -LiteralPath $x86TransPayload -Recurse -Force }
    New-Item -ItemType Directory -Path $x86TransPayload -Force | Out-Null
    foreach ($f in (Get-ChildItem -LiteralPath $PayloadDir -File)) { Copy-Item -LiteralPath $f.FullName -Destination $x86TransPayload -Force }
    $patchTargetTrans = Join-Path $x86TransPayload 'libde265.dll'
    Assert-True (Test-Path -LiteralPath $patchTargetTrans -PathType Leaf) "payload has no libde265.dll to patch for the x86 transitive test"
    $patchBytesTrans = [System.IO.File]::ReadAllBytes($patchTargetTrans)
    $patchPeOffTrans = [BitConverter]::ToUInt32($patchBytesTrans, 0x3C)
    $patchBytesTrans[$patchPeOffTrans + 4] = 0x4C
    $patchBytesTrans[$patchPeOffTrans + 5] = 0x01
    [System.IO.File]::WriteAllBytes($patchTargetTrans, $patchBytesTrans)
    $r7b = Invoke-Packaging -Payload $x86TransPayload -Out (Join-Path $WorkDir 'out-x86-trans') -MetadataPath $transMetadataPath -ProvenancePath $Provenance.Path
    Assert-True ($r7b.ExitCode -ne 0) "x86 transitive dependency did not fail closed"
    Assert-True ($r7b.Output -match 'libde265\.dll') "x86 transitive dependency error did not name libde265.dll:`n$($r7b.Output)"
    Assert-True (($r7b.Output -match '0x014C') -and ($r7b.Output -match '0x8664')) "x86 transitive dependency error did not report the detected (0x014C) and expected (0x8664) machines:`n$($r7b.Output)"
    Pass "x86 unseeded dependency fails closed via closure traversal with clear diagnostic"

    # 7b-iii (isolation control): with no dynamic DLL seeded, heif.dll and
    # libde265.dll are unreachable, so the same x86-patched libde265.dll must
    # NOT be rejected and must NOT be staged. This proves the rejection above is
    # caused by closure traversal reaching the DLL, not by the patched file
    # merely being present in the payload.
    #
    # The synthetic provenance must describe exactly the reduced staged PE set,
    # because package-portable.ps1 requires every staged PE to be listed and no
    # listed candidate to be absent.
    $noSeedPayloadForProvenance = Join-Path $WorkDir 'payload-no-seed-provenance'
    if (Test-Path -LiteralPath $noSeedPayloadForProvenance) { Remove-Item -LiteralPath $noSeedPayloadForProvenance -Recurse -Force }
    New-Item -ItemType Directory -Path $noSeedPayloadForProvenance -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $x86TransPayload 'ozviewer.exe') -Destination $noSeedPayloadForProvenance -Force
    Copy-Item -LiteralPath (Join-Path $x86TransPayload 'localdep.dll') -Destination $noSeedPayloadForProvenance -Force
    $noSeedProvenance = New-TestBuildProvenance -Path (Join-Path $WorkDir 'no-seed\BUILD-PROVENANCE.json') `
        -CommitSha $fixture.Head -TreeSha $fixture.Tree -PayloadDir $noSeedPayloadForProvenance -LgplAssetPath $LgplAsset

    $noSeedMeta = Get-Content -LiteralPath $fixture.MetadataPath -Raw | ConvertFrom-Json
    $noSeedMeta.requiredRuntimeDlls = @()
    $noSeedMetadataPath = Join-Path $WorkDir 'portable-package-no-dynamic.json'
    [System.IO.File]::WriteAllText($noSeedMetadataPath, ($noSeedMeta | ConvertTo-Json -Depth 8), (New-Object System.Text.UTF8Encoding($false)))
    $out7bIsolated = Join-Path $WorkDir 'out-x86-trans-unreachable'
    $r7bIsolated = Invoke-Packaging -Payload $x86TransPayload -Out $out7bIsolated -MetadataPath $noSeedMetadataPath -ProvenancePath $noSeedProvenance.Path
    Assert-True ($r7bIsolated.ExitCode -eq 0) "unreachable x86 dependency should not be rejected:`n$($r7bIsolated.Output)"
    $entries7bIsolated = Get-ZipEntries -ZipPath (Join-Path $out7bIsolated 'pikaview-0.1.1-win-x64-portable.zip')
    Assert-True ($entries7bIsolated -notcontains 'libde265.dll') "unreachable libde265.dll was staged"
    Assert-True ($entries7bIsolated -notcontains 'heif.dll') "unreachable heif.dll was staged"
    Pass "x86 dependency is rejected only when reached by dependency closure traversal"

    Write-Host "test-portable: ALL CASES PASSED (synthetic contract fixtures; real clean-commit build remains a separate release step)" -ForegroundColor Green
    exit 0
}
catch {
    Write-Host "test-portable: FAIL: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}

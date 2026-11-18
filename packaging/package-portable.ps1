<#
.SYNOPSIS
  Stage and ZIP the unsigned portable x64 OZ image viewer package (Stage 9.9).

.DESCRIPTION
  Consumes an existing Release x64 viewer payload (a directory that already
  contains ozviewer.exe plus its app-local runtime DLLs) and produces a
  versioned portable ZIP. This is a packaging boundary only: it does not build,
  sign, install, register file associations, write the registry, create
  shortcuts, or change any system setting.

  The staged file set is allowlist-only:
    * the entrypoint (ozviewer.exe),
    * the transitive non-system import closure of the entrypoint and every
      staged DLL, resolved to real files (fails closed on a missing or
      ambiguous dependency),
    * declared required runtime DLLs that are loaded dynamically (heif.dll,
      libde265.dll) and their own import closures,
    * the declared license/attribution files,
    * a generated RUNNING.txt and package-manifest.json.

  Build artifacts, dev tools, test fixtures, symbols and debug files are never
  staged; the script asserts the staged set contains none of them.

.PARAMETER PayloadDir
  Existing Release x64 payload directory (ozviewer.exe + runtime DLLs).

.PARAMETER OutDir
  Output directory. Staging goes to <OutDir>/<package folder> and the ZIP to
  <OutDir>/<versioned zip name>.

.PARAMETER Version
  Package version. Defaults to the version in packaging/portable-package.json.

.PARAMETER RuntimeDir
  Directory that provides the app-local MSVC C++ runtime DLLs (the Visual
  Studio "Microsoft.VC143.CRT" redistribution folder). When omitted the script
  auto-discovers it via %VCToolsRedistDir% or vswhere. It is only required when
  the dependency closure contains a VC runtime DLL.

.PARAMETER MetadataPath
  Path to the package metadata JSON. Defaults to portable-package.json next to
  this script.

.PARAMETER ExtraSearchDirs
  Additional directories searched for app-local dependencies (after PayloadDir
  and RuntimeDir).

.PARAMETER NoZip
  Stage only; skip ZIP creation.

.NOTES
  Exit codes: 0 = success, 1 = validation/packaging failure.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$PayloadDir,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [string]$Version = "",
    [string]$RuntimeDir = "",
    [string]$MetadataPath = "",
    [string]$ProvenancePath = "",
    [string[]]$ExtraSearchDirs = @(),
    [switch]$NoZip
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Split-Path -Parent $ScriptDir
if ([string]::IsNullOrWhiteSpace($MetadataPath)) {
    $MetadataPath = Join-Path $ScriptDir 'portable-package.json'
}

function Fail([string]$Message) {
    Write-Host "package-portable: ERROR: $Message" -ForegroundColor Red
    exit 1
}

function Info([string]$Message) {
    Write-Host "package-portable: $Message"
}

# ---------------------------------------------------------------------------
# Minimal PE reader: x64 machine type + import directory DLL names.
# No external tool (dumpbin/link) or Developer prompt is required.
# ---------------------------------------------------------------------------
function Get-PeInfo {
    param([Parameter(Mandatory = $true)][string]$Path)

    $bytes = [System.IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 0x40) { throw "not a PE file (too small): $Path" }
    $peOff = [BitConverter]::ToUInt32($bytes, 0x3C)
    if ($peOff -le 0 -or ($peOff + 24) -ge $bytes.Length) { throw "invalid PE header offset: $Path" }
    if ([BitConverter]::ToUInt32($bytes, $peOff) -ne 0x00004550) { throw "missing PE signature: $Path" }

    $machine = [BitConverter]::ToUInt16($bytes, $peOff + 4)
    $magic = [BitConverter]::ToUInt16($bytes, $peOff + 24)
    if ($magic -ne 0x20B -and $magic -ne 0x10B) { throw "unsupported PE optional header magic: $Path" }
    $linkerVersion = "$([int]$bytes[$peOff + 26]).$([int]$bytes[$peOff + 27])"

    $numSections = [BitConverter]::ToUInt16($bytes, $peOff + 6)
    $optSize = [BitConverter]::ToUInt16($bytes, $peOff + 20)
    $dataDirOff = $peOff + 24 + $(if ($magic -eq 0x20B) { 112 } else { 96 })
    $importRva = 0
    if (($dataDirOff + 16) -le $bytes.Length) {
        $importRva = [BitConverter]::ToUInt32($bytes, $dataDirOff + 8)
    }

    $sections = @()
    $secOff = $peOff + 24 + $optSize
    for ($i = 0; $i -lt $numSections; $i++) {
        $b = $secOff + ($i * 40)
        if (($b + 40) -gt $bytes.Length) { break }
        $sections += [pscustomobject]@{
            VirtualAddress = [BitConverter]::ToUInt32($bytes, $b + 12)
            VirtualSize    = [BitConverter]::ToUInt32($bytes, $b + 8)
            RawPointer     = [BitConverter]::ToUInt32($bytes, $b + 20)
            RawSize        = [BitConverter]::ToUInt32($bytes, $b + 16)
        }
    }

    $rvaToOffset = {
        param($rva)
        foreach ($s in $sections) {
            $size = if ($s.VirtualSize -gt 0) { $s.VirtualSize } else { $s.RawSize }
            if ($rva -ge $s.VirtualAddress -and $rva -lt ($s.VirtualAddress + $size)) {
                return [int]($s.RawPointer + ($rva - $s.VirtualAddress))
            }
        }
        return -1
    }

    $imports = @()
    if ($importRva -ne 0) {
        $d = & $rvaToOffset $importRva
        if ($d -lt 0) { throw "import directory RVA is not mapped: $Path" }
        $guard = 0
        while ($guard -lt 8192) {
            $guard++
            if (($d + 20) -gt $bytes.Length) { throw "truncated import descriptor: $Path" }
            $nameRva = [BitConverter]::ToUInt32($bytes, $d + 12)
            if ($nameRva -eq 0) { break }
            $n = & $rvaToOffset $nameRva
            if ($n -lt 0) { throw "import name RVA is not mapped: $Path" }
            $sb = New-Object System.Text.StringBuilder
            while ($n -lt $bytes.Length -and $bytes[$n] -ne 0) {
                [void]$sb.Append([char]$bytes[$n])
                $n++
            }
            if ($sb.Length -gt 0) { $imports += $sb.ToString() }
            $d += 20
        }
    }
    return [pscustomobject]@{ Machine = $machine; LinkerVersion = $linkerVersion; Imports = $imports }
}

function Test-IsOsDependency {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string[]]$VcPrefixes,
        [Parameter(Mandatory = $true)][string[]]$SystemPrefixes
    )
    $lower = $Name.ToLowerInvariant()
    foreach ($p in $SystemPrefixes) {
        if ($lower.StartsWith($p)) { return $true }
    }
    # The MSVC C++ runtime must be deployed app-local for a portable package,
    # even when a developer machine happens to have a redist copy in System32.
    foreach ($p in $VcPrefixes) {
        if ($lower.StartsWith($p)) { return $false }
    }
    $systemCopy = Join-Path $env:SystemRoot ("System32\" + $Name)
    if (Test-Path -LiteralPath $systemCopy -PathType Leaf) { return $true }
    return $false
}

function Find-VcRuntimeDir {
    param([string]$PreferredLinkerVersion = "")

    $candidates = @()
    if ($env:VCToolsRedistDir) { $candidates += $env:VCToolsRedistDir }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
        $installs = & $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
        foreach ($inst in @($installs)) {
            if (-not [string]::IsNullOrWhiteSpace($inst)) {
                $candidates += (Join-Path $inst 'VC\Redist\MSVC')
            }
        }
    }
    $crtDirs = @()
    foreach ($c in $candidates) {
        if (Test-Path -LiteralPath $c -PathType Container) {
            # %VCToolsRedistDir% is ...\VC\Redist\MSVC\; vswhere candidate is ...\VC\Redist\MSVC
            $roots = @($c, (Join-Path $c 'MSVC'))
            foreach ($root in $roots) {
                if (Test-Path -LiteralPath $root -PathType Container) {
                    $crtDirs += Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue |
                        ForEach-Object { Join-Path $_.FullName 'x64' } |
                        Where-Object { Test-Path -LiteralPath $_ -PathType Container } |
                        ForEach-Object { Get-ChildItem -LiteralPath $_ -Directory -ErrorAction SilentlyContinue } |
                        Where-Object { $_.Name -like 'Microsoft.VC*.CRT' } |
                        ForEach-Object { $_.FullName }
                }
            }
        }
    }
    if ($crtDirs.Count -eq 0) { return "" }
    # Prefer the redistribution folder whose toolset version matches the
    # compiler that produced the payload (PE linker version), so the staged CRT
    # matches the build; otherwise fall back to the highest version present.
    if (-not [string]::IsNullOrWhiteSpace($PreferredLinkerVersion)) {
        $matching = @($crtDirs | Where-Object { $_ -like "*\$PreferredLinkerVersion.*" })
        if ($matching.Count -gt 0) { return ($matching | Sort-Object -Descending | Select-Object -First 1) }
    }
    return ($crtDirs | Sort-Object -Descending | Select-Object -First 1)
}

function Get-SupportedExtensions {
    param(
        [Parameter(Mandatory = $true)][string]$ExePath,
        [Parameter(Mandatory = $true)][string[]]$KnownExtensions
    )
    $bytes = [System.IO.File]::ReadAllBytes($ExePath)
    $ascii = [System.Text.Encoding]::ASCII.GetString($bytes)
    $utf16 = [System.Text.Encoding]::Unicode.GetString($bytes)
    $rx = [regex]'\*\.([A-Za-z0-9]{1,6})'
    $found = @{}
    foreach ($text in @($ascii, $utf16)) {
        foreach ($m in $rx.Matches($text)) {
            $token = $m.Groups[1].Value.ToLowerInvariant()
            foreach ($known in $KnownExtensions) {
                if ($token -eq $known.ToLowerInvariant()) {
                    $found[$known.ToLowerInvariant()] = $true
                }
            }
        }
    }
    $ordered = @()
    foreach ($known in $KnownExtensions) {
        if ($found.ContainsKey($known.ToLowerInvariant())) { $ordered += $known }
    }
    return $ordered
}

function Resolve-DependencyClosure {
    param(
        [Parameter(Mandatory = $true)][string[]]$Seeds,
        [Parameter(Mandatory = $true)][string[]]$SearchDirs,
        [Parameter(Mandatory = $true)][string[]]$VcPrefixes,
        [Parameter(Mandatory = $true)][string[]]$SystemPrefixes
    )
    $resolved = @{}     # lower name -> full path
    $visited = @{}
    $queue = New-Object System.Collections.Queue
    foreach ($s in $Seeds) { $queue.Enqueue($s) }

    while ($queue.Count -gt 0) {
        $name = [string]$queue.Dequeue()
        if ([string]::IsNullOrWhiteSpace($name)) { continue }
        $key = $name.ToLowerInvariant()
        if ($visited.ContainsKey($key)) { continue }
        $visited[$key] = $true

        if (Test-IsOsDependency -Name $name -VcPrefixes $VcPrefixes -SystemPrefixes $SystemPrefixes) {
            continue
        }

        $matches = @()
        foreach ($dir in $SearchDirs) {
            $cand = Join-Path $dir $name
            if (Test-Path -LiteralPath $cand -PathType Leaf) {
                $matches += (Get-Item -LiteralPath $cand)
            }
        }
        if ($matches.Count -eq 0) {
            $where = ($SearchDirs -join '; ')
            throw "missing dependency '$name' (searched: $where). Provide it in the payload or with -RuntimeDir/-ExtraSearchDirs."
        }

        $byHash = @{}
        foreach ($m in $matches) {
            $h = (Get-FileHash -LiteralPath $m.FullName -Algorithm SHA256).Hash
            if (-not $byHash.ContainsKey($h)) { $byHash[$h] = New-Object System.Collections.ArrayList }
            [void]$byHash[$h].Add($m)
        }
        if ($byHash.Keys.Count -gt 1) {
            $detail = ($matches | ForEach-Object { $_.FullName } ) -join '; '
            throw "ambiguous dependency '$name': $($byHash.Keys.Count) distinct copies found ($detail). Remove the conflicting copy."
        }

        $chosen = $matches[0]
        $resolved[$key] = $chosen.FullName
        $pe = Get-PeInfo -Path $chosen.FullName
        if ($pe.Machine -ne 0x8664) {
            throw ("dependency '{0}' resolved to '{1}' is not x64 (PE machine 0x{2:X4}); expected 0x8664" -f $name, $chosen.FullName, $pe.Machine)
        }
        foreach ($imp in $pe.Imports) { $queue.Enqueue($imp) }
    }
    return $resolved
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
if (-not (Test-Path -LiteralPath $MetadataPath -PathType Leaf)) {
    Fail "metadata file not found: $MetadataPath"
}
$metadata = Get-Content -LiteralPath $MetadataPath -Raw | ConvertFrom-Json

if (-not (Test-Path -LiteralPath $PayloadDir -PathType Container)) {
    Fail "payload directory not found: $PayloadDir"
}
$PayloadDir = (Resolve-Path -LiteralPath $PayloadDir).Path

$entrypointName = [string]$metadata.entrypoint
$entrypointPath = Join-Path $PayloadDir $entrypointName
if (-not (Test-Path -LiteralPath $entrypointPath -PathType Leaf)) {
    Fail "payload is missing the entrypoint '$entrypointName' in $PayloadDir"
}

$entryPe = Get-PeInfo -Path $entrypointPath
if ($entryPe.Machine -ne 0x8664) {
    Fail ("payload '$entrypointName' is not x64 (PE machine 0x{0:X4}); expected 0x8664" -f $entryPe.Machine)
}

if ([string]::IsNullOrWhiteSpace($Version)) { $Version = [string]$metadata.version }
if ([string]::IsNullOrWhiteSpace($Version)) { Fail "no package version supplied and none in metadata" }

$vcPrefixes = @($metadata.vcRuntimePrefixes)
$systemPrefixes = @($metadata.systemDependencyPrefixes)

if ([string]::IsNullOrWhiteSpace($RuntimeDir)) {
    $RuntimeDir = Find-VcRuntimeDir -PreferredLinkerVersion $entryPe.LinkerVersion
    if (-not [string]::IsNullOrWhiteSpace($RuntimeDir)) {
        Info "auto-discovered MSVC runtime directory: $RuntimeDir"
    }
}

$searchDirs = @($PayloadDir)
if (-not [string]::IsNullOrWhiteSpace($RuntimeDir)) {
    if (-not (Test-Path -LiteralPath $RuntimeDir -PathType Container)) {
        Fail "runtime directory not found: $RuntimeDir"
    }
    $searchDirs += (Resolve-Path -LiteralPath $RuntimeDir).Path
}
foreach ($d in $ExtraSearchDirs) {
    if (-not [string]::IsNullOrWhiteSpace($d)) { $searchDirs += $d }
}

$seeds = @($entrypointName)
foreach ($dll in @($metadata.requiredRuntimeDlls)) { $seeds += [string]$dll }

Info "resolving dependency closure from: $($searchDirs -join '; ')"
try {
    $closure = Resolve-DependencyClosure -Seeds $seeds -SearchDirs $searchDirs -VcPrefixes $vcPrefixes -SystemPrefixes $systemPrefixes
}
catch {
    Fail $_.Exception.Message
}

foreach ($dll in @($metadata.requiredRuntimeDlls)) {
    $key = ([string]$dll).ToLowerInvariant()
    if (-not $closure.ContainsKey($key)) {
        Fail "declared required runtime DLL '$dll' was not resolved"
    }
}

$extensions = @(Get-SupportedExtensions -ExePath $entrypointPath -KnownExtensions @($metadata.knownImageExtensions))
if ($extensions.Count -eq 0) {
    Fail "could not derive any supported image extension from '$entrypointName' (build too old or strings changed)"
}

$stageName = "$($metadata.packageName)-$Version-$($metadata.platform)-portable"
if (-not (Test-Path -LiteralPath $OutDir -PathType Container)) {
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
}
$OutDir = (Resolve-Path -LiteralPath $OutDir).Path
$stageDir = Join-Path $OutDir $stageName
if (Test-Path -LiteralPath $stageDir) {
    Fail "output stage directory already exists (refusing destructive overwrite): $stageDir"
}
New-Item -ItemType Directory -Path $stageDir -Force | Out-Null

$staged = @()
foreach ($key in ($closure.Keys | Sort-Object)) {
    $src = $closure[$key]
    $dest = Join-Path $stageDir ([System.IO.Path]::GetFileName($src))
    Copy-Item -LiteralPath $src -Destination $dest -Force
    $destItem = Get-Item -LiteralPath $dest
    $staged += $destItem

    # Enforce AMD64 machine (0x8664) for every staged PE executable / DLL
    $destExt = [System.IO.Path]::GetExtension($destItem.Name).ToLowerInvariant()
    if ($destExt -eq '.dll' -or $destExt -eq '.exe') {
        $stagedPe = Get-PeInfo -Path $destItem.FullName
        if ($stagedPe.Machine -ne 0x8664) {
            Fail ("staged PE '{0}' is not x64 (PE machine 0x{1:X4}); expected 0x8664" -f $destItem.Name, $stagedPe.Machine)
        }
    }
}

foreach ($license in @($metadata.licenseFiles)) {
    $licenseName = [string]$license
    $src = ""
    if ($licenseName -eq 'BUILD-PROVENANCE.json') {
        if (-not [string]::IsNullOrWhiteSpace($ProvenancePath)) {
            $src = $ProvenancePath
        }
        else {
            $payloadProv = Join-Path $PayloadDir 'BUILD-PROVENANCE.json'
            if (Test-Path -LiteralPath $payloadProv -PathType Leaf) {
                $src = $payloadProv
            }
        }
        if ([string]::IsNullOrWhiteSpace($src) -or -not (Test-Path -LiteralPath $src -PathType Leaf)) {
            Fail "BUILD-PROVENANCE.json is required for package staging. Provide it via -ProvenancePath or place it in the payload directory. Do not rely on root fallback."
        }
        $src = (Resolve-Path -LiteralPath $src).Path
        try {
            $provData = Get-Content -LiteralPath $src -Raw | ConvertFrom-Json
            if ([string]$provData.product.version -ne $Version) {
                Fail "BUILD-PROVENANCE.json version '$($provData.product.version)' does not match package version '$Version'"
            }
            if ([string]$provData.product.tag -ne "v$Version") {
                Fail "BUILD-PROVENANCE.json tag '$($provData.product.tag)' does not match expected 'v$Version'"
            }
            if ([string]$provData.product.packageName -ne [string]$metadata.packageName) {
                Fail "BUILD-PROVENANCE.json packageName '$($provData.product.packageName)' does not match metadata packageName '$($metadata.packageName)'"
            }
            if ($provData.PSObject.Properties['buildConfiguration']) {
                if ($provData.buildConfiguration.diagnostics -eq $true -or [string]$provData.buildConfiguration.diagnostics -eq "ON" -or [string]$provData.buildConfiguration.diagnostics -ne "OFF") {
                    Fail "BUILD-PROVENANCE.json specifies diagnostics '$([string]$provData.buildConfiguration.diagnostics)'; release packages require diagnostics OFF"
                }
            } else {
                Fail "BUILD-PROVENANCE.json is missing buildConfiguration"
            }
            if ($provData.PSObject.Properties['source']) {
                if ([string]$provData.source.cleanStatus -ne "clean" -or $provData.source.isClean -ne $true) {
                    Fail "BUILD-PROVENANCE.json declares unclean source tree; release packages strictly require a clean commit"
                }
                if ([string]::IsNullOrWhiteSpace($provData.source.commitSha) -or $provData.source.commitSha -notmatch '^[0-9a-f]{40}$') {
                    Fail "BUILD-PROVENANCE.json lacks valid 40-character source commitSha"
                }
                if ([string]::IsNullOrWhiteSpace($provData.source.treeSha) -or $provData.source.treeSha -notmatch '^[0-9a-f]{40}$') {
                    Fail "BUILD-PROVENANCE.json lacks valid 40-character source treeSha"
                }
                # Independently require a readable Git worktree and clean HEAD/tree matching provenance
                if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
                    Fail "git command is required on PATH to verify source provenance."
                }
                $insideWt = (& git -C $RepoRoot rev-parse --is-inside-work-tree 2>&1).Trim()
                if ($LASTEXITCODE -ne 0 -or $insideWt -ne 'true') {
                    Fail "Repository root '$RepoRoot' is not a valid git worktree; cannot verify source provenance."
                }
                $gitSt = & git -C $RepoRoot status --porcelain 2>&1
                if ($LASTEXITCODE -ne 0) { Fail "git status failed in RepoRoot" }
                $dirtyLines = @($gitSt | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
                if ($dirtyLines.Count -gt 0) {
                    Fail "Repository working tree '$RepoRoot' is not clean ($($dirtyLines.Count) uncommitted/untracked entries); refusing release packaging."
                }
                $actualHead = (& git -C $RepoRoot rev-parse HEAD 2>&1).Trim()
                if ($LASTEXITCODE -ne 0 -or $actualHead -notmatch '^[0-9a-f]{40}$') { Fail "git rev-parse HEAD failed in RepoRoot" }
                $actualTree = (& git -C $RepoRoot rev-parse "HEAD^{tree}" 2>&1).Trim()
                if ($LASTEXITCODE -ne 0 -or $actualTree -notmatch '^[0-9a-f]{40}$') { Fail "git rev-parse HEAD^{tree} failed in RepoRoot" }
                if ([string]$provData.source.commitSha -ne $actualHead) {
                    Fail "BUILD-PROVENANCE.json commitSha '$([string]$provData.source.commitSha)' does not match RepoRoot HEAD '$actualHead'"
                }
                if ([string]$provData.source.treeSha -ne $actualTree) {
                    Fail "BUILD-PROVENANCE.json treeSha '$([string]$provData.source.treeSha)' does not match RepoRoot HEAD^{tree} '$actualTree'"
                }
            } else {
                Fail "BUILD-PROVENANCE.json is missing source evidence section"
            }

            # Authenticate candidate binary observations in provenance against actual staged runtime closure
            if ($provData.PSObject.Properties['stagedCandidateBinaries']) {
                $stagedPeFiles = @{}
                foreach ($sf in $staged) {
                    $sfExt = [System.IO.Path]::GetExtension($sf.Name).ToLowerInvariant()
                    if ($sfExt -eq '.exe' -or $sfExt -eq '.dll') {
                        $stagedPeFiles[$sf.Name.ToLowerInvariant()] = $sf
                    }
                }
                $seenCandNames = @{}
                foreach ($cb in @($provData.stagedCandidateBinaries)) {
                    $cbName = [string]$cb.name
                    if ([string]::IsNullOrWhiteSpace($cbName) -or [System.IO.Path]::GetFileName($cbName) -ne $cbName) {
                        Fail "BUILD-PROVENANCE.json contains unsafe candidate binary name '$cbName'"
                    }
                    $cbKey = $cbName.ToLowerInvariant()
                    if ($seenCandNames.ContainsKey($cbKey)) {
                        Fail "BUILD-PROVENANCE.json contains duplicate candidate binary '$cbName'"
                    }
                    $seenCandNames[$cbKey] = $true
                    if (-not $stagedPeFiles.ContainsKey($cbKey)) {
                        Fail "BUILD-PROVENANCE.json references candidate binary '$cbName' not in staged runtime closure"
                    }
                    $sfItem = $stagedPeFiles[$cbKey]
                    $cbHash = (Get-FileHash -LiteralPath $sfItem.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
                    if ($cbHash -ne [string]$cb.sha256) {
                        Fail "BUILD-PROVENANCE.json hash for '$cbName' ($([string]$cb.sha256)) does not match actual staged hash ($cbHash)"
                    }
                    if ([long]$sfItem.Length -ne [long]$cb.bytes) {
                        Fail "BUILD-PROVENANCE.json byte size for '$cbName' ($([long]$cb.bytes)) does not match actual staged size ($($sfItem.Length))"
                    }
                    if ([string]$cb.machine -ne '0x8664') {
                        Fail "BUILD-PROVENANCE.json machine for '$cbName' is not 0x8664 ($([string]$cb.machine))"
                    }
                    if ([string]$cb.signatureStatus -ne 'unsigned' -and [string]$cb.signatureStatus -ne 'valid-signed') {
                        Fail "BUILD-PROVENANCE.json signatureStatus for '$cbName' is untrusted/invalid: $([string]$cb.signatureStatus)"
                    }
                }
                foreach ($sfKey in $stagedPeFiles.Keys) {
                    if (-not $seenCandNames.ContainsKey($sfKey)) {
                        Fail "Staged runtime binary '$($stagedPeFiles[$sfKey].Name)' is missing from BUILD-PROVENANCE.json stagedCandidateBinaries"
                    }
                }
            } else {
                Fail "BUILD-PROVENANCE.json is missing stagedCandidateBinaries"
            }

            # Authenticate LGPL corresponding source asset evidence
            if ($provData.PSObject.Properties['lgplCorrespondingSourceAsset'] -and $provData.lgplCorrespondingSourceAsset) {
                $lgplAsset = $provData.lgplCorrespondingSourceAsset
                $expectedLgplName = "pikaview-$Version-lgpl-corresponding-source.zip"
                if ([string]$lgplAsset.assetName -ne $expectedLgplName) {
                    Fail "BUILD-PROVENANCE.json lgplCorrespondingSourceAsset name '$([string]$lgplAsset.assetName)' does not match expected '$expectedLgplName'"
                }
                if ([string]::IsNullOrWhiteSpace($lgplAsset.sha256) -or $lgplAsset.sha256 -notmatch '^[0-9a-f]{64}$') {
                    Fail "BUILD-PROVENANCE.json lgplCorrespondingSourceAsset lacks valid SHA-256 hash"
                }
            } else {
                Fail "BUILD-PROVENANCE.json is missing lgplCorrespondingSourceAsset"
            }
        }
        catch {
            Fail "Failed to validate BUILD-PROVENANCE.json at '$src': $($_.Exception.Message)"
        }
    }
    else {
        $src = Join-Path $RepoRoot $licenseName
    }

    if (-not (Test-Path -LiteralPath $src -PathType Leaf)) {
        Fail "declared license/attribution file not found: $src"
    }
    $dest = Join-Path $stageDir ([System.IO.Path]::GetFileName($src))
    Copy-Item -LiteralPath $src -Destination $dest -Force
    $staged += (Get-Item -LiteralPath $dest)
}

# --- RUNNING.txt -----------------------------------------------------------
$dllNames = @($closure.Values | ForEach-Object { [System.IO.Path]::GetFileName($_) } | Where-Object { $_ -ne $entrypointName } | Sort-Object)
$extLine = ($extensions | ForEach-Object { '.' + $_ }) -join ' '
$running = New-Object System.Text.StringBuilder
[void]$running.AppendLine("PikaView $Version - portable $($metadata.platform) build")
[void]$running.AppendLine("")
[void]$running.AppendLine("Quick start")
foreach ($line in @($metadata.runInstructions)) { [void]$running.AppendLine("  $line") }
[void]$running.AppendLine("")
[void]$running.AppendLine("Supported image formats (this build)")
[void]$running.AppendLine("  $extLine")
[void]$running.AppendLine("  Extensions are discovery hints only; the decoder is always chosen from the file signature.")
[void]$running.AppendLine("")
[void]$running.AppendLine("Bundled runtime files")
[void]$running.AppendLine("  $entrypointName")
foreach ($d in $dllNames) { [void]$running.AppendLine("  $d") }
[void]$running.AppendLine("")
[void]$running.AppendLine("Third-party notices")
[void]$running.AppendLine("  See THIRD-PARTY-NOTICES.md in this folder.")
$runningPath = Join-Path $stageDir 'RUNNING.txt'
[System.IO.File]::WriteAllText($runningPath, $running.ToString(), (New-Object System.Text.UTF8Encoding($false)))
$staged += (Get-Item -LiteralPath $runningPath)

# --- package-manifest.json (evidence) --------------------------------------
$fileEntries = @()
foreach ($f in ($staged | Sort-Object Name)) {
    $fileEntries += [ordered]@{
        name   = $f.Name
        bytes  = $f.Length
        sha256 = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}
$manifest = [ordered]@{
    schema             = 1
    packageName        = [string]$metadata.packageName
    version            = $Version
    platform           = [string]$metadata.platform
    entrypoint         = $entrypointName
    supportedExtensions = @($extensions | ForEach-Object { '.' + $_ })
    dynamicDependencies = @($metadata.requiredRuntimeDlls)
    files              = $fileEntries
}
$manifestPath = Join-Path $stageDir 'package-manifest.json'
[System.IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 6), (New-Object System.Text.UTF8Encoding($false)))
$staged += (Get-Item -LiteralPath $manifestPath)

# --- exclusion assertions --------------------------------------------------
$excludedExt = @($metadata.excludedExtensions | ForEach-Object { ([string]$_).ToLowerInvariant() })
$excludedFrag = @($metadata.excludedNameFragments | ForEach-Object { ([string]$_).ToLowerInvariant() })
foreach ($f in $staged) {
    $ext = [System.IO.Path]::GetExtension($f.Name).ToLowerInvariant()
    if ($excludedExt -contains $ext) {
        Fail "excluded build/debug artifact staged: $($f.Name)"
    }
    $lower = $f.Name.ToLowerInvariant()
    foreach ($frag in $excludedFrag) {
        if ($lower.Contains($frag)) {
            Fail "staged file '$($f.Name)' matches forbidden name fragment '$frag'"
        }
    }
}

Info "staged $($staged.Count) files in $stageDir"
Info "supported extensions: $extLine"

# --- deterministic ZIP -----------------------------------------------------
if (-not $NoZip) {
    $zipName = "$stageName.zip"
    $zipPath = Join-Path $OutDir $zipName
    if (Test-Path -LiteralPath $zipPath) {
        Fail "output ZIP already exists (refusing destructive overwrite): $zipPath"
    }

    Add-Type -AssemblyName System.IO.Compression | Out-Null
    Add-Type -AssemblyName System.IO.Compression.FileSystem | Out-Null
    $stamp = [DateTimeOffset]::new([DateTime]::new(2000, 1, 1, 0, 0, 0, [DateTimeKind]::Utc))
    $archive = [System.IO.Compression.ZipFile]::Open($zipPath, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($f in ($staged | Sort-Object Name)) {
            $entry = $archive.CreateEntry($f.Name, [System.IO.Compression.CompressionLevel]::Optimal)
            $entry.LastWriteTime = $stamp
            $outStream = $entry.Open()
            $inStream = [System.IO.File]::Open($f.FullName, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read)
            try { $inStream.CopyTo($outStream) } finally { $inStream.Dispose(); $outStream.Dispose() }
        }
    }
    finally { $archive.Dispose() }

    $zipHash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $shaPath = Join-Path $OutDir ($zipName + '.sha256')
    [System.IO.File]::WriteAllText($shaPath, "$zipHash *$zipName`n", (New-Object System.Text.UTF8Encoding($false)))

    Info "ZIP: $zipPath"
    Info "SHA256: $zipHash"
    Write-Output "PACKAGE_ZIP=$zipPath"
    Write-Output "PACKAGE_SHA256=$zipHash"
    Write-Output "PACKAGE_STAGE=$stageDir"
}
else {
    Write-Output "PACKAGE_STAGE=$stageDir"
}
exit 0

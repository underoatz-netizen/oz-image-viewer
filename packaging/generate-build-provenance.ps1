<#
.SYNOPSIS
  Generate the acyclic BUILD-PROVENANCE.json release envelope contract.

.DESCRIPTION
  Inspects the verified source state, build toolchain, effective vcpkg dependency
  build, staged candidate runtime binaries, and LGPL corresponding source asset
  to produce an authentic, acyclic BUILD-PROVENANCE.json. Fails closed if:
    - -OutPath is not provided or points inside the repository root
    - -StagedBinariesDir is missing, not found, or lacks required binaries
    - -LgplSourceZipPath is missing or not found
    - -BuildDir is missing, lacks CMakeCache.txt, points to a mismatched source dir,
      or lacks required OFF/Release/x64 settings
    - the source tree is dirty or untracked files are present (fail-closed, no bypass)
    - CMakeCache.txt does not identify the effective vcpkg toolchain, manifest mode,
      installed tree, target triplet, or overlay triplet path
    - the effective vcpkg target triplet or overlay/registry recipes do not match
      packaging/lgpl-dependency-policy.json
    - the installed libheif/libde265 outputs or staged heif.dll/libde265.dll do not
      match the effective installed package metadata and bytes
    - the LGPL source ZIP does not contain the exact effective recipes, triplet, and
      pinned upstream archives
    - binaries have wrong PE architecture, invalid PE headers, or unresolvable signature status
    - versions or expected tag disagree with packaging metadata
    - build configuration is not Release or diagnostics are not OFF
    - required toolchain tools cannot be observed

  The trust anchor is a separately controlled clean configure/build. The observed
  CMakeCache, vcpkg registry checkout/versions database, installed package status,
  and file bytes are consistency evidence, not cryptographic proof of compilation,
  custody, or bit-for-bit reproducibility.

.PARAMETER OutPath
  Mandatory destination path for BUILD-PROVENANCE.json. Must be outside the repository root.

.PARAMETER StagedBinariesDir
  Mandatory directory containing staged candidate binaries (ozviewer.exe, heif.dll, libde265.dll, and app-local CRTs).

.PARAMETER LgplSourceZipPath
  Mandatory path to the generated pikaview-<version>-lgpl-corresponding-source.zip asset.

.PARAMETER BuildDir
  Mandatory build directory to observe compiler/CMake configuration and CMakeCache.txt.

.PARAMETER ExpectedTag
  Expected git tag (e.g. "v0.1.1"). Defaults to "v" + metadata version.

.PARAMETER VcpkgRoot
  Optional assertion: when supplied, it must resolve to the same vcpkg checkout
  recorded by the effective CMakeCache toolchain. It is never used as an
  alternative root for recipe or version evidence.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$OutPath,
    [Parameter(Mandatory = $true)][string]$StagedBinariesDir,
    [Parameter(Mandatory = $true)][string]$LgplSourceZipPath,
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [string]$BuildEvidencePath = "",
    [string]$ExpectedTag = "",
    [string]$VcpkgRoot = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = (Resolve-Path -LiteralPath (Split-Path -Parent $ScriptDir)).Path

function Fail([string]$Message) {
    Write-Host "generate-build-provenance: ERROR: $Message" -ForegroundColor Red
    exit 1
}

function Info([string]$Message) {
    Write-Host "generate-build-provenance: $Message"
}

function Get-Sha256File([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-Sha512File([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA512).Hash.ToLowerInvariant()
}

function Get-ZipEntryHash($Entry, [string]$Algorithm) {
    $stream = $Entry.Open()
    try {
        if ($Algorithm -eq 'SHA512') {
            $sha = [System.Security.Cryptography.SHA512]::Create()
        } else {
            $sha = [System.Security.Cryptography.SHA256]::Create()
        }
        try {
            $hashBytes = $sha.ComputeHash($stream)
            return [BitConverter]::ToString($hashBytes).Replace('-', '').ToLowerInvariant()
        } finally {
            $sha.Dispose()
        }
    } finally {
        $stream.Dispose()
    }
}

function Read-VcpkgStatus([string]$Path) {
    $blocks = @()
    $current = @{}
    foreach ($line in (Get-Content -LiteralPath $Path)) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            if ($current.Count -gt 0) {
                $blocks += ,$current
                $current = @{}
            }
            continue
        }
        if ($line -match '^([A-Za-z0-9-]+):\s*(.*)$') {
            $key = $Matches[1].ToLowerInvariant()
            if (-not $current.ContainsKey($key)) {
                $current[$key] = $Matches[2].Trim()
            }
        }
    }
    if ($current.Count -gt 0) { $blocks += ,$current }
    return $blocks
}

function Get-JsonProperty($Object, [string]$Name) {
    if ($null -eq $Object) { return $null }
    $prop = $Object.PSObject.Properties[$Name]
    if ($null -eq $prop) { return $null }
    return $prop.Value
}

function Normalize-PathText([string]$PathText) {
    if ([string]::IsNullOrWhiteSpace($PathText)) { return "" }
    try {
        return [System.IO.Path]::GetFullPath($PathText.Replace('/', '\')).TrimEnd([char[]]@('\', '/'))
    } catch {
        Fail "Invalid absolute path '$PathText': $($_.Exception.Message)"
    }
}

function Get-GitText([string]$GitRoot, [string[]]$GitArguments, [string]$Context) {
    $previousPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $output = @(& git -C $GitRoot @GitArguments 2>&1)
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousPreference
    }
    if ($code -ne 0) {
        Fail "$Context failed (exit $code): $($output -join ' / ')"
    }
    return (($output | ForEach-Object { $_.ToString() }) -join "`n")
}

function Get-RecipeFileMap([string]$RecipeDir) {
    $map = @{}
    foreach ($file in (Get-ChildItem -LiteralPath $RecipeDir -File -Recurse -Force | Sort-Object FullName)) {
        $relative = $file.FullName.Substring($RecipeDir.Length).TrimStart([char[]]@('\', '/')).Replace('\', '/')
        $map[$relative] = $file.FullName
    }
    return $map
}

function Get-CacheValue([string]$Key) {
    if ($cacheMap.ContainsKey($Key)) { return [string]$cacheMap[$Key] }
    return ""
}

# Controlled dependency expectations come from a committed policy file, never
# from BUILD-PROVENANCE.json or from a caller-selected vcpkg checkout.
$PolicyPath = Join-Path $ScriptDir 'lgpl-dependency-policy.json'
if (-not (Test-Path -LiteralPath $PolicyPath -PathType Leaf)) {
    Fail "Dependency policy not found: $PolicyPath"
}
try {
    $dependencyPolicy = Get-Content -LiteralPath $PolicyPath -Raw | ConvertFrom-Json
} catch {
    Fail "Dependency policy is not valid JSON: $($_.Exception.Message)"
}
if (-not ($dependencyPolicy.PSObject.Properties['schema']) -or [int]$dependencyPolicy.schema -ne 1) {
    Fail "Dependency policy schema must be 1."
}
$ExpectedTargetTriplet = "oz-x64-windows-static-md"
$policyTriplet = [string](Get-JsonProperty $dependencyPolicy 'targetTriplet')
if ($policyTriplet -ne $ExpectedTargetTriplet) {
    Fail "Dependency policy targetTriplet '$policyTriplet' must be '$ExpectedTargetTriplet'."
}
$policyTripletFile = [string](Get-JsonProperty $dependencyPolicy 'tripletFile')
if ([string]::IsNullOrWhiteSpace($policyTripletFile) -or
    [System.IO.Path]::IsPathRooted($policyTripletFile) -or
    $policyTripletFile -match '(^|[\\/])\.\.([\\/]|$)') {
    Fail "Dependency policy tripletFile must be a safe repository-relative path."
}
$policyDeps = @($dependencyPolicy.dependencies)
if ($policyDeps.Count -ne 2) {
    Fail "Dependency policy must contain exactly the two release LGPL dependencies (found $($policyDeps.Count))."
}
$policyByPort = @{}
foreach ($policyDep in $policyDeps) {
    $port = [string](Get-JsonProperty $policyDep 'port')
    if ([string]::IsNullOrWhiteSpace($port) -or $port -notmatch '^[A-Za-z0-9_-]+$') {
        Fail "Dependency policy contains an invalid port name '$port'."
    }
    if ($policyByPort.ContainsKey($port)) {
        Fail "Dependency policy contains duplicate port '$port'."
    }
    $version = [string](Get-JsonProperty $policyDep 'version')
    $portVersionValue = Get-JsonProperty $policyDep 'portVersion'
    $stagedDll = [string](Get-JsonProperty $policyDep 'stagedDll')
    $installedRelative = [string](Get-JsonProperty $policyDep 'installedDllRelativePath')
    $archiveName = [string](Get-JsonProperty $policyDep 'archiveName')
    $archiveSha256 = [string](Get-JsonProperty $policyDep 'archiveSha256')
    $archiveSha512 = [string](Get-JsonProperty $policyDep 'archiveSha512')
    $upstreamRepository = [string](Get-JsonProperty $policyDep 'upstreamRepository')
    if ([string]::IsNullOrWhiteSpace($version)) { Fail "Dependency policy port '$port' lacks a version." }
    if ($null -eq $portVersionValue -or [int]$portVersionValue -lt 0) { Fail "Dependency policy port '$port' lacks a nonnegative portVersion." }
    if ([string]::IsNullOrWhiteSpace($stagedDll) -or [System.IO.Path]::GetFileName($stagedDll) -ne $stagedDll) {
        Fail "Dependency policy port '$port' stagedDll must be a bare filename."
    }
    if ([string]::IsNullOrWhiteSpace($installedRelative) -or
        [System.IO.Path]::IsPathRooted($installedRelative) -or
        $installedRelative -match '(^|[\\/])\.\.([\\/]|$)') {
        Fail "Dependency policy port '$port' installedDllRelativePath must be a safe relative path."
    }
    if ([string]::IsNullOrWhiteSpace($archiveName) -or [System.IO.Path]::GetFileName($archiveName) -ne $archiveName) {
        Fail "Dependency policy port '$port' archiveName must be a bare filename."
    }
    if ($archiveSha256 -notmatch '^[0-9a-f]{64}$') { Fail "Dependency policy port '$port' archiveSha256 is invalid." }
    if ($archiveSha512 -notmatch '^[0-9a-f]{128}$') { Fail "Dependency policy port '$port' archiveSha512 is invalid." }
    if ([string]::IsNullOrWhiteSpace($upstreamRepository)) { Fail "Dependency policy port '$port' lacks upstreamRepository." }
    $policyByPort[$port] = [ordered]@{
        port                     = $port
        version                  = $version
        portVersion              = [int]$portVersionValue
        stagedDll                = $stagedDll
        installedDllRelativePath = $installedRelative
        archiveName              = $archiveName
        archiveSha256            = $archiveSha256
        archiveSha512            = $archiveSha512
        upstreamRepository       = $upstreamRepository
    }
}
foreach ($requiredPort in @('libheif', 'libde265')) {
    if (-not $policyByPort.ContainsKey($requiredPort)) {
        Fail "Dependency policy is missing required port '$requiredPort'."
    }
}
$policySha256 = Get-Sha256File $PolicyPath

# 1. Validate OutPath: Must be provided and strictly OUTSIDE $RepoRoot
if ([string]::IsNullOrWhiteSpace($OutPath)) {
    Fail "-OutPath is mandatory."
}
$fullOutPath = [System.IO.Path]::GetFullPath($OutPath)
if ($fullOutPath.StartsWith($RepoRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
    Fail "OutPath '$fullOutPath' resides inside repository root '$RepoRoot'. Generated provenance must stay outside source to prevent self-dirtying acyclic contract violations."
}

# 2. Version resolution and consistency
$metaPath = Join-Path $ScriptDir 'portable-package.json'
if (-not (Test-Path -LiteralPath $metaPath -PathType Leaf)) {
    Fail "packaging metadata not found: $metaPath"
}
$meta = Get-Content -LiteralPath $metaPath -Raw | ConvertFrom-Json
$Version = [string]$meta.version
$PackageName = [string]$meta.packageName
if ([string]::IsNullOrWhiteSpace($Version)) { Fail "version missing in portable-package.json" }
if ($PackageName -ne "pikaview") {
    Fail "packageName in portable-package.json must be 'pikaview', found '$PackageName'"
}

if ([string]::IsNullOrWhiteSpace($ExpectedTag)) {
    $ExpectedTag = "v$Version"
}
if ($ExpectedTag -ne "v$Version") {
    Fail "Expected tag '$ExpectedTag' does not match version 'v$Version'"
}

# 3. Source git state (Strict fail-closed: no AllowDirty bypass)
if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Fail "git command is required on PATH to verify source provenance."
}
$gitStatus = & git -C $RepoRoot status --porcelain 2>&1
if ($LASTEXITCODE -ne 0) {
    Fail "git status failed: $($gitStatus -join "`n")"
}
$statusLines = @($gitStatus | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
if ($statusLines.Count -gt 0) {
    Fail ("Source working tree is not clean ($($statusLines.Count) modified/untracked files). Release provenance strictly requires a clean committed tree:`n" + ($statusLines -join "`n"))
}

$headCommit = (& git -C $RepoRoot rev-parse HEAD 2>&1).Trim()
$treeSha = (& git -C $RepoRoot rev-parse "HEAD^{tree}" 2>&1).Trim()
if ($headCommit -notmatch '^[0-9a-f]{40}$') {
    Fail "Unable to obtain valid 40-character HEAD commit SHA: $headCommit"
}
if ($treeSha -notmatch '^[0-9a-f]{40}$') {
    Fail "Unable to obtain valid 40-character tree SHA: $treeSha"
}

# 4. Mandatory BuildDir verification & CMakeCache inspection (Item 1)
if ([string]::IsNullOrWhiteSpace($BuildDir) -or -not (Test-Path -LiteralPath $BuildDir -PathType Container)) {
    Fail "-BuildDir is mandatory and must point to an existing build directory."
}
$buildDirResolved = (Resolve-Path -LiteralPath $BuildDir).Path
$cacheFile = Join-Path $buildDirResolved "CMakeCache.txt"
if (-not (Test-Path -LiteralPath $cacheFile -PathType Leaf)) {
    Fail "BuildDir '$buildDirResolved' is missing CMakeCache.txt. Actual build evidence is required."
}

$cacheLines = Get-Content -LiteralPath $cacheFile
$cacheMap = @{}
foreach ($line in $cacheLines) {
    if ($line -match '^([^#:\s]+)(?::([^=]+))?=(.*)$') {
        $key = $Matches[1].Trim()
        $val = $Matches[3].Trim()
        $cacheMap[$key] = $val
    }
}

# Verify CMake home directory matches RepoRoot
$cachedSourceDir = ""
foreach ($srcKey in @('CMAKE_HOME_DIRECTORY', 'ozbench_SOURCE_DIR')) {
    if ($cacheMap.ContainsKey($srcKey)) {
        $cachedSourceDir = $cacheMap[$srcKey]
        break
    }
}
if ([string]::IsNullOrWhiteSpace($cachedSourceDir)) {
    Fail "CMakeCache.txt lacks CMAKE_HOME_DIRECTORY / ozbench_SOURCE_DIR."
}
$normCachedSrc = [System.IO.Path]::GetFullPath($cachedSourceDir.Replace('/', '\')).TrimEnd('\')
$normRepoRoot = [System.IO.Path]::GetFullPath($RepoRoot).TrimEnd('\')
if ([string]::Compare($normCachedSrc, $normRepoRoot, $true) -ne 0) {
    Fail "CMakeCache source dir mismatch (cached '$normCachedSrc' vs repo '$normRepoRoot')."
}

# Require explicit OZVIEWER_PRINT_DIAGNOSTICS=OFF
if (-not $cacheMap.ContainsKey('OZVIEWER_PRINT_DIAGNOSTICS')) {
    Fail "CMakeCache.txt lacks required entry OZVIEWER_PRINT_DIAGNOSTICS. Explicit build evidence is required."
}
$diagVal = $cacheMap['OZVIEWER_PRINT_DIAGNOSTICS'].Trim().ToUpperInvariant()
if ($diagVal -ne 'OFF' -and $diagVal -ne '0' -and $diagVal -ne 'FALSE') {
    Fail "CMakeCache.txt specifies OZVIEWER_PRINT_DIAGNOSTICS=$diagVal. Release binaries require diagnostics explicitly OFF."
}
$observedDiagnostics = "OFF"

# Verify target architecture x64
$observedArch = ""
if ($cacheMap.ContainsKey('CMAKE_GENERATOR_PLATFORM') -and -not [string]::IsNullOrWhiteSpace($cacheMap['CMAKE_GENERATOR_PLATFORM'])) {
    $observedArch = $cacheMap['CMAKE_GENERATOR_PLATFORM']
} elseif ($cacheMap.ContainsKey('CMAKE_CXX_COMPILER_ARCHITECTURE_ID')) {
    $observedArch = $cacheMap['CMAKE_CXX_COMPILER_ARCHITECTURE_ID']
} elseif ($cacheMap.ContainsKey('CMAKE_EXE_LINKER_FLAGS') -and $cacheMap['CMAKE_EXE_LINKER_FLAGS'] -match '/machine:x64') {
    $observedArch = "x64"
}
if ($observedArch -ne "x64") {
    Fail "Build configuration architecture '$observedArch' does not match required 'x64'."
}

# Verify Release build configuration (single-config or multi-config with Release evidence)
$observedConfiguration = ""
$isMultiConfig = $false
$releaseExeFound = $null
if ($cacheMap.ContainsKey('CMAKE_BUILD_TYPE') -and -not [string]::IsNullOrWhiteSpace($cacheMap['CMAKE_BUILD_TYPE'])) {
    $bt = $cacheMap['CMAKE_BUILD_TYPE'].Trim()
    if ($bt -ne 'Release') {
        Fail "CMakeCache.txt CMAKE_BUILD_TYPE is '$bt'; release provenance requires 'Release'."
    }
    $singleExeCandidates = @(
        (Join-Path $buildDirResolved "src\viewer\ozviewer.exe"),
        (Join-Path $buildDirResolved "ozviewer.exe")
    )
    foreach ($cand in $singleExeCandidates) {
        if (Test-Path -LiteralPath $cand -PathType Leaf) {
            $releaseExeFound = $cand
            break
        }
    }
    if ($null -eq $releaseExeFound) {
        Fail "Single-config Release BuildDir '$buildDirResolved' has no observed ozviewer.exe build output."
    }
    $observedConfiguration = "Release"
} elseif ($cacheMap.ContainsKey('CMAKE_CONFIGURATION_TYPES') -and -not [string]::IsNullOrWhiteSpace($cacheMap['CMAKE_CONFIGURATION_TYPES'])) {
    $isMultiConfig = $true
    $cfgTypes = @($cacheMap['CMAKE_CONFIGURATION_TYPES'] -split ';' | ForEach-Object { $_.Trim() })
    if ($cfgTypes -notcontains 'Release') {
        Fail "CMakeCache.txt CMAKE_CONFIGURATION_TYPES ('$($cacheMap['CMAKE_CONFIGURATION_TYPES'])') does not include 'Release'."
    }
    # Multi-config CMakeCache alone does not prove Release was built; require Release build evidence in BuildDir
    $releaseExeCandidates = @(
        (Join-Path $buildDirResolved "src\viewer\Release\ozviewer.exe"),
        (Join-Path $buildDirResolved "Release\ozviewer.exe")
    )
    $releaseExeFound = $null
    foreach ($cand in $releaseExeCandidates) {
        if (Test-Path -LiteralPath $cand -PathType Leaf) {
            $releaseExeFound = $cand
            break
        }
    }
    if ($null -eq $releaseExeFound) {
        Fail "Multi-config BuildDir '$buildDirResolved' has no observed Release ozviewer.exe build output; cannot prove Release configuration was built."
    }
    $observedConfiguration = "Release"
} else {
    Fail "CMakeCache.txt lacks both CMAKE_BUILD_TYPE and CMAKE_CONFIGURATION_TYPES; build configuration is unknown."
}

# 5. Observe toolchain facts dynamically without embedding user home paths (Item 3)
function Sanitize-Path([string]$PathText) {
    if ([string]::IsNullOrWhiteSpace($PathText)) { return "" }
    $userProf = $env:USERPROFILE
    if (-not [string]::IsNullOrWhiteSpace($userProf) -and $PathText.StartsWith($userProf, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $PathText.Substring($userProf.Length).TrimStart('\', '/')
    }
    return $PathText
}

$hostOs = [System.Environment]::OSVersion.VersionString + " (" + $(if ([System.Environment]::Is64BitOperatingSystem) { "x64" } else { "x86" }) + ")"
$compilerObserved = ""
$cmakeObserved = ""

# Observe compiler from CMakeCache, CMakeFiles, or environment
if ($cacheMap.ContainsKey('CMAKE_CXX_COMPILER')) {
    $clPath = $cacheMap['CMAKE_CXX_COMPILER']
    if (Test-Path -LiteralPath $clPath -PathType Leaf) {
        $clVer = (Get-Item -LiteralPath $clPath).VersionInfo.ProductVersion
        $compilerObserved = "MSVC $clVer ($(Sanitize-Path $clPath))"
    }
}
if ([string]::IsNullOrWhiteSpace($compilerObserved)) {
    # Check CMakeFiles/<ver>/CMakeCXXCompiler.cmake in BuildDir
    $cxxFiles = Get-ChildItem -LiteralPath (Join-Path $buildDirResolved "CMakeFiles") -Recurse -Filter "CMakeCXXCompiler.cmake" -ErrorAction SilentlyContinue
    foreach ($cxxFile in $cxxFiles) {
        $cxxContent = Get-Content -LiteralPath $cxxFile.FullName
        $matchLine = $cxxContent | Where-Object { $_ -match 'set\(CMAKE_CXX_COMPILER\s+"([^"]+)"\)' } | Select-Object -First 1
        if ($matchLine -and $matchLine -match 'set\(CMAKE_CXX_COMPILER\s+"([^"]+)"\)') {
            $candClPath = $Matches[1]
            if (Test-Path -LiteralPath $candClPath -PathType Leaf) {
                $clVer = (Get-Item -LiteralPath $candClPath).VersionInfo.ProductVersion
                $compilerObserved = "MSVC $clVer ($(Sanitize-Path $candClPath))"
                break
            }
        }
    }
}
if ([string]::IsNullOrWhiteSpace($compilerObserved)) {
    $clCmd = Get-Command cl.exe -ErrorAction SilentlyContinue
    if ($clCmd) {
        $clVer = (Get-Item -LiteralPath $clCmd.Source).VersionInfo.ProductVersion
        $compilerObserved = "MSVC $clVer ($(Sanitize-Path $clCmd.Source))"
    } else {
        Fail "cl.exe compiler could not be observed from CMakeCache, CMakeFiles, or PATH."
    }
}

# Observe CMake version
if ($cacheMap.ContainsKey('CMAKE_COMMAND')) {
    $cmPath = $cacheMap['CMAKE_COMMAND']
    if (Test-Path -LiteralPath $cmPath -PathType Leaf) {
        $cmVer = (Get-Item -LiteralPath $cmPath).VersionInfo.ProductVersion
        $cmakeObserved = "CMake $cmVer ($(Sanitize-Path $cmPath))"
    }
}
if ([string]::IsNullOrWhiteSpace($cmakeObserved)) {
    $cmakeCmd = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($cmakeCmd) {
        $cmVer = (Get-Item -LiteralPath $cmakeCmd.Source).VersionInfo.ProductVersion
        $cmakeObserved = "CMake $cmVer ($(Sanitize-Path $cmakeCmd.Source))"
    } else {
        Fail "cmake.exe could not be observed from CMakeCache or PATH."
    }
}

# Verify explicit controlled build evidence (binds source commit/tree, Release config, and build command)
if ([string]::IsNullOrWhiteSpace($BuildEvidencePath)) {
    $candEv = Join-Path $buildDirResolved "build-evidence.json"
    if (Test-Path -LiteralPath $candEv -PathType Leaf) {
        $BuildEvidencePath = $candEv
    }
}
if ([string]::IsNullOrWhiteSpace($BuildEvidencePath) -or -not (Test-Path -LiteralPath $BuildEvidencePath -PathType Leaf)) {
    Fail "Explicit controlled build evidence is required (-BuildEvidencePath or build-evidence.json in BuildDir)."
}
$buildEv = Get-Content -LiteralPath $BuildEvidencePath -Raw | ConvertFrom-Json
if (-not ($buildEv.PSObject.Properties['schema']) -or [int]$buildEv.schema -ne 1) {
    Fail "Build evidence schema must be 1."
}
if ([string]$buildEv.wrapper -ne "invoke-release-build.ps1") {
    Fail "Build evidence wrapper '$([string]$buildEv.wrapper)' must be 'invoke-release-build.ps1'."
}
if ([string]$buildEv.target -ne "ozviewer") {
    Fail "Build evidence target '$([string]$buildEv.target)' must be 'ozviewer'."
}
if ([string]::IsNullOrWhiteSpace([string]$buildEv.buildDir) -or [string]::Compare([System.IO.Path]::GetFullPath([string]$buildEv.buildDir).TrimEnd('\'), $buildDirResolved.TrimEnd('\'), $true) -ne 0) {
    Fail "Build evidence buildDir '$([string]$buildEv.buildDir)' does not match '$buildDirResolved'."
}
if ([string]$buildEv.sourceCommitSha -ne $headCommit) {
    Fail "Build evidence sourceCommitSha '$([string]$buildEv.sourceCommitSha)' does not match clean HEAD '$headCommit'."
}
if ([string]$buildEv.sourceTreeSha -ne $treeSha) {
    Fail "Build evidence sourceTreeSha '$([string]$buildEv.sourceTreeSha)' does not match clean tree '$treeSha'."
}
if ([string]$buildEv.configuration -ne "Release") {
    Fail "Build evidence configuration '$([string]$buildEv.configuration)' is not 'Release'."
}
if ([string]$buildEv.diagnostics -ne "OFF") {
    Fail "Build evidence diagnostics '$([string]$buildEv.diagnostics)' is not 'OFF'."
}
if ([string]::IsNullOrWhiteSpace([string]$buildEv.buildCommand)) {
    Fail "Build evidence is missing observed buildCommand."
}
if (-not ($buildEv.PSObject.Properties['exitCode']) -or [int]$buildEv.exitCode -ne 0) {
    Fail "Build evidence does not record a successful exitCode=0 build."
}
if (-not ($buildEv.PSObject.Properties['builtOzviewerSha256']) -or [string]$buildEv.builtOzviewerSha256 -notmatch '^[0-9a-f]{64}$') {
    Fail "Build evidence lacks valid 64-character builtOzviewerSha256."
}
$builtExeShaFromCache = (Get-FileHash -LiteralPath $releaseExeFound -Algorithm SHA256).Hash.ToLowerInvariant()
if ([string]$buildEv.builtOzviewerSha256 -ne $builtExeShaFromCache) {
    Fail "Build evidence builtOzviewerSha256 ($([string]$buildEv.builtOzviewerSha256)) does not match BuildDir Release ozviewer.exe ($builtExeShaFromCache)."
}

# 6. Candidate runtime binaries inspection including CRTs (Item 2)
if (-not (Test-Path -LiteralPath $StagedBinariesDir -PathType Container)) {
    Fail "StagedBinariesDir does not exist or is not a directory: $StagedBinariesDir"
}
$stagedBinDirResolved = (Resolve-Path -LiteralPath $StagedBinariesDir).Path

# Require core executable and DLLs
$coreRequired = @('ozviewer.exe', 'heif.dll', 'libde265.dll')
foreach ($cr in $coreRequired) {
    if (-not (Test-Path -LiteralPath (Join-Path $stagedBinDirResolved $cr) -PathType Leaf)) {
        Fail "Required candidate binary missing from staged directory: $cr"
    }
}

# Bind staged ozviewer.exe to the Release build output in BuildDir
if ($null -ne $releaseExeFound) {
    $builtExeSha = (Get-FileHash -LiteralPath $releaseExeFound -Algorithm SHA256).Hash.ToLowerInvariant()
    $stagedExeSha = (Get-FileHash -LiteralPath (Join-Path $stagedBinDirResolved 'ozviewer.exe') -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($builtExeSha -ne $stagedExeSha) {
        Fail "Staged ozviewer.exe SHA-256 ($stagedExeSha) does not match Release build output '$releaseExeFound' ($builtExeSha)."
    }
}

# Inspect EVERY PE executable / dynamic-library in StagedBinariesDir (including CRT DLLs)
$allBinFiles = Get-ChildItem -LiteralPath $stagedBinDirResolved -File | Where-Object {
    $ext = $_.Extension.ToLowerInvariant()
    $ext -eq '.exe' -or $ext -eq '.dll'
} | Sort-Object Name

if ($allBinFiles.Count -lt 3) {
    Fail "StagedBinariesDir contains fewer than required runtime binaries: $($allBinFiles.Count)"
}

$stagedBinaries = @()
foreach ($item in $allBinFiles) {
    $name = $item.Name
    $binPath = $item.FullName
    $sha = (Get-FileHash -LiteralPath $binPath -Algorithm SHA256).Hash.ToLowerInvariant()

    # Read and inspect PE machine header
    $bytes = [System.IO.File]::ReadAllBytes($binPath)
    if ($bytes.Length -lt 0x40) { Fail "Binary too small to be PE: $name" }
    $peOff = [BitConverter]::ToUInt32($bytes, 0x3C)
    if (($peOff + 24) -ge $bytes.Length) { Fail "Invalid PE header offset in: $name" }
    if ([BitConverter]::ToUInt32($bytes, $peOff) -ne 0x00004550) { Fail "Missing PE signature in: $name" }
    $machine = [BitConverter]::ToUInt16($bytes, $peOff + 4)
    if ($machine -ne 0x8664) {
        Fail ("Binary '{0}' is not x64 (PE machine 0x{1:X4}); expected 0x8664" -f $name, $machine)
    }

    # Inspect signature state via Authenticode (Strict fail-closed: null/error blocks release)
    $sigObj = $null
    try {
        $sigObj = Get-AuthenticodeSignature -FilePath $binPath
    } catch {
        Fail "Authenticode signature query failed for '$name': $($_.Exception.Message)"
    }
    if ($null -eq $sigObj) {
        Fail "Authenticode signature query returned null for '$name'; signature status is unavailable."
    }

    $sigStatus = ""
    switch ($sigObj.Status) {
        ([System.Management.Automation.SignatureStatus]::NotSigned) {
            $sigStatus = "unsigned"
        }
        ([System.Management.Automation.SignatureStatus]::Valid) {
            $sigStatus = "valid-signed"
        }
        Default {
            # Any unverified/error/incompatible status fails closed or records explicit status
            Fail "Binary '$name' has untrusted/invalid signature status '$($sigObj.Status)'. Details: $($sigObj.StatusMessage)"
        }
    }

    $kind = if ($name.EndsWith(".exe", [System.StringComparison]::OrdinalIgnoreCase)) { "pe-executable" } else { "dynamic-library" }
    $stagedBinaries += [ordered]@{
        name            = $name
        bytes           = [long]$item.Length
        sha256          = $sha
        kind            = $kind
        machine         = "0x8664"
        signatureStatus = $sigStatus
    }
}
$stagedHashByLowerName = @{}
$stagedBytesByLowerName = @{}
foreach ($stagedBinary in $stagedBinaries) {
    $stagedHashByLowerName[$stagedBinary.name.ToLowerInvariant()] = [string]$stagedBinary.sha256
    $stagedBytesByLowerName[$stagedBinary.name.ToLowerInvariant()] = [long]$stagedBinary.bytes
}

# 7. LGPL source zip inspection & file content binding (Item 4)
if (-not (Test-Path -LiteralPath $LgplSourceZipPath -PathType Leaf)) {
    Fail "LGPL source ZIP not found: $LgplSourceZipPath"
}
$zipItem = Get-Item -LiteralPath $LgplSourceZipPath
$expectedZipName = "pikaview-$Version-lgpl-corresponding-source.zip"
if ($zipItem.Name -ne $expectedZipName) {
    Fail "LGPL source ZIP file name '$($zipItem.Name)' does not match expected '$expectedZipName'"
}
$zipSha = (Get-FileHash -LiteralPath $LgplSourceZipPath -Algorithm SHA256).Hash.ToLowerInvariant()

# Inspect actual ZIP entries and assert presence of exact required contents
Add-Type -AssemblyName System.IO.Compression.FileSystem | Out-Null
$zipArch = [System.IO.Compression.ZipFile]::OpenRead($zipItem.FullName)
$normalizedEntries = @{}
$expectedPrefix = "pikaview-$Version-lgpl-corresponding-source/"

foreach ($entry in $zipArch.Entries) {
    $rawName = $entry.FullName.Replace('\', '/')
    if ($rawName.EndsWith('/')) { continue }
    if ($rawName.StartsWith('/') -or $rawName -match '^[A-Za-z]:' -or $rawName -match '(^|/)\.\.(/|$)') {
        $zipArch.Dispose()
        Fail "LGPL source ZIP contains unsafe path-traversal or absolute entry: '$rawName'"
    }
    $relName = if ($rawName.StartsWith($expectedPrefix, [System.StringComparison]::Ordinal)) {
        $rawName.Substring($expectedPrefix.Length)
    } else {
        $rawName
    }
    if ($normalizedEntries.ContainsKey($relName)) {
        $zipArch.Dispose()
        Fail "LGPL source ZIP contains duplicate entry: '$relName'"
    }
    $normalizedEntries[$relName] = $entry
}

$requiredPaths = @()
foreach ($policyDep in $policyDeps) {
    $requiredPaths += "upstream-sources/$($policyDep.archiveName)"
    $requiredPaths += "vcpkg-ports/$($policyDep.port)/portfile.cmake"
    $requiredPaths += "vcpkg-ports/$($policyDep.port)/vcpkg.json"
}
$requiredPaths += $policyTripletFile.Replace('\', '/')
$requiredPaths += "licenses/libheif-COPYING.txt"
$requiredPaths += "licenses/libde265-COPYING.txt"
$requiredPaths += "README-BUILD-LGPL.txt"
$requiredPaths += "build-lgpl-dlls.ps1"

$missingInZip = @()
foreach ($req in $requiredPaths) {
    if (-not $normalizedEntries.ContainsKey($req)) {
        $missingInZip += $req
    }
}

if ($missingInZip.Count -gt 0) {
    $zipArch.Dispose()
    Fail "LGPL source ZIP '$($zipItem.Name)' is missing required files: $($missingInZip -join ', ')"
}

# Bind the actual source archives in the ZIP to the committed dependency policy.
# This does not trust the recipe or the provenance being generated for expectations.
$archiveEvidence = @{}
foreach ($policyDep in $policyDeps) {
    $archiveRelative = "upstream-sources/$($policyDep.archiveName)"
    $archiveEntry = $normalizedEntries[$archiveRelative]
    $archiveSha256 = Get-ZipEntryHash $archiveEntry 'SHA256'
    $archiveSha512 = Get-ZipEntryHash $archiveEntry 'SHA512'
    if ($archiveSha256 -ne [string]$policyDep.archiveSha256) {
        $zipArch.Dispose()
        Fail "LGPL source archive '$($policyDep.archiveName)' SHA-256 in ZIP ($archiveSha256) does not match dependency policy ($($policyDep.archiveSha256))."
    }
    if ($archiveSha512 -ne [string]$policyDep.archiveSha512) {
        $zipArch.Dispose()
        Fail "LGPL source archive '$($policyDep.archiveName)' SHA-512 in ZIP ($archiveSha512) does not match dependency policy ($($policyDep.archiveSha512))."
    }
    $archiveEvidence[[string]$policyDep.port] = [ordered]@{
        name   = [string]$policyDep.archiveName
        bytes  = [long]$archiveEntry.Length
        sha256 = $archiveSha256
        sha512 = $archiveSha512
    }
}

# 7b. Effective vcpkg toolchain, target triplet, recipes, and installed outputs.
# These observations come from CMakeCache.txt, the pinned vcpkg checkout, the
# installed package metadata, and actual file bytes. A caller-supplied
# -VcpkgRoot can only assert equality with the effective cache toolchain root.
$toolchainRaw = Get-CacheValue 'CMAKE_TOOLCHAIN_FILE'
if ([string]::IsNullOrWhiteSpace($toolchainRaw)) {
    $zipArch.Dispose()
    Fail "CMakeCache.txt lacks CMAKE_TOOLCHAIN_FILE; the effective vcpkg toolchain cannot be observed."
}
$toolchainPath = Normalize-PathText $toolchainRaw
if (-not (Test-Path -LiteralPath $toolchainPath -PathType Leaf)) {
    $zipArch.Dispose()
    Fail "Effective CMAKE_TOOLCHAIN_FILE does not exist: '$toolchainPath'"
}
if ($toolchainPath -notmatch '[\\/]scripts[\\/]buildsystems[\\/]vcpkg\.cmake$') {
    $zipArch.Dispose()
    Fail "Effective CMAKE_TOOLCHAIN_FILE '$toolchainPath' is not a vcpkg toolchain (expected scripts/buildsystems/vcpkg.cmake)."
}
$toolchainBuildsystemsDir = Split-Path -Parent $toolchainPath
$toolchainScriptsDir = Split-Path -Parent $toolchainBuildsystemsDir
$effectiveVcpkgRoot = (Split-Path -Parent $toolchainScriptsDir).TrimEnd([char[]]@('\', '/'))
if (-not (Test-Path -LiteralPath (Join-Path $effectiveVcpkgRoot '.vcpkg-root') -PathType Leaf) -or
    -not (Test-Path -LiteralPath (Join-Path $effectiveVcpkgRoot '.git') -PathType Container)) {
    $zipArch.Dispose()
    Fail "Effective vcpkg root '$effectiveVcpkgRoot' lacks .vcpkg-root or a Git checkout; recipe provenance cannot be established."
}
if (-not [string]::IsNullOrWhiteSpace($VcpkgRoot)) {
    $suppliedRoot = Normalize-PathText $VcpkgRoot
    if ([string]::Compare($suppliedRoot, $effectiveVcpkgRoot, $true) -ne 0) {
        $zipArch.Dispose()
        Fail "Supplied -VcpkgRoot '$suppliedRoot' is not the effective CMakeCache toolchain root '$effectiveVcpkgRoot'."
    }
}
foreach ($cacheRootKey in @('VCPKG_ROOT', 'Z_VCPKG_ROOT_DIR')) {
    $cacheRootValue = Get-CacheValue $cacheRootKey
    if (-not [string]::IsNullOrWhiteSpace($cacheRootValue)) {
        $cacheRoot = Normalize-PathText $cacheRootValue
        if ([string]::Compare($cacheRoot, $effectiveVcpkgRoot, $true) -ne 0) {
            $zipArch.Dispose()
            Fail "CMakeCache $cacheRootKey '$cacheRoot' does not match the effective toolchain root '$effectiveVcpkgRoot'."
        }
    }
}

$manifestMode = (Get-CacheValue 'VCPKG_MANIFEST_MODE').Trim().ToUpperInvariant()
if ($manifestMode -notin @('ON', '1', 'TRUE')) {
    $zipArch.Dispose()
    Fail "CMakeCache.txt must record VCPKG_MANIFEST_MODE=ON for release dependency provenance."
}
$manifestDir = Normalize-PathText (Get-CacheValue 'VCPKG_MANIFEST_DIR')
if ([string]::IsNullOrWhiteSpace($manifestDir) -or [string]::Compare($manifestDir, $normRepoRoot, $true) -ne 0) {
    $zipArch.Dispose()
    Fail "CMakeCache VCPKG_MANIFEST_DIR '$manifestDir' does not match the clean repository root '$normRepoRoot'."
}

$installedDirValues = @()
foreach ($installedKey in @('VCPKG_INSTALLED_DIR', '_VCPKG_INSTALLED_DIR')) {
    $installedValue = Get-CacheValue $installedKey
    if (-not [string]::IsNullOrWhiteSpace($installedValue)) {
        $installedDirValues += (Normalize-PathText $installedValue)
    }
}
if ($installedDirValues.Count -eq 0) {
    $zipArch.Dispose()
    Fail "CMakeCache.txt lacks VCPKG_INSTALLED_DIR; effective dependency outputs cannot be observed."
}
$installedRoot = $installedDirValues[0]
foreach ($installedCandidate in $installedDirValues) {
    if ([string]::Compare($installedCandidate, $installedRoot, $true) -ne 0) {
        $zipArch.Dispose()
        Fail "CMakeCache VCPKG_INSTALLED_DIR and _VCPKG_INSTALLED_DIR disagree ('$installedCandidate' vs '$installedRoot')."
    }
}
if (-not (Test-Path -LiteralPath $installedRoot -PathType Container)) {
    $zipArch.Dispose()
    Fail "Effective vcpkg installed directory does not exist: '$installedRoot'"
}

$observedTriplet = (Get-CacheValue 'VCPKG_TARGET_TRIPLET').Trim()
if ($observedTriplet -ne $ExpectedTargetTriplet) {
    $zipArch.Dispose()
    Fail "Effective vcpkg target triplet '$observedTriplet' does not match required '$ExpectedTargetTriplet'."
}
$repoTripletPath = Join-Path $RepoRoot ($policyTripletFile.Replace('/', '\'))
if (-not (Test-Path -LiteralPath $repoTripletPath -PathType Leaf)) {
    $zipArch.Dispose()
    Fail "Repository triplet file is missing: $repoTripletPath"
}
$repoTripletSha256 = Get-Sha256File $repoTripletPath

$overlayTripletDirs = @()
$overlayTripletsRaw = Get-CacheValue 'VCPKG_OVERLAY_TRIPLETS'
foreach ($tripletDirText in ($overlayTripletsRaw -split ';')) {
    if ([string]::IsNullOrWhiteSpace($tripletDirText)) { continue }
    $tripletDir = Normalize-PathText $tripletDirText
    if (-not (Test-Path -LiteralPath $tripletDir -PathType Container)) {
        $zipArch.Dispose()
        Fail "CMakeCache VCPKG_OVERLAY_TRIPLETS entry does not exist: '$tripletDir'"
    }
    $overlayTripletDirs += $tripletDir
}
if ($overlayTripletDirs.Count -eq 0) {
    $zipArch.Dispose()
    Fail "CMakeCache.txt lacks VCPKG_OVERLAY_TRIPLETS; the custom release triplet cannot be bound to the build."
}
$effectiveTripletCandidates = @()
foreach ($tripletDir in $overlayTripletDirs) {
    $candidate = Join-Path $tripletDir "$ExpectedTargetTriplet.cmake"
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
        $effectiveTripletCandidates += (Resolve-Path -LiteralPath $candidate).Path
    }
}
if ($effectiveTripletCandidates.Count -eq 0) {
    $zipArch.Dispose()
    Fail "No effective overlay triplet '$ExpectedTargetTriplet.cmake' was found in VCPKG_OVERLAY_TRIPLETS."
}
if ($effectiveTripletCandidates.Count -gt 1) {
    $zipArch.Dispose()
    Fail "Ambiguous effective overlay triplet: multiple candidate files were found ($($effectiveTripletCandidates -join '; '))."
}
$effectiveTripletPath = $effectiveTripletCandidates[0]
$effectiveTripletSha256 = Get-Sha256File $effectiveTripletPath
if ($effectiveTripletSha256 -ne $repoTripletSha256) {
    $zipArch.Dispose()
    Fail "Effective overlay triplet '$effectiveTripletPath' SHA-256 ($effectiveTripletSha256) does not match repository triplet ($repoTripletSha256)."
}

$overlayPortDirs = @()
$overlayPortsRaw = Get-CacheValue 'VCPKG_OVERLAY_PORTS'
foreach ($overlayPortText in ($overlayPortsRaw -split ';')) {
    if ([string]::IsNullOrWhiteSpace($overlayPortText)) { continue }
    $overlayPortDir = Normalize-PathText $overlayPortText
    if (-not (Test-Path -LiteralPath $overlayPortDir -PathType Container)) {
        $zipArch.Dispose()
        Fail "CMakeCache VCPKG_OVERLAY_PORTS entry does not exist: '$overlayPortDir'"
    }
    $overlayPortDirs += $overlayPortDir
}

$vcpkgCheckoutHead = (Get-GitText $effectiveVcpkgRoot @('rev-parse', 'HEAD') "Observing vcpkg checkout HEAD").Trim()
if ($vcpkgCheckoutHead -notmatch '^[0-9a-f]{40}$') {
    $zipArch.Dispose()
    Fail "Unable to observe a valid 40-character vcpkg checkout HEAD: '$vcpkgCheckoutHead'"
}
$vcpkgConfigPath = Join-Path $RepoRoot 'vcpkg-configuration.json'
if (-not (Test-Path -LiteralPath $vcpkgConfigPath -PathType Leaf)) {
    $zipArch.Dispose()
    Fail "vcpkg-configuration.json is required to bind the effective registry baseline."
}
try {
    $vcpkgConfig = Get-Content -LiteralPath $vcpkgConfigPath -Raw | ConvertFrom-Json
} catch {
    $zipArch.Dispose()
    Fail "vcpkg-configuration.json is not valid JSON: $($_.Exception.Message)"
}
$defaultRegistry = Get-JsonProperty $vcpkgConfig 'default-registry'
if ($null -eq $defaultRegistry) {
    $zipArch.Dispose()
    Fail "vcpkg-configuration.json lacks default-registry; registry recipe provenance is unobserved."
}
$registryKind = [string](Get-JsonProperty $defaultRegistry 'kind')
$vcpkgRegistryRepository = [string](Get-JsonProperty $defaultRegistry 'repository')
$vcpkgRegistryBaseline = [string](Get-JsonProperty $defaultRegistry 'baseline')
if ($registryKind -ne 'git' -or [string]::IsNullOrWhiteSpace($vcpkgRegistryRepository) -or $vcpkgRegistryBaseline -notmatch '^[0-9a-f]{40}$') {
    $zipArch.Dispose()
    Fail "vcpkg-configuration.json default-registry must be a Git registry with a 40-character baseline."
}
$null = Get-GitText $effectiveVcpkgRoot @('cat-file', '-e', ($vcpkgRegistryBaseline + '^{commit}')) "Verifying vcpkg registry baseline commit $vcpkgRegistryBaseline"

$dependencyEvidence = @()
$recipeMapsByPort = @{}
foreach ($policyDep in $policyDeps) {
    $port = [string]$policyDep.port
    $dep = $policyByPort[$port]

    $overlayCandidates = @()
    foreach ($overlayPortDir in $overlayPortDirs) {
        $candidate = Join-Path $overlayPortDir $port
        if (Test-Path -LiteralPath $candidate -PathType Container) {
            if (-not (Test-Path -LiteralPath (Join-Path $candidate 'vcpkg.json') -PathType Leaf)) {
                $zipArch.Dispose()
                Fail "Overlay port directory '$candidate' is missing vcpkg.json; its recipe selection is unobserved."
            }
            $overlayCandidates += (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    if ($overlayCandidates.Count -gt 1) {
        $zipArch.Dispose()
        Fail "Ambiguous VCPKG_OVERLAY_PORTS selection for '$port': $($overlayCandidates -join '; ')"
    }

    $recipeSource = 'registry'
    $recipeGitTree = $null
    if ($overlayCandidates.Count -eq 1) {
        $recipeSource = 'overlay'
        $recipeDir = $overlayCandidates[0]
    } else {
        $baselineJsonText = Get-GitText $effectiveVcpkgRoot @('show', ($vcpkgRegistryBaseline + ':versions/baseline.json')) "Reading registry baseline for $port"
        try {
            $baselineJson = $baselineJsonText | ConvertFrom-Json
        } catch {
            $zipArch.Dispose()
            Fail "Registry baseline versions/baseline.json is not valid JSON: $($_.Exception.Message)"
        }
        $baselineDefault = Get-JsonProperty $baselineJson 'default'
        $baselineEntry = Get-JsonProperty $baselineDefault $port
        if ($null -eq $baselineEntry) {
            $zipArch.Dispose()
            Fail "Registry baseline does not select port '$port'."
        }
        $selectedVersion = [string](Get-JsonProperty $baselineEntry 'baseline')
        $selectedPortVersionValue = Get-JsonProperty $baselineEntry 'port-version'
        $selectedPortVersion = if ($null -eq $selectedPortVersionValue) { 0 } else { [int]$selectedPortVersionValue }
        if ($selectedVersion -ne [string]$dep.version -or $selectedPortVersion -ne [int]$dep.portVersion) {
            $zipArch.Dispose()
            Fail "Registry baseline selects $port $selectedVersion port-version $selectedPortVersion, but dependency policy requires $($dep.version) port-version $($dep.portVersion)."
        }
        $versionsRelative = 'versions/' + $port.Substring(0, 1).ToLowerInvariant() + '-/' + $port + '.json'
        $versionsJsonText = Get-GitText $effectiveVcpkgRoot @('show', ($vcpkgRegistryBaseline + ':' + $versionsRelative)) "Reading registry version map for $port"
        try {
            $versionsJson = $versionsJsonText | ConvertFrom-Json
        } catch {
            $zipArch.Dispose()
            Fail "Registry version map '$versionsRelative' is not valid JSON: $($_.Exception.Message)"
        }
        $selectedTreeCandidates = @()
        foreach ($versionEntry in @($versionsJson.versions)) {
            $entryVersion = [string](Get-JsonProperty $versionEntry 'version')
            $entryPortVersionValue = Get-JsonProperty $versionEntry 'port-version'
            $entryPortVersion = if ($null -eq $entryPortVersionValue) { 0 } else { [int]$entryPortVersionValue }
            if ($entryVersion -eq $selectedVersion -and $entryPortVersion -eq $selectedPortVersion) {
                $selectedTreeCandidates += $versionEntry
            }
        }
        if ($selectedTreeCandidates.Count -ne 1) {
            $zipArch.Dispose()
            Fail "Registry version map '$versionsRelative' does not uniquely select $selectedVersion port-version $selectedPortVersion."
        }
        $recipeGitTree = [string](Get-JsonProperty $selectedTreeCandidates[0] 'git-tree')
        if ($recipeGitTree -notmatch '^[0-9a-f]{40}$') {
            $zipArch.Dispose()
            Fail "Registry version map '$versionsRelative' lacks a valid recipe git-tree for $port."
        }
        $portRelative = "ports/$port"
        $portStatus = (Get-GitText $effectiveVcpkgRoot @('status', '--porcelain=v1', '--ignored', '--', $portRelative) "Checking effective vcpkg port '$port'").Trim()
        if (-not [string]::IsNullOrWhiteSpace($portStatus)) {
            $zipArch.Dispose()
            Fail "Effective vcpkg port '$portRelative' has uncommitted/ignored content; recipe bytes cannot be bound to the pinned registry tree:`n$portStatus"
        }
        $headPortTree = (Get-GitText $effectiveVcpkgRoot @('rev-parse', ('HEAD:' + $portRelative)) "Reading checkout recipe tree for $port").Trim()
        if ($headPortTree -notmatch '^[0-9a-f]{40}$') {
            $zipArch.Dispose()
            Fail "Unable to observe a valid Git tree for '$portRelative' in the effective vcpkg checkout."
        }
        if ($headPortTree -ne $recipeGitTree) {
            $zipArch.Dispose()
            Fail "Effective vcpkg checkout recipe tree for '$port' ($headPortTree) does not match the pinned registry git-tree ($recipeGitTree)."
        }
        $recipeDir = Join-Path $effectiveVcpkgRoot "ports\$port"
    }

    if (-not (Test-Path -LiteralPath $recipeDir -PathType Container)) {
        $zipArch.Dispose()
        Fail "Effective recipe directory for '$port' does not exist: '$recipeDir'"
    }
    $recipeVcpkgJsonPath = Join-Path $recipeDir 'vcpkg.json'
    $recipePortfilePath = Join-Path $recipeDir 'portfile.cmake'
    if (-not (Test-Path -LiteralPath $recipeVcpkgJsonPath -PathType Leaf) -or
        -not (Test-Path -LiteralPath $recipePortfilePath -PathType Leaf)) {
        $zipArch.Dispose()
        Fail "Effective recipe for '$port' lacks vcpkg.json or portfile.cmake in '$recipeDir'."
    }
    try {
        $recipeVcpkgJson = Get-Content -LiteralPath $recipeVcpkgJsonPath -Raw | ConvertFrom-Json
    } catch {
        $zipArch.Dispose()
        Fail "Effective recipe vcpkg.json for '$port' is not valid JSON: $($_.Exception.Message)"
    }
    $recipeName = [string](Get-JsonProperty $recipeVcpkgJson 'name')
    $recipeVersion = [string](Get-JsonProperty $recipeVcpkgJson 'version')
    $recipePortVersionValue = Get-JsonProperty $recipeVcpkgJson 'port-version'
    $recipePortVersion = if ($null -eq $recipePortVersionValue) { 0 } else { [int]$recipePortVersionValue }
    if ($recipeName -ne $port) {
        $zipArch.Dispose()
        Fail "Effective recipe '$recipeDir' declares port name '$recipeName', expected '$port'."
    }
    if ($recipeVersion -ne [string]$dep.version -or $recipePortVersion -ne [int]$dep.portVersion) {
        $zipArch.Dispose()
        Fail "Effective recipe for '$port' declares $recipeVersion port-version $recipePortVersion, but dependency policy requires $($dep.version) port-version $($dep.portVersion)."
    }
    $portfileText = Get-Content -LiteralPath $recipePortfilePath -Raw
    if ($portfileText.IndexOf([string]$dep.archiveSha512, [System.StringComparison]::OrdinalIgnoreCase) -lt 0) {
        $zipArch.Dispose()
        Fail "Effective recipe portfile for '$port' does not pin the dependency-policy upstream archive SHA-512 ($($dep.archiveSha512))."
    }
    if ($portfileText.IndexOf([string]$dep.upstreamRepository, [System.StringComparison]::OrdinalIgnoreCase) -lt 0) {
        $zipArch.Dispose()
        Fail "Effective recipe portfile for '$port' does not reference expected upstream repository '$($dep.upstreamRepository)'."
    }
    $recipeMap = Get-RecipeFileMap $recipeDir
    if (-not $recipeMap.ContainsKey('portfile.cmake') -or -not $recipeMap.ContainsKey('vcpkg.json')) {
        $zipArch.Dispose()
        Fail "Effective recipe for '$port' lacks the required portfile.cmake/vcpkg.json file set."
    }

    $statusPath = Join-Path $installedRoot 'vcpkg\status'
    if (-not (Test-Path -LiteralPath $statusPath -PathType Leaf)) {
        $zipArch.Dispose()
        Fail "Effective vcpkg installed status is missing: '$statusPath'. Installed dependency evidence is unobserved."
    }
    $statusBlocks = @(Read-VcpkgStatus $statusPath)
    $packageBlocks = @($statusBlocks | Where-Object {
        $_.ContainsKey('package') -and [string]$_['package'] -eq $port -and $_.ContainsKey('version')
    })
    if ($packageBlocks.Count -eq 0) {
        $zipArch.Dispose()
        Fail "Effective vcpkg installed status has no versioned '$port' package entry."
    }
    if ($packageBlocks.Count -gt 1) {
        $zipArch.Dispose()
        Fail "Effective vcpkg installed status has ambiguous duplicate '$port' package entries."
    }
    $packageBlock = $packageBlocks[0]
    $installedVersion = [string]$packageBlock['version']
    $installedPortVersion = if ($packageBlock.ContainsKey('port-version')) { [int]$packageBlock['port-version'] } else { 0 }
    $installedArchitecture = [string]$packageBlock['architecture']
    $installedStatus = [string]$packageBlock['status']
    $installedAbi = [string]$packageBlock['abi']
    if ($installedStatus -ne 'install ok installed') {
        $zipArch.Dispose()
        Fail "Effective vcpkg installed status for '$port' is '$installedStatus', expected 'install ok installed'."
    }
    if ($installedArchitecture -ne $ExpectedTargetTriplet) {
        $zipArch.Dispose()
        Fail "Effective vcpkg installed architecture for '$port' is '$installedArchitecture', expected '$ExpectedTargetTriplet'."
    }
    if ($installedVersion -ne [string]$dep.version -or $installedPortVersion -ne [int]$dep.portVersion) {
        $zipArch.Dispose()
        Fail "Effective vcpkg installed $port is $installedVersion port-version $installedPortVersion, but dependency policy requires $($dep.version) port-version $($dep.portVersion)."
    }
    if ($installedAbi -notmatch '^[0-9a-f]{64}$') {
        $zipArch.Dispose()
        Fail "Effective vcpkg installed status for '$port' lacks a valid 64-character ABI hash."
    }

    $infoDir = Join-Path $installedRoot 'vcpkg\info'
    if (-not (Test-Path -LiteralPath $infoDir -PathType Container)) {
        $zipArch.Dispose()
        Fail "Effective vcpkg installed info directory is missing: '$infoDir'"
    }
    $listPattern = '^' + [regex]::Escape($port) + '_.+_' + [regex]::Escape($ExpectedTargetTriplet) + '\.list$'
    $listCandidates = @(Get-ChildItem -LiteralPath $infoDir -File | Where-Object { $_.Name -match $listPattern })
    if ($listCandidates.Count -ne 1) {
        $zipArch.Dispose()
        Fail "Effective vcpkg installed file list for '$port' is missing or ambiguous (found $($listCandidates.Count))."
    }
    $installedRelative = ([string]$dep.installedDllRelativePath).Replace('\', '/')
    $expectedListLine = "$ExpectedTargetTriplet/$installedRelative"
    $listLines = @(Get-Content -LiteralPath $listCandidates[0].FullName | ForEach-Object { $_.Trim().Replace('\', '/') })
    if ($listLines -notcontains $expectedListLine) {
        $zipArch.Dispose()
        Fail "Effective vcpkg installed file list '$($listCandidates[0].Name)' does not contain '$expectedListLine'."
    }

    $installedDllPath = Join-Path $installedRoot ($ExpectedTargetTriplet + '\' + ([string]$dep.installedDllRelativePath).Replace('/', '\'))
    if (-not (Test-Path -LiteralPath $installedDllPath -PathType Leaf)) {
        $zipArch.Dispose()
        Fail "Effective vcpkg installed output is missing: '$installedDllPath'"
    }
    $installedDllSha256 = Get-Sha256File $installedDllPath
    $installedDllBytes = (Get-Item -LiteralPath $installedDllPath).Length
    $stagedDllPath = Join-Path $stagedBinDirResolved ([string]$dep.stagedDll)
    if (-not (Test-Path -LiteralPath $stagedDllPath -PathType Leaf)) {
        $zipArch.Dispose()
        Fail "Staged dependency output is missing: '$stagedDllPath'"
    }
    $stagedDllKey = ([string]$dep.stagedDll).ToLowerInvariant()
    if (-not $stagedHashByLowerName.ContainsKey($stagedDllKey)) {
        $zipArch.Dispose()
        Fail "Staged dependency output '$($dep.stagedDll)' was not observed in the staged binary inspection."
    }
    $stagedDllSha256 = [string]$stagedHashByLowerName[$stagedDllKey]
    $stagedDllBytes = [long]$stagedBytesByLowerName[$stagedDllKey]
    if ($stagedDllSha256 -ne $installedDllSha256) {
        $zipArch.Dispose()
        Fail "Staged $port output '$($dep.stagedDll)' SHA-256 ($stagedDllSha256) does not match the effective vcpkg installed output '$installedDllPath' ($installedDllSha256). Substituted dependency binaries are not accepted."
    }
    if ($stagedDllBytes -ne [long]$installedDllBytes) {
        $zipArch.Dispose()
        Fail "Staged $port output '$($dep.stagedDll)' byte size ($stagedDllBytes) does not match the effective vcpkg installed output ($installedDllBytes)."
    }

    $recipeFileEvidence = @()
    foreach ($recipeRelative in ($recipeMap.Keys | Sort-Object)) {
        $recipeFileEvidence += [ordered]@{
            path   = $recipeRelative
            bytes  = [long](Get-Item -LiteralPath $recipeMap[$recipeRelative]).Length
            sha256 = Get-Sha256File $recipeMap[$recipeRelative]
        }
    }
    $observedPatches = @($recipeMap.Keys | Where-Object { $_ -notin @('portfile.cmake', 'vcpkg.json') } | Sort-Object)
    $recipeMapsByPort[$port] = $recipeMap
    $dependencyEvidence += [ordered]@{
        port           = $port
        version        = [string]$dep.version
        portVersion    = [int]$dep.portVersion
        recipeSource   = $recipeSource
        recipeGitTree  = $recipeGitTree
        recipeFiles    = $recipeFileEvidence
        upstreamArchive = $archiveEvidence[$port]
        installed      = [ordered]@{
            version      = $installedVersion
            portVersion  = $installedPortVersion
            architecture = $installedArchitecture
            status       = $installedStatus
            abi          = $installedAbi
            listFile     = $listCandidates[0].Name
            dllRelative  = $installedRelative
            dllBytes     = [long]$installedDllBytes
            dllSha256    = $installedDllSha256
        }
        staged         = [ordered]@{
            name   = [string]$dep.stagedDll
            bytes  = [long]$stagedDllBytes
            sha256 = $stagedDllSha256
        }
        patches        = $observedPatches
    }
}

# Bind the exact effective recipe file set and bytes to the LGPL source ZIP.
foreach ($policyDep in $policyDeps) {
    $port = [string]$policyDep.port
    $recipeMap = $recipeMapsByPort[$port]
    $zipRecipePrefix = "vcpkg-ports/$port/"
    $zipRecipeKeys = @($normalizedEntries.Keys | Where-Object { $_.StartsWith($zipRecipePrefix, [System.StringComparison]::Ordinal) })
    foreach ($recipeRelative in ($recipeMap.Keys | Sort-Object)) {
        $zipRelative = $zipRecipePrefix + $recipeRelative
        if (-not $normalizedEntries.ContainsKey($zipRelative)) {
            $zipArch.Dispose()
            Fail "LGPL source ZIP is missing effective recipe file '$zipRelative'."
        }
        $diskSha = Get-Sha256File $recipeMap[$recipeRelative]
        $zipSha256 = Get-ZipEntryHash $normalizedEntries[$zipRelative] 'SHA256'
        if ($diskSha -ne $zipSha256) {
            $zipArch.Dispose()
            Fail "LGPL source ZIP recipe '$zipRelative' SHA-256 ($zipSha256) does not match the effective recipe ($diskSha)."
        }
    }
    foreach ($zipKey in $zipRecipeKeys) {
        $zipRelative = $zipKey.Substring($zipRecipePrefix.Length)
        if (-not $recipeMap.ContainsKey($zipRelative)) {
            $zipArch.Dispose()
            Fail "LGPL source ZIP contains recipe file '$zipKey' that is not part of the effective '$port' recipe."
        }
    }
}

$zipTripletPath = $policyTripletFile.Replace('\', '/')
$zipTripletEntry = $normalizedEntries[$zipTripletPath]
if ($null -eq $zipTripletEntry) {
    $zipArch.Dispose()
    Fail "LGPL source ZIP is missing triplet '$zipTripletPath'."
}
$zipTripletSha256 = Get-ZipEntryHash $zipTripletEntry 'SHA256'
if ($zipTripletSha256 -ne $effectiveTripletSha256 -or $zipTripletSha256 -ne $repoTripletSha256) {
    $zipArch.Dispose()
    Fail "LGPL source ZIP triplet SHA-256 ($zipTripletSha256) does not match the effective/repository triplet ($effectiveTripletSha256 / $repoTripletSha256)."
}

$zipArch.Dispose()

$lgplAssetInfo = [ordered]@{
    assetName     = $zipItem.Name
    bytes         = [long]$zipItem.Length
    sha256        = $zipSha
    deliveryBasis = "GPL-3.0 §6(d) source distribution / LGPL-3.0 §4 combined-work shared-library provisions"
    contains      = @(
        "Exact upstream tarballs for libheif $($policyByPort['libheif'].version) and libde265 $($policyByPort['libde265'].version)",
        "Exact effective vcpkg recipes and patches",
        $policyTripletFile.Replace('\', '/'),
        "Full LGPL-3.0 and GPL-3.0 license texts",
        "README-BUILD-LGPL.txt and build-lgpl-dlls.ps1"
    )
}

# 8. Third-party source input evidence verified from source
$thirdPartyInputs = @()
foreach ($policyDep in $policyDeps) {
    $port = [string]$policyDep.port
    $thirdPartyInputs += [ordered]@{
        name    = $port
        version = [string]$policyDep.version
        license = "LGPL-3.0-only"
        archive = [string]$policyDep.archiveName
        bytes   = [long]$archiveEvidence[$port].bytes
        sha256  = [string]$archiveEvidence[$port].sha256
        sha512  = [string]$archiveEvidence[$port].sha512
        patches = @($dependencyEvidence | Where-Object { $_.port -eq $port } | ForEach-Object { $_.patches })
    }
}

# 9. Build full acyclic provenance record
$provenance = [ordered]@{
    '$schema'                 = "https://json-schema.org/draft/2020-12/schema"
    provenanceSchemaVersion   = "1.0.0"
    product                   = [ordered]@{
        name        = "PikaView"
        packageName = $PackageName
        version     = $Version
        tag         = $ExpectedTag
        publisher   = "OZ Lab"
        homepage    = "https://underoatz-netizen.github.io/software/oz-image-viewer/"
        repository  = "https://github.com/underoatz-netizen/oz-image-viewer"
    }
    source                    = [ordered]@{
        commitSha   = $headCommit
        treeSha     = $treeSha
        cleanStatus = "clean"
        isClean     = $true
    }
    buildConfiguration        = [ordered]@{
        targetArchitecture = $observedArch
        diagnostics        = $observedDiagnostics
        configuration      = "Release"
    }
    toolchain                 = [ordered]@{
        hostOs                = $hostOs
        compiler              = $compilerObserved
        cmake                 = $cmakeObserved
        targetTriplet         = $observedTriplet
        vcpkgCheckoutHead     = $vcpkgCheckoutHead
        vcpkgRegistryBaseline = $vcpkgRegistryBaseline
    }
    dependencyProvenance      = [ordered]@{
        trustBasis      = "Controlled clean configure/build plus byte equality with the effective CMakeCache vcpkg toolchain, pinned registry/overlay recipes, installed package metadata and outputs, and the corresponding-source asset."
        claimLimits     = @(
            "Byte equality between staged DLLs, effective installed outputs, recipes, and source archives demonstrates consistency with the recorded controlled build; it is not cryptographic proof of compilation, custody, or bit-for-bit reproducibility.",
            "Vcpkg registry and installed-package metadata are observations from the effective build environment. A party able to modify that environment after the controlled build could forge them; release review and environment custody remain external controls.",
            "CMakeCache values are treated as the record of the effective configure/build inputs. Overlay or toolchain settings introduced only through a build-time environment variable and absent from the cache are outside this evidence.",
            "The committed dependency policy is a controlled source input. Changing expected versions, archives, or triplet requires a source change and release review; this record does not make that policy self-authenticating.",
            "A reproducible-build proof and a real clean isolated dependency rebuild in a fresh workspace remain required release evidence and are not established by this record."
        )
        policyPath      = Sanitize-Path $PolicyPath
        policySha256    = $policySha256
        effectiveVcpkg  = [ordered]@{
            toolchainFile       = Sanitize-Path $toolchainPath
            vcpkgRoot           = Sanitize-Path $effectiveVcpkgRoot
            checkoutHead        = $vcpkgCheckoutHead
            registryRepository  = $vcpkgRegistryRepository
            registryBaseline    = $vcpkgRegistryBaseline
            targetTriplet       = $observedTriplet
            effectiveTriplet    = Sanitize-Path $effectiveTripletPath
            tripletSha256       = $effectiveTripletSha256
            installedDir        = Sanitize-Path $installedRoot
            manifestDir         = Sanitize-Path $manifestDir
            overlayPorts        = @($overlayPortDirs | ForEach-Object { Sanitize-Path $_ })
            overlayTripletDirs  = @($overlayTripletDirs | ForEach-Object { Sanitize-Path $_ })
        }
        dependencies    = $dependencyEvidence
    }
    thirdPartySourceInputs    = $thirdPartyInputs
    stagedCandidateBinaries   = $stagedBinaries
    lgplCorrespondingSourceAsset = $lgplAssetInfo
}

$outParent = Split-Path -Parent $fullOutPath
if (-not [string]::IsNullOrWhiteSpace($outParent) -and -not (Test-Path -LiteralPath $outParent -PathType Container)) {
    New-Item -ItemType Directory -Path $outParent -Force | Out-Null
}

$jsonText = $provenance | ConvertTo-Json -Depth 8
[System.IO.File]::WriteAllText($fullOutPath, $jsonText, (New-Object System.Text.UTF8Encoding($false)))
Info "Generated BUILD-PROVENANCE.json at: $fullOutPath"
Write-Output "PROVENANCE_PATH=$fullOutPath"
exit 0

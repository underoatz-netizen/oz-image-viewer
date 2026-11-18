<#
.SYNOPSIS
  Controlled Release-build wrapper that invokes CMake build and records build-evidence.json.

.DESCRIPTION
  Verifies clean git working tree before and after invoking
  `cmake --build <BuildDir> --config Release`, checks CMakeCache.txt for a
  source directory matching the resolved clean worktree and
  OZVIEWER_PRINT_DIAGNOSTICS=OFF, and writes `build-evidence.json` binding the
  observed commit, tree, command, exit code, and built ozviewer.exe SHA-256.

  Fresh-build precondition: this wrapper does not prove that every object file
  in BuildDir is new. It requires a controlled fresh-configure precondition
  (BuildDir configured from the same clean HEAD, diagnostics OFF) and fails
  closed if any candidate ozviewer.exe already exists before the build. It
  never deletes user files. Use a fresh BuildDir for release evidence.

  This is retained execution evidence, not tamper-proof attestation.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [string]$CmakeExe = "cmake.exe",
    [string]$Target = "ozviewer"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = (Resolve-Path -LiteralPath (Split-Path -Parent $ScriptDir)).Path

function Fail([string]$Message) {
    Write-Host "invoke-release-build: ERROR: $Message" -ForegroundColor Red
    exit 1
}

function Invoke-GitText([string[]]$GitArguments) {
    $previousPreference = $ErrorActionPreference
    try {
        # Native stderr becomes an error record under Windows PowerShell; keep
        # it captured so a Git failure is reported as a Git failure below.
        $ErrorActionPreference = 'Continue'
        $output = @(& git -C $RepoRoot @GitArguments 2>&1)
        $code = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousPreference
    }
    if ($code -ne 0) {
        Fail "git $($GitArguments -join ' ') failed (exit $code): $($output -join ' / ')"
    }
    return (($output | ForEach-Object { $_.ToString() }) -join "`n")
}

if (-not (Test-Path -LiteralPath $BuildDir -PathType Container)) {
    Fail "BuildDir does not exist: $BuildDir"
}
$buildDirResolved = (Resolve-Path -LiteralPath $BuildDir).Path
$cacheFile = Join-Path $buildDirResolved "CMakeCache.txt"
if (-not (Test-Path -LiteralPath $cacheFile -PathType Leaf)) {
    Fail "CMakeCache.txt not found in BuildDir: $buildDirResolved"
}

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Fail "git is required on PATH."
}
$beforeStatusText = Invoke-GitText @('status', '--porcelain')
$beforeStatus = @($beforeStatusText -split "`n" | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
if ($beforeStatus.Count -gt 0) {
    Fail "Source working tree is not clean before build:`n$($beforeStatus -join "`n")"
}
$beforeCommit = (Invoke-GitText @('rev-parse', 'HEAD')).Trim()
$beforeTree = (Invoke-GitText @('rev-parse', 'HEAD^{tree}')).Trim()
if ($beforeCommit -notmatch '^[0-9a-f]{40}$') {
    Fail "Unable to obtain a valid 40-character HEAD commit SHA: $beforeCommit"
}
if ($beforeTree -notmatch '^[0-9a-f]{40}$') {
    Fail "Unable to obtain a valid 40-character HEAD tree SHA: $beforeTree"
}

$cacheLines = @(Get-Content -LiteralPath $cacheFile)
$cacheSource = ""
foreach ($key in @('CMAKE_HOME_DIRECTORY', 'ozbench_SOURCE_DIR')) {
    $pattern = '^' + [regex]::Escape($key) + ':[^=]+=(.*)$'
    $match = $cacheLines | Where-Object { $_ -match $pattern } | Select-Object -First 1
    if ($match -and $match -match $pattern) {
        $cacheSource = $Matches[1].Trim()
        break
    }
}
if ([string]::IsNullOrWhiteSpace($cacheSource)) {
    Fail "CMakeCache.txt lacks CMAKE_HOME_DIRECTORY / ozbench_SOURCE_DIR; cannot bind BuildDir to the resolved clean worktree."
}
if (-not [System.IO.Path]::IsPathRooted($cacheSource)) {
    Fail "CMakeCache source directory is not an absolute path: $cacheSource"
}
try {
    $normalizedCacheSource = [System.IO.Path]::GetFullPath($cacheSource.Replace('/', '\')).TrimEnd('\')
}
catch {
    Fail "CMakeCache source directory is not a valid path: $cacheSource"
}
$normalizedRepoRoot = [System.IO.Path]::GetFullPath($RepoRoot).TrimEnd('\')
if ([string]::Compare($normalizedCacheSource, $normalizedRepoRoot, $true) -ne 0) {
    Fail "CMakeCache source dir mismatch (cached '$normalizedCacheSource' vs resolved clean worktree '$normalizedRepoRoot'). Configure a fresh BuildDir from the current worktree."
}

$diagLine = $cacheLines | Where-Object { $_ -match '^OZVIEWER_PRINT_DIAGNOSTICS:BOOL=(.*)$' } | Select-Object -First 1
if (-not $diagLine -or ($diagLine -match '^OZVIEWER_PRINT_DIAGNOSTICS:BOOL=(.*)$' -and $Matches[1].Trim().ToUpperInvariant() -notin @('OFF', '0', 'FALSE'))) {
    Fail "CMakeCache.txt must have OZVIEWER_PRINT_DIAGNOSTICS=OFF."
}

$exeCandidates = @(
    (Join-Path $buildDirResolved "src\viewer\Release\ozviewer.exe"),
    (Join-Path $buildDirResolved "Release\ozviewer.exe"),
    (Join-Path $buildDirResolved "src\viewer\ozviewer.exe"),
    (Join-Path $buildDirResolved "ozviewer.exe")
)
$preexistingExes = @()
foreach ($c in $exeCandidates) {
    if (Test-Path -LiteralPath $c -PathType Leaf) { $preexistingExes += $c }
}
if ($preexistingExes.Count -gt 0) {
    Fail "BuildDir already contains target executable(s): $($preexistingExes -join '; '). Refusing to record a build receipt for a preexisting/stale binary. Use a freshly configured BuildDir; this wrapper never deletes user files."
}

$buildArgs = @('--build', $buildDirResolved, '--config', 'Release', '--target', $Target)
$buildCmdStr = "$CmakeExe " + ($buildArgs -join ' ')
& $CmakeExe @buildArgs
$exitCode = $LASTEXITCODE
if ($exitCode -ne 0) {
    Fail "CMake build failed with exit code $exitCode"
}

$afterStatusText = Invoke-GitText @('status', '--porcelain')
$afterStatus = @($afterStatusText -split "`n" | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
if ($afterStatus.Count -gt 0) {
    Fail "Source working tree became dirty during build:`n$($afterStatus -join "`n")"
}
$afterCommit = (Invoke-GitText @('rev-parse', 'HEAD')).Trim()
$afterTree = (Invoke-GitText @('rev-parse', 'HEAD^{tree}')).Trim()
if ($afterCommit -notmatch '^[0-9a-f]{40}$' -or $afterTree -notmatch '^[0-9a-f]{40}$') {
    Fail "Unable to obtain valid 40-character HEAD/tree SHA values after build."
}
if ($afterCommit -ne $beforeCommit -or $afterTree -ne $beforeTree) {
    Fail "Source commit/tree changed during build."
}

$builtExes = @()
foreach ($c in $exeCandidates) {
    if (Test-Path -LiteralPath $c -PathType Leaf) { $builtExes += $c }
}
if ($builtExes.Count -eq 0) {
    Fail "Built ozviewer.exe not found in BuildDir after Release build."
}
if ($builtExes.Count -gt 1) {
    Fail "Ambiguous post-build target executables found: $($builtExes -join '; '). Refusing to select an arbitrary binary."
}
$builtExe = $builtExes[0]

$builtSha = (Get-FileHash -LiteralPath $builtExe -Algorithm SHA256).Hash.ToLowerInvariant()
$ev = [ordered]@{
    schema                      = 1
    wrapper                     = "invoke-release-build.ps1"
    target                      = $Target
    buildDir                    = $buildDirResolved
    cacheSourceDir              = $normalizedCacheSource
    targetExeAbsentBeforeBuild  = $true
    sourceCommitSha             = $afterCommit
    sourceTreeSha               = $afterTree
    configuration               = "Release"
    diagnostics                 = "OFF"
    buildCommand                = $buildCmdStr
    exitCode                    = 0
    builtOzviewerExe            = $builtExe
    builtOzviewerSha256         = $builtSha
}
$evPath = Join-Path $buildDirResolved "build-evidence.json"
[System.IO.File]::WriteAllText($evPath, ($ev | ConvertTo-Json -Depth 4), (New-Object System.Text.UTF8Encoding($false)))
Write-Output "BUILD_EVIDENCE_PATH=$evPath"
exit 0

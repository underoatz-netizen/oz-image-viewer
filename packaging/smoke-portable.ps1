<#
.SYNOPSIS
  Smoke-test the staged portable OZ image viewer package in an isolated
  environment (Stage 9.9).

.DESCRIPTION
  Runs the packaged ozviewer.exe in benchmark mode against a checked-in
  fixture, from a copy of the staging directory placed in a temporary work
  directory, with PATH scrubbed to %SystemRoot%\System32;%SystemRoot% only so
  the run cannot depend on vcpkg or any build-output directory.

  Given -PayloadDir the script first builds the package into -WorkDir via
  package-portable.ps1; given -PackageDir it tests an already-staged directory.

  This is a host smoke test, not a clean-machine validation. It cannot prove
  the package runs on a Windows installation without the MSVC runtime present;
  for that use the documented clean-machine procedure.

.PARAMETER PayloadDir
  Existing Release x64 payload directory to package and then smoke-test.

.PARAMETER PackageDir
  Already-staged package directory to smoke-test (mutually exclusive with
  -PayloadDir).

.PARAMETER WorkDir
  Scratch directory for the isolated copy and the generated package.

.PARAMETER Fixture
  Image fixture to open. Defaults to tests/data/webp/bench_64x48.webp and falls
  back to tests/data/phasea/bench_64x48.bmp.

.PARAMETER RuntimeDir
  Optional MSVC runtime directory forwarded to package-portable.ps1.

.PARAMETER TimeoutSeconds
  Per-run timeout (default 60).

.PARAMETER AllowSkipNoDesktop
  Return 77 instead of failing when the viewer cannot present (no interactive
  desktop).

.NOTES
  Exit codes: 0 = pass, 77 = skipped, 1 = failure.
#>
[CmdletBinding()]
param(
    [string]$PayloadDir = "",
    [string]$PackageDir = "",
    [Parameter(Mandatory = $true)][string]$WorkDir,
    [string]$Fixture = "",
    [string]$RuntimeDir = "",
    [int]$TimeoutSeconds = 60,
    [switch]$AllowSkipNoDesktop
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Split-Path -Parent $ScriptDir
$PackageScript = Join-Path $ScriptDir 'package-portable.ps1'

function Fail([string]$Message) {
    Write-Host "smoke-portable: ERROR: $Message" -ForegroundColor Red
    exit 1
}
function Skip([string]$Message) {
    Write-Host "smoke-portable: SKIP: $Message" -ForegroundColor Yellow
    exit 77
}
function Info([string]$Message) { Write-Host "smoke-portable: $Message" }

if ([string]::IsNullOrWhiteSpace($PackageDir)) {
    if ([string]::IsNullOrWhiteSpace($PayloadDir)) {
        Fail "one of -PayloadDir or -PackageDir is required"
    }
    $packageOut = Join-Path $WorkDir 'package'
    Info "staging package from payload: $PayloadDir"
    $pkgArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PackageScript,
                 '-PayloadDir', $PayloadDir, '-OutDir', $packageOut)
    if (-not [string]::IsNullOrWhiteSpace($RuntimeDir)) { $pkgArgs += @('-RuntimeDir', $RuntimeDir) }
    $pkgOutput = & powershell @pkgArgs 2>&1
    if ($LASTEXITCODE -ne 0) {
        Fail "package-portable.ps1 failed:`n$($pkgOutput -join "`n")"
    }
    $stageLine = @($pkgOutput | Where-Object { $_ -like 'PACKAGE_STAGE=*' })
    if ($stageLine.Count -eq 0) { Fail "package-portable.ps1 did not report PACKAGE_STAGE" }
    $PackageDir = [string]($stageLine[0] -replace '^PACKAGE_STAGE=', '')
}
if (-not (Test-Path -LiteralPath $PackageDir -PathType Container)) {
    Fail "package directory not found: $PackageDir"
}
$PackageDir = (Resolve-Path -LiteralPath $PackageDir).Path
$exeName = 'ozviewer.exe'
$exePath = Join-Path $PackageDir $exeName
if (-not (Test-Path -LiteralPath $exePath -PathType Leaf)) {
    Fail "package is missing $exeName in $PackageDir"
}

if ([string]::IsNullOrWhiteSpace($Fixture)) {
    $candidates = @(
        (Join-Path $RepoRoot 'tests\data\webp\bench_64x48.webp'),
        (Join-Path $RepoRoot 'tests\data\phasea\bench_64x48.bmp')
    )
    foreach ($c in $candidates) {
        if (Test-Path -LiteralPath $c -PathType Leaf) { $Fixture = $c; break }
    }
}
if ([string]::IsNullOrWhiteSpace($Fixture) -or -not (Test-Path -LiteralPath $Fixture -PathType Leaf)) {
    Fail "no usable fixture found (pass -Fixture)"
}

# --- isolated environment ---------------------------------------------------
if (-not (Test-Path -LiteralPath $WorkDir -PathType Container)) {
    New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null
}
$WorkDir = (Resolve-Path -LiteralPath $WorkDir).Path
$isolated = Join-Path $WorkDir 'isolated'
if (Test-Path -LiteralPath $isolated) { Remove-Item -LiteralPath $isolated -Recurse -Force }
$isolatedPkg = Join-Path $isolated 'pkg'
$isolatedFixtureDir = Join-Path $isolated 'fixture'
New-Item -ItemType Directory -Path $isolatedPkg -Force | Out-Null
New-Item -ItemType Directory -Path $isolatedFixtureDir -Force | Out-Null
Copy-Item -Path (Join-Path $PackageDir '*') -Destination $isolatedPkg -Recurse -Force
$isolatedFixture = Join-Path $isolatedFixtureDir ([System.IO.Path]::GetFileName($Fixture))
Copy-Item -LiteralPath $Fixture -Destination $isolatedFixture -Force

$outFile = Join-Path $WorkDir 'benchmark.jsonl'
$errFile = Join-Path $WorkDir 'benchmark.err'
if (Test-Path -LiteralPath $outFile) { Remove-Item -LiteralPath $outFile -Force }
if (Test-Path -LiteralPath $errFile) { Remove-Item -LiteralPath $errFile -Force }

$oldPath = $env:PATH
$oldCwd = (Get-Location).Path
$stdout = ''
$stderr = ''
$exitCode = -1
try {
    # Scrub PATH so the run can only use the package directory and the OS.
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
    Set-Location -LiteralPath $isolated
    Info "launching isolated payload: $isolatedPkg\$exeName --benchmark $isolatedFixture"
    Info "PATH (scrubbed): $env:PATH"

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = (Join-Path $isolatedPkg $exeName)
    $psi.Arguments = "--benchmark `"$isolatedFixture`""
    $psi.WorkingDirectory = $isolated
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $proc = [System.Diagnostics.Process]::Start($psi)
    $outTask = $proc.StandardOutput.ReadToEndAsync()
    $errTask = $proc.StandardError.ReadToEndAsync()
    $exited = $proc.WaitForExit($TimeoutSeconds * 1000)
    if (-not $exited) {
        try { $proc.Kill() } catch { }
        if ($AllowSkipNoDesktop) { Skip "viewer did not exit within ${TimeoutSeconds}s (no interactive desktop?)" }
        Fail "viewer did not exit within ${TimeoutSeconds}s"
    }
    $stdout = $outTask.Result
    $stderr = $errTask.Result
    $exitCode = $proc.ExitCode
    [System.IO.File]::WriteAllText($outFile, $stdout)
    [System.IO.File]::WriteAllText($errFile, $stderr)
}
finally {
    $env:PATH = $oldPath
    Set-Location -LiteralPath $oldCwd
}

if ([string]::IsNullOrWhiteSpace($stdout)) { $stdout = '' }

$markers = @()
foreach ($m in ([regex]'"marker"\s*:\s*"([A-Za-z]+)"').Matches($stdout)) {
    $markers += $m.Groups[1].Value
}

if ($markers.Count -eq 0) {
    if ($AllowSkipNoDesktop) {
        Skip "no markers emitted (exit $exitCode; no interactive desktop?); stderr: $stderr"
    }
    Fail "no timing markers emitted (exit $exitCode); stderr: $stderr"
}
if ($exitCode -ne 0) { Fail "viewer exited with $exitCode" }

$expected = @('ProcessStart', 'RendererReady', 'FileOpened', 'CodecDetected', 'HeaderParsed',
              'DecodeStart', 'DecodeComplete', 'GpuUploadStart', 'GpuUploadComplete', 'FirstPresent')
$actual = @()
foreach ($m in $markers) {
    $actual += $m
    if ($m -eq 'FirstPresent') { break }
}
if (($actual -join ';') -ne ($expected -join ';')) {
    Fail "marker prefix mismatch.`n  expected: $($expected -join ';')`n  actual:   $($actual -join ';')`n  all: $($markers -join ',')"
}
foreach ($m in $markers) {
    if ($m -like '*FolderEnumeration*') { Fail "folder-enumeration marker emitted in benchmark mode: $m" }
}

Info "PASS: isolated staged payload emitted the full prefix through FirstPresent and auto-exited"
Write-Output "SMOKE_RESULT=PASS"
Write-Output "SMOKE_PACKAGE=$PackageDir"
exit 0

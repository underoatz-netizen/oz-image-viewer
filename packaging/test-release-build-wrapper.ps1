<#
.SYNOPSIS
  Focused non-installing tests for the controlled Release-build wrapper.

.DESCRIPTION
  Exercises packaging/invoke-release-build.ps1 inside an isolated, explicitly
  synthetic contract-test fixture repository. No real build, installer,
  printer, or Git mutation of the user worktree occurs. The "cmake" used here
  is a mock script that only writes deterministic fixture files.

  Covers:
    * CMakeCache source directory must match the resolved clean worktree and
      fails before the mock build;
    * a preexisting/stale target executable is rejected before the mock build
      without deleting any user file;
    * Git command failures are reported as Git failures, not misreported as a
      dirty working tree;
    * a valid clean mock build writes build-evidence.json with the observed
      commit/tree and synthetic output hash;
    * a mock build failure does not write build-evidence.json;
    * ambiguous post-build target executables are rejected.

  The wrapper verifies a controlled fresh-configure precondition, not
  tamper-proof attestation. These fixtures are contract-test evidence only.

.PARAMETER WorkDir
  Scratch directory for the fixture repository, mock build directories, and
  mock cmake script.

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

function Skip([string]$Message) {
    Write-Host "release-wrapper-test: SKIP: $Message" -ForegroundColor Yellow
    exit 77
}
function Pass([string]$Message) { Write-Host "release-wrapper-test: PASS: $Message" -ForegroundColor Green }
function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "ASSERT FAILED: $Message" }
}

if (-not (Test-Path -LiteralPath $FixtureHelper -PathType Leaf)) {
    Skip "contract fixture helper not found: $FixtureHelper"
}
. $FixtureHelper
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

$Fixture = New-TestPackagingFixture -Root (Join-Path $WorkDir 'clean-fixture') `
    -SourceScriptDir $ScriptDir -SourceRepoRoot $RepoRoot `
    -ExtraScriptNames @('invoke-release-build.ps1')
$Wrapper = Join-Path $Fixture.PackagingDir 'invoke-release-build.ps1'
Write-Host "release-wrapper-test: NOTE: using synthetic clean fixture repo $($Fixture.RepoRoot) at $($Fixture.Head) (not a real build)" -ForegroundColor Yellow

# A separate copy outside Git so Git commands fail with a real Git error.
$NoGitRoot = Join-Path $WorkDir 'no-git-fixture'
if (Test-Path -LiteralPath $NoGitRoot) { Remove-Item -LiteralPath $NoGitRoot -Recurse -Force }
$NoGitPackaging = Join-Path $NoGitRoot 'packaging'
New-Item -ItemType Directory -Path $NoGitPackaging -Force | Out-Null
Copy-Item -LiteralPath $Wrapper -Destination (Join-Path $NoGitPackaging 'invoke-release-build.ps1') -Force
$NoGitWrapper = Join-Path $NoGitPackaging 'invoke-release-build.ps1'

# Deterministic mock cmake: no compilation, no installer, no system changes.
$MockCmake = Join-Path $WorkDir 'mock-cmake.ps1'
$mockSource = @'
$buildIndex = [Array]::IndexOf($args, '--build')
if ($buildIndex -lt 0 -or $args.Count -le ($buildIndex + 1)) { exit 88 }
$buildDir = [string]$args[$buildIndex + 1]
[IO.File]::WriteAllText((Join-Path $buildDir 'mock-cmake-invoked.txt'), "SYNTHETIC MOCK BUILD INVOCATION`n")
if (-not [string]::IsNullOrWhiteSpace($env:MOCK_CMAKE_EXIT) -and $env:MOCK_CMAKE_EXIT -ne '0') {
    exit [int]$env:MOCK_CMAKE_EXIT
}
$exeDir = Join-Path $buildDir 'src\viewer\Release'
New-Item -ItemType Directory -Path $exeDir -Force | Out-Null
[IO.File]::WriteAllText((Join-Path $exeDir 'ozviewer.exe'), "SYNTHETIC MOCK BUILD OUTPUT - NOT A REAL BINARY`n")
if ($env:MOCK_CMAKE_CREATE_AMBIGUOUS -eq '1') {
    $secondDir = Join-Path $buildDir 'Release'
    New-Item -ItemType Directory -Path $secondDir -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $secondDir 'ozviewer.exe'), "SYNTHETIC SECOND MOCK OUTPUT`n")
}
exit 0
'@
[IO.File]::WriteAllText($MockCmake, $mockSource, (New-Object System.Text.UTF8Encoding($false)))

function Write-MockCache {
    param(
        [Parameter(Mandatory = $true)][string]$BuildDir,
        [Parameter(Mandatory = $true)][string]$SourceDir,
        [string]$Diagnostics = 'OFF'
    )
    New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null
    $cache = "CMAKE_HOME_DIRECTORY:INTERNAL=$SourceDir`nOZVIEWER_PRINT_DIAGNOSTICS:BOOL=$Diagnostics`nCMAKE_BUILD_TYPE:STRING=Release`n"
    [IO.File]::WriteAllText((Join-Path $BuildDir 'CMakeCache.txt'), $cache, (New-Object System.Text.UTF8Encoding($false)))
}

function Invoke-Wrapper {
    param(
        [Parameter(Mandatory = $true)][string]$WrapperPath,
        [Parameter(Mandatory = $true)][string]$BuildDir,
        [string]$MockExit = '0',
        [string]$CreateAmbiguous = '0',
        [string]$GitCeilingDirectories = ''
    )
    $names = @('MOCK_CMAKE_EXIT', 'MOCK_CMAKE_CREATE_AMBIGUOUS', 'GIT_CEILING_DIRECTORIES')
    $previous = @{}
    foreach ($name in $names) { $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
    $previousPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        [Environment]::SetEnvironmentVariable('MOCK_CMAKE_EXIT', $MockExit, 'Process')
        [Environment]::SetEnvironmentVariable('MOCK_CMAKE_CREATE_AMBIGUOUS', $CreateAmbiguous, 'Process')
        if (-not [string]::IsNullOrWhiteSpace($GitCeilingDirectories)) {
            # Git stops directory discovery only at a parent of the start path;
            # setting the ceiling to the start path itself still lets Git ascend.
            $ceiling = [System.IO.Path]::GetFullPath($GitCeilingDirectories)
            $existing = $previous['GIT_CEILING_DIRECTORIES']
            if ([string]::IsNullOrWhiteSpace($existing)) {
                $ceilingValue = $ceiling
            }
            else {
                $separator = [string][System.IO.Path]::PathSeparator
                $ceilingValue = $existing.Trim().TrimEnd([char[]]@([char]$separator, [char]':')) + $separator + $ceiling
            }
            [Environment]::SetEnvironmentVariable('GIT_CEILING_DIRECTORIES', $ceilingValue, 'Process')
        }
        $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $WrapperPath,
                       '-BuildDir', $BuildDir, '-CmakeExe', $MockCmake)
        $output = & powershell @arguments 2>&1
        return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Output = ($output -join "`n") }
    }
    finally {
        $ErrorActionPreference = $previousPreference
        foreach ($name in $names) {
            $value = $previous[$name]
            if ($null -eq $value) { [Environment]::SetEnvironmentVariable($name, $null, 'Process') }
            else { [Environment]::SetEnvironmentVariable($name, $value, 'Process') }
        }
    }
}

try {
    # --- 1. wrong CMakeCache source directory --------------------------------
    $badCacheBuild = Join-Path $WorkDir 'wrong-source-build'
    Write-MockCache -BuildDir $badCacheBuild -SourceDir (Join-Path $WorkDir 'wrong-source') -Diagnostics 'OFF'
    $r = Invoke-Wrapper -WrapperPath $Wrapper -BuildDir $badCacheBuild
    Assert-True ($r.ExitCode -ne 0) "wrapper accepted a CMakeCache source dir mismatch"
    Assert-True ($r.Output -match 'CMakeCache source dir mismatch') "wrong source dir error not reported:`n$($r.Output)"
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $badCacheBuild 'mock-cmake-invoked.txt'))) "mock build ran despite a wrong CMakeCache source dir"
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $badCacheBuild 'build-evidence.json'))) "build evidence written despite a wrong CMakeCache source dir"
    Pass "wrong CMakeCache source dir fails before invoking the build"

    # --- 2. preexisting stale target executable ------------------------------
    $staleBuild = Join-Path $WorkDir 'stale-exe-build'
    Write-MockCache -BuildDir $staleBuild -SourceDir $Fixture.RepoRoot -Diagnostics 'OFF'
    $staleExe = Join-Path $staleBuild 'src\viewer\Release\ozviewer.exe'
    New-Item -ItemType Directory -Path (Split-Path -Parent $staleExe) -Force | Out-Null
    [IO.File]::WriteAllText($staleExe, "STALE SYNTHETIC BINARY - MUST NOT BE ACCEPTED`n", (New-Object System.Text.UTF8Encoding($false)))
    $staleHashBefore = (Get-FileHash -LiteralPath $staleExe -Algorithm SHA256).Hash
    $r = Invoke-Wrapper -WrapperPath $Wrapper -BuildDir $staleBuild
    Assert-True ($r.ExitCode -ne 0) "wrapper accepted a preexisting stale target executable"
    Assert-True ($r.Output -match 'already contains target executable') "preexisting executable error not reported:`n$($r.Output)"
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $staleBuild 'mock-cmake-invoked.txt'))) "mock build ran despite a preexisting stale executable"
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $staleBuild 'build-evidence.json'))) "build evidence written for a preexisting stale executable"
    Assert-True ((Get-FileHash -LiteralPath $staleExe -Algorithm SHA256).Hash -eq $staleHashBefore) "wrapper modified/deleted the preexisting executable"
    Pass "preexisting stale target executable is rejected without deleting user files"

    # --- 3. Git command failure is reported honestly -------------------------
    $gitErrorBuild = Join-Path $WorkDir 'git-error-build'
    Write-MockCache -BuildDir $gitErrorBuild -SourceDir $NoGitRoot -Diagnostics 'OFF'
    # Ceiling must be the parent of the fixture root, not the fixture root
    # itself, so Git cannot discover the enclosing user worktree.
    $gitCeiling = Split-Path -Parent $NoGitRoot
    $r = Invoke-Wrapper -WrapperPath $NoGitWrapper -BuildDir $gitErrorBuild -GitCeilingDirectories $gitCeiling
    Assert-True ($r.ExitCode -ne 0) "wrapper accepted a non-Git worktree"
    Assert-True ($r.Output -match 'git status --porcelain failed \(exit [0-9]+\)') "Git command failure was not reported explicitly:`n$($r.Output)"
    Assert-True ($r.Output -notmatch 'Source working tree is not clean') "Git command failure was misreported as a dirty tree:`n$($r.Output)"
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $gitErrorBuild 'mock-cmake-invoked.txt'))) "mock build ran despite a Git command failure"
    Pass "Git command errors fail explicitly before the build"

    # --- 4. valid clean mock build -------------------------------------------
    $validBuild = Join-Path $WorkDir 'valid-clean-build'
    Write-MockCache -BuildDir $validBuild -SourceDir $Fixture.RepoRoot -Diagnostics 'OFF'
    $r = Invoke-Wrapper -WrapperPath $Wrapper -BuildDir $validBuild
    Assert-True ($r.ExitCode -eq 0) "valid clean mock build failed:`n$($r.Output)"
    Assert-True (Test-Path -LiteralPath (Join-Path $validBuild 'mock-cmake-invoked.txt')) "mock build was not invoked for the valid case"
    $evidencePath = Join-Path $validBuild 'build-evidence.json'
    Assert-True (Test-Path -LiteralPath $evidencePath -PathType Leaf) "valid clean mock build did not write build-evidence.json"
    $evidence = Get-Content -LiteralPath $evidencePath -Raw | ConvertFrom-Json
    Assert-True ($evidence.sourceCommitSha -eq $Fixture.Head) "build evidence commit does not match fixture HEAD"
    Assert-True ($evidence.sourceTreeSha -eq $Fixture.Tree) "build evidence tree does not match fixture HEAD^{tree}"
    Assert-True ($evidence.configuration -eq 'Release') "build evidence configuration is not Release"
    Assert-True ($evidence.diagnostics -eq 'OFF') "build evidence diagnostics is not OFF"
    Assert-True ($evidence.exitCode -eq 0) "build evidence exitCode is not 0"
    Assert-True ($evidence.target -eq 'ozviewer') "build evidence target is not ozviewer"
    Assert-True ($evidence.targetExeAbsentBeforeBuild -eq $true) "build evidence did not record the pre-build target-exe check"
    $expectedCacheSource = [System.IO.Path]::GetFullPath($Fixture.RepoRoot).TrimEnd('\')
    Assert-True ($evidence.cacheSourceDir -eq $expectedCacheSource) "build evidence cacheSourceDir is wrong"
    $mockExe = Join-Path $validBuild 'src\viewer\Release\ozviewer.exe'
    $mockSha = (Get-FileHash -LiteralPath $mockExe -Algorithm SHA256).Hash.ToLowerInvariant()
    Assert-True ($evidence.builtOzviewerSha256 -eq $mockSha) "build evidence exe hash does not match the mock output"
    Pass "valid clean mock build records honest fixture commit/tree and synthetic output hash"

    # --- 5. mock build failure writes no evidence ----------------------------
    $failedBuild = Join-Path $WorkDir 'mock-failure-build'
    Write-MockCache -BuildDir $failedBuild -SourceDir $Fixture.RepoRoot -Diagnostics 'OFF'
    $r = Invoke-Wrapper -WrapperPath $Wrapper -BuildDir $failedBuild -MockExit '7'
    Assert-True ($r.ExitCode -ne 0) "wrapper accepted a failing mock build"
    Assert-True ($r.Output -match 'CMake build failed with exit code 7') "mock build failure was not reported:`n$($r.Output)"
    Assert-True (Test-Path -LiteralPath (Join-Path $failedBuild 'mock-cmake-invoked.txt')) "mock build was not invoked for the failure case"
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $failedBuild 'build-evidence.json'))) "build evidence written after a failed mock build"
    Pass "failed mock build does not write build evidence"

    # --- 6. ambiguous post-build executables fail closed ---------------------
    $ambiguousBuild = Join-Path $WorkDir 'ambiguous-output-build'
    Write-MockCache -BuildDir $ambiguousBuild -SourceDir $Fixture.RepoRoot -Diagnostics 'OFF'
    $r = Invoke-Wrapper -WrapperPath $Wrapper -BuildDir $ambiguousBuild -CreateAmbiguous '1'
    Assert-True ($r.ExitCode -ne 0) "wrapper selected an arbitrary binary from ambiguous mock output"
    Assert-True ($r.Output -match 'Ambiguous post-build target executables') "ambiguous post-build output was not reported:`n$($r.Output)"
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $ambiguousBuild 'build-evidence.json'))) "build evidence written for ambiguous post-build output"
    Pass "ambiguous post-build target executables are rejected"

    Write-Host "release-wrapper-test: ALL CASES PASSED (synthetic mock build evidence; real Release build remains a separate release step)" -ForegroundColor Green
    exit 0
}
catch {
    Write-Host "release-wrapper-test: FAIL: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}

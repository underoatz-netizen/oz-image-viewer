<#
.SYNOPSIS
  Focused test suite for v0.1.1 release envelope, packaging, and provenance boundary (PikaView).

.DESCRIPTION
  Exercises actual production scripts using isolated unique temporary fixture directories.
  Never modifies git config, never assumes the real repository is dirty, and never mutates
  production code.

  The packaging/build chain uses SYNTHETIC, hand-authored contract-test inputs
  (dummy PEs, CMakeCache, and build-evidence). They are never real build evidence
  or release provenance. The generator positive chain additionally needs the
  exact pinned real libheif/libde265 tarballs; when they are absent or do not
  match, this test reports the required-fixture omission explicitly and exits 77
  (CTest skipped) instead of printing a false pass.
#>
[CmdletBinding()]
param(
    [string]$ScratchDir = "",
    # Optional explicit directory containing the two pinned real upstream
    # tarballs. When omitted, deterministic candidate locations are checked and
    # a verified match is required before the generator positive chain runs.
    [string]$LgplTarballDir = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = (Resolve-Path -LiteralPath (Split-Path -Parent $ScriptDir)).Path
$GeneratorScript = Join-Path $ScriptDir 'generate-build-provenance.ps1'
$PackageScript = Join-Path $ScriptDir 'package-portable.ps1'
$ValidatorScript = Join-Path $ScriptDir 'validate-portable-payload.ps1'
$LgplScript = Join-Path $ScriptDir 'build-lgpl-source-package.ps1'
$FixtureHelper = Join-Path $ScriptDir 'test-contract-fixtures.ps1'
if (-not (Test-Path -LiteralPath $FixtureHelper -PathType Leaf)) {
    Write-Host "envelope-test: SKIP: contract fixture helper not found: $FixtureHelper" -ForegroundColor Yellow
    exit 77
}
. $FixtureHelper
if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Write-Host "envelope-test: SKIP: git is required to construct the isolated contract-test fixture repositories" -ForegroundColor Yellow
    exit 77
}
$positiveChainOmitted = $false

function Assert-True($Condition, [string]$Message) {
    if (-not [bool]$Condition) { throw "ASSERTION FAILED: $Message" }
}

function Pass([string]$Message) {
    Write-Host "envelope-test: PASS: $Message" -ForegroundColor Green
}

function Invoke-Script {
    param(
        [Parameter(Mandatory = $true)][string]$ScriptPath,
        [string[]]$Arguments = @()
    )
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out = & powershell -NoProfile -ExecutionPolicy Bypass -File $ScriptPath @Arguments *>&1
        $code = $LASTEXITCODE
        return [pscustomobject]@{
            ExitCode = $code
            Output   = (($out | ForEach-Object { $_.ToString() }) -join "`n")
        }
    }
    finally {
        $ErrorActionPreference = $prev
    }
}

if ([string]::IsNullOrWhiteSpace($ScratchDir)) {
    $ScratchDir = Join-Path ([System.IO.Path]::GetTempPath()) ("opencode\ozviewer-v011-envelope-tests-" + [System.Guid]::NewGuid().ToString("N"))
}
if (-not (Test-Path -LiteralPath $ScratchDir -PathType Container)) {
    New-Item -ItemType Directory -Path $ScratchDir -Force | Out-Null
}
$ScratchDir = (Resolve-Path -LiteralPath $ScratchDir).Path

function New-DummyPe64 {
    param([string]$Path, [uint16]$Machine = 0x8664)
    $bytes = New-Object byte[] 1024
    $bytes[0] = 0x4D # 'M'
    $bytes[1] = 0x5A # 'Z'
    $peOff = 0x80
    [BitConverter]::GetBytes([uint32]$peOff).CopyTo($bytes, 0x3C)
    $bytes[$peOff] = [byte][char]'P'
    $bytes[$peOff + 1] = [byte][char]'E'
    [BitConverter]::GetBytes([uint16]$Machine).CopyTo($bytes, $peOff + 4)
    [BitConverter]::GetBytes([uint16]0x20B).CopyTo($bytes, $peOff + 24)
    $bytes[$peOff + 26] = 14
    $bytes[$peOff + 27] = 44
    [System.IO.File]::WriteAllBytes($Path, $bytes)
}

try {
    # -----------------------------------------------------------------------
    # TEST 1: Version and Package Name Alignment
    # -----------------------------------------------------------------------
    $liveVersion = '0.1.2'
    $cmakeText = Get-Content -LiteralPath (Join-Path $RepoRoot "CMakeLists.txt") -Raw
    Assert-True ($cmakeText -match 'project\(ozbench\s+VERSION\s+0\.1\.2') "CMakeLists.txt project version not $liveVersion"

    $vcpkgJson = Get-Content -LiteralPath (Join-Path $RepoRoot "vcpkg.json") -Raw | ConvertFrom-Json
    Assert-True ($vcpkgJson."version-string" -eq $liveVersion) "vcpkg.json version-string not $liveVersion"

    $portJson = Get-Content -LiteralPath (Join-Path $ScriptDir "portable-package.json") -Raw | ConvertFrom-Json
    Assert-True ($portJson.version -eq $liveVersion) "portable-package.json version not $liveVersion"
    Assert-True ($portJson.packageName -eq "pikaview") "portable-package.json packageName not pikaview"
    Assert-True ($portJson.entrypoint -eq "ozviewer.exe") "portable-package.json entrypoint must remain ozviewer.exe"

    $runnerMain = Get-Content -LiteralPath (Join-Path $RepoRoot "src\runner\main.cpp") -Raw
    Assert-True ($runnerMain.Contains('OZBENCH_PROJECT_VERSION')) "src/runner/main.cpp does not use OZBENCH_PROJECT_VERSION macro"
    Assert-True ($runnerMain.Contains('#define OZBENCH_PROJECT_VERSION "' + $liveVersion + '"')) "src/runner/main.cpp fallback version not $liveVersion"

    Pass "Version $liveVersion and packageName pikaview aligned across CMakeLists.txt, vcpkg.json, portable-package.json, and runner fallback"

    # -----------------------------------------------------------------------
    # TEST 2: Old v0.1.0 BUILD-PROVENANCE.json preservation & no root fallback
    # -----------------------------------------------------------------------
    Assert-True (Test-Path -LiteralPath (Join-Path $RepoRoot "docs\releases\v0.1.0\BUILD-PROVENANCE.json") -PathType Leaf) "Old v0.1.0 BUILD-PROVENANCE.json not preserved in docs/releases/v0.1.0/"
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $RepoRoot "BUILD-PROVENANCE.json") -PathType Leaf)) "Root BUILD-PROVENANCE.json must not exist as an uncommitted fallback"
    Pass "v0.1.0 BUILD-PROVENANCE.json safely archived in docs/releases/v0.1.0/ and root fallback removed"

    # -----------------------------------------------------------------------
    # TEST 3: Generator Fail-Closed Checks in Isolated Fixture Repo
    # -----------------------------------------------------------------------
    $runId = [System.Guid]::NewGuid().ToString("N").Substring(0, 8)
    $testGenDir = Join-Path $ScratchDir "gen-$runId"
    New-Item -ItemType Directory -Path $testGenDir -Force | Out-Null

    # 3a. Rejects inside-repo destination path (acyclic defense)
    $insideRepoDest = Join-Path $RepoRoot "BUILD-PROVENANCE.json"
    $res = Invoke-Script -ScriptPath $GeneratorScript -Arguments @('-OutPath', $insideRepoDest, '-StagedBinariesDir', $testGenDir, '-LgplSourceZipPath', (Join-Path $testGenDir "dummy.zip"), '-BuildDir', $testGenDir)
    Assert-True ($res.ExitCode -ne 0) "Generator allowed writing into repo root"
    Assert-True ($res.Output -match "resides inside repository root") "Error did not flag inside-repo OutPath"
    Pass "Generator rejects OutPath inside repository root"

    # Setup isolated clean git fixture repo (no git config writes!)
    $fixtureRepo = Join-Path $testGenDir "fixture-repo"
    New-Item -ItemType Directory -Path $fixtureRepo -Force | Out-Null
    & git -C $fixtureRepo init -q
    $fixturePkgDir = Join-Path $fixtureRepo "packaging"
    New-Item -ItemType Directory -Path $fixturePkgDir -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $ScriptDir "portable-package.json") -Destination $fixturePkgDir -Force
    # This isolated generator/E2E fixture is intentionally frozen to v0.1.1, so
    # its fixed synthetic asset names below are consistent. Live metadata
    # alignment is asserted separately in TEST 1.
    $fixtureMetadataPath = Join-Path $fixturePkgDir "portable-package.json"
    $fixtureMetadata = Get-Content -LiteralPath $fixtureMetadataPath -Raw | ConvertFrom-Json
    $fixtureMetadata.version = '0.1.1'
    [System.IO.File]::WriteAllText($fixtureMetadataPath, ($fixtureMetadata | ConvertTo-Json -Depth 8), (New-Object System.Text.UTF8Encoding($false)))
    Copy-Item -LiteralPath $GeneratorScript -Destination $fixturePkgDir -Force
    Copy-Item -LiteralPath (Join-Path $ScriptDir "lgpl-dependency-policy.json") -Destination $fixturePkgDir -Force
    $fixtureTripletDir = Join-Path $fixtureRepo "triplets"
    New-Item -ItemType Directory -Path $fixtureTripletDir -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $RepoRoot "triplets\oz-x64-windows-static-md.cmake") -Destination $fixtureTripletDir -Force
    [System.IO.File]::WriteAllText((Join-Path $fixtureRepo "README.md"), "clean repo")
    & git -C $fixtureRepo add .
    & git -C $fixtureRepo -c user.email="test@example.com" -c user.name="Test Committer" commit -q -m "initial commit"

    $fixtureGenScript = Join-Path $fixturePkgDir "generate-build-provenance.ps1"
    $externalDest = Join-Path $testGenDir "BUILD-PROVENANCE.json"

    # 3b. Rejects dirty working tree in isolated fixture
    [System.IO.File]::WriteAllText((Join-Path $fixtureRepo "untracked.txt"), "dirty")
    $r = & powershell -NoProfile -ExecutionPolicy Bypass -File $fixtureGenScript -OutPath $externalDest -StagedBinariesDir $testGenDir -LgplSourceZipPath (Join-Path $testGenDir "dummy.zip") -BuildDir $testGenDir 2>&1
    Assert-True ($LASTEXITCODE -ne 0) "Generator succeeded on dirty working tree"
    Assert-True ($r -match "Source working tree is not clean") "Error did not flag unclean working tree"
    Remove-Item -LiteralPath (Join-Path $fixtureRepo "untracked.txt") -Force
    Pass "Generator fails closed on dirty working tree (no AllowDirty bypass)"

    # Setup fixture binaries & vcpkg
    $fixtureBinsDir = Join-Path $testGenDir "fixture-bins"
    New-Item -ItemType Directory -Path $fixtureBinsDir -Force | Out-Null
    New-DummyPe64 -Path (Join-Path $fixtureBinsDir "ozviewer.exe")
    New-DummyPe64 -Path (Join-Path $fixtureBinsDir "heif.dll")
    New-DummyPe64 -Path (Join-Path $fixtureBinsDir "libde265.dll")
    New-DummyPe64 -Path (Join-Path $fixtureBinsDir "msvcp140.dll")

    $dependencyFixture = New-TestVcpkgDependencyFixture -Root (Join-Path $testGenDir "dependency-fixture") `
        -PolicyPath (Join-Path $fixturePkgDir "lgpl-dependency-policy.json") `
        -StagedBinariesDir $fixtureBinsDir
    $fixtureVcpkg = $dependencyFixture.VcpkgRoot
    Set-TestVcpkgConfiguration -RepoRoot $fixtureRepo -Baseline $dependencyFixture.Baseline
    & git -C $fixtureRepo add .
    & git -C $fixtureRepo -c user.email="test@example.com" -c user.name="Test Committer" commit -q -m "synthetic vcpkg baseline binding"
    $fixHead = (& git -C $fixtureRepo rev-parse HEAD).Trim()
    $fixTree = (& git -C $fixtureRepo rev-parse "HEAD^{tree}").Trim()

    # Setup fixture LGPL zip
    Add-Type -AssemblyName System.IO.Compression.FileSystem | Out-Null
    $zipStageDir = Join-Path $testGenDir "zip-stage"
    New-Item -ItemType Directory -Path (Join-Path $zipStageDir "upstream-sources") -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $zipStageDir "vcpkg-ports\libheif") -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $zipStageDir "vcpkg-ports\libde265") -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $zipStageDir "triplets") -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $zipStageDir "licenses") -Force | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $zipStageDir "vcpkg-ports\libheif\portfile.cmake"), "# portfile")
    [System.IO.File]::WriteAllText((Join-Path $zipStageDir "vcpkg-ports\libde265\portfile.cmake"), "# portfile")
    [System.IO.File]::WriteAllText((Join-Path $zipStageDir "vcpkg-ports\libheif\vcpkg.json"), '{"name":"libheif","version":"1.23.2","port-version":0}')
    [System.IO.File]::WriteAllText((Join-Path $zipStageDir "vcpkg-ports\libde265\vcpkg.json"), '{"name":"libde265","version":"1.1.1","port-version":0}')
    [System.IO.File]::WriteAllText((Join-Path $zipStageDir "triplets\oz-x64-windows-static-md.cmake"), "# triplet")
    [System.IO.File]::WriteAllText((Join-Path $zipStageDir "licenses\libheif-COPYING.txt"), "# license")
    [System.IO.File]::WriteAllText((Join-Path $zipStageDir "licenses\libde265-COPYING.txt"), "# license")
    [System.IO.File]::WriteAllText((Join-Path $zipStageDir "README-BUILD-LGPL.txt"), "# instructions")
    [System.IO.File]::WriteAllText((Join-Path $zipStageDir "build-lgpl-dlls.ps1"), "# build script")
    [System.IO.File]::WriteAllText((Join-Path $zipStageDir "upstream-sources\strukturag-libheif-v1.23.2.tar.gz"), "dummy heif tarball")
    [System.IO.File]::WriteAllText((Join-Path $zipStageDir "upstream-sources\strukturag-libde265-v1.1.1.tar.gz"), "dummy de265 tarball")
    $fixtureLgplZip = Join-Path $testGenDir "pikaview-0.1.1-lgpl-corresponding-source.zip"
    [System.IO.Compression.ZipFile]::CreateFromDirectory($zipStageDir, $fixtureLgplZip)

    # 3c. Rejects missing CMakeCache.txt
    $emptyBuildDir = Join-Path $testGenDir "empty-build"
    New-Item -ItemType Directory -Path $emptyBuildDir -Force | Out-Null
    $r = & powershell -NoProfile -ExecutionPolicy Bypass -File $fixtureGenScript -OutPath $externalDest -StagedBinariesDir $fixtureBinsDir -LgplSourceZipPath $fixtureLgplZip -BuildDir $emptyBuildDir 2>&1
    Assert-True ($LASTEXITCODE -ne 0) "Generator accepted BuildDir without CMakeCache.txt"
    Assert-True ($r -match "missing CMakeCache.txt") "Error did not flag missing CMakeCache.txt"
    Pass "Generator rejects BuildDir missing CMakeCache.txt"

    # 3d. Rejects CMakeCache with mismatched source directory
    $badSrcBuildDir = Join-Path $testGenDir "bad-src-build"
    New-Item -ItemType Directory -Path $badSrcBuildDir -Force | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $badSrcBuildDir "CMakeCache.txt"), "CMAKE_HOME_DIRECTORY:INTERNAL=C:/some/other/repo`nOZVIEWER_PRINT_DIAGNOSTICS:BOOL=OFF`nCMAKE_BUILD_TYPE:STRING=Release`nCMAKE_EXE_LINKER_FLAGS:STRING=/machine:x64`n")
    $r = & powershell -NoProfile -ExecutionPolicy Bypass -File $fixtureGenScript -OutPath $externalDest -StagedBinariesDir $fixtureBinsDir -LgplSourceZipPath $fixtureLgplZip -BuildDir $badSrcBuildDir -VcpkgRoot $fixtureVcpkg 2>&1
    Assert-True ($LASTEXITCODE -ne 0) "Generator accepted mismatched source directory"
    Assert-True ($r -match "CMakeCache source dir mismatch") "Error did not flag source dir mismatch"
    Pass "Generator rejects CMakeCache with wrong source directory"

    $normFixRepo = $fixtureRepo.Replace('\', '/')

    # 3e. Rejects non-Release CMAKE_BUILD_TYPE=Debug
    $debugBuildDir = Join-Path $testGenDir "debug-build"
    New-Item -ItemType Directory -Path $debugBuildDir -Force | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $debugBuildDir "CMakeCache.txt"), "CMAKE_HOME_DIRECTORY:INTERNAL=$normFixRepo`nOZVIEWER_PRINT_DIAGNOSTICS:BOOL=OFF`nCMAKE_BUILD_TYPE:STRING=Debug`nCMAKE_EXE_LINKER_FLAGS:STRING=/machine:x64`n")
    $r = & powershell -NoProfile -ExecutionPolicy Bypass -File $fixtureGenScript -OutPath $externalDest -StagedBinariesDir $fixtureBinsDir -LgplSourceZipPath $fixtureLgplZip -BuildDir $debugBuildDir -VcpkgRoot $fixtureVcpkg 2>&1
    Assert-True ($LASTEXITCODE -ne 0) "Generator accepted CMAKE_BUILD_TYPE=Debug"
    Assert-True ($r -match "requires 'Release'") "Error did not flag non-Release build type"
    Pass "Generator rejects CMAKE_BUILD_TYPE=Debug"

    # 3f. Rejects multi-config CMakeCache without Release build output
    $multiNoRelDir = Join-Path $testGenDir "multi-no-rel"
    New-Item -ItemType Directory -Path $multiNoRelDir -Force | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $multiNoRelDir "CMakeCache.txt"), "CMAKE_HOME_DIRECTORY:INTERNAL=$normFixRepo`nOZVIEWER_PRINT_DIAGNOSTICS:BOOL=OFF`nCMAKE_CONFIGURATION_TYPES:STRING=Debug;Release`nCMAKE_EXE_LINKER_FLAGS:STRING=/machine:x64`n")
    $r = & powershell -NoProfile -ExecutionPolicy Bypass -File $fixtureGenScript -OutPath $externalDest -StagedBinariesDir $fixtureBinsDir -LgplSourceZipPath $fixtureLgplZip -BuildDir $multiNoRelDir -VcpkgRoot $fixtureVcpkg 2>&1
    Assert-True ($LASTEXITCODE -ne 0) "Generator accepted multi-config without Release build output"
    Assert-True ($r -match "no observed Release ozviewer.exe") "Error did not flag missing Release build output"
    Pass "Generator rejects multi-config CMakeCache lacking Release build output"

    # 3g. Rejects diagnostics ON
    $diagOnBuildDir = Join-Path $testGenDir "diag-on-build"
    New-Item -ItemType Directory -Path $diagOnBuildDir -Force | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $diagOnBuildDir "CMakeCache.txt"), "CMAKE_HOME_DIRECTORY:INTERNAL=$normFixRepo`nOZVIEWER_PRINT_DIAGNOSTICS:BOOL=ON`nCMAKE_BUILD_TYPE:STRING=Release`nCMAKE_EXE_LINKER_FLAGS:STRING=/machine:x64`n")
    $r = & powershell -NoProfile -ExecutionPolicy Bypass -File $fixtureGenScript -OutPath $externalDest -StagedBinariesDir $fixtureBinsDir -LgplSourceZipPath $fixtureLgplZip -BuildDir $diagOnBuildDir -VcpkgRoot $fixtureVcpkg 2>&1
    Assert-True ($LASTEXITCODE -ne 0) "Generator accepted diagnostics ON"
    Assert-True ($r -match "diagnostics explicitly OFF") "Error did not flag diagnostics ON"
    Pass "Generator rejects diagnostics ON"

    # 3h. Rejects unobserved compiler / vcpkg
    $noCompDir = Join-Path $testGenDir "no-comp-build"
    New-Item -ItemType Directory -Path $noCompDir -Force | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $noCompDir "CMakeCache.txt"), "CMAKE_HOME_DIRECTORY:INTERNAL=$normFixRepo`nOZVIEWER_PRINT_DIAGNOSTICS:BOOL=OFF`nCMAKE_BUILD_TYPE:STRING=Release`nCMAKE_EXE_LINKER_FLAGS:STRING=/machine:x64`n")
    $r = & powershell -NoProfile -ExecutionPolicy Bypass -File $fixtureGenScript -OutPath $externalDest -StagedBinariesDir $fixtureBinsDir -LgplSourceZipPath $fixtureLgplZip -BuildDir $noCompDir -VcpkgRoot "non-existent-vcpkg" 2>&1
    Assert-True ($LASTEXITCODE -ne 0) "Generator accepted unobserved compiler/vcpkg"
    Pass "Generator rejects unobserved compiler and unobserved vcpkg baseline"

    # 3h-ii. Rejects missing build-evidence.json
    $cleanBuildDir = Join-Path $testGenDir "clean-build"
    New-Item -ItemType Directory -Path $cleanBuildDir -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $fixtureBinsDir "ozviewer.exe") -Destination (Join-Path $cleanBuildDir "ozviewer.exe") -Force
    $normClPath = "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe"
    $normCmPath = "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
    [System.IO.File]::WriteAllText((Join-Path $cleanBuildDir "CMakeCache.txt"), "CMAKE_HOME_DIRECTORY:INTERNAL=$normFixRepo`nOZVIEWER_PRINT_DIAGNOSTICS:BOOL=OFF`nCMAKE_BUILD_TYPE:STRING=Release`nCMAKE_EXE_LINKER_FLAGS:STRING=/machine:x64`nCMAKE_CXX_COMPILER:FILEPATH=$normClPath`nCMAKE_COMMAND:INTERNAL=$normCmPath`n")
    $r = Invoke-Script -ScriptPath $fixtureGenScript -Arguments @('-OutPath', $externalDest, '-StagedBinariesDir', $fixtureBinsDir, '-LgplSourceZipPath', $fixtureLgplZip, '-BuildDir', $cleanBuildDir, '-VcpkgRoot', $fixtureVcpkg)
    Assert-True ($r.ExitCode -ne 0) "Generator accepted missing build-evidence.json"
    Assert-True ($r.Output -match "Explicit controlled build evidence is required") "Error did not flag missing build-evidence.json"
    Pass "Generator rejects missing build-evidence.json"

    # Write valid build-evidence.json for fixture repo
    $fixHead = (& git -C $fixtureRepo rev-parse HEAD).Trim()
    $fixTree = (& git -C $fixtureRepo rev-parse "HEAD^{tree}").Trim()
    $builtExeSha = (Get-FileHash -LiteralPath (Join-Path $cleanBuildDir "ozviewer.exe") -Algorithm SHA256).Hash.ToLowerInvariant()
    $buildEvObj = [ordered]@{
        schema              = 1
        wrapper             = "invoke-release-build.ps1"
        target              = "ozviewer"
        buildDir            = $cleanBuildDir
        sourceCommitSha     = $fixHead
        sourceTreeSha       = $fixTree
        configuration       = "Release"
        diagnostics         = "OFF"
        buildCommand        = "cmake --build build --config Release"
        exitCode            = 0
        builtOzviewerSha256 = $builtExeSha
    }
    [System.IO.File]::WriteAllText((Join-Path $cleanBuildDir "build-evidence.json"), ($buildEvObj | ConvertTo-Json))

    # 3i. Rejects fake nonempty LGPL source zip

    $incompleteZipStage = Join-Path $testGenDir "incomplete-zip-stage"
    New-Item -ItemType Directory -Path $incompleteZipStage -Force | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $incompleteZipStage "some-random-file.txt"), "not a portfile")
    $fakeLgplZip = Join-Path $testGenDir "pikaview-0.1.1-lgpl-corresponding-source-fake.zip"
    if (Test-Path -LiteralPath $fakeLgplZip) { Remove-Item -LiteralPath $fakeLgplZip -Force }
    [System.IO.Compression.ZipFile]::CreateFromDirectory($incompleteZipStage, $fakeLgplZip)
    # Rename to expected name for content test
    $fakeNamedZip = Join-Path $testGenDir "pikaview-0.1.1-lgpl-corresponding-source.zip"
    if (Test-Path -LiteralPath $fakeNamedZip) { Remove-Item -LiteralPath $fakeNamedZip -Force }
    Copy-Item -LiteralPath $fakeLgplZip -Destination $fakeNamedZip -Force
    $r = Invoke-Script -ScriptPath $fixtureGenScript -Arguments @('-OutPath', $externalDest, '-StagedBinariesDir', $fixtureBinsDir, '-LgplSourceZipPath', $fakeNamedZip, '-BuildDir', $cleanBuildDir, '-VcpkgRoot', $fixtureVcpkg)
    Assert-True ($r.ExitCode -ne 0) "Generator accepted fake nonempty LGPL zip"
    Assert-True ($r.Output -match "missing required files") "Error did not flag missing required files. Got: $($r.Output)"
    Pass "Generator rejects fake nonempty LGPL source zip"

    # 3j. Rejects tampered upstream tarball in LGPL zip
    Remove-Item -LiteralPath $fakeNamedZip -Force
    [System.IO.Compression.ZipFile]::CreateFromDirectory($zipStageDir, $fakeNamedZip)
    $r = Invoke-Script -ScriptPath $fixtureGenScript -Arguments @('-OutPath', $externalDest, '-StagedBinariesDir', $fixtureBinsDir, '-LgplSourceZipPath', $fakeNamedZip, '-BuildDir', $cleanBuildDir, '-VcpkgRoot', $fixtureVcpkg)
    Assert-True ($r.ExitCode -ne 0) "Generator accepted tampered tarball in LGPL zip"
    Assert-True ($r.Output -match "does not match dependency policy") "Error did not flag mismatched tarball hash"
    Pass "Generator rejects tampered upstream tarball in LGPL source zip"

    # -----------------------------------------------------------------------
    # TEST 4: Packaging & Validator Pipeline (Non-Destructive + Closure Binding)
    # -----------------------------------------------------------------------
    $pkgTestDir = Join-Path $ScratchDir "pkg-$runId"
    New-Item -ItemType Directory -Path $pkgTestDir -Force | Out-Null

    $payloadDir = Join-Path $pkgTestDir "payload"
    New-Item -ItemType Directory -Path $payloadDir -Force | Out-Null
    New-DummyPe64 -Path (Join-Path $payloadDir "ozviewer.exe")
    New-DummyPe64 -Path (Join-Path $payloadDir "heif.dll")
    New-DummyPe64 -Path (Join-Path $payloadDir "libde265.dll")
    $exePath = Join-Path $payloadDir "ozviewer.exe"
    $exeBytes = [System.IO.File]::ReadAllBytes($exePath)
    $extAscii = [System.Text.Encoding]::ASCII.GetBytes("*.jpg`0*.jpeg`0*.png`0*.webp`0*.avif`0")
    $newBytes = New-Object byte[] ($exeBytes.Length + $extAscii.Length)
    [Array]::Copy($exeBytes, $newBytes, $exeBytes.Length)
    [Array]::Copy($extAscii, 0, $newBytes, $exeBytes.Length, $extAscii.Length)
    [System.IO.File]::WriteAllBytes($exePath, $newBytes)

    # 4a. Fails closed when BUILD-PROVENANCE is missing
    $outMissing = Join-Path $pkgTestDir "out-missing"
    $r = & powershell -NoProfile -ExecutionPolicy Bypass -File $PackageScript -PayloadDir $payloadDir -OutDir $outMissing 2>&1
    Assert-True ($LASTEXITCODE -ne 0) "Packaging succeeded without BUILD-PROVENANCE"
    Assert-True ($r -match "BUILD-PROVENANCE\.json is required") "Error did not flag missing BUILD-PROVENANCE"
    Pass "package-portable.ps1 fails closed when BUILD-PROVENANCE is missing"

    # 4b. Fails closed on preexisting stage directory (no destructive overwrite)
    $outPreexist = Join-Path $pkgTestDir "out-preexist"
    $preStage = Join-Path $outPreexist "pikaview-$liveVersion-win-x64-portable"
    New-Item -ItemType Directory -Path $preStage -Force | Out-Null
    $r = Invoke-Script -ScriptPath $PackageScript -Arguments @('-PayloadDir', $payloadDir, '-OutDir', $outPreexist)
    Assert-True ($r.ExitCode -ne 0) "Packaging overwrote preexisting stage directory"
    Assert-True ($r.Output -match "refusing destructive overwrite") "Error did not flag preexisting stage directory"
    Pass "package-portable.ps1 refuses destructive overwrite of preexisting stage directory"

    # 4c. Always-run synthetic contract chain: package-portable -> validator.
    # This is contract-test evidence only, not a real build. It exercises the
    # real production scripts against a clean throwaway fixture repository.
    $contractFixture = New-TestPackagingFixture -Root (Join-Path $ScratchDir "contract-fixture-$runId") `
        -SourceScriptDir $ScriptDir -SourceRepoRoot $RepoRoot
    $contractPayload = Join-Path $pkgTestDir 'contract-payload'
    New-TestPayload -Dir $contractPayload
    $contractLgplZip = Join-Path $pkgTestDir 'pikaview-0.1.1-lgpl-corresponding-source.zip'
    $contractLgplSha = New-TestLgplFixtureAsset -Path $contractLgplZip
    $contractProv = New-TestBuildProvenance -Path (Join-Path $pkgTestDir 'contract-provenance\BUILD-PROVENANCE.json') `
        -CommitSha $contractFixture.Head -TreeSha $contractFixture.Tree -PayloadDir $contractPayload -LgplAssetPath $contractLgplZip
    $contractPkgOut = Join-Path $pkgTestDir 'contract-pkg-out'
    $rContractPkg = Invoke-Script -ScriptPath (Join-Path $contractFixture.PackagingDir 'package-portable.ps1') -Arguments @('-PayloadDir', $contractPayload, '-OutDir', $contractPkgOut, '-ProvenancePath', $contractProv.Path)
    Assert-True ($rContractPkg.ExitCode -eq 0) "contract package-portable failed: $($rContractPkg.Output)"
    $contractStage = Join-Path $contractPkgOut 'pikaview-0.1.1-win-x64-portable'
    $contractValScript = Join-Path $contractFixture.PackagingDir 'validate-portable-payload.ps1'
    $rContractVal = Invoke-Script -ScriptPath $contractValScript -Arguments @('-PackageDir', $contractStage, '-ExpectedCommitSha', $contractFixture.Head, '-ExpectedTreeSha', $contractFixture.Tree, '-ExpectedLgplSourceSha256', $contractLgplSha)
    Assert-True ($rContractVal.ExitCode -eq 0) "contract validate-portable-payload failed: $($rContractVal.Output)"
    Pass "contract: package-portable + validator positive chain (synthetic fixture, not real build evidence)"

    # Negative controlled-binding cases. These do not depend on real tarballs.
    $rContractMissing = Invoke-Script -ScriptPath $contractValScript -Arguments @('-PackageDir', $contractStage)
    Assert-True ($rContractMissing.ExitCode -ne 0) "Validator accepted missing ExpectedCommitSha"
    Assert-True ($rContractMissing.Output -match "ExpectedCommitSha is required") "Error did not flag missing ExpectedCommitSha"

    $rContractMissingTree = Invoke-Script -ScriptPath $contractValScript -Arguments @('-PackageDir', $contractStage, '-ExpectedCommitSha', $contractFixture.Head, '-ExpectedLgplSourceSha256', $contractLgplSha)
    Assert-True ($rContractMissingTree.ExitCode -ne 0) "Validator accepted missing ExpectedTreeSha"
    Assert-True ($rContractMissingTree.Output -match "ExpectedTreeSha is required") "Error did not flag missing ExpectedTreeSha"

    $rContractMissingLgpl = Invoke-Script -ScriptPath $contractValScript -Arguments @('-PackageDir', $contractStage, '-ExpectedCommitSha', $contractFixture.Head, '-ExpectedTreeSha', $contractFixture.Tree)
    Assert-True ($rContractMissingLgpl.ExitCode -ne 0) "Validator accepted missing ExpectedLgplSourceSha256"
    Assert-True ($rContractMissingLgpl.Output -match "ExpectedLgplSourceSha256 is required") "Error did not flag missing ExpectedLgplSourceSha256"

    $rContractWrongCommit = Invoke-Script -ScriptPath $contractValScript -Arguments @('-PackageDir', $contractStage, '-ExpectedCommitSha', ('0' * 40), '-ExpectedTreeSha', $contractFixture.Tree, '-ExpectedLgplSourceSha256', $contractLgplSha)
    Assert-True ($rContractWrongCommit.ExitCode -ne 0) "Validator accepted a wrong ExpectedCommitSha"
    Assert-True ($rContractWrongCommit.Output -match "commitSha .* does not match expected") "Error did not flag wrong ExpectedCommitSha"

    $rContractWrongTree = Invoke-Script -ScriptPath $contractValScript -Arguments @('-PackageDir', $contractStage, '-ExpectedCommitSha', $contractFixture.Head, '-ExpectedTreeSha', ('1' * 40), '-ExpectedLgplSourceSha256', $contractLgplSha)
    Assert-True ($rContractWrongTree.ExitCode -ne 0) "Validator accepted a wrong ExpectedTreeSha"
    Assert-True ($rContractWrongTree.Output -match "treeSha .* does not match expected") "Error did not flag wrong ExpectedTreeSha"

    $rContractWrongLgpl = Invoke-Script -ScriptPath $contractValScript -Arguments @('-PackageDir', $contractStage, '-ExpectedCommitSha', $contractFixture.Head, '-ExpectedTreeSha', $contractFixture.Tree, '-ExpectedLgplSourceSha256', ('2' * 64))
    Assert-True ($rContractWrongLgpl.ExitCode -ne 0) "Validator accepted a wrong ExpectedLgplSourceSha256"
    Assert-True ($rContractWrongLgpl.Output -match "lgplCorrespondingSourceAsset sha256 .* does not match expected") "Error did not flag wrong ExpectedLgplSourceSha256"
    Pass "contract: validator fails closed on missing/wrong commit, tree, and LGPL-source bindings"

    # 4d. Optional real-tarball generator chain:
    # generate-build-provenance -> package-portable -> validate-portable-payload.
    #
    # The generator pins the upstream libheif/libde265 tarball hashes, so this
    # positive chain requires those exact real files. The payload, CMakeCache,
    # and build-evidence remain hand-authored SYNTHETIC contract fixtures. When
    # the tarballs are absent, the chain is reported as a skipped required
    # fixture (exit 77) instead of silently passing.
    $expectedHeifSha256 = "1405ed070421459b569ff49deab109b7f1a30a447e72a9b20a4154f774634a44"
    $expectedDe265Sha256 = "5b4fac677018e6074196e8f9889f3e4a5310e46afbf22a893f620d4e24d3510e"
    $heifTarballName = "strukturag-libheif-v1.23.2.tar.gz"
    $de265TarballName = "strukturag-libde265-v1.1.1.tar.gz"
    $tarballCandidates = New-Object System.Collections.Generic.List[string]
    if (-not [string]::IsNullOrWhiteSpace($LgplTarballDir)) { [void]$tarballCandidates.Add($LgplTarballDir) }
    if (-not [string]::IsNullOrWhiteSpace($env:OZVIEWER_LGPL_TARBALL_DIR)) { [void]$tarballCandidates.Add($env:OZVIEWER_LGPL_TARBALL_DIR) }
    [void]$tarballCandidates.Add((Join-Path $RepoRoot 'packaging\lgpl-fixtures'))
    [void]$tarballCandidates.Add((Join-Path ([System.IO.Path]::GetTempPath()) 'opencode\v010-extracted-be9a05e9'))

    $foundTarballDir = ""
    foreach ($candidate in $tarballCandidates) {
        if (-not (Test-Path -LiteralPath $candidate -PathType Container)) { continue }
        $heifCandidate = Join-Path $candidate $heifTarballName
        $de265Candidate = Join-Path $candidate $de265TarballName
        if (-not ((Test-Path -LiteralPath $heifCandidate -PathType Leaf) -and (Test-Path -LiteralPath $de265Candidate -PathType Leaf))) { continue }
        $heifObserved = (Get-FileHash -LiteralPath $heifCandidate -Algorithm SHA256).Hash.ToLowerInvariant()
        $de265Observed = (Get-FileHash -LiteralPath $de265Candidate -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($heifObserved -eq $expectedHeifSha256 -and $de265Observed -eq $expectedDe265Sha256) {
            $foundTarballDir = $candidate
            break
        }
        Write-Host "envelope-test: WARNING: candidate tarball directory '$candidate' does not match the pinned hashes; ignoring." -ForegroundColor Yellow
    }

    if ([string]::IsNullOrWhiteSpace($foundTarballDir)) {
        $positiveChainOmitted = $true
        Write-Host "envelope-test: CONTRACT FIXTURE OMISSION: the pinned real libheif/libde265 tarballs were not found or did not match." -ForegroundColor Red
        Write-Host "  searched: $($tarballCandidates -join '; ')" -ForegroundColor Yellow
        Write-Host "  required: $heifTarballName SHA-256 $expectedHeifSha256" -ForegroundColor Yellow
        Write-Host "  required: $de265TarballName SHA-256 $expectedDe265Sha256" -ForegroundColor Yellow
        Write-Host "  The generate-build-provenance -> package-portable -> validate-portable-payload positive chain was NOT run." -ForegroundColor Yellow
        Write-Host "  Pass -LgplTarballDir <dir> or set OZVIEWER_LGPL_TARBALL_DIR to run it." -ForegroundColor Yellow
    }
    else {
        $e2eDir = Join-Path $ScratchDir "e2e-$runId"
        New-Item -ItemType Directory -Path $e2eDir -Force | Out-Null

        # Copy packaging scripts into the clean fixtureRepo so package-portable.ps1
        # runs against the fixture worktree; the production gate stays unchanged.
        Copy-Item -LiteralPath (Join-Path $ScriptDir "package-portable.ps1") -Destination $fixturePkgDir -Force
        Copy-Item -LiteralPath (Join-Path $ScriptDir "validate-portable-payload.ps1") -Destination $fixturePkgDir -Force
        Copy-Item -LiteralPath (Join-Path $RepoRoot "LICENSE") -Destination $fixtureRepo -Force
        Copy-Item -LiteralPath (Join-Path $RepoRoot "THIRD-PARTY-NOTICES.md") -Destination $fixtureRepo -Force
        Copy-Item -LiteralPath (Join-Path $RepoRoot "THIRD-PARTY-LICENSES.txt") -Destination $fixtureRepo -Force
        Copy-Item -LiteralPath (Join-Path $RepoRoot "THIRD-PARTY-SOURCE.txt") -Destination $fixtureRepo -Force
        & git -C $fixtureRepo add .
        & git -C $fixtureRepo -c user.email="test@example.com" -c user.name="Test Committer" commit -q -m "add packaging scripts"

        $e2eHead = (& git -C $fixtureRepo rev-parse HEAD).Trim()
        $e2eTree = (& git -C $fixtureRepo rev-parse "HEAD^{tree}").Trim()
        $e2eStatus = @(& git -C $fixtureRepo status --porcelain | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
        Assert-True ($e2eStatus.Count -eq 0) "E2E fixture repository is not clean: $($e2eStatus -join '; ')"

        # Build the LGPL source zip from the real pinned tarballs and the
        # fixture's own vcpkg port recipes. All wrapper metadata is synthetic.
        $realZipStage = Join-Path $e2eDir "real-zip-stage"
        New-Item -ItemType Directory -Path (Join-Path $realZipStage "upstream-sources") -Force | Out-Null
        New-Item -ItemType Directory -Path (Join-Path $realZipStage "triplets") -Force | Out-Null
        New-Item -ItemType Directory -Path (Join-Path $realZipStage "licenses") -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $foundTarballDir $heifTarballName) -Destination (Join-Path $realZipStage "upstream-sources") -Force
        Copy-Item -LiteralPath (Join-Path $foundTarballDir $de265TarballName) -Destination (Join-Path $realZipStage "upstream-sources") -Force
        Copy-Item -LiteralPath (Join-Path $fixtureVcpkg "ports") -Destination (Join-Path $realZipStage "vcpkg-ports") -Recurse -Force
        Copy-Item -LiteralPath (Join-Path $fixtureRepo "triplets\oz-x64-windows-static-md.cmake") -Destination (Join-Path $realZipStage "triplets\oz-x64-windows-static-md.cmake") -Force
        [System.IO.File]::WriteAllText((Join-Path $realZipStage "licenses\libheif-COPYING.txt"), "# synthetic contract-test license placeholder`n")
        [System.IO.File]::WriteAllText((Join-Path $realZipStage "licenses\libde265-COPYING.txt"), "# synthetic contract-test license placeholder`n")
        [System.IO.File]::WriteAllText((Join-Path $realZipStage "README-BUILD-LGPL.txt"), "# synthetic contract-test instructions (not a real rebuild guide)`n")
        [System.IO.File]::WriteAllText((Join-Path $realZipStage "build-lgpl-dlls.ps1"), "# synthetic contract-test build script (not executed)`n")
        $realLgplZip = Join-Path $e2eDir "pikaview-0.1.1-lgpl-corresponding-source.zip"
        [System.IO.Compression.ZipFile]::CreateFromDirectory($realZipStage, $realLgplZip)
        $realLgplSha = (Get-FileHash -LiteralPath $realLgplZip -Algorithm SHA256).Hash.ToLowerInvariant()

        # Setup the synthetic clean build directory (hand-authored CMakeCache and
        # build-evidence; contract-test data only).
        $e2eBuildDir = Join-Path $e2eDir "build"
        $null = New-TestDependencyBuildCache -BuildDir $e2eBuildDir -SourceRepoRoot $fixtureRepo `
            -VcpkgRoot $fixtureVcpkg -InstalledDir $dependencyFixture.InstalledRoot -Triplet $dependencyFixture.Triplet
        Copy-Item -LiteralPath $exePath -Destination (Join-Path $e2eBuildDir "ozviewer.exe") -Force
        $e2eBuiltSha = (Get-FileHash -LiteralPath (Join-Path $e2eBuildDir "ozviewer.exe") -Algorithm SHA256).Hash.ToLowerInvariant()
        $e2eBuildEv = [ordered]@{
            schema              = 1
            wrapper             = "invoke-release-build.ps1"
            target              = "ozviewer"
            buildDir            = $e2eBuildDir
            sourceCommitSha     = $e2eHead
            sourceTreeSha       = $e2eTree
            configuration       = "Release"
            diagnostics         = "OFF"
            buildCommand        = "cmake --build build --config Release"
            exitCode            = 0
            builtOzviewerSha256 = $e2eBuiltSha
        }
        [System.IO.File]::WriteAllText((Join-Path $e2eBuildDir "build-evidence.json"), ($e2eBuildEv | ConvertTo-Json))

        # 1) Generate BUILD-PROVENANCE.json with the real production generator.
        $e2eProv = Join-Path $e2eDir "BUILD-PROVENANCE.json"
        $rGen = Invoke-Script -ScriptPath $fixtureGenScript -Arguments @('-OutPath', $e2eProv, '-StagedBinariesDir', $payloadDir, '-LgplSourceZipPath', $realLgplZip, '-BuildDir', $e2eBuildDir, '-VcpkgRoot', $fixtureVcpkg)
        Assert-True ($rGen.ExitCode -eq 0) "E2E generate-build-provenance failed: $($rGen.Output)"
        Pass "E2E: generate-build-provenance.ps1 succeeded with real pinned tarballs and synthetic contract build evidence"

        # 2) Package portable with the real production script.
        $e2ePkgOut = Join-Path $e2eDir "pkg-out"
        $rPkg = Invoke-Script -ScriptPath (Join-Path $fixturePkgDir "package-portable.ps1") -Arguments @('-PayloadDir', $payloadDir, '-OutDir', $e2ePkgOut, '-ProvenancePath', $e2eProv)
        Assert-True ($rPkg.ExitCode -eq 0) "E2E package-portable failed: $($rPkg.Output)"
        $e2eStageDir = Join-Path $e2ePkgOut "pikaview-0.1.1-win-x64-portable"
        Pass "E2E: package-portable.ps1 succeeded against the clean fixture worktree"

        # 3) Validate portable payload (positive).
        $e2eValScript = Join-Path $fixturePkgDir "validate-portable-payload.ps1"
        $rVal = Invoke-Script -ScriptPath $e2eValScript -Arguments @('-PackageDir', $e2eStageDir, '-ExpectedCommitSha', $e2eHead, '-ExpectedTreeSha', $e2eTree, '-ExpectedLgplSourceSha256', $realLgplSha)
        Assert-True ($rVal.ExitCode -eq 0) "E2E validate-portable-payload failed: $($rVal.Output)"
        Pass "E2E: validate-portable-payload.ps1 succeeded with explicit commit/tree/source-asset binding"

        # 4) Negative bindings through the real production validator.
        $rValMiss = Invoke-Script -ScriptPath $e2eValScript -Arguments @('-PackageDir', $e2eStageDir)
        Assert-True ($rValMiss.ExitCode -ne 0) "Validator accepted missing ExpectedCommitSha"
        Assert-True ($rValMiss.Output -match "ExpectedCommitSha is required") "Error did not flag missing ExpectedCommitSha"

        $rValMissTree = Invoke-Script -ScriptPath $e2eValScript -Arguments @('-PackageDir', $e2eStageDir, '-ExpectedCommitSha', $e2eHead, '-ExpectedLgplSourceSha256', $realLgplSha)
        Assert-True ($rValMissTree.ExitCode -ne 0) "Validator accepted missing ExpectedTreeSha"
        Assert-True ($rValMissTree.Output -match "ExpectedTreeSha is required") "Error did not flag missing ExpectedTreeSha"

        $rValMissLgpl = Invoke-Script -ScriptPath $e2eValScript -Arguments @('-PackageDir', $e2eStageDir, '-ExpectedCommitSha', $e2eHead, '-ExpectedTreeSha', $e2eTree)
        Assert-True ($rValMissLgpl.ExitCode -ne 0) "Validator accepted missing ExpectedLgplSourceSha256"
        Assert-True ($rValMissLgpl.Output -match "ExpectedLgplSourceSha256 is required") "Error did not flag missing ExpectedLgplSourceSha256"

        $rValWrongHash = Invoke-Script -ScriptPath $e2eValScript -Arguments @('-PackageDir', $e2eStageDir, '-ExpectedCommitSha', $e2eHead, '-ExpectedTreeSha', $e2eTree, '-ExpectedLgplSourceSha256', ('2' * 64))
        Assert-True ($rValWrongHash.ExitCode -ne 0) "Validator accepted a wrong ExpectedLgplSourceSha256"
        Assert-True ($rValWrongHash.Output -match "lgplCorrespondingSourceAsset sha256 .* does not match expected") "Error did not flag wrong ExpectedLgplSourceSha256"
        Pass "E2E: validator fails closed on missing/wrong commit, tree, and LGPL-source bindings"
    }

    # -----------------------------------------------------------------------
    # TEST 5: Release Notes Draft Verification
    # -----------------------------------------------------------------------
    $enDraft = Join-Path $RepoRoot "docs\releases\v0.1.1\RELEASE_NOTES.md"
    $thDraft = Join-Path $RepoRoot "docs\releases\v0.1.1\RELEASE_NOTES.th.md"
    Assert-True (Test-Path -LiteralPath $enDraft -PathType Leaf) "EN draft release notes missing"
    Assert-True (Test-Path -LiteralPath $thDraft -PathType Leaf) "TH draft release notes missing"
    $enText = Get-Content -LiteralPath $enDraft -Raw
    Assert-True ($enText.Contains('PikaView 0.1.1 (formerly OZ Image Viewer)')) "EN draft missing PikaView naming"
    Assert-True ($enText.Contains('SHA256SUMS')) "EN draft missing SHA256SUMS asset reference"
    Pass "Draft release notes verified with PikaView naming and truthful disclosures"

    if ($positiveChainOmitted) {
        Write-Host "`nENVELOPE TESTS INCOMPLETE: required real-tarball LGPL fixture omitted; the positive generator chain was NOT exercised." -ForegroundColor Red
        Write-Host "This is a skipped required fixture, not a pass." -ForegroundColor Red
        exit 77
    }

    Write-Host "`nALL ENVELOPE & PROVENANCE TESTS PASSED (synthetic contract build evidence; real clean-commit build remains a separate release step)" -ForegroundColor Green
    exit 0
}
catch {
    Write-Host "`nTEST SUITE FAILED: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}

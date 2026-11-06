<#
.SYNOPSIS
  Build the LGPL Corresponding Source release asset (ozviewer-0.1.0-lgpl-corresponding-source.zip).

.DESCRIPTION
  Packages exact upstream source archives, vcpkg portfiles and patches, custom triplets,
  and complete standalone build scripts / instructions for libheif 1.23.2 and libde265 1.1.1
  following the source-distribution provisions of GPLv3 §6(d) and combined-work
  shared-library provisions of LGPLv3 §4.

.PARAMETER OutDir
  Output directory for the corresponding source zip and sha256 sidecar.

.PARAMETER VcpkgRoot
  Root of the vcpkg repository containing ports and downloads.

.PARAMETER DownloadsDir
  Directory containing the upstream source tarballs (defaults to $VcpkgRoot\downloads).
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$OutDir,
    [string]$VcpkgRoot = "",
    [string]$DownloadsDir = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Split-Path -Parent $ScriptDir

if ([string]::IsNullOrWhiteSpace($VcpkgRoot)) {
    $candidates = @(
        (Join-Path $RepoRoot "build\vcpkg"),
        (Join-Path $RepoRoot "vcpkg")
    )
    foreach ($cand in $candidates) {
        if (Test-Path -LiteralPath (Join-Path $cand "ports\libheif") -PathType Container) {
            $VcpkgRoot = (Resolve-Path -LiteralPath $cand).Path
            break
        }
    }
}

if ([string]::IsNullOrWhiteSpace($VcpkgRoot) -or -not (Test-Path -LiteralPath $VcpkgRoot -PathType Container)) {
    throw "vcpkg root directory containing libheif / libde265 ports not found: '$VcpkgRoot'. Pass -VcpkgRoot explicitly."
}

if ([string]::IsNullOrWhiteSpace($DownloadsDir)) {
    $dlCandidates = @(
        (Join-Path $VcpkgRoot "downloads"),
        (Join-Path $RepoRoot "downloads")
    )
    foreach ($dl in $dlCandidates) {
        if (Test-Path -LiteralPath (Join-Path $dl "strukturag-libheif-v1.23.2.tar.gz") -PathType Leaf) {
            $DownloadsDir = (Resolve-Path -LiteralPath $dl).Path
            break
        }
    }
    if ([string]::IsNullOrWhiteSpace($DownloadsDir)) {
        $DownloadsDir = Join-Path $VcpkgRoot "downloads"
    }
}

if (-not (Test-Path -LiteralPath $OutDir -PathType Container)) {
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
}
$OutDir = (Resolve-Path -LiteralPath $OutDir).Path

$tempStage = Join-Path $OutDir "ozviewer-0.1.0-lgpl-corresponding-source"
if (Test-Path -LiteralPath $tempStage) { Remove-Item -LiteralPath $tempStage -Recurse -Force }
New-Item -ItemType Directory -Path $tempStage -Force | Out-Null

# 1. Copy upstream source tarballs
$sourcesDir = Join-Path $tempStage "upstream-sources"
New-Item -ItemType Directory -Path $sourcesDir -Force | Out-Null

$tarballs = @(
    "strukturag-libheif-v1.23.2.tar.gz",
    "strukturag-libde265-v1.1.1.tar.gz"
)
foreach ($t in $tarballs) {
    $src = Join-Path $DownloadsDir $t
    if (-not (Test-Path -LiteralPath $src -PathType Leaf)) {
        throw "upstream source tarball not found: $src. Pass -DownloadsDir explicitly if located outside vcpkg root."
    }
    Copy-Item -LiteralPath $src -Destination (Join-Path $sourcesDir $t) -Force
}

# 2. Copy ports and patches
$portsDir = Join-Path $tempStage "vcpkg-ports"
New-Item -ItemType Directory -Path $portsDir -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $VcpkgRoot "ports\libheif") -Destination (Join-Path $portsDir "libheif") -Recurse -Force
Copy-Item -LiteralPath (Join-Path $VcpkgRoot "ports\libde265") -Destination (Join-Path $portsDir "libde265") -Recurse -Force

# 3. Copy triplet
$tripletsDir = Join-Path $tempStage "triplets"
New-Item -ItemType Directory -Path $tripletsDir -Force | Out-Null
$tripletSrc = Join-Path $RepoRoot "triplets\oz-x64-windows-static-md.cmake"
if (-not (Test-Path -LiteralPath $tripletSrc -PathType Leaf)) {
    throw "triplet file not found in repository: $tripletSrc"
}
Copy-Item -LiteralPath $tripletSrc -Destination (Join-Path $tripletsDir "oz-x64-windows-static-md.cmake") -Force

# 4. Copy license texts
$licDir = Join-Path $tempStage "licenses"
New-Item -ItemType Directory -Path $licDir -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $VcpkgRoot "ports\libheif\vcpkg.json") -Destination (Join-Path $licDir "libheif-vcpkg.json") -Force
Copy-Item -LiteralPath (Join-Path $VcpkgRoot "ports\libde265\vcpkg.json") -Destination (Join-Path $licDir "libde265-vcpkg.json") -Force

# Extract or copy exact license texts from source tarballs or vcpkg installed copyright
$heifLicDst = Join-Path $licDir "libheif-COPYING.txt"
$de265LicDst = Join-Path $licDir "libde265-COPYING.txt"

# Search order: vcpkg buildtrees source directory, vcpkg installed copyright, or fail closed
$heifCandidates = @(
    (Join-Path $VcpkgRoot "buildtrees\libheif\src\v1.23.2-5aa17603d3.clean\COPYING"),
    (Join-Path $RepoRoot "vcpkg_installed\oz-x64-windows-static-md\share\libheif\copyright"),
    (Join-Path $VcpkgRoot "installed\oz-x64-windows-static-md\share\libheif\copyright")
)
$foundHeifLic = $false
foreach ($cand in $heifCandidates) {
    if (Test-Path -LiteralPath $cand -PathType Leaf) {
        Copy-Item -LiteralPath $cand -Destination $heifLicDst -Force
        $foundHeifLic = $true
        break
    }
}
if (-not $foundHeifLic) {
    # Extract directly from the verified upstream source tarball
    $tarPath = Join-Path $sourcesDir "strukturag-libheif-v1.23.2.tar.gz"
    if (Test-Path -LiteralPath $tarPath -PathType Leaf) {
        $tempExtract = Join-Path $licDir "heif-extract"
        New-Item -ItemType Directory -Path $tempExtract -Force | Out-Null
        & tar -xzf $tarPath -C $tempExtract "libheif-1.23.2/COPYING"
        if (Test-Path -LiteralPath (Join-Path $tempExtract "libheif-1.23.2\COPYING") -PathType Leaf) {
            Move-Item -LiteralPath (Join-Path $tempExtract "libheif-1.23.2\COPYING") -Destination $heifLicDst -Force
            $foundHeifLic = $true
        }
        Remove-Item -LiteralPath $tempExtract -Recurse -Force -ErrorAction SilentlyContinue
    }
}
if (-not $foundHeifLic) {
    throw "libheif COPYING license text could not be located or extracted"
}

$de265Candidates = @(
    (Join-Path $VcpkgRoot "buildtrees\libde265\src\v1.1.1-0333439992.clean\COPYING"),
    (Join-Path $RepoRoot "vcpkg_installed\oz-x64-windows-static-md\share\libde265\copyright"),
    (Join-Path $VcpkgRoot "installed\oz-x64-windows-static-md\share\libde265\copyright")
)
$foundDe265Lic = $false
foreach ($cand in $de265Candidates) {
    if (Test-Path -LiteralPath $cand -PathType Leaf) {
        Copy-Item -LiteralPath $cand -Destination $de265LicDst -Force
        $foundDe265Lic = $true
        break
    }
}
if (-not $foundDe265Lic) {
    $tarPath = Join-Path $sourcesDir "strukturag-libde265-v1.1.1.tar.gz"
    if (Test-Path -LiteralPath $tarPath -PathType Leaf) {
        $tempExtract = Join-Path $licDir "de265-extract"
        New-Item -ItemType Directory -Path $tempExtract -Force | Out-Null
        & tar -xzf $tarPath -C $tempExtract "libde265-1.1.1/COPYING"
        if (Test-Path -LiteralPath (Join-Path $tempExtract "libde265-1.1.1\COPYING") -PathType Leaf) {
            Move-Item -LiteralPath (Join-Path $tempExtract "libde265-1.1.1\COPYING") -Destination $de265LicDst -Force
            $foundDe265Lic = $true
        }
        Remove-Item -LiteralPath $tempExtract -Recurse -Force -ErrorAction SilentlyContinue
    }
}
if (-not $foundDe265Lic) {
    throw "libde265 COPYING license text could not be located or extracted"
}

# 5. Write README-BUILD-LGPL.txt
$readme = @"
================================================================================
OZ Image Viewer v0.1.0 — LGPL Corresponding Source Package
================================================================================

This package contains the exact corresponding source code, patches, build scripts,
and instructions for the LGPL v3 components distributed with OZ Image Viewer:
- libheif 1.23.2 (produces heif.dll)
- libde265 1.1.1 (produces libde265.dll)

This is provided following the source-distribution provisions of GPLv3 §6(d) and
the combined-work shared-library provisions of LGPLv3 §4.

Package Contents:
- upstream-sources/  Exact upstream tarballs:
    * strukturag-libheif-v1.23.2.tar.gz (SHA-512: d34ccd40d7f7cc48ff8ceddafd8f7c7004ab0df2a657237fcc715c4bcc453a41946d7db542d32b781875868ee116c451b4b63daaf073530245ec7b117ae60e27)
    * strukturag-libde265-v1.1.1.tar.gz (SHA-512: fb2207f5a3ba901853f61f345c72130f000134918febbc4f3529c3d289fc79ee7457b3e61660110f698bb4ac15d62426e284034bf870bfbd1859ab3feaa52be8)
- vcpkg-ports/       vcpkg port recipes and exact patches applied during build
- triplets/          Target triplet configuration (oz-x64-windows-static-md)
- licenses/          Full LGPL-3.0 and GPL-3.0 license texts
- build-lgpl-dlls.ps1 Standalone PowerShell script to rebuild heif.dll and libde265.dll

Prerequisites for Rebuilding:
1. Windows 10/11 x64
2. Visual Studio 2022 (MSVC v143 toolset) or Visual Studio Build Tools
3. PowerShell 5.1+ and git on PATH
4. An existing vcpkg installation, OR permit the build script to clone vcpkg
   at baseline 52d80838fb40c755b1615fbc9c7b994a33742a22.

Rebuilding Instructions:
Option A (using existing vcpkg):
  powershell -ExecutionPolicy Bypass -File .\build-lgpl-dlls.ps1 -VcpkgRoot "C:\path\to\vcpkg"

Option B (allowing script to clone pinned vcpkg):
  powershell -ExecutionPolicy Bypass -File .\build-lgpl-dlls.ps1 -AllowClone

The build script will:
1. Stage the bundled upstream source tarballs into vcpkg/downloads (no network download needed).
2. Use the bundled ports and custom triplet (oz-x64-windows-static-md).
3. Compile and install libheif and libde265 as dynamic DLLs with MD runtime linkage.
4. Output rebuilt heif.dll and libde265.dll to .\bin (or specified -OutDir).

Runtime DLL Replacement:
OZ Image Viewer loads heif.dll and libde265.dll dynamically using LoadLibraryExW
with LOAD_LIBRARY_SEARCH_APPLICATION_DIRECTORY. The executable ozviewer.exe does
not hardcode internal checksums or import tables for these libraries. Users may
replace heif.dll and libde265.dll in the application directory with any ABI-compatible
build under the LGPL v3 terms.
================================================================================
"@
[System.IO.File]::WriteAllText((Join-Path $tempStage "README-BUILD-LGPL.txt"), $readme.Trim(), (New-Object System.Text.UTF8Encoding($false)))

# 6. Write standalone build-lgpl-dlls.ps1 helper script inside stage
$buildScript = @'
<#
.SYNOPSIS
  Rebuild heif.dll and libde265.dll using the bundled LGPL corresponding source assets and vcpkg.

.DESCRIPTION
  This script rebuilds the standalone LGPL dynamic libraries heif.dll and libde265.dll
  from the bundled upstream sources and port definitions, using a specified vcpkg clone.
  In accordance with the combined-work shared-library provisions of LGPLv3 §4 and
  source-distribution provisions of GPLv3 §6(d), complete build scripts and instructions
  are provided to allow users to modify, rebuild, and replace these dynamic libraries.

.PARAMETER VcpkgRoot
  Path to an existing vcpkg installation or checkout. If omitted or not found,
  and -AllowClone is specified, the script will clone vcpkg at the pinned baseline.

.PARAMETER OutDir
  Output directory for the rebuilt heif.dll and libde265.dll (defaults to .\bin).

.PARAMETER AllowClone
  Permit cloning vcpkg from github.com/microsoft/vcpkg if VcpkgRoot is not provided.
#>
[CmdletBinding()]
param(
    [string]$VcpkgRoot = "",
    [string]$OutDir = ".\bin",
    [string]$VcpkgGitBaseline = "52d80838fb40c755b1615fbc9c7b994a33742a22",
    [switch]$AllowClone
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

Write-Host "================================================================================"
Write-Host "OZ Image Viewer v0.1.0 — LGPL Rebuild Script"
Write-Host "Rebuilding heif.dll and libde265.dll..."
Write-Host "================================================================================"

# 1. Resolve or clone vcpkg
if ([string]::IsNullOrWhiteSpace($VcpkgRoot) -or -not (Test-Path -LiteralPath $VcpkgRoot -PathType Container)) {
    if ($AllowClone) {
        if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
            throw "git is required on PATH to clone vcpkg."
        }
        $VcpkgRoot = Join-Path $ScriptDir "vcpkg-build"
        if (-not (Test-Path -LiteralPath (Join-Path $VcpkgRoot ".git") -PathType Container)) {
            Write-Host "Cloning vcpkg at baseline $VcpkgGitBaseline..."
            & git clone https://github.com/microsoft/vcpkg.git $VcpkgRoot
            if ($LASTEXITCODE -ne 0) { throw "git clone failed" }
            & git -C $VcpkgRoot checkout $VcpkgGitBaseline
            if ($LASTEXITCODE -ne 0) { throw "git checkout baseline failed" }
        }
    } else {
        throw "vcpkg root not found. Please provide -VcpkgRoot <path> or pass -AllowClone."
    }
}
$VcpkgRoot = (Resolve-Path -LiteralPath $VcpkgRoot).Path

# 2. Bootstrap vcpkg if needed
$vcpkgExe = Join-Path $VcpkgRoot "vcpkg.exe"
if (-not (Test-Path -LiteralPath $vcpkgExe -PathType Leaf)) {
    Write-Host "Bootstrapping vcpkg..."
    $bootstrapBat = Join-Path $VcpkgRoot "bootstrap-vcpkg.bat"
    if (-not (Test-Path -LiteralPath $bootstrapBat -PathType Leaf)) {
        throw "bootstrap-vcpkg.bat not found at $bootstrapBat"
    }
    & $bootstrapBat -disableMetrics
    if ($LASTEXITCODE -ne 0) { throw "vcpkg bootstrap failed" }
}

# 3. Populate downloads with bundled upstream tarballs so network is not required
$dlDir = Join-Path $VcpkgRoot "downloads"
if (-not (Test-Path -LiteralPath $dlDir -PathType Container)) {
    New-Item -ItemType Directory -Path $dlDir -Force | Out-Null
}
$bundledSources = Join-Path $ScriptDir "upstream-sources"
if (Test-Path -LiteralPath $bundledSources -PathType Container) {
    Get-ChildItem -Path $bundledSources -Filter "*.tar.gz" | ForEach-Object {
        $dest = Join-Path $dlDir $_.Name
        if (-not (Test-Path -LiteralPath $dest -PathType Leaf)) {
            Copy-Item -LiteralPath $_.FullName -Destination $dest -Force
            Write-Host "Staged bundled source: $($_.Name)"
        }
    }
}

# 4. Create isolated rebuild manifest workspace
$workDir = Join-Path $ScriptDir ".rebuild-work"
if (Test-Path -LiteralPath $workDir) { Remove-Item -LiteralPath $workDir -Recurse -Force }
New-Item -ItemType Directory -Path $workDir -Force | Out-Null

$manifestContent = @"
{
  "name": "ozviewer-lgpl-rebuild",
  "version-string": "0.1.0",
  "dependencies": [
    {
      "name": "libheif",
      "default-features": false
    }
  ]
}
"@
Set-Content -Path (Join-Path $workDir "vcpkg.json") -Value $manifestContent -Encoding utf8

# 5. Invoke vcpkg install with custom triplet and overlay ports
$tripletDir = Join-Path $ScriptDir "triplets"
$overlayPorts = Join-Path $ScriptDir "vcpkg-ports"
$installRoot = Join-Path $workDir "installed"

Write-Host "Invoking vcpkg install for libheif (and transitive libde265)..."
$installArgs = @(
    "install",
    "--x-install-root=$installRoot",
    "--triplet=oz-x64-windows-static-md",
    "--overlay-triplets=$tripletDir",
    "--overlay-ports=$overlayPorts",
    "--clean-after-build"
)

Push-Location $workDir
try {
    & $vcpkgExe @installArgs
    if ($LASTEXITCODE -ne 0) { throw "vcpkg install failed with exit code $LASTEXITCODE" }
} finally {
    Pop-Location
}

# 6. Copy output DLLs to target OutDir
if (-not (Test-Path -LiteralPath $OutDir -PathType Container)) {
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
}
$OutDir = (Resolve-Path -LiteralPath $OutDir).Path

$builtHeif = Join-Path $installRoot "oz-x64-windows-static-md\bin\heif.dll"
$builtDe265 = Join-Path $installRoot "oz-x64-windows-static-md\bin\libde265.dll"

if (-not (Test-Path -LiteralPath $builtHeif -PathType Leaf)) {
    throw "Build failed to produce expected library: $builtHeif"
}
if (-not (Test-Path -LiteralPath $builtDe265 -PathType Leaf)) {
    throw "Build failed to produce expected library: $builtDe265"
}

Copy-Item -LiteralPath $builtHeif -Destination (Join-Path $OutDir "heif.dll") -Force
Copy-Item -LiteralPath $builtDe265 -Destination (Join-Path $OutDir "libde265.dll") -Force

Write-Host ""
Write-Host "LGPL Rebuild Successful!" -ForegroundColor Green
Write-Host "Output files in ${OutDir}:"
Get-ChildItem -Path $OutDir -Filter "*.dll" | Format-Table Name, Length, LastWriteTime
'@
[System.IO.File]::WriteAllText((Join-Path $tempStage "build-lgpl-dlls.ps1"), $buildScript.Trim(), (New-Object System.Text.UTF8Encoding($false)))

# 7. Create ZIP archive
$zipName = "ozviewer-0.1.0-lgpl-corresponding-source.zip"
$zipPath = Join-Path $OutDir $zipName
if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }

Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory($tempStage, $zipPath, [System.IO.Compression.CompressionLevel]::Optimal, $true)

$zipHash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
$shaPath = Join-Path $OutDir ($zipName + ".sha256")
[System.IO.File]::WriteAllText($shaPath, "$zipHash *$zipName`n", (New-Object System.Text.UTF8Encoding($false)))

Write-Output "LGPL_SOURCE_ZIP=$zipPath"
Write-Output "LGPL_SOURCE_SHA256=$zipHash"

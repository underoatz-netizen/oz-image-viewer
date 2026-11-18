<#
.SYNOPSIS
  Deterministic, clearly labelled contract-test fixtures for the packaging
  boundary tests.

.DESCRIPTION
  These helpers create SYNTHETIC contract-test evidence only. Nothing produced
  here is a real build, a real dependency record, or release provenance. The
  PE files are hand-authored minimal images used to exercise the packaging
  parsers and import-closure logic deterministically without a Release build.

  The helpers never modify the source repository, never write git config, and
  never touch the host registry. Every generated fixture carries an explicit
  contract-test marker where the production scripts permit one.

  Intended to be dot-sourced by:
    * packaging/test-portable-script.ps1
    * packaging/test-installer-script.ps1
    * packaging/test-envelope-provenance.ps1
#>
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function New-TestPe64 {
    <#
      Writes an intentionally minimal x64 PE image. The optional import table
      is real enough for package-portable.ps1's PE reader (one .text section,
      import descriptors, DLL name strings). This is contract-test data only.
    #>
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [string[]]$Imports = @(),
        [uint16]$Machine = 0x8664,
        [string]$ExtraText = ''
    )

    $parent = Split-Path -Parent $Path
    if (-not [string]::IsNullOrWhiteSpace($parent) -and -not (Test-Path -LiteralPath $parent -PathType Container)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }

    $peOff = 0x80
    $optSize = 0xF0
    $secOff = $peOff + 24 + $optSize
    $rawPtr = 0x200
    $rawSize = 0x200
    $textRva = 0x1000

    $needed = 0
    if ($Imports.Count -gt 0) {
        $namesLength = 0
        foreach ($imp in $Imports) { $namesLength += ([Text.Encoding]::ASCII.GetByteCount($imp) + 1) }
        $needed = 0x80 + (20 * ($Imports.Count + 1)) + $namesLength
    }
    if ($needed -gt $rawSize) {
        $rawSize = [int]([math]::Ceiling($needed / 0x200) * 0x200)
    }

    $bytes = New-Object byte[] ($rawPtr + $rawSize)
    $bytes[0] = 0x4D # M
    $bytes[1] = 0x5A # Z
    [BitConverter]::GetBytes([uint32]$peOff).CopyTo($bytes, 0x3C)
    $bytes[$peOff] = 0x50     # P
    $bytes[$peOff + 1] = 0x45 # E

    [BitConverter]::GetBytes([uint16]$Machine).CopyTo($bytes, $peOff + 4)
    [BitConverter]::GetBytes([uint16]1).CopyTo($bytes, $peOff + 6)
    [BitConverter]::GetBytes([uint16]$optSize).CopyTo($bytes, $peOff + 20)
    [BitConverter]::GetBytes([uint16]0x20B).CopyTo($bytes, $peOff + 24)
    $bytes[$peOff + 26] = 14
    $bytes[$peOff + 27] = 44
    [BitConverter]::GetBytes([uint32]16).CopyTo($bytes, $peOff + 24 + 108)

    # .text section: VirtualSize 0x1000, VirtualAddress 0x1000, raw size/data.
    [BitConverter]::GetBytes([uint32]0x1000).CopyTo($bytes, $secOff + 8)
    [BitConverter]::GetBytes([uint32]$textRva).CopyTo($bytes, $secOff + 12)
    [BitConverter]::GetBytes([uint32]$rawSize).CopyTo($bytes, $secOff + 16)
    [BitConverter]::GetBytes([uint32]$rawPtr).CopyTo($bytes, $secOff + 20)
    [BitConverter]::GetBytes([uint32]0x60000020).CopyTo($bytes, $secOff + 36)

    if ($Imports.Count -gt 0) {
        [BitConverter]::GetBytes([uint32]$textRva).CopyTo($bytes, $peOff + 24 + 112 + 8)
        [BitConverter]::GetBytes([uint32](20 * ($Imports.Count + 1))).CopyTo($bytes, $peOff + 24 + 112 + 12)

        $descOff = $rawPtr
        $nameOff = $rawPtr + (20 * ($Imports.Count + 1))
        foreach ($imp in $Imports) {
            $nameRva = $textRva + ($nameOff - $rawPtr)
            [BitConverter]::GetBytes([uint32]$nameRva).CopyTo($bytes, $descOff + 12)
            $nameBytes = [Text.Encoding]::ASCII.GetBytes($imp)
            [Array]::Copy($nameBytes, 0, $bytes, $nameOff, $nameBytes.Length)
            $nameOff += $nameBytes.Length + 1
            $descOff += 20
        }
    }

    if (-not [string]::IsNullOrEmpty($ExtraText)) {
        $extra = [Text.Encoding]::ASCII.GetBytes($ExtraText)
        $combined = New-Object byte[] ($bytes.Length + $extra.Length)
        [Array]::Copy($bytes, $combined, $bytes.Length)
        [Array]::Copy($extra, 0, $combined, $bytes.Length, $extra.Length)
        $bytes = $combined
    }

    [IO.File]::WriteAllBytes($Path, $bytes)
}

function New-TestPayload {
    <#
      Creates the deterministic contract-test payload:
        ozviewer.exe  : imports localdep.dll; carries supported-extension text
        heif.dll      : imports libde265.dll (for closure traversal tests)
        libde265.dll  : leaf dependency
        localdep.dll  : non-system app-local dependency for ambiguity tests
      No filename matches portable-package.json's excludedNameFragments.
    #>
    param([Parameter(Mandatory = $true)][string]$Dir)

    if (-not (Test-Path -LiteralPath $Dir -PathType Container)) {
        New-Item -ItemType Directory -Path $Dir -Force | Out-Null
    }
    New-TestPe64 -Path (Join-Path $Dir 'ozviewer.exe') -Imports @('localdep.dll') -ExtraText "*.jpg`0*.jpeg`0*.png`0"
    New-TestPe64 -Path (Join-Path $Dir 'heif.dll') -Imports @('libde265.dll')
    New-TestPe64 -Path (Join-Path $Dir 'libde265.dll')
    New-TestPe64 -Path (Join-Path $Dir 'localdep.dll')
}

function New-TestPackagingFixture {
    <#
      Creates a throwaway clean git repository containing the production
      packaging scripts, metadata, icon, and license files. package-portable.ps1
      derives its clean-source gate from this fixture root, so the production
      gate is exercised unchanged (it is not weakened or bypassed).

      All content is committed with an explicit one-off committer identity; no
      global or repository git config is written.
    #>
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$SourceScriptDir,
        [Parameter(Mandatory = $true)][string]$SourceRepoRoot,
        [string[]]$ExtraScriptNames = @(),
        [string]$Version = '0.1.1',
        [string]$PackageName = 'pikaview',
        [string]$DependencyPolicyPath = '',
        [string]$VcpkgBaseline = '',
        [string]$Triplet = 'oz-x64-windows-static-md'
    )

    if (Test-Path -LiteralPath $Root) { Remove-Item -LiteralPath $Root -Recurse -Force }
    $packagingDir = Join-Path $Root 'packaging'
    $iconDir = Join-Path $Root 'assets\icons'
    New-Item -ItemType Directory -Path $packagingDir -Force | Out-Null
    New-Item -ItemType Directory -Path $iconDir -Force | Out-Null

    $scriptNames = @('package-portable.ps1', 'validate-portable-payload.ps1', 'build-installer.ps1', 'ozviewer-installer.iss', 'portable-package.json')
    foreach ($extra in $ExtraScriptNames) {
        if (-not [string]::IsNullOrWhiteSpace($extra)) { $scriptNames += $extra }
    }
    foreach ($name in $scriptNames) {
        $src = Join-Path $SourceScriptDir $name
        if (-not (Test-Path -LiteralPath $src -PathType Leaf)) { throw "contract fixture source script missing: $src" }
        Copy-Item -LiteralPath $src -Destination (Join-Path $packagingDir $name) -Force
    }

    $policySource = if (-not [string]::IsNullOrWhiteSpace($DependencyPolicyPath)) {
        $DependencyPolicyPath
    } else {
        Join-Path $SourceScriptDir 'lgpl-dependency-policy.json'
    }
    if (-not (Test-Path -LiteralPath $policySource -PathType Leaf)) { throw "contract fixture dependency policy missing: $policySource" }
    Copy-Item -LiteralPath $policySource -Destination (Join-Path $packagingDir 'lgpl-dependency-policy.json') -Force

    $tripletRelative = "triplets/$Triplet.cmake"
    $tripletSource = Join-Path $SourceRepoRoot $tripletRelative.Replace('/', '\')
    if (-not (Test-Path -LiteralPath $tripletSource -PathType Leaf)) { throw "contract fixture triplet missing: $tripletSource" }
    $tripletDir = Join-Path $Root 'triplets'
    New-Item -ItemType Directory -Path $tripletDir -Force | Out-Null
    Copy-Item -LiteralPath $tripletSource -Destination (Join-Path $tripletDir "$Triplet.cmake") -Force

    if (-not [string]::IsNullOrWhiteSpace($VcpkgBaseline)) {
        Set-TestVcpkgConfiguration -RepoRoot $Root -Baseline $VcpkgBaseline
    }

    $metadataPath = Join-Path $packagingDir 'portable-package.json'
    $metadata = Get-Content -LiteralPath $metadataPath -Raw | ConvertFrom-Json
    $metadata.version = $Version
    $metadata.packageName = $PackageName
    [IO.File]::WriteAllText($metadataPath, ($metadata | ConvertTo-Json -Depth 8), (New-Object System.Text.UTF8Encoding($false)))

    $iconSrc = Join-Path $SourceRepoRoot 'assets\icons\ozviewer.ico'
    if (-not (Test-Path -LiteralPath $iconSrc -PathType Leaf)) { throw "contract fixture icon missing: $iconSrc" }
    Copy-Item -LiteralPath $iconSrc -Destination (Join-Path $iconDir 'ozviewer.ico') -Force

    foreach ($name in @('LICENSE', 'THIRD-PARTY-NOTICES.md', 'THIRD-PARTY-LICENSES.txt', 'THIRD-PARTY-SOURCE.txt')) {
        $src = Join-Path $SourceRepoRoot $name
        if (-not (Test-Path -LiteralPath $src -PathType Leaf)) { throw "contract fixture license/attribution file missing: $src" }
        Copy-Item -LiteralPath $src -Destination (Join-Path $Root $name) -Force
    }

    [IO.File]::WriteAllText((Join-Path $Root 'README.md'), "SYNTHETIC CONTRACT-TEST FIXTURE - NOT A REAL BUILD OR RELEASE SOURCE`n", (New-Object System.Text.UTF8Encoding($false)))

    & git -C $Root init -q
    if ($LASTEXITCODE -ne 0) { throw "git init failed for contract fixture: $Root" }
    & git -C $Root add .
    if ($LASTEXITCODE -ne 0) { throw "git add failed for contract fixture: $Root" }
    & git -C $Root -c user.email="contract-tests@example.invalid" -c user.name="Contract Test Fixture" commit -q -m "synthetic packaging contract-test fixture"
    if ($LASTEXITCODE -ne 0) { throw "git commit failed for contract fixture: $Root" }

    $statusLines = @(& git -C $Root status --porcelain | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    if ($statusLines.Count -ne 0) { throw "contract fixture is not clean after commit: $($statusLines -join '; ')" }
    $head = (& git -C $Root rev-parse HEAD).Trim()
    $tree = (& git -C $Root rev-parse "HEAD^{tree}").Trim()
    if ($head -notmatch '^[0-9a-f]{40}$' -or $tree -notmatch '^[0-9a-f]{40}$') {
        throw "contract fixture did not produce valid HEAD/tree SHA values"
    }

    return [pscustomobject]@{
        RepoRoot     = $Root
        PackagingDir = $packagingDir
        MetadataPath = $metadataPath
        Head         = $head
        Tree         = $tree
    }
}

function New-TestLgplFixtureAsset {
    <#
      Creates a small synthetic LGPL-asset fixture ZIP used only to bind the
      validator's required ExpectedLgplSourceSha256 check. Its contents say it
      is contract-test data; it is never presented as a real source archive.
    #>
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [string]$Version = '0.1.1'
    )

    $parent = Split-Path -Parent $Path
    if (-not [string]::IsNullOrWhiteSpace($parent) -and -not (Test-Path -LiteralPath $parent -PathType Container)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    $stage = Join-Path $parent ("lgpl-asset-stage-" + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $stage -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $stage 'CONTRACT-TEST-FIXTURE.txt'), "SYNTHETIC CONTRACT-TEST FIXTURE - NOT A REAL LGPL SOURCE ARCHIVE`n", (New-Object System.Text.UTF8Encoding($false)))
    Add-Type -AssemblyName System.IO.Compression.FileSystem | Out-Null
    if (Test-Path -LiteralPath $Path) { Remove-Item -LiteralPath $Path -Force }
    [System.IO.Compression.ZipFile]::CreateFromDirectory($stage, $Path)
    Remove-Item -LiteralPath $stage -Recurse -Force
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function New-TestBuildProvenance {
    <#
      Writes a SYNTHETIC BUILD-PROVENANCE.json bound to the fixture repository
      HEAD/tree and the synthetic payload bytes. It deliberately carries a
      contractTestFixture marker. It is contract-test input only: never call it
      real build evidence or release provenance.
    #>
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$CommitSha,
        [Parameter(Mandatory = $true)][string]$TreeSha,
        [Parameter(Mandatory = $true)][string]$PayloadDir,
        [Parameter(Mandatory = $true)][string]$LgplAssetPath,
        [string]$Version = '0.1.1',
        [string]$PackageName = 'pikaview',
        [string]$ExpectedTag = ''
    )

    if ([string]::IsNullOrWhiteSpace($ExpectedTag)) { $ExpectedTag = "v$Version" }
    $assetSha = (Get-FileHash -LiteralPath $LgplAssetPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $assetBytes = (Get-Item -LiteralPath $LgplAssetPath).Length

    $binaries = @()
    $peFiles = @(Get-ChildItem -LiteralPath $PayloadDir -File | Where-Object {
        $ext = $_.Extension.ToLowerInvariant()
        $ext -eq '.exe' -or $ext -eq '.dll'
    } | Sort-Object Name)
    foreach ($item in $peFiles) {
        $sigStatus = ''
        $sig = Get-AuthenticodeSignature -FilePath $item.FullName
        switch ($sig.Status) {
            ([System.Management.Automation.SignatureStatus]::NotSigned) { $sigStatus = 'unsigned' }
            ([System.Management.Automation.SignatureStatus]::Valid) { $sigStatus = 'valid-signed' }
            default { throw "synthetic contract PE '$($item.Name)' has unexpected signature status '$($sig.Status)'" }
        }
        $binaries += [ordered]@{
            name            = $item.Name
            bytes           = [long]$item.Length
            sha256          = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            kind            = $(if ($item.Extension -ieq '.exe') { 'pe-executable' } else { 'dynamic-library' })
            machine         = '0x8664'
            signatureStatus = $sigStatus
        }
    }

    $provenance = [ordered]@{
        '$schema'                    = 'https://json-schema.org/draft/2020-12/schema'
        provenanceSchemaVersion      = '1.0.0'
        contractTestFixture          = 'SYNTHETIC CONTRACT-TEST FIXTURE - NOT REAL BUILD OR DEPENDENCY PROVENANCE'
        product                      = [ordered]@{
            name        = 'PikaView'
            packageName = $PackageName
            version     = $Version
            tag         = $ExpectedTag
        }
        source                       = [ordered]@{
            commitSha   = $CommitSha
            treeSha     = $TreeSha
            cleanStatus = 'clean'
            isClean     = $true
        }
        buildConfiguration           = [ordered]@{
            targetArchitecture = 'x64'
            diagnostics        = 'OFF'
            configuration      = 'Release'
        }
        stagedCandidateBinaries      = $binaries
        lgplCorrespondingSourceAsset = [ordered]@{
            assetName = "$PackageName-$Version-lgpl-corresponding-source.zip"
            bytes     = [long]$assetBytes
            sha256    = $assetSha
        }
    }

    $parent = Split-Path -Parent $Path
    if (-not [string]::IsNullOrWhiteSpace($parent) -and -not (Test-Path -LiteralPath $parent -PathType Container)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    [IO.File]::WriteAllText($Path, ($provenance | ConvertTo-Json -Depth 8), (New-Object System.Text.UTF8Encoding($false)))
    return [pscustomobject]@{ Path = $Path; CommitSha = $CommitSha; TreeSha = $TreeSha; LgplSourceSha256 = $assetSha }
}

function New-TestDependencyPolicy {
    <#
      Writes a SYNTHETIC dependency policy and two SYNTHETIC upstream archives.
      The resulting policy is contract-test input only; it is not the real LGPL
      dependency policy and its archives are not upstream source.
    #>
    param(
        [Parameter(Mandatory = $true)][string]$PolicyPath,
        [Parameter(Mandatory = $true)][string]$TarballDir,
        [string]$TargetTriplet = 'oz-x64-windows-static-md'
    )

    if (-not (Test-Path -LiteralPath $TarballDir -PathType Container)) {
        New-Item -ItemType Directory -Path $TarballDir -Force | Out-Null
    }
    $archives = [ordered]@{
        libheif = [ordered]@{
            version = '1.23.2'
            name    = 'strukturag-libheif-v1.23.2.tar.gz'
            repo    = 'strukturag/libheif'
            dll     = 'heif.dll'
        }
        libde265 = [ordered]@{
            version = '1.1.1'
            name    = 'strukturag-libde265-v1.1.1.tar.gz'
            repo    = 'strukturag/libde265'
            dll     = 'libde265.dll'
        }
    }
    $deps = @()
    foreach ($port in @('libheif', 'libde265')) {
        $spec = $archives[$port]
        $archivePath = Join-Path $TarballDir $spec.name
        $seed = "SYNTHETIC CONTRACT-TEST ARCHIVE FOR $port $($spec.version) - NOT UPSTREAM SOURCE`n"
        $seed = (($seed * 64) -join '')
        [IO.File]::WriteAllText($archivePath, $seed, (New-Object System.Text.UTF8Encoding($false)))
        $deps += [ordered]@{
            port                     = $port
            version                  = $spec.version
            portVersion              = 0
            stagedDll                = $spec.dll
            installedDllRelativePath = "bin/$($spec.dll)"
            archiveName              = $spec.name
            archiveSha256            = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
            archiveSha512            = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA512).Hash.ToLowerInvariant()
            upstreamRepository       = $spec.repo
        }
    }
    $policy = [ordered]@{
        schema         = 1
        contractTestFixture = 'SYNTHETIC CONTRACT-TEST FIXTURE - NOT THE REAL LGPL DEPENDENCY POLICY'
        targetTriplet  = $TargetTriplet
        tripletFile    = "triplets/$TargetTriplet.cmake"
        dependencies   = $deps
    }
    $parent = Split-Path -Parent $PolicyPath
    if (-not [string]::IsNullOrWhiteSpace($parent) -and -not (Test-Path -LiteralPath $parent -PathType Container)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    [IO.File]::WriteAllText($PolicyPath, ($policy | ConvertTo-Json -Depth 8), (New-Object System.Text.UTF8Encoding($false)))
    return [pscustomobject]@{ Path = $PolicyPath; TarballDir = $TarballDir; Policy = $policy }
}

function New-TestVcpkgDependencyFixture {
    <#
      Builds a SYNTHETIC vcpkg-like Git checkout, versions database, and
      installed package tree for contract tests. It is not a real vcpkg
      installation, does not run vcpkg, and must never be presented as build
      evidence. The caller must write the returned baseline into the fixture
      repository's vcpkg-configuration.json and commit it before running the
      production generator.
    #>
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$PolicyPath,
        [Parameter(Mandatory = $true)][string]$StagedBinariesDir,
        [string]$Triplet = ''
    )

    $policy = Get-Content -LiteralPath $PolicyPath -Raw | ConvertFrom-Json
    if ([string]::IsNullOrWhiteSpace($Triplet)) { $Triplet = [string]$policy.targetTriplet }
    if (Test-Path -LiteralPath $Root) { throw "synthetic dependency fixture root already exists: $Root" }
    New-Item -ItemType Directory -Path $Root -Force | Out-Null

    $vcpkgRoot = Join-Path $Root 'vcpkg'
    $installedRoot = Join-Path $Root 'vcpkg_installed'
    New-Item -ItemType Directory -Path (Join-Path $vcpkgRoot 'scripts\buildsystems') -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $vcpkgRoot '.vcpkg-root'), "SYNTHETIC CONTRACT-TEST VCPKG ROOT - NOT A REAL VCPKG CHECKOUT`n", (New-Object System.Text.UTF8Encoding($false)))
    [IO.File]::WriteAllText((Join-Path $vcpkgRoot 'scripts\buildsystems\vcpkg.cmake'), "# SYNTHETIC CONTRACT-TEST TOOLCHAIN - NOT REAL VCPKG`n", (New-Object System.Text.UTF8Encoding($false)))

    foreach ($dep in @($policy.dependencies)) {
        $port = [string]$dep.port
        $portDir = Join-Path $vcpkgRoot "ports\$port"
        New-Item -ItemType Directory -Path (Join-Path $portDir 'patches') -Force | Out-Null
        $recipeJson = [ordered]@{
            name          = $port
            version       = [string]$dep.version
            'port-version' = [int]$dep.portVersion
            description   = 'SYNTHETIC CONTRACT-TEST FIXTURE - NOT A REAL VCPKG PORT'
        }
        [IO.File]::WriteAllText((Join-Path $portDir 'vcpkg.json'), ($recipeJson | ConvertTo-Json -Depth 4), (New-Object System.Text.UTF8Encoding($false)))
        $portfile = @"
# SYNTHETIC CONTRACT-TEST FIXTURE - NOT A REAL VCPKG PORT
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO $($dep.upstreamRepository)
    REF "v`${VERSION}"
    SHA512 $($dep.archiveSha512)
    HEAD_REF master
)
"@
        [IO.File]::WriteAllText((Join-Path $portDir 'portfile.cmake'), $portfile, (New-Object System.Text.UTF8Encoding($false)))
        [IO.File]::WriteAllText((Join-Path $portDir 'patches\0001-synthetic-contract-test.patch'), "SYNTHETIC CONTRACT-TEST PATCH - NOT A REAL PATCH`n", (New-Object System.Text.UTF8Encoding($false)))
    }

    & git -C $vcpkgRoot init -q
    if ($LASTEXITCODE -ne 0) { throw "git init failed for synthetic vcpkg fixture" }
    & git -C $vcpkgRoot add .
    if ($LASTEXITCODE -ne 0) { throw "git add failed for synthetic vcpkg fixture" }
    & git -C $vcpkgRoot -c user.email="contract-tests@example.invalid" -c user.name="Contract Test Fixture" commit -q -m "synthetic vcpkg ports"
    if ($LASTEXITCODE -ne 0) { throw "git commit failed for synthetic vcpkg ports" }

    $treeHashes = @{}
    foreach ($dep in @($policy.dependencies)) {
        $port = [string]$dep.port
        $tree = (& git -C $vcpkgRoot rev-parse "HEAD:ports/$port").Trim()
        if ($tree -notmatch '^[0-9a-f]{40}$') { throw "synthetic vcpkg tree hash unavailable for $port" }
        $treeHashes[$port] = $tree
    }

    $defaults = [ordered]@{}
    $versionFiles = @{}
    foreach ($dep in @($policy.dependencies)) {
        $port = [string]$dep.port
        $defaults[$port] = [ordered]@{
            baseline      = [string]$dep.version
            'port-version' = [int]$dep.portVersion
        }
        $versionFiles[$port] = [ordered]@{
            versions = @(
                [ordered]@{
                    'git-tree'    = $treeHashes[$port]
                    version      = [string]$dep.version
                    'port-version' = [int]$dep.portVersion
                }
            )
        }
    }
    New-Item -ItemType Directory -Path (Join-Path $vcpkgRoot 'versions') -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $vcpkgRoot 'versions\baseline.json'), ([ordered]@{ default = $defaults } | ConvertTo-Json -Depth 8), (New-Object System.Text.UTF8Encoding($false)))
    foreach ($dep in @($policy.dependencies)) {
        $port = [string]$dep.port
        $letterDir = Join-Path $vcpkgRoot ('versions\' + $port.Substring(0, 1).ToLowerInvariant() + '-')
        New-Item -ItemType Directory -Path $letterDir -Force | Out-Null
        [IO.File]::WriteAllText((Join-Path $letterDir ($port + '.json')), ($versionFiles[$port] | ConvertTo-Json -Depth 8), (New-Object System.Text.UTF8Encoding($false)))
    }
    & git -C $vcpkgRoot add .
    if ($LASTEXITCODE -ne 0) { throw "git add failed for synthetic vcpkg versions" }
    & git -C $vcpkgRoot -c user.email="contract-tests@example.invalid" -c user.name="Contract Test Fixture" commit -q -m "synthetic vcpkg versions"
    if ($LASTEXITCODE -ne 0) { throw "git commit failed for synthetic vcpkg versions" }
    $baseline = (& git -C $vcpkgRoot rev-parse HEAD).Trim()
    if ($baseline -notmatch '^[0-9a-f]{40}$') { throw "synthetic vcpkg baseline unavailable" }

    New-Item -ItemType Directory -Path (Join-Path $installedRoot "$Triplet\bin") -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $installedRoot 'vcpkg\info') -Force | Out-Null
    $statusBlocks = @()
    $listFiles = @{}
    foreach ($dep in @($policy.dependencies)) {
        $port = [string]$dep.port
        $stagedPath = Join-Path $StagedBinariesDir ([string]$dep.stagedDll)
        if (-not (Test-Path -LiteralPath $stagedPath -PathType Leaf)) { throw "synthetic installed fixture is missing staged $($dep.stagedDll)" }
        $installedRelative = ([string]$dep.installedDllRelativePath).Replace('\', '/')
        $installedPath = Join-Path $installedRoot ($Triplet + '\' + $installedRelative.Replace('/', '\'))
        $installedParent = Split-Path -Parent $installedPath
        if (-not (Test-Path -LiteralPath $installedParent -PathType Container)) { New-Item -ItemType Directory -Path $installedParent -Force | Out-Null }
        Copy-Item -LiteralPath $stagedPath -Destination $installedPath -Force
        $dllSha = (Get-FileHash -LiteralPath $stagedPath -Algorithm SHA256).Hash.ToLowerInvariant()
        $statusBlocks += @"
Package: $port
Version: $($dep.version)
Architecture: $Triplet
Multi-Arch: same
Abi: $dllSha
Status: install ok installed
"@
        $listName = "$($port)_$($dep.version)_$Triplet.list"
        $listFiles[$port] = $listName
        $listText = "$Triplet/$installedRelative`n$Triplet/share/$port/copyright`n"
        [IO.File]::WriteAllText((Join-Path $installedRoot "vcpkg\info\$listName"), $listText, (New-Object System.Text.UTF8Encoding($false)))
    }
    [IO.File]::WriteAllText((Join-Path $installedRoot 'vcpkg\status'), (($statusBlocks -join "`n`n") + "`n"), (New-Object System.Text.UTF8Encoding($false)))

    return [pscustomobject]@{
        Root          = $Root
        VcpkgRoot     = $vcpkgRoot
        InstalledRoot = $installedRoot
        Baseline      = $baseline
        Triplet       = $Triplet
        StatusPath    = (Join-Path $installedRoot 'vcpkg\status')
        ListFiles     = $listFiles
    }
}

function Set-TestVcpkgConfiguration {
    param(
        [Parameter(Mandatory = $true)][string]$RepoRoot,
        [Parameter(Mandatory = $true)][string]$Baseline,
        [string]$Repository = 'https://github.com/microsoft/vcpkg'
    )
    $config = [ordered]@{
        'default-registry' = [ordered]@{
            kind       = 'git'
            repository = $Repository
            baseline   = $Baseline
        }
        registries = @()
    }
    [IO.File]::WriteAllText((Join-Path $RepoRoot 'vcpkg-configuration.json'), ($config | ConvertTo-Json -Depth 6), (New-Object System.Text.UTF8Encoding($false)))
}

function New-TestDependencyBuildCache {
    <#
      Writes a SYNTHETIC CMakeCache.txt containing the effective vcpkg values
      expected by generate-build-provenance.ps1. The dummy cl.exe/cmake.exe
      files exist only so the generator's observation checks can run.
    #>
    param(
        [Parameter(Mandatory = $true)][string]$BuildDir,
        [Parameter(Mandatory = $true)][string]$SourceRepoRoot,
        [Parameter(Mandatory = $true)][string]$VcpkgRoot,
        [Parameter(Mandatory = $true)][string]$InstalledDir,
        [Parameter(Mandatory = $true)][string]$Triplet,
        [string]$OverlayPorts = '',
        [string]$OverlayTriplets = ''
    )
    if (-not (Test-Path -LiteralPath $BuildDir -PathType Container)) { New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null }
    $dummyCl = Join-Path $BuildDir 'synthetic-cl.exe'
    $dummyCmake = Join-Path $BuildDir 'synthetic-cmake.exe'
    if (-not (Test-Path -LiteralPath $dummyCl -PathType Leaf)) { [IO.File]::WriteAllText($dummyCl, "SYNTHETIC CONTRACT-TEST cl.exe PLACEHOLDER`n", (New-Object System.Text.UTF8Encoding($false))) }
    if (-not (Test-Path -LiteralPath $dummyCmake -PathType Leaf)) { [IO.File]::WriteAllText($dummyCmake, "SYNTHETIC CONTRACT-TEST cmake.exe PLACEHOLDER`n", (New-Object System.Text.UTF8Encoding($false))) }
    if ([string]::IsNullOrWhiteSpace($OverlayTriplets)) { $OverlayTriplets = Join-Path $SourceRepoRoot 'triplets' }
    $sourceText = $SourceRepoRoot.Replace('\', '/')
    $cache = @(
        "CMAKE_HOME_DIRECTORY:INTERNAL=$sourceText",
        "OZVIEWER_PRINT_DIAGNOSTICS:BOOL=OFF",
        "CMAKE_BUILD_TYPE:STRING=Release",
        "CMAKE_EXE_LINKER_FLAGS:STRING=/machine:x64",
        "CMAKE_CXX_COMPILER:FILEPATH=$($dummyCl.Replace('\', '/'))",
        "CMAKE_COMMAND:INTERNAL=$($dummyCmake.Replace('\', '/'))",
        "CMAKE_TOOLCHAIN_FILE:FILEPATH=$((Join-Path $VcpkgRoot 'scripts\buildsystems\vcpkg.cmake').Replace('\', '/'))",
        "Z_VCPKG_ROOT_DIR:INTERNAL=$($VcpkgRoot.Replace('\', '/'))",
        "VCPKG_INSTALLED_DIR:PATH=$($InstalledDir.Replace('\', '/'))",
        "_VCPKG_INSTALLED_DIR:PATH=$($InstalledDir.Replace('\', '/'))",
        "VCPKG_MANIFEST_MODE:BOOL=ON",
        "VCPKG_MANIFEST_DIR:PATH=$sourceText",
        "VCPKG_TARGET_TRIPLET:STRING=$Triplet",
        "VCPKG_OVERLAY_PORTS:STRING=$($OverlayPorts.Replace('\', '/'))",
        "VCPKG_OVERLAY_TRIPLETS:STRING=$($OverlayTriplets.Replace('\', '/'))"
    )
    [IO.File]::WriteAllText((Join-Path $BuildDir 'CMakeCache.txt'), (($cache -join "`n") + "`n"), (New-Object System.Text.UTF8Encoding($false)))
    return [pscustomobject]@{ BuildDir = $BuildDir; CompilerPath = $dummyCl; CmakePath = $dummyCmake }
}

function New-TestLgplSourceZip {
    <#
      Creates a SYNTHETIC LGPL corresponding-source ZIP from a synthetic vcpkg
      recipe checkout, the synthetic triplet, and caller-supplied archives.
      Every entry is labelled contract-test data.
    #>
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$PolicyPath,
        [Parameter(Mandatory = $true)][string]$VcpkgRoot,
        [Parameter(Mandatory = $true)][string]$RepoRoot,
        [Parameter(Mandatory = $true)][string]$TarballDir
    )
    $policy = Get-Content -LiteralPath $PolicyPath -Raw | ConvertFrom-Json
    $parent = Split-Path -Parent $Path
    if (-not [string]::IsNullOrWhiteSpace($parent) -and -not (Test-Path -LiteralPath $parent -PathType Container)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    $stage = Join-Path $parent ('lgpl-source-stage-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path (Join-Path $stage 'upstream-sources') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $stage 'vcpkg-ports') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $stage 'triplets') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $stage 'licenses') -Force | Out-Null
    foreach ($dep in @($policy.dependencies)) {
        $archiveSource = Join-Path $TarballDir ([string]$dep.archiveName)
        if (-not (Test-Path -LiteralPath $archiveSource -PathType Leaf)) { throw "synthetic LGPL archive missing: $archiveSource" }
        Copy-Item -LiteralPath $archiveSource -Destination (Join-Path $stage ('upstream-sources\' + [string]$dep.archiveName)) -Force
        $portSource = Join-Path $VcpkgRoot ('ports\' + [string]$dep.port)
        if (-not (Test-Path -LiteralPath $portSource -PathType Container)) { throw "synthetic vcpkg port missing: $portSource" }
        Copy-Item -LiteralPath $portSource -Destination (Join-Path $stage ('vcpkg-ports\' + [string]$dep.port)) -Recurse -Force
    }
    $tripletRelative = ([string]$policy.tripletFile).Replace('\', '/')
    $tripletSource = Join-Path $RepoRoot $tripletRelative.Replace('/', '\')
    if (-not (Test-Path -LiteralPath $tripletSource -PathType Leaf)) { throw "synthetic repository triplet missing: $tripletSource" }
    Copy-Item -LiteralPath $tripletSource -Destination (Join-Path $stage ($tripletRelative.Replace('/', '\'))) -Force
    [IO.File]::WriteAllText((Join-Path $stage 'licenses\libheif-COPYING.txt'), "SYNTHETIC CONTRACT-TEST LICENSE PLACEHOLDER`n", (New-Object System.Text.UTF8Encoding($false)))
    [IO.File]::WriteAllText((Join-Path $stage 'licenses\libde265-COPYING.txt'), "SYNTHETIC CONTRACT-TEST LICENSE PLACEHOLDER`n", (New-Object System.Text.UTF8Encoding($false)))
    [IO.File]::WriteAllText((Join-Path $stage 'README-BUILD-LGPL.txt'), "SYNTHETIC CONTRACT-TEST INSTRUCTIONS - NOT A REAL REBUILD GUIDE`n", (New-Object System.Text.UTF8Encoding($false)))
    [IO.File]::WriteAllText((Join-Path $stage 'build-lgpl-dlls.ps1'), "SYNTHETIC CONTRACT-TEST SCRIPT - NOT EXECUTED`n", (New-Object System.Text.UTF8Encoding($false)))
    [IO.File]::WriteAllText((Join-Path $stage 'SYNTHETIC-CONTRACT-TEST.txt'), "SYNTHETIC CONTRACT-TEST LGPL SOURCE ZIP - NOT A REAL RELEASE ASSET`n", (New-Object System.Text.UTF8Encoding($false)))
    Add-Type -AssemblyName System.IO.Compression.FileSystem | Out-Null
    if (Test-Path -LiteralPath $Path) { Remove-Item -LiteralPath $Path -Force }
    [System.IO.Compression.ZipFile]::CreateFromDirectory($stage, $Path)
    return [pscustomobject]@{ Path = $Path; Sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant(); StageDir = $stage }
}



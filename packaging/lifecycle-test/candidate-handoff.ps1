<#
.SYNOPSIS
  Candidate handoff helper for creating and verifying immutable candidate handoff manifests and layouts.

.DESCRIPTION
  Provides:
    New-CandidateHandoffManifest:
      Collects metadata and file hashes from a clean candidate directory and produces
      a data-only JSON manifest (no secrets or arbitrary paths).
    Resolve-CandidateHandoffDirectory:
      Verifies that a downloaded artifact parent directory contains EXACTLY ONE single
      expected directory matching the upload artifact name, with no extra files/folders
      or reparse points. Returns the verified inner candidate directory path.
    Test-CandidateHandoffDirectory:
      Verifies that the inner candidate directory contains EXACTLY the expected files
      (manifest + fixed candidate files), matching manifest metadata (repo, workflow SHA,
      run ID, run attempt, candidate release ID, candidate tag), no directory traversal,
      no directories or reparse points, no case collisions, and validates that file hashes
      match expected hashes.
    Assert-SourceIdentityFormat:
      Validates independently supplied expected source commit/tree identities.
    New-PortableClosureManifest:
      Builds a deterministic, hash-pinned closure from a payload directory that has already
      passed validate-portable-payload.ps1. The closure records declared file digests plus
      the independently supplied source identity; it is not a substitute for provenance
      verification and never reads source identity out of BUILD-PROVENANCE.json.
    Test-PortableClosureManifest:
      Re-verifies and parses a pinned closure before it is handed to a lifecycle run.
    Test-InstalledRuntimeClosure:
      Compares the installed file set against the validated closure digests.
    Test-InstallLocationMatches / Test-CommandTargetMatches:
      Pure checks for uninstall InstallLocation and the registered shell command target.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function New-CandidateHandoffManifest {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Repository,
        [Parameter(Mandatory = $true)][string]$WorkflowSha,
        [Parameter(Mandatory = $true)][string]$RunId,
        [Parameter(Mandatory = $true)][string]$RunAttempt,
        [Parameter(Mandatory = $true)][string]$CandidateReleaseId,
        [Parameter(Mandatory = $true)][string]$CandidateTag,
        [Parameter(Mandatory = $true)][string]$CandidateDir,
        [Parameter(Mandatory = $true)][string[]]$FixedAssetNames,
        [Parameter(Mandatory = $true)][string]$ManifestPath,
        [string]$ExpectedCommitSha = "",
        [string]$ExpectedTreeSha = "",
        [string]$TagCommitSha = ""
    )

    $hasIdentity = -not [string]::IsNullOrWhiteSpace($ExpectedCommitSha) -or
                   -not [string]::IsNullOrWhiteSpace($ExpectedTreeSha) -or
                   -not [string]::IsNullOrWhiteSpace($TagCommitSha)
    if ($hasIdentity) {
        if ([string]::IsNullOrWhiteSpace($ExpectedCommitSha) -or
            [string]::IsNullOrWhiteSpace($ExpectedTreeSha) -or
            [string]::IsNullOrWhiteSpace($TagCommitSha)) {
            throw "ExpectedCommitSha, ExpectedTreeSha, and TagCommitSha must all be supplied together."
        }
        Assert-SourceIdentityFormat -CommitSha $ExpectedCommitSha -TreeSha $ExpectedTreeSha -TagCommitSha $TagCommitSha
    }

    if (-not (Test-Path -LiteralPath $CandidateDir -PathType Container)) {
        throw "Candidate directory not found: '$CandidateDir'"
    }

    $filesList = [System.Collections.Generic.List[PSObject]]::new()
    foreach ($name in ($FixedAssetNames | Sort-Object)) {
        $filePath = Join-Path $CandidateDir $name
        if (-not (Test-Path -LiteralPath $filePath -PathType Leaf)) {
            throw "Fixed asset '$name' not found in candidate directory: '$CandidateDir'"
        }
        $item = Get-Item -LiteralPath $filePath
        $hash = (Get-FileHash -LiteralPath $filePath -Algorithm SHA256).Hash.ToLowerInvariant()
        $filesList.Add([ordered]@{
            name   = $name
            bytes  = $item.Length
            sha256 = $hash
        })
    }

    $manifest = [ordered]@{
        schema             = 1
        repository         = $Repository.Trim()
        workflowSha        = $WorkflowSha.Trim()
        runId              = $RunId.Trim()
        runAttempt         = $RunAttempt.Trim()
        candidateReleaseId = $CandidateReleaseId.Trim()
        candidateTag       = $CandidateTag.Trim()
        files              = $filesList.ToArray()
    }
    if ($hasIdentity) {
        $manifest['expectedSourceCommitSha'] = $ExpectedCommitSha.Trim().ToLowerInvariant()
        $manifest['expectedSourceTreeSha'] = $ExpectedTreeSha.Trim().ToLowerInvariant()
        $manifest['resolvedTagCommitSha'] = $TagCommitSha.Trim().ToLowerInvariant()
    }

    $manifestDir = Split-Path -Parent $ManifestPath
    if (-not [string]::IsNullOrWhiteSpace($manifestDir) -and -not (Test-Path -LiteralPath $manifestDir)) {
        New-Item -ItemType Directory -Path $manifestDir -Force | Out-Null
    }

    $json = $manifest | ConvertTo-Json -Depth 5
    [System.IO.File]::WriteAllText($ManifestPath, $json, (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "Created candidate handoff manifest: $ManifestPath"
}

function Resolve-CandidateHandoffDirectory {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$ParentDir,
        [Parameter(Mandatory = $true)][string]$ExpectedArtifactDirectoryName
    )

    if (-not (Test-Path -LiteralPath $ParentDir -PathType Container)) {
        throw "Parent handoff directory not found: '$ParentDir'"
    }

    # 1. Enumerate immediate children of ParentDir
    $parentEntries = @(Get-ChildItem -LiteralPath $ParentDir -Force)
    if ($parentEntries.Count -eq 0) {
        throw "Handoff parent directory is empty: '$ParentDir'"
    }

    # Ensure no reparse points exist in the parent directory
    foreach ($entry in $parentEntries) {
        if (($entry.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Security violation: Reparse point or symbolic link detected in handoff parent directory: '$($entry.Name)'"
        }
    }

    # Ensure there are no loose files in the parent directory
    $looseFiles = @($parentEntries | Where-Object { -not $_.PSIsContainer })
    if ($looseFiles.Count -gt 0) {
        throw "Security violation: Unexpected file(s) found in handoff parent directory: $(($looseFiles | ForEach-Object { $_.Name }) -join ', ')"
    }

    # Ensure there is exactly ONE directory in the parent directory
    $subDirs = @($parentEntries | Where-Object { $_.PSIsContainer })
    if ($subDirs.Count -ne 1) {
        throw "Security violation: Expected exactly 1 artifact directory in handoff parent, found $($subDirs.Count): $(($subDirs | ForEach-Object { $_.Name }) -join ', ')"
    }

    $innerDir = $subDirs[0]
    if ($innerDir.Name -ne $ExpectedArtifactDirectoryName) {
        throw "Security violation: Artifact directory name mismatch! Expected '$ExpectedArtifactDirectoryName', got '$($innerDir.Name)'"
    }

    return $innerDir.FullName
}

function Test-CandidateHandoffDirectory {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$CandidateDir,
        [Parameter(Mandatory = $true)][string]$ExpectedRepository,
        [Parameter(Mandatory = $true)][string]$ExpectedWorkflowSha,
        [Parameter(Mandatory = $true)][string]$ExpectedRunId,
        [Parameter(Mandatory = $true)][string]$ExpectedRunAttempt,
        [Parameter(Mandatory = $true)][string]$ExpectedCandidateReleaseId,
        [Parameter(Mandatory = $true)][string]$ExpectedCandidateTag,
        [Parameter(Mandatory = $true)][hashtable]$ExpectedHashesByName, # name -> sha256
        [string]$ManifestFileName = "candidate-handoff-manifest.json",
        [string]$ExpectedCommitSha = "",
        [string]$ExpectedTreeSha = "",
        [string]$ExpectedTagCommitSha = ""
    )

    if (-not (Test-Path -LiteralPath $CandidateDir -PathType Container)) {
        throw "Handoff validation failed: Candidate directory not found at '$CandidateDir'"
    }

    # 1. Enumerate all entries in CandidateDir
    $allEntries = @(Get-ChildItem -LiteralPath $CandidateDir -Force)
    $allNames = @($allEntries | ForEach-Object { $_.Name })

    # Check for duplicate names (case-insensitive collisions)
    $lowerSeen = @{}
    foreach ($entry in $allEntries) {
        $l = $entry.Name.ToLowerInvariant()
        if ($lowerSeen.ContainsKey($l)) {
            throw "Security violation: Duplicate or case-colliding entry found in handoff directory: '$($entry.Name)'"
        }
        $lowerSeen[$l] = $true
    }

    # Check that there are NO subdirectories or reparse points / symlinks
    foreach ($entry in $allEntries) {
        if ($entry.PSIsContainer) {
            throw "Security violation: Unexpected directory found in candidate handoff: '$($entry.Name)'"
        }
        if (($entry.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Security violation: Reparse point or symbolic link detected in handoff directory: '$($entry.Name)'"
        }
    }

    # 2. Check for manifest presence
    $manifestPath = Join-Path $CandidateDir $ManifestFileName
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw "Handoff validation failed: Manifest '$ManifestFileName' not found in candidate directory"
    }

    # Verify exact set of files allowed: Manifest + keys in ExpectedHashesByName
    $expectedNames = @($ManifestFileName) + @($ExpectedHashesByName.Keys)
    $expectedNamesSorted = @($expectedNames | Sort-Object)
    $actualNamesSorted = @($allNames | Sort-Object)

    $extraFiles = @($actualNamesSorted | Where-Object { $expectedNamesSorted -notcontains $_ })
    if ($extraFiles.Count -gt 0) {
        throw "Security violation: Extra unapproved file(s) found in candidate handoff: $(($extraFiles) -join ', ')"
    }

    $missingFiles = @($expectedNamesSorted | Where-Object { $actualNamesSorted -notcontains $_ })
    if ($missingFiles.Count -gt 0) {
        throw "Handoff validation failed: Missing expected file(s) in candidate handoff: $(($missingFiles) -join ', ')"
    }

    # 3. Parse and validate manifest contents
    $manifestRaw = Get-Content -LiteralPath $manifestPath -Raw
    $manifest = $manifestRaw | ConvertFrom-Json

    if ("$($manifest.repository)".Trim() -ne $ExpectedRepository.Trim()) {
        throw "Manifest repository mismatch: expected '$ExpectedRepository', got '$($manifest.repository)'"
    }
    if ("$($manifest.workflowSha)".Trim().ToLowerInvariant() -ne $ExpectedWorkflowSha.Trim().ToLowerInvariant()) {
        throw "Manifest workflowSha mismatch: expected '$ExpectedWorkflowSha', got '$($manifest.workflowSha)'"
    }
    if ("$($manifest.runId)".Trim() -ne $ExpectedRunId.Trim()) {
        throw "Manifest runId mismatch: expected '$ExpectedRunId', got '$($manifest.runId)'"
    }
    if ("$($manifest.runAttempt)".Trim() -ne $ExpectedRunAttempt.Trim()) {
        throw "Manifest runAttempt mismatch: expected '$ExpectedRunAttempt', got '$($manifest.runAttempt)'"
    }
    if ("$($manifest.candidateReleaseId)".Trim() -ne $ExpectedCandidateReleaseId.Trim()) {
        throw "Manifest candidateReleaseId mismatch: expected '$ExpectedCandidateReleaseId', got '$($manifest.candidateReleaseId)'"
    }
    if ("$($manifest.candidateTag)".Trim() -ne $ExpectedCandidateTag.Trim()) {
        throw "Manifest candidateTag mismatch: expected '$ExpectedCandidateTag', got '$($manifest.candidateTag)'"
    }

    if (-not [string]::IsNullOrWhiteSpace($ExpectedCommitSha) -or
        -not [string]::IsNullOrWhiteSpace($ExpectedTreeSha) -or
        -not [string]::IsNullOrWhiteSpace($ExpectedTagCommitSha)) {
        Assert-SourceIdentityFormat -CommitSha $ExpectedCommitSha -TreeSha $ExpectedTreeSha -TagCommitSha $ExpectedTagCommitSha
        foreach ($pair in @(
            @('expectedSourceCommitSha', $ExpectedCommitSha),
            @('expectedSourceTreeSha', $ExpectedTreeSha),
            @('resolvedTagCommitSha', $ExpectedTagCommitSha)
        )) {
            $propName = [string]$pair[0]
            $expectedValue = [string]$pair[1]
            if (-not $manifest.PSObject.Properties[$propName]) {
                throw "Manifest is missing independently supplied identity field '$propName'."
            }
            if ("$($manifest.$propName)".Trim().ToLowerInvariant() -ne $expectedValue.Trim().ToLowerInvariant()) {
                throw "Manifest $propName mismatch: expected '$expectedValue', got '$($manifest.$propName)'"
            }
        }
    }

    # 4. Rehash every expected file against the ORIGINAL workflow dispatch expected hashes
    foreach ($assetName in $ExpectedHashesByName.Keys) {
        $filePath = Join-Path $CandidateDir $assetName
        $actualHash = (Get-FileHash -LiteralPath $filePath -Algorithm SHA256).Hash.ToLowerInvariant()
        $expectedHash = $ExpectedHashesByName[$assetName].Trim().ToLowerInvariant()

        if ($expectedHash.Length -ne 64 -or $expectedHash -notmatch '^[0-9a-f]{64}$') {
            throw "Security violation: Expected SHA-256 for '$assetName' is not a valid 64-character lowercase hex string: '$expectedHash'"
        }

        if ($actualHash -ne $expectedHash) {
            throw "Cryptographic hash mismatch for candidate asset '$assetName': expected '$expectedHash', computed '$actualHash'"
        }
    }

    Write-Host "Candidate handoff directory and manifest validation PASSED for repository $ExpectedRepository, run $ExpectedRunId attempt $ExpectedRunAttempt."
}

function Resolve-ReleaseAssetSpec {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$CandidateTag
    )

    if ($null -eq $CandidateTag -or -not ($CandidateTag -cmatch '\Av(\d+\.\d+\.\d+)\z')) {
        throw "Invalid CandidateTag syntax: '$CandidateTag'. Expected strict semver tag 'vX.Y.Z' (e.g. 'v0.1.2')."
    }
    $trimmedTag = $CandidateTag
    $version = $Matches[1]

    if ($version -in @("0.1.1", "0.1.2")) {
        $family = "pikaview"
        $installer = "$family-$version-win-x64-installer.exe"
        $portable = "$family-$version-win-x64-portable.zip"
        $source = "$family-$version-lgpl-corresponding-source.zip"
        $checksum = "SHA256SUMS"
        $expected = @($installer, $portable, $source, $checksum)
    } else {
        throw "Unsupported CandidateTag version '$trimmedTag'. This workflow only validates v0.1.1 or v0.1.2 (pikaview); v0.1.0 is supported solely as the historical upgrade base."
    }

    return [ordered]@{
        Tag                = $trimmedTag
        Version            = $version
        PackageFamily      = $family
        InstallerName      = $installer
        PortableName       = $portable
        SourceName         = $source
        ChecksumFileName   = $checksum
        ExpectedAssetNames = $expected
    }
}

function Test-Sha256SumsFile {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Sha256SumsPath,
        [Parameter(Mandatory = $true)][hashtable]$ExpectedHashesByName
    )

    if (-not (Test-Path -LiteralPath $Sha256SumsPath -PathType Leaf)) {
        throw "SHA256SUMS file not found: '$Sha256SumsPath'"
    }

    $lines = Get-Content -LiteralPath $Sha256SumsPath -Encoding UTF8
    $parsed = @{}
    foreach ($rawLine in $lines) {
        if ([string]::IsNullOrWhiteSpace($rawLine)) {
            continue
        }
        $line = $rawLine.Trim()
        if ($line -notmatch '^([0-9a-f]{64})\s+\*?([^\s/\\]+)$') {
            throw "Malformed line in SHA256SUMS: '$rawLine'"
        }
        $hash = $Matches[1]
        $name = $Matches[2]
        if ($name.Contains('..')) {
            throw "Security violation: Path traversal in SHA256SUMS entry '$name'."
        }
        if ($parsed.ContainsKey($name)) {
            throw "Duplicate entry in SHA256SUMS: '$name'."
        }
        $parsed[$name] = $hash
    }

    if ($parsed.Count -ne $ExpectedHashesByName.Count) {
        throw "SHA256SUMS entry count mismatch: expected $($ExpectedHashesByName.Count), got $($parsed.Count)."
    }

    foreach ($expectedName in $ExpectedHashesByName.Keys) {
        if (-not $parsed.ContainsKey($expectedName)) {
            throw "SHA256SUMS missing expected asset '$expectedName'."
        }
        $expectedHash = $ExpectedHashesByName[$expectedName].Trim().ToLowerInvariant()
        if ($parsed[$expectedName] -ne $expectedHash) {
            throw "SHA256SUMS hash mismatch for '$expectedName': expected '$expectedHash', got '$($parsed[$expectedName])'."
        }
    }

    Write-Host "SHA256SUMS validation PASSED for $($parsed.Count) expected assets."
}

function Assert-SourceIdentityFormat {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$CommitSha,
        [Parameter(Mandatory = $true)][string]$TreeSha,
        [string]$TagCommitSha = ""
    )

    $commit = $CommitSha.Trim().ToLowerInvariant()
    if ($commit -notmatch '^[0-9a-f]{40}$') {
        throw "Expected candidate commit SHA must be exactly 40 lowercase hex characters; got '$CommitSha'."
    }
    $tree = $TreeSha.Trim().ToLowerInvariant()
    if ($tree -notmatch '^[0-9a-f]{40}$') {
        throw "Expected candidate tree SHA must be exactly 40 lowercase hex characters; got '$TreeSha'."
    }
    if (-not [string]::IsNullOrWhiteSpace($TagCommitSha)) {
        $tagCommit = $TagCommitSha.Trim().ToLowerInvariant()
        if ($tagCommit -notmatch '^[0-9a-f]{40}$') {
            throw "Resolved tag commit SHA must be exactly 40 lowercase hex characters; got '$TagCommitSha'."
        }
    }
    return [ordered]@{
        commitSha = $commit
        treeSha   = $tree
    }
}

function Read-JsonFileStrict {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Description
    )
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description not found: '$Path'"
    }
    try {
        return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    }
    catch {
        throw "$Description is not valid JSON: $($_.Exception.Message)"
    }
}

function Assert-BarePayloadFileName {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Context
    )
    if ([string]::IsNullOrWhiteSpace($Name) -or
        [System.IO.Path]::GetFileName($Name) -ne $Name -or
        $Name -eq '.' -or $Name -eq '..' -or
        $Name.Contains('/') -or $Name.Contains('\')) {
        throw "$Context contains an unsafe non-bare filename: '$Name'"
    }
}

function New-PortableClosureManifest {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$PackageDir,
        [Parameter(Mandatory = $true)][string]$PortableZipPath,
        [Parameter(Mandatory = $true)][string]$ExpectedPayloadSha256,
        [Parameter(Mandatory = $true)][string]$ExpectedVersion,
        [Parameter(Mandatory = $true)][string]$ExpectedPackageName,
        [Parameter(Mandatory = $true)][string]$ExpectedCommitSha,
        [Parameter(Mandatory = $true)][string]$ExpectedTreeSha,
        [Parameter(Mandatory = $true)][string]$OutputPath,
        [string]$ExpectedPlatform = "win-x64"
    )

    Assert-SourceIdentityFormat -CommitSha $ExpectedCommitSha -TreeSha $ExpectedTreeSha | Out-Null

    $payloadHash = $ExpectedPayloadSha256.Trim().ToLowerInvariant()
    if ($payloadHash -notmatch '^[0-9a-f]{64}$') {
        throw "Expected payload SHA-256 must be exactly 64 lowercase hex characters; got '$ExpectedPayloadSha256'."
    }
    if (-not (Test-Path -LiteralPath $PackageDir -PathType Container)) {
        throw "Portable package directory not found: '$PackageDir'"
    }
    if (-not (Test-Path -LiteralPath $PortableZipPath -PathType Leaf)) {
        throw "Portable ZIP not found: '$PortableZipPath'"
    }
    $PackageDir = (Resolve-Path -LiteralPath $PackageDir).Path
    $PortableZipPath = (Resolve-Path -LiteralPath $PortableZipPath).Path

    $actualPayloadHash = (Get-FileHash -LiteralPath $PortableZipPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualPayloadHash -ne $payloadHash) {
        throw "Portable ZIP SHA-256 mismatch: expected '$payloadHash', computed '$actualPayloadHash'."
    }

    $manifestPath = Join-Path $PackageDir "package-manifest.json"
    $manifest = Read-JsonFileStrict -Path $manifestPath -Description "package-manifest.json"
    foreach ($field in @('schema', 'packageName', 'version', 'platform', 'entrypoint', 'supportedExtensions', 'dynamicDependencies', 'files')) {
        if (-not $manifest.PSObject.Properties[$field]) {
            throw "package-manifest.json is missing required field '$field'."
        }
    }
    if ([int]$manifest.schema -ne 1) {
        throw "package-manifest.json schema must be 1; got '$($manifest.schema)'."
    }
    if ([string]$manifest.packageName -ne $ExpectedPackageName.Trim()) {
        throw "package-manifest.json packageName '$($manifest.packageName)' does not match expected '$ExpectedPackageName'."
    }
    if ([string]$manifest.version -ne $ExpectedVersion.Trim()) {
        throw "package-manifest.json version '$($manifest.version)' does not match expected '$ExpectedVersion'."
    }
    if ([string]$manifest.platform -ne $ExpectedPlatform.Trim()) {
        throw "package-manifest.json platform '$($manifest.platform)' does not match expected '$ExpectedPlatform'."
    }
    $entrypoint = [string]$manifest.entrypoint
    Assert-BarePayloadFileName -Name $entrypoint -Context "package-manifest.json entrypoint"

    $fileEntries = @($manifest.files)
    if ($fileEntries.Count -eq 0) {
        throw "package-manifest.json files list is empty."
    }
    $seen = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    $closureFiles = [System.Collections.Generic.List[object]]::new()
    $declaredNames = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in $fileEntries) {
        foreach ($field in @('name', 'bytes', 'sha256')) {
            if (-not $entry.PSObject.Properties[$field]) {
                throw "package-manifest.json file entry is missing '$field'."
            }
        }
        $name = [string]$entry.name
        Assert-BarePayloadFileName -Name $name -Context "package-manifest.json files"
        if (-not $seen.Add($name)) {
            throw "package-manifest.json lists file '$name' more than once."
        }
        if ($name -eq "package-manifest.json") {
            throw "package-manifest.json must not list itself as a payload file."
        }
        $hash = ([string]$entry.sha256).Trim().ToLowerInvariant()
        if ($hash -notmatch '^[0-9a-f]{64}$') {
            throw "package-manifest.json file '$name' has an invalid SHA-256 value."
        }
        $bytes = [long]$entry.bytes
        if ($bytes -lt 0) {
            throw "package-manifest.json file '$name' has a negative byte count."
        }
        $path = Join-Path $PackageDir $name
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Declared payload file '$name' is missing from '$PackageDir'."
        }
        $item = Get-Item -LiteralPath $path
        if ([long]$item.Length -ne $bytes) {
            throw "Declared payload file '$name' byte count mismatch: manifest $bytes, on-disk $($item.Length)."
        }
        $actualHash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actualHash -ne $hash) {
            throw "Declared payload file '$name' SHA-256 mismatch: manifest $hash, on-disk $actualHash."
        }
        [void]$declaredNames.Add($name)
        $closureFiles.Add([ordered]@{ name = $name; bytes = $bytes; sha256 = $hash })
    }
    if (-not $declaredNames.Contains($entrypoint)) {
        throw "package-manifest.json entrypoint '$entrypoint' is not listed in files."
    }
    foreach ($dep in @($manifest.dynamicDependencies)) {
        $depName = [string]$dep
        if (-not $declaredNames.Contains($depName)) {
            throw "package-manifest.json dynamic dependency '$depName' is not listed in files."
        }
    }

    $packageManifestHash = (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $closure = [ordered]@{
        schema                    = 1
        packageName               = [string]$manifest.packageName
        version                   = [string]$manifest.version
        platform                  = [string]$manifest.platform
        entrypoint                = $entrypoint
        payloadSha256             = $payloadHash
        packageManifestSha256     = $packageManifestHash
        expectedSourceCommitSha   = $ExpectedCommitSha.Trim().ToLowerInvariant()
        expectedSourceTreeSha     = $ExpectedTreeSha.Trim().ToLowerInvariant()
        supportedExtensions       = @($manifest.supportedExtensions | ForEach-Object { [string]$_ })
        dynamicDependencies       = @($manifest.dynamicDependencies | ForEach-Object { [string]$_ })
        files                     = @($closureFiles.ToArray() | Sort-Object { $_.name })
    }

    $outDir = Split-Path -Parent $OutputPath
    if (-not [string]::IsNullOrWhiteSpace($outDir) -and -not (Test-Path -LiteralPath $outDir -PathType Container)) {
        New-Item -ItemType Directory -Path $outDir -Force | Out-Null
    }
    if (Test-Path -LiteralPath $OutputPath) {
        throw "Portable closure output already exists (refusing to replace): '$OutputPath'"
    }
    [System.IO.File]::WriteAllText($OutputPath, ($closure | ConvertTo-Json -Depth 8), (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "Created pinned portable closure manifest: $OutputPath"
}

function Test-PortableClosureManifest {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$ExpectedSha256,
        [string]$ExpectedVersion = "",
        [string]$ExpectedPackageName = "",
        [string]$ExpectedPayloadSha256 = "",
        [string]$ExpectedCommitSha = "",
        [string]$ExpectedTreeSha = ""
    )

    $expectedFileHash = $ExpectedSha256.Trim().ToLowerInvariant()
    if ($expectedFileHash -notmatch '^[0-9a-f]{64}$') {
        throw "Expected portable closure SHA-256 must be exactly 64 lowercase hex characters."
    }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Portable closure manifest not found: '$Path'"
    }
    $actualFileHash = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualFileHash -ne $expectedFileHash) {
        throw "Portable closure manifest SHA-256 mismatch: expected '$expectedFileHash', computed '$actualFileHash'."
    }

    $closure = Read-JsonFileStrict -Path $Path -Description "Portable closure manifest"
    foreach ($field in @('schema', 'packageName', 'version', 'platform', 'entrypoint', 'payloadSha256', 'packageManifestSha256', 'expectedSourceCommitSha', 'expectedSourceTreeSha', 'supportedExtensions', 'dynamicDependencies', 'files')) {
        if (-not $closure.PSObject.Properties[$field]) {
            throw "Portable closure manifest is missing required field '$field'."
        }
    }
    if ([int]$closure.schema -ne 1) {
        throw "Portable closure manifest schema must be 1; got '$($closure.schema)'."
    }
    if ([string]::IsNullOrWhiteSpace($ExpectedVersion) -or [string]$closure.version -ne $ExpectedVersion.Trim()) {
        throw "Portable closure version '$($closure.version)' does not match expected '$ExpectedVersion'."
    }
    if (-not [string]::IsNullOrWhiteSpace($ExpectedPackageName) -and [string]$closure.packageName -ne $ExpectedPackageName.Trim()) {
        throw "Portable closure packageName '$($closure.packageName)' does not match expected '$ExpectedPackageName'."
    }
    $closurePayloadHash = ([string]$closure.payloadSha256).Trim().ToLowerInvariant()
    if ($closurePayloadHash -notmatch '^[0-9a-f]{64}$') {
        throw "Portable closure payloadSha256 is not a valid SHA-256."
    }
    if (-not [string]::IsNullOrWhiteSpace($ExpectedPayloadSha256)) {
        $expectedPayload = $ExpectedPayloadSha256.Trim().ToLowerInvariant()
        if ($expectedPayload -notmatch '^[0-9a-f]{64}$' -or $expectedPayload -ne $closurePayloadHash) {
            throw "Portable closure payloadSha256 '$closurePayloadHash' does not match expected portable ZIP hash '$ExpectedPayloadSha256'."
        }
    }
    $closureManifestHash = ([string]$closure.packageManifestSha256).Trim().ToLowerInvariant()
    if ($closureManifestHash -notmatch '^[0-9a-f]{64}$') {
        throw "Portable closure packageManifestSha256 is not a valid SHA-256."
    }
    $closureCommit = ([string]$closure.expectedSourceCommitSha).Trim().ToLowerInvariant()
    $closureTree = ([string]$closure.expectedSourceTreeSha).Trim().ToLowerInvariant()
    if ($closureCommit -notmatch '^[0-9a-f]{40}$' -or $closureTree -notmatch '^[0-9a-f]{40}$') {
        throw "Portable closure source identity is not a valid 40-hex commit/tree pair."
    }
    if (-not [string]::IsNullOrWhiteSpace($ExpectedCommitSha) -and $closureCommit -ne $ExpectedCommitSha.Trim().ToLowerInvariant()) {
        throw "Portable closure expectedSourceCommitSha '$closureCommit' does not match expected '$ExpectedCommitSha'."
    }
    if (-not [string]::IsNullOrWhiteSpace($ExpectedTreeSha) -and $closureTree -ne $ExpectedTreeSha.Trim().ToLowerInvariant()) {
        throw "Portable closure expectedSourceTreeSha '$closureTree' does not match expected '$ExpectedTreeSha'."
    }

    $entrypoint = [string]$closure.entrypoint
    Assert-BarePayloadFileName -Name $entrypoint -Context "Portable closure entrypoint"
    $files = @($closure.files)
    if ($files.Count -eq 0) {
        throw "Portable closure files list is empty."
    }
    $seen = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    $declaredNames = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in $files) {
        foreach ($field in @('name', 'bytes', 'sha256')) {
            if (-not $entry.PSObject.Properties[$field]) {
                throw "Portable closure file entry is missing '$field'."
            }
        }
        $name = [string]$entry.name
        Assert-BarePayloadFileName -Name $name -Context "Portable closure files"
        if (-not $seen.Add($name)) {
            throw "Portable closure lists file '$name' more than once."
        }
        $hash = ([string]$entry.sha256).Trim().ToLowerInvariant()
        if ($hash -notmatch '^[0-9a-f]{64}$') {
            throw "Portable closure file '$name' has an invalid SHA-256 value."
        }
        if ([long]$entry.bytes -lt 0) {
            throw "Portable closure file '$name' has a negative byte count."
        }
        [void]$declaredNames.Add($name)
    }
    if (-not $declaredNames.Contains($entrypoint)) {
        throw "Portable closure entrypoint '$entrypoint' is not listed in files."
    }
    foreach ($dep in @($closure.dynamicDependencies)) {
        $depName = [string]$dep
        if (-not $declaredNames.Contains($depName)) {
            throw "Portable closure dynamic dependency '$depName' is not listed in files."
        }
    }
    return $closure
}

function Test-InstalledRuntimeClosure {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$InstallDir,
        [Parameter(Mandatory = $true)]$Closure
    )

    if (-not (Test-Path -LiteralPath $InstallDir -PathType Container)) {
        throw "Installed runtime directory not found: '$InstallDir'"
    }
    if ($null -eq $Closure -or -not $Closure.PSObject.Properties['files']) {
        throw "Validated portable closure is missing."
    }
    $files = @($Closure.files)
    if ($files.Count -eq 0) {
        throw "Validated portable closure contains no files."
    }
    foreach ($entry in $files) {
        foreach ($field in @('name', 'bytes', 'sha256')) {
            if (-not $entry.PSObject.Properties[$field]) {
                throw "Portable closure file entry is missing '$field'."
            }
        }
        $name = [string]$entry.name
        Assert-BarePayloadFileName -Name $name -Context "Portable closure files"
        $expectedHash = ([string]$entry.sha256).Trim().ToLowerInvariant()
        if ($expectedHash -notmatch '^[0-9a-f]{64}$') {
            throw "Portable closure file '$name' has an invalid SHA-256 value."
        }
        $path = Join-Path $InstallDir $name
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Installed runtime is missing closure file '$name' at '$path'."
        }
        $item = Get-Item -LiteralPath $path
        if ([long]$item.Length -ne [long]$entry.bytes) {
            throw "Installed runtime file '$name' byte count mismatch: closure $($entry.bytes), installed $($item.Length)."
        }
        $actualHash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actualHash -ne $expectedHash) {
            throw "Installed runtime file '$name' SHA-256 mismatch: closure $expectedHash, installed $actualHash."
        }
    }

    $manifestPath = Join-Path $InstallDir "package-manifest.json"
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw "Installed runtime is missing package-manifest.json."
    }
    $actualManifestHash = (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualManifestHash -ne ([string]$Closure.packageManifestSha256).Trim().ToLowerInvariant()) {
        throw "Installed package-manifest.json SHA-256 does not match the validated portable closure."
    }
    return $true
}

function Test-InstallLocationMatches {
    [CmdletBinding()]
    param(
        [AllowNull()][string]$Actual,
        [Parameter(Mandatory = $true)][string]$Expected
    )
    if ([string]::IsNullOrWhiteSpace($Actual) -or [string]::IsNullOrWhiteSpace($Expected)) {
        return $false
    }
    $a = $Actual.Trim().TrimEnd('\')
    $e = $Expected.Trim().TrimEnd('\')
    return [string]::Equals($a, $e, [System.StringComparison]::OrdinalIgnoreCase)
}

function Test-CommandTargetMatches {
    [CmdletBinding()]
    param(
        [AllowNull()][string]$CommandValue,
        [Parameter(Mandatory = $true)][string]$ExpectedExePath
    )
    if ([string]::IsNullOrWhiteSpace($CommandValue) -or [string]::IsNullOrWhiteSpace($ExpectedExePath)) {
        return $false
    }
    $match = [regex]::Match($CommandValue, '^\s*"([^"]+)"\s+"%1"\s*$', [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
    if (-not $match.Success) {
        return $false
    }
    $actualExe = $match.Groups[1].Value.Trim().TrimEnd('\')
    $expectedExe = $ExpectedExePath.Trim().TrimEnd('\')
    return [string]::Equals($actualExe, $expectedExe, [System.StringComparison]::OrdinalIgnoreCase)
}
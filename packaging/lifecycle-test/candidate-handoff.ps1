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
      Verifies that the inner candidate directory contains EXACTLY the expected 4 files
      (manifest + 3 fixed candidate files), matching manifest metadata (repo, workflow SHA,
      run ID, run attempt, candidate release ID, candidate tag), no directory traversal,
      no directories or reparse points, no case collisions, and validates that file hashes
      match expected hashes.
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
        [Parameter(Mandatory = $true)][string]$ManifestPath
    )

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
        [string]$ManifestFileName = "candidate-handoff-manifest.json"
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

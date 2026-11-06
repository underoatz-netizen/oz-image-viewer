param (
    [Parameter(Mandatory = $true)]
    [string]$Repository,

    [Parameter(Mandatory = $true)]
    [string]$CandidateReleaseId,

    [Parameter(Mandatory = $true)]
    [string]$CandidateTag,

    [Parameter(Mandatory = $true)]
    [string[]]$ExpectedAssetNames,

    [Parameter(Mandatory = $true)]
    [string]$DestinationDir,

    [Parameter(Mandatory = $false)]
    [string]$AuthToken = $null
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# 1. Resolve token from parameter or GH_TOKEN environment variable
$resolvedToken = $AuthToken
if ([string]::IsNullOrWhiteSpace($resolvedToken)) {
    $resolvedToken = $env:GH_TOKEN
}
if ([string]::IsNullOrWhiteSpace($resolvedToken)) {
    throw "GitHub authentication token is required (supply via GH_TOKEN environment variable or -AuthToken parameter)."
}

# 2. Validate inputs
$trimmedRepo = $Repository.Trim()
if ($trimmedRepo -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$') {
    throw "Invalid repository format: '$Repository'. Expected 'owner/repo'."
}

$trimmedId = $CandidateReleaseId.Trim()
if ($trimmedId -notmatch '^[1-9][0-9]*$') {
    throw "Invalid CandidateReleaseId: '$CandidateReleaseId'. Expected strictly positive decimal digits."
}

$trimmedTag = $CandidateTag.Trim()
if ([string]::IsNullOrWhiteSpace($trimmedTag)) {
    throw "CandidateTag cannot be empty."
}

# Validate ExpectedAssetNames are safe, non-empty, flat names without path separators or directory traversal, and unique
if ($null -eq $ExpectedAssetNames -or $ExpectedAssetNames.Count -eq 0) {
    throw "ExpectedAssetNames cannot be empty."
}

$seenNames = @{}
$validatedExpectedAssetNames = [System.Collections.Generic.List[string]]::new()
foreach ($rawName in $ExpectedAssetNames) {
    if ([string]::IsNullOrWhiteSpace($rawName)) {
        throw "Expected asset name cannot be empty or whitespace."
    }
    $trimmedName = $rawName.Trim()
    if ($trimmedName.Contains('/') -or $trimmedName.Contains('\') -or $trimmedName.Contains('..')) {
        throw "Security policy violation: Expected asset name '$trimmedName' contains illegal path characters or directory traversal."
    }
    if ($seenNames.ContainsKey($trimmedName)) {
        throw "Security policy violation: Duplicate expected asset name provided in ExpectedAssetNames: '$trimmedName'."
    }
    $seenNames[$trimmedName] = $true
    $validatedExpectedAssetNames.Add($trimmedName)
}

if (-not (Test-Path -LiteralPath $DestinationDir -PathType Container)) {
    New-Item -ItemType Directory -Path $DestinationDir -Force | Out-Null
}

# 3. Fetch release record from GitHub REST API
$releaseApiUrl = "https://api.github.com/repos/$trimmedRepo/releases/$trimmedId"
Write-Host "Querying release metadata from: $releaseApiUrl"

$requestHeaders = @{
    'Authorization' = "token $resolvedToken"
    'Accept'        = 'application/vnd.github+json'
    'User-Agent'    = 'ozviewer-ci-validator'
}

$releaseResponse = $null
try {
    $releaseResponse = Invoke-RestMethod -Method Get -Uri $releaseApiUrl -Headers $requestHeaders
}
catch {
    $status = $null
    if ($_.Exception.Response) {
        $status = [int]$_.Exception.Response.StatusCode
    }
    if ($status -eq 401 -or $status -eq 403) {
        throw "GitHub API returned HTTP $status (insufficient permission or authentication failure accessing release $trimmedId)."
    }
    elseif ($status -eq 404) {
        throw "GitHub API returned HTTP 404 (release $trimmedId not found or insufficient permission for contents:read)."
    }
    else {
        $msg = $_.Exception.Message
        throw "Failed to fetch release $trimmedId from ${releaseApiUrl}: $msg"
    }
}

# 4. Strictly validate release properties
if ($null -eq $releaseResponse) {
    throw "Received empty response from release API."
}

if ("$($releaseResponse.id)" -ne $trimmedId) {
    throw "Release ID mismatch! Expected '$trimmedId', got '$($releaseResponse.id)'."
}

if ($releaseResponse.draft -ne $true) {
    throw "Security policy violation: Release $trimmedId is NOT a draft release (draft=$($releaseResponse.draft))."
}

if ("$($releaseResponse.tag_name)" -ne $trimmedTag) {
    throw "Release tag mismatch! Expected '$trimmedTag', got '$($releaseResponse.tag_name)'."
}

Write-Host "Release validation successful: ID $($releaseResponse.id), tag '$($releaseResponse.tag_name)', draft=$($releaseResponse.draft)."

# 5. Validate assets collection
$assets = @($releaseResponse.assets)
if ($assets.Count -eq 0) {
    throw "Release $trimmedId contains no assets."
}

foreach ($expectedName in $validatedExpectedAssetNames) {
    $matchingAssets = @($assets | Where-Object { $_.name -eq $expectedName })
    if ($matchingAssets.Count -eq 0) {
        throw "Expected asset '$expectedName' was not found in release $trimmedId."
    }
    if ($matchingAssets.Count -gt 1) {
        throw "Duplicate asset entries found for '$expectedName' in release $trimmedId (count: $($matchingAssets.Count))."
    }

    $asset = $matchingAssets[0]
    $assetId = "$($asset.id)".Trim()
    if ($assetId -notmatch '^[1-9][0-9]*$') {
        throw "Asset '$expectedName' has invalid numeric id: '$assetId'."
    }

    $expectedCanonicalUrl = "https://api.github.com/repos/$trimmedRepo/releases/assets/$assetId"
    if ($asset.url -ne $expectedCanonicalUrl) {
        throw "Asset '$expectedName' API URL mismatch! Expected '$expectedCanonicalUrl', got '$($asset.url)'."
    }

    $destFile = Join-Path $DestinationDir $expectedName
    Write-Host "Downloading asset '$expectedName' from $expectedCanonicalUrl..."

    $downloadHeaders = @{
        'Authorization' = "token $resolvedToken"
        'Accept'        = 'application/octet-stream'
        'User-Agent'    = 'ozviewer-ci-validator'
    }

    try {
        Invoke-WebRequest -UseBasicParsing -Method Get -Uri $expectedCanonicalUrl -Headers $downloadHeaders -OutFile $destFile
    }
    catch {
        $msg = $_.Exception.Message
        throw "Failed to download asset '$expectedName' from ${expectedCanonicalUrl}: $msg"
    }

    if (-not (Test-Path -LiteralPath $destFile -PathType Leaf)) {
        throw "Downloaded asset file does not exist on disk: $destFile"
    }
    $fileSize = (Get-Item -LiteralPath $destFile).Length
    if ($fileSize -le 0) {
        throw "Downloaded asset file is empty (0 bytes): $destFile"
    }
    Write-Host "Downloaded '$expectedName' successfully ($fileSize bytes)."
}

Write-Host "All expected assets downloaded and validated successfully."

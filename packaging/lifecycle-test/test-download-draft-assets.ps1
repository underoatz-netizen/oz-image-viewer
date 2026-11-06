param (
    [switch]$RunLiveDownloadTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$scriptPath = Join-Path $scriptDir "DownloadReleaseDraftAssets.ps1"
if (-not (Test-Path -LiteralPath $scriptPath)) {
    throw "Script under test not found: $scriptPath"
}

$tempDir = Join-Path $env:TEMP ("download-test-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempDir -Force | Out-Null

try {
    Write-Host "Running DownloadReleaseDraftAssets parameter & input validation tests..."

    # Test 1: Invalid repository format
    $threw = $false
    try {
        & $scriptPath -Repository "invalid..repo" -CandidateReleaseId "436240766" -CandidateTag "v0.1.0" -ExpectedAssetNames @("test.zip") -DestinationDir $tempDir -AuthToken "dummy"
    } catch {
        $threw = $true
        if ($_.Exception.Message -notmatch "Invalid repository format") {
            throw "Test 1 failed: Unexpected error message: $($_.Exception.Message)"
        }
    }
    if (-not $threw) { throw "Test 1 failed: Expected exception on invalid repository format." }
    Write-Host "  Test 1 (Bad repo format) PASSED"

    # Test 2: Invalid release ID (non-positive / command injection)
    $threw = $false
    try {
        & $scriptPath -Repository "owner/repo" -CandidateReleaseId "0" -CandidateTag "v0.1.0" -ExpectedAssetNames @("test.zip") -DestinationDir $tempDir -AuthToken "dummy"
    } catch {
        $threw = $true
        if ($_.Exception.Message -notmatch "Invalid CandidateReleaseId") {
            throw "Test 2 failed: Unexpected error message: $($_.Exception.Message)"
        }
    }
    if (-not $threw) { throw "Test 2 failed: Expected exception on release ID 0." }

    $threw = $false
    try {
        & $scriptPath -Repository "owner/repo" -CandidateReleaseId "436240766; rm -rf /" -CandidateTag "v0.1.0" -ExpectedAssetNames @("test.zip") -DestinationDir $tempDir -AuthToken "dummy"
    } catch {
        $threw = $true
        if ($_.Exception.Message -notmatch "Invalid CandidateReleaseId") {
            throw "Test 2b failed: Unexpected error message: $($_.Exception.Message)"
        }
    }
    if (-not $threw) { throw "Test 2b failed: Expected exception on command injection." }
    Write-Host "  Test 2 (Bad release ID) PASSED"

    # Test 3: Empty CandidateTag
    $threw = $false
    try {
        & $scriptPath -Repository "owner/repo" -CandidateReleaseId "436240766" -CandidateTag "   " -ExpectedAssetNames @("test.zip") -DestinationDir $tempDir -AuthToken "dummy"
    } catch {
        $threw = $true
        if ($_.Exception.Message -notmatch "CandidateTag cannot be empty") {
            throw "Test 3 failed: Unexpected error message: $($_.Exception.Message)"
        }
    }
    if (-not $threw) { throw "Test 3 failed: Expected exception on empty CandidateTag." }
    Write-Host "  Test 3 (Empty CandidateTag) PASSED"

    # Test 4: ExpectedAssetNames validation (illegal path separators, traversal, empty, duplicate)
    $threw = $false
    try {
        & $scriptPath -Repository "owner/repo" -CandidateReleaseId "436240766" -CandidateTag "v0.1.0" -ExpectedAssetNames @("../escape.zip") -DestinationDir $tempDir -AuthToken "dummy"
    } catch {
        $threw = $true
        if ($_.Exception.Message -notmatch "illegal path characters or directory traversal") {
            throw "Test 4a failed: Unexpected message: $($_.Exception.Message)"
        }
    }
    if (-not $threw) { throw "Test 4a failed: Expected traversal rejection." }

    $threw = $false
    try {
        & $scriptPath -Repository "owner/repo" -CandidateReleaseId "436240766" -CandidateTag "v0.1.0" -ExpectedAssetNames @("sub/dir.zip") -DestinationDir $tempDir -AuthToken "dummy"
    } catch {
        $threw = $true
        if ($_.Exception.Message -notmatch "illegal path characters or directory traversal") {
            throw "Test 4b failed: Unexpected message: $($_.Exception.Message)"
        }
    }
    if (-not $threw) { throw "Test 4b failed: Expected separator rejection." }

    $threw = $false
    try {
        & $scriptPath -Repository "owner/repo" -CandidateReleaseId "436240766" -CandidateTag "v0.1.0" -ExpectedAssetNames @("file.zip", "file.zip") -DestinationDir $tempDir -AuthToken "dummy"
    } catch {
        $threw = $true
        if ($_.Exception.Message -notmatch "Duplicate expected asset name") {
            throw "Test 4c failed: Unexpected message: $($_.Exception.Message)"
        }
    }
    if (-not $threw) { throw "Test 4c failed: Expected duplicate rejection." }
    Write-Host "  Test 4 (Safe flat unique ExpectedAssetNames) PASSED"

    # Test 5: Missing auth token (neither param nor env)
    $threw = $false
    $savedToken = $env:GH_TOKEN
    try {
        $env:GH_TOKEN = ""
        & $scriptPath -Repository "owner/repo" -CandidateReleaseId "436240766" -CandidateTag "v0.1.0" -ExpectedAssetNames @("file.zip") -DestinationDir $tempDir
    } catch {
        $threw = $true
        if ($_.Exception.Message -notmatch "GitHub authentication token is required") {
            throw "Test 5 failed: Unexpected message: $($_.Exception.Message)"
        }
    } finally {
        $env:GH_TOKEN = $savedToken
    }
    if (-not $threw) { throw "Test 5 failed: Expected missing token rejection." }
    Write-Host "  Test 5 (Missing token validation) PASSED"

    Write-Host "Running Mocked Invoke-RestMethod / Invoke-WebRequest response tests..."

    # Test 6: Mocked REST API responses exercising internal validation branches
    # Test 6a: Release ID mismatch
    $mockIdMismatch = [PSCustomObject]@{
        id = 999999
        draft = $true
        tag_name = "v0.1.0"
        assets = @()
    }
    $threw = $false
    try {
        function Invoke-RestMethod { return $mockIdMismatch }
        & $scriptPath -Repository "owner/repo" -CandidateReleaseId "436240766" -CandidateTag "v0.1.0" -ExpectedAssetNames @("file.zip") -DestinationDir $tempDir -AuthToken "dummy"
    } catch {
        $threw = $true
        if ($_.Exception.Message -notmatch "Release ID mismatch") {
            throw "Test 6a failed: Unexpected message: $($_.Exception.Message)"
        }
    } finally {
        Remove-Item Function:\Invoke-RestMethod -ErrorAction SilentlyContinue
    }
    if (-not $threw) { throw "Test 6a failed: Expected ID mismatch error." }
    Write-Host "  Test 6a (Mocked ID mismatch) PASSED"

    # Test 6b: Non-draft release rejection
    $mockNonDraft = [PSCustomObject]@{
        id = 436240766
        draft = $false
        tag_name = "v0.1.0"
        assets = @()
    }
    $threw = $false
    try {
        function Invoke-RestMethod { return $mockNonDraft }
        & $scriptPath -Repository "owner/repo" -CandidateReleaseId "436240766" -CandidateTag "v0.1.0" -ExpectedAssetNames @("file.zip") -DestinationDir $tempDir -AuthToken "dummy"
    } catch {
        $threw = $true
        if ($_.Exception.Message -notmatch "Release 436240766 is NOT a draft release") {
            throw "Test 6b failed: Unexpected message: $($_.Exception.Message)"
        }
    } finally {
        Remove-Item Function:\Invoke-RestMethod -ErrorAction SilentlyContinue
    }
    if (-not $threw) { throw "Test 6b failed: Expected non-draft error." }
    Write-Host "  Test 6b (Mocked non-draft rejection) PASSED"

    # Test 6c: Wrong tag rejection
    $mockWrongTag = [PSCustomObject]@{
        id = 436240766
        draft = $true
        tag_name = "v9.9.9"
        assets = @()
    }
    $threw = $false
    try {
        function Invoke-RestMethod { return $mockWrongTag }
        & $scriptPath -Repository "owner/repo" -CandidateReleaseId "436240766" -CandidateTag "v0.1.0" -ExpectedAssetNames @("file.zip") -DestinationDir $tempDir -AuthToken "dummy"
    } catch {
        $threw = $true
        if ($_.Exception.Message -notmatch "Release tag mismatch") {
            throw "Test 6c failed: Unexpected message: $($_.Exception.Message)"
        }
    } finally {
        Remove-Item Function:\Invoke-RestMethod -ErrorAction SilentlyContinue
    }
    if (-not $threw) { throw "Test 6c failed: Expected tag mismatch error." }
    Write-Host "  Test 6c (Mocked tag mismatch) PASSED"

    # Test 6d: Missing expected asset in release
    $mockMissingAsset = [PSCustomObject]@{
        id = 436240766
        draft = $true
        tag_name = "v0.1.0"
        assets = @(
            [PSCustomObject]@{ id = 111; name = "other.zip"; url = "https://api.github.com/repos/owner/repo/releases/assets/111" }
        )
    }
    $threw = $false
    try {
        function Invoke-RestMethod { return $mockMissingAsset }
        & $scriptPath -Repository "owner/repo" -CandidateReleaseId "436240766" -CandidateTag "v0.1.0" -ExpectedAssetNames @("file.zip") -DestinationDir $tempDir -AuthToken "dummy"
    } catch {
        $threw = $true
        if ($_.Exception.Message -notmatch "Expected asset 'file.zip' was not found") {
            throw "Test 6d failed: Unexpected message: $($_.Exception.Message)"
        }
    } finally {
        Remove-Item Function:\Invoke-RestMethod -ErrorAction SilentlyContinue
    }
    if (-not $threw) { throw "Test 6d failed: Expected missing asset error." }
    Write-Host "  Test 6d (Mocked missing asset) PASSED"

    # Test 6e: Duplicate asset in release
    $mockDupAsset = [PSCustomObject]@{
        id = 436240766
        draft = $true
        tag_name = "v0.1.0"
        assets = @(
            [PSCustomObject]@{ id = 111; name = "file.zip"; url = "https://api.github.com/repos/owner/repo/releases/assets/111" },
            [PSCustomObject]@{ id = 112; name = "file.zip"; url = "https://api.github.com/repos/owner/repo/releases/assets/112" }
        )
    }
    $threw = $false
    try {
        function Invoke-RestMethod { return $mockDupAsset }
        & $scriptPath -Repository "owner/repo" -CandidateReleaseId "436240766" -CandidateTag "v0.1.0" -ExpectedAssetNames @("file.zip") -DestinationDir $tempDir -AuthToken "dummy"
    } catch {
        $threw = $true
        if ($_.Exception.Message -notmatch "Duplicate asset entries found for 'file.zip'") {
            throw "Test 6e failed: Unexpected message: $($_.Exception.Message)"
        }
    } finally {
        Remove-Item Function:\Invoke-RestMethod -ErrorAction SilentlyContinue
    }
    if (-not $threw) { throw "Test 6e failed: Expected duplicate asset error." }
    Write-Host "  Test 6e (Mocked duplicate remote asset) PASSED"

    # Test 6f: Wrong canonical asset URL / host mismatch
    $mockWrongHost = [PSCustomObject]@{
        id = 436240766
        draft = $true
        tag_name = "v0.1.0"
        assets = @(
            [PSCustomObject]@{ id = 111; name = "file.zip"; url = "https://evil.example.com/asset/111" }
        )
    }
    $threw = $false
    try {
        function Invoke-RestMethod { return $mockWrongHost }
        & $scriptPath -Repository "owner/repo" -CandidateReleaseId "436240766" -CandidateTag "v0.1.0" -ExpectedAssetNames @("file.zip") -DestinationDir $tempDir -AuthToken "dummy"
    } catch {
        $threw = $true
        if ($_.Exception.Message -notmatch "API URL mismatch") {
            throw "Test 6f failed: Unexpected message: $($_.Exception.Message)"
        }
    } finally {
        Remove-Item Function:\Invoke-RestMethod -ErrorAction SilentlyContinue
    }
    if (-not $threw) { throw "Test 6f failed: Expected wrong host error." }
    Write-Host "  Test 6f (Mocked asset host/url mismatch) PASSED"

    # Test 6g: Mocked Invoke-WebRequest receives -UseBasicParsing and -OutFile
    $mockValidRelease = [PSCustomObject]@{
        id = 436240766
        draft = $true
        tag_name = "v0.1.0"
        assets = @(
            [PSCustomObject]@{
                id = 111
                name = "file.zip"
                url = "https://api.github.com/repos/owner/repo/releases/assets/111"
            }
        )
    }
    $global:invokedUseBasicParsing = $false
    $global:invokedOutFile = $null
    $global:invokedUri = $null
    $global:invokedHeaders = $null

    try {
        function Invoke-RestMethod { return $mockValidRelease }
        function Invoke-WebRequest {
            param(
                [switch]$UseBasicParsing,
                [string]$Method,
                [string]$Uri,
                [hashtable]$Headers,
                [string]$OutFile
            )
            $global:invokedUseBasicParsing = $UseBasicParsing.IsPresent
            $global:invokedOutFile = $OutFile
            $global:invokedUri = $Uri
            $global:invokedHeaders = $Headers
            # Write non-empty mock bytes to satisfy file existence and > 0 check
            [System.IO.File]::WriteAllBytes($OutFile, [byte[]]@(0x50, 0x4B, 0x05, 0x06))
        }

        & $scriptPath -Repository "owner/repo" -CandidateReleaseId "436240766" -CandidateTag "v0.1.0" -ExpectedAssetNames @("file.zip") -DestinationDir $tempDir -AuthToken "dummy"

        if (-not $global:invokedUseBasicParsing) {
            throw "Test 6g failed: Invoke-WebRequest did NOT receive -UseBasicParsing switch!"
        }
        $expectedDest = Join-Path $tempDir "file.zip"
        if ($global:invokedOutFile -ne $expectedDest) {
            throw "Test 6g failed: Invoke-WebRequest received OutFile '$($global:invokedOutFile)', expected '$expectedDest'."
        }
        if ($global:invokedHeaders['Accept'] -ne 'application/octet-stream') {
            throw "Test 6g failed: Invoke-WebRequest did not receive Accept: application/octet-stream header."
        }
    } finally {
        Remove-Item Function:\Invoke-RestMethod -ErrorAction SilentlyContinue
        Remove-Item Function:\Invoke-WebRequest -ErrorAction SilentlyContinue
    }
    Write-Host "  Test 6g (Mocked Invoke-WebRequest -UseBasicParsing & -OutFile assertion) PASSED"

    # Test 7: Live non-binary check against draft release 436240766 if requested (SHA256SUMS text file only)
    if ($RunLiveDownloadTest) {
        $token = (& gh auth token).Trim()
        if (-not [string]::IsNullOrWhiteSpace($token)) {
            Write-Host "Running Test 7 (Live non-binary check: metadata + SHA256SUMS text download)..."
            $liveDir = Join-Path $tempDir "live"
            $env:GH_TOKEN = $token
            & $scriptPath `
                -Repository "underoatz-netizen/oz-image-viewer" `
                -CandidateReleaseId "436240766" `
                -CandidateTag "v0.1.0" `
                -ExpectedAssetNames @("SHA256SUMS") `
                -DestinationDir $liveDir
            $downloadedSums = Join-Path $liveDir "SHA256SUMS"
            if (-not (Test-Path -LiteralPath $downloadedSums)) {
                throw "Test 7 failed: SHA256SUMS not downloaded."
            }
            $content = Get-Content -LiteralPath $downloadedSums -Raw
            if ($content -notmatch "ozviewer-0.1.0-win-x64-installer.exe") {
                throw "Test 7 failed: Downloaded SHA256SUMS content mismatch."
            }
            Write-Host "  Test 7 (Live non-binary check) PASSED."
        }
    }

    Write-Host "All DownloadReleaseDraftAssets unit and mock tests PASSED!"
}
finally {
    if (Test-Path -LiteralPath $tempDir) {
        Remove-Item -Recurse -Force -LiteralPath $tempDir
    }
}

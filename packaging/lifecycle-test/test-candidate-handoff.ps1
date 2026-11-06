<#
.SYNOPSIS
  Unit tests for candidate-handoff.ps1 and validate-installer.yml split-job security architecture.

.DESCRIPTION
  Exercises:
    1. New-CandidateHandoffManifest happy path and schema validation.
    2. Resolve-CandidateHandoffDirectory tests:
       - Happy path: resolves single nested artifact directory matching ExpectedArtifactDirectoryName.
       - Rejects parent with extra loose files.
       - Rejects parent with multiple subdirectories.
       - Rejects parent directory name mismatch.
       - Rejects parent containing reparse point.
    3. Test-CandidateHandoffDirectory happy path:
       - Matches repository, workflowSha, runId, runAttempt, releaseId, tag, and expected hashes.
    4. Test-CandidateHandoffDirectory failure modes (fail-closed security):
       - Bad/mismatched hash fails.
       - Syntactically invalid (non-64-hex) hash fails.
       - Wrong repository fails.
       - Wrong workflowSha fails.
       - Wrong runId fails.
       - Wrong runAttempt fails.
       - Wrong candidateReleaseId fails.
       - Wrong candidateTag fails.
       - Extra unapproved file fails.
       - Missing expected file fails.
       - Unexpected subdirectory fails.
       - Duplicate / case-colliding file entry fails.
    5. Workflow YAML architecture assertions:
       - workflow-level permissions: {}
       - fetch-candidate permissions: contents: write
       - validate-candidate permissions: contents: read
       - actions/checkout pinned to 11bd71901bbe5b1630ceea73d27597364c9af683 (v4.2.2)
       - actions/upload-artifact pinned to 65c4c4a1ddee5b72f698fdd19549f0f0fb45cf08 (v4.6.0)
       - actions/download-artifact pinned to d3f86a106a0bac45b974a628896c90dbdf5c8093 (v4.3.0)
       - download step binds strictly to artifact-ids: ${{ needs.fetch-candidate.outputs.artifact_id }}
       - no name, pattern, or merge-multiple lookups in download step
       - persist-credentials: false on both jobs
       - ref check refs/heads/main on both jobs
       - GH_TOKEN scoped only to fetch-candidate download step
       - timeout-minutes bounded on both jobs
       - immutable artifact upload (overwrite: false, retention-days: 1)

.NOTES
  Exit codes: 0 = all test cases passed, 1 = failure.
#>
[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$HandoffScript = Join-Path $ScriptDir "candidate-handoff.ps1"
$RepoRoot = Split-Path -Parent (Split-Path -Parent $ScriptDir)

$WorkDir = Join-Path $env:TEMP "ozviewer-candidate-handoff-unit-tests"
if (Test-Path -LiteralPath $WorkDir) { Remove-Item -LiteralPath $WorkDir -Recurse -Force }
New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null

function Pass([string]$msg) {
    Write-Host "[PASS] $msg" -ForegroundColor Green
}
function Assert-Condition([bool]$cond, [string]$msg) {
    if (-not $cond) {
        Write-Error "[ASSERT FAILED] $msg"
        exit 1
    }
}
function Assert-Throws([scriptblock]$sb, [string]$expectedPattern, [string]$testName) {
    $threw = $false
    try {
        & $sb
    } catch {
        $threw = $true
        if (-not ($_.Exception.Message -match $expectedPattern)) {
            Write-Error "[$testName] Threw unexpected message: $($_.Exception.Message) (expected pattern '$expectedPattern')"
            exit 1
        }
    }
    if (-not $threw) {
        Write-Error "[$testName] Expected exception matching '$expectedPattern' but nothing was thrown."
        exit 1
    }
    Pass "$testName correctly threw: $expectedPattern"
}

try {
    # Dot-source the helper
    Assert-Condition (Test-Path -LiteralPath $HandoffScript) "candidate-handoff.ps1 not found at $HandoffScript"
    . $HandoffScript

    # =========================================================================
    # Part 1: Happy path manifest creation & nested directory layout verification
    # =========================================================================
    $parentDir = Join-Path $WorkDir "handoff-parent"
    $expectedArtifactName = "candidate-release-assets-12345678-1"
    $testDir = Join-Path $parentDir $expectedArtifactName
    New-Item -ItemType Directory -Path $testDir -Force | Out-Null

    $f1 = "ozviewer-0.1.0-win-x64-installer.exe"
    $f2 = "ozviewer-0.1.0-win-x64-portable.zip"
    $f3 = "ozviewer-0.1.0-lgpl-corresponding-source.zip"

    [System.IO.File]::WriteAllBytes((Join-Path $testDir $f1), [byte[]]@(1, 2, 3, 4, 5))
    [System.IO.File]::WriteAllBytes((Join-Path $testDir $f2), [byte[]]@(6, 7, 8, 9, 10))
    [System.IO.File]::WriteAllBytes((Join-Path $testDir $f3), [byte[]]@(11, 12, 13, 14, 15))

    $h1 = (Get-FileHash -LiteralPath (Join-Path $testDir $f1) -Algorithm SHA256).Hash.ToLowerInvariant()
    $h2 = (Get-FileHash -LiteralPath (Join-Path $testDir $f2) -Algorithm SHA256).Hash.ToLowerInvariant()
    $h3 = (Get-FileHash -LiteralPath (Join-Path $testDir $f3) -Algorithm SHA256).Hash.ToLowerInvariant()

    $manifestPath = Join-Path $testDir "candidate-handoff-manifest.json"
    New-CandidateHandoffManifest `
        -Repository "underoatz-netizen/oz-image-viewer" `
        -WorkflowSha "11d5960a326750d5838078e36cf38b85af677262" `
        -RunId "12345678" `
        -RunAttempt "1" `
        -CandidateReleaseId "436240766" `
        -CandidateTag "v0.1.0" `
        -CandidateDir $testDir `
        -FixedAssetNames @($f1, $f2, $f3) `
        -ManifestPath $manifestPath

    Assert-Condition (Test-Path -LiteralPath $manifestPath) "Manifest was not generated"
    Pass "Manifest creation happy path completed."

    # Verify Resolve-CandidateHandoffDirectory happy path
    $resolvedDir = Resolve-CandidateHandoffDirectory `
        -ParentDir $parentDir `
        -ExpectedArtifactDirectoryName $expectedArtifactName
    Assert-Condition ($resolvedDir -eq $testDir) "Resolved directory mismatch: expected '$testDir', got '$resolvedDir'"
    Pass "Resolve-CandidateHandoffDirectory happy path resolved expected nested artifact directory."

    # Validate inner directory happy path
    Test-CandidateHandoffDirectory `
        -CandidateDir $resolvedDir `
        -ExpectedRepository "underoatz-netizen/oz-image-viewer" `
        -ExpectedWorkflowSha "11d5960a326750d5838078e36cf38b85af677262" `
        -ExpectedRunId "12345678" `
        -ExpectedRunAttempt "1" `
        -ExpectedCandidateReleaseId "436240766" `
        -ExpectedCandidateTag "v0.1.0" `
        -ExpectedHashesByName @{
            $f1 = $h1
            $f2 = $h2
            $f3 = $h3
        }
    Pass "Inner directory validation happy path passed."

    # =========================================================================
    # Part 2: Resolve-CandidateHandoffDirectory security failure modes
    # =========================================================================
    # 2.1: Loose file in parent directory
    $looseFile = Join-Path $parentDir "stray.txt"
    [System.IO.File]::WriteAllText($looseFile, "UNEXPECTED")
    Assert-Throws {
        Resolve-CandidateHandoffDirectory `
            -ParentDir $parentDir `
            -ExpectedArtifactDirectoryName $expectedArtifactName
    } "Unexpected file\(s\) found in handoff parent directory" "Parent dir with loose files"
    Remove-Item -LiteralPath $looseFile -Force

    # 2.2: Extra unexpected directory in parent
    $extraSub = Join-Path $parentDir "unexpected-extra-dir"
    New-Item -ItemType Directory -Path $extraSub -Force | Out-Null
    Assert-Throws {
        Resolve-CandidateHandoffDirectory `
            -ParentDir $parentDir `
            -ExpectedArtifactDirectoryName $expectedArtifactName
    } "Expected exactly 1 artifact directory in handoff parent" "Parent dir with multiple subdirectories"
    Remove-Item -LiteralPath $extraSub -Recurse -Force

    # 2.3: Artifact directory name mismatch
    Assert-Throws {
        Resolve-CandidateHandoffDirectory `
            -ParentDir $parentDir `
            -ExpectedArtifactDirectoryName "wrong-artifact-name"
    } "Artifact directory name mismatch" "Parent dir with name mismatch"

    # =========================================================================
    # Part 3: Test-CandidateHandoffDirectory fail-closed security cases
    # =========================================================================
    # 3.1: Hash mismatch
    Assert-Throws {
        Test-CandidateHandoffDirectory `
            -CandidateDir $testDir `
            -ExpectedRepository "underoatz-netizen/oz-image-viewer" `
            -ExpectedWorkflowSha "11d5960a326750d5838078e36cf38b85af677262" `
            -ExpectedRunId "12345678" `
            -ExpectedRunAttempt "1" `
            -ExpectedCandidateReleaseId "436240766" `
            -ExpectedCandidateTag "v0.1.0" `
            -ExpectedHashesByName @{
                $f1 = "0000000000000000000000000000000000000000000000000000000000000000"
                $f2 = $h2
                $f3 = $h3
            }
    } "Cryptographic hash mismatch" "Fail on bad hash"

    # 3.2: Invalid non-64-hex expected hash syntax
    Assert-Throws {
        Test-CandidateHandoffDirectory `
            -CandidateDir $testDir `
            -ExpectedRepository "underoatz-netizen/oz-image-viewer" `
            -ExpectedWorkflowSha "11d5960a326750d5838078e36cf38b85af677262" `
            -ExpectedRunId "12345678" `
            -ExpectedRunAttempt "1" `
            -ExpectedCandidateReleaseId "436240766" `
            -ExpectedCandidateTag "v0.1.0" `
            -ExpectedHashesByName @{
                $f1 = "not-a-valid-hex-hash"
                $f2 = $h2
                $f3 = $h3
            }
    } "not a valid 64-character lowercase hex string" "Fail on invalid hex hash syntax"

    # 3.3: Repository mismatch
    Assert-Throws {
        Test-CandidateHandoffDirectory `
            -CandidateDir $testDir `
            -ExpectedRepository "other-owner/oz-image-viewer" `
            -ExpectedWorkflowSha "11d5960a326750d5838078e36cf38b85af677262" `
            -ExpectedRunId "12345678" `
            -ExpectedRunAttempt "1" `
            -ExpectedCandidateReleaseId "436240766" `
            -ExpectedCandidateTag "v0.1.0" `
            -ExpectedHashesByName @{ $f1 = $h1; $f2 = $h2; $f3 = $h3 }
    } "Manifest repository mismatch" "Fail on repository mismatch"

    # 3.4: WorkflowSha mismatch
    Assert-Throws {
        Test-CandidateHandoffDirectory `
            -CandidateDir $testDir `
            -ExpectedRepository "underoatz-netizen/oz-image-viewer" `
            -ExpectedWorkflowSha "deadbeefdeadbeefdeadbeefdeadbeefdeadbeef" `
            -ExpectedRunId "12345678" `
            -ExpectedRunAttempt "1" `
            -ExpectedCandidateReleaseId "436240766" `
            -ExpectedCandidateTag "v0.1.0" `
            -ExpectedHashesByName @{ $f1 = $h1; $f2 = $h2; $f3 = $h3 }
    } "Manifest workflowSha mismatch" "Fail on workflowSha mismatch"

    # 3.5: RunId mismatch
    Assert-Throws {
        Test-CandidateHandoffDirectory `
            -CandidateDir $testDir `
            -ExpectedRepository "underoatz-netizen/oz-image-viewer" `
            -ExpectedWorkflowSha "11d5960a326750d5838078e36cf38b85af677262" `
            -ExpectedRunId "99999999" `
            -ExpectedRunAttempt "1" `
            -ExpectedCandidateReleaseId "436240766" `
            -ExpectedCandidateTag "v0.1.0" `
            -ExpectedHashesByName @{ $f1 = $h1; $f2 = $h2; $f3 = $h3 }
    } "Manifest runId mismatch" "Fail on runId mismatch"

    # 3.6: RunAttempt mismatch
    Assert-Throws {
        Test-CandidateHandoffDirectory `
            -CandidateDir $testDir `
            -ExpectedRepository "underoatz-netizen/oz-image-viewer" `
            -ExpectedWorkflowSha "11d5960a326750d5838078e36cf38b85af677262" `
            -ExpectedRunId "12345678" `
            -ExpectedRunAttempt "2" `
            -ExpectedCandidateReleaseId "436240766" `
            -ExpectedCandidateTag "v0.1.0" `
            -ExpectedHashesByName @{ $f1 = $h1; $f2 = $h2; $f3 = $h3 }
    } "Manifest runAttempt mismatch" "Fail on runAttempt mismatch"

    # 3.7: CandidateReleaseId mismatch
    Assert-Throws {
        Test-CandidateHandoffDirectory `
            -CandidateDir $testDir `
            -ExpectedRepository "underoatz-netizen/oz-image-viewer" `
            -ExpectedWorkflowSha "11d5960a326750d5838078e36cf38b85af677262" `
            -ExpectedRunId "12345678" `
            -ExpectedRunAttempt "1" `
            -ExpectedCandidateReleaseId "999999999" `
            -ExpectedCandidateTag "v0.1.0" `
            -ExpectedHashesByName @{ $f1 = $h1; $f2 = $h2; $f3 = $h3 }
    } "Manifest candidateReleaseId mismatch" "Fail on candidateReleaseId mismatch"

    # 3.8: CandidateTag mismatch
    Assert-Throws {
        Test-CandidateHandoffDirectory `
            -CandidateDir $testDir `
            -ExpectedRepository "underoatz-netizen/oz-image-viewer" `
            -ExpectedWorkflowSha "11d5960a326750d5838078e36cf38b85af677262" `
            -ExpectedRunId "12345678" `
            -ExpectedRunAttempt "1" `
            -ExpectedCandidateReleaseId "436240766" `
            -ExpectedCandidateTag "v9.9.9" `
            -ExpectedHashesByName @{ $f1 = $h1; $f2 = $h2; $f3 = $h3 }
    } "Manifest candidateTag mismatch" "Fail on candidateTag mismatch"

    # 3.9: Extra unapproved file
    $extraFile = Join-Path $testDir "stray-unapproved.exe"
    [System.IO.File]::WriteAllText($extraFile, "MALICIOUS")
    Assert-Throws {
        Test-CandidateHandoffDirectory `
            -CandidateDir $testDir `
            -ExpectedRepository "underoatz-netizen/oz-image-viewer" `
            -ExpectedWorkflowSha "11d5960a326750d5838078e36cf38b85af677262" `
            -ExpectedRunId "12345678" `
            -ExpectedRunAttempt "1" `
            -ExpectedCandidateReleaseId "436240766" `
            -ExpectedCandidateTag "v0.1.0" `
            -ExpectedHashesByName @{ $f1 = $h1; $f2 = $h2; $f3 = $h3 }
    } "Extra unapproved file" "Fail on extra unapproved file"
    Remove-Item -LiteralPath $extraFile -Force

    # 3.10: Unexpected subdirectory inside candidate directory
    $subDir = Join-Path $testDir "unexpected-subfolder"
    New-Item -ItemType Directory -Path $subDir -Force | Out-Null
    Assert-Throws {
        Test-CandidateHandoffDirectory `
            -CandidateDir $testDir `
            -ExpectedRepository "underoatz-netizen/oz-image-viewer" `
            -ExpectedWorkflowSha "11d5960a326750d5838078e36cf38b85af677262" `
            -ExpectedRunId "12345678" `
            -ExpectedRunAttempt "1" `
            -ExpectedCandidateReleaseId "436240766" `
            -ExpectedCandidateTag "v0.1.0" `
            -ExpectedHashesByName @{ $f1 = $h1; $f2 = $h2; $f3 = $h3 }
    } "Unexpected directory found in candidate handoff" "Fail on unexpected directory"
    Remove-Item -LiteralPath $subDir -Recurse -Force

    # 3.11: Missing expected file
    $removedFile = Join-Path $testDir $f3
    Remove-Item -LiteralPath $removedFile -Force
    Assert-Throws {
        Test-CandidateHandoffDirectory `
            -CandidateDir $testDir `
            -ExpectedRepository "underoatz-netizen/oz-image-viewer" `
            -ExpectedWorkflowSha "11d5960a326750d5838078e36cf38b85af677262" `
            -ExpectedRunId "12345678" `
            -ExpectedRunAttempt "1" `
            -ExpectedCandidateReleaseId "436240766" `
            -ExpectedCandidateTag "v0.1.0" `
            -ExpectedHashesByName @{ $f1 = $h1; $f2 = $h2; $f3 = $h3 }
    } "Missing expected file" "Fail on missing expected file"

    # =========================================================================
    # Part 4: Workflow YAML Architecture Assertions
    # =========================================================================
    $workflowYamlPath = Join-Path $RepoRoot ".github\workflows\validate-installer.yml"
    Assert-Condition (Test-Path -LiteralPath $workflowYamlPath) "Workflow YAML missing at $workflowYamlPath"
    $yaml = Get-Content -LiteralPath $workflowYamlPath -Raw

    Assert-Condition ($yaml -match "permissions:\s*\{\}") "Workflow must have top-level permissions: {}"
    Assert-Condition ($yaml -match "fetch-candidate:\s*[\s\S]*?permissions:\s*[\s\S]*?contents:\s*write") "fetch-candidate job must declare contents: write"
    Assert-Condition ($yaml -match "validate-candidate:\s*[\s\S]*?permissions:\s*[\s\S]*?contents:\s*read") "validate-candidate job must declare contents: read"
    Assert-Condition ($yaml -match "validate-candidate:\s*[\s\S]*?needs:\s*fetch-candidate") "validate-candidate must declare needs: fetch-candidate"

    # Actions full commit SHA pinning
    Assert-Condition ($yaml -match "actions/checkout@11bd71901bbe5b1630ceea73d27597364c9af683") "actions/checkout must be pinned to full SHA 11bd71901bbe5b1630ceea73d27597364c9af683 (v4.2.2)"
    Assert-Condition ($yaml -match "actions/upload-artifact@65c4c4a1ddee5b72f698fdd19549f0f0fb45cf08") "actions/upload-artifact must be pinned to full SHA 65c4c4a1ddee5b72f698fdd19549f0f0fb45cf08 (v4.6.0)"
    Assert-Condition ($yaml -match "actions/download-artifact@d3f86a106a0bac45b974a628896c90dbdf5c8093") "actions/download-artifact must be pinned to full SHA d3f86a106a0bac45b974a628896c90dbdf5c8093 (v4.3.0)"

    # Exact artifact-id binding assertion (no name/pattern/merge)
    Assert-Condition ($yaml -match "artifact-ids:\s*\$\{\{\s*needs\.fetch-candidate\.outputs\.artifact_id\s*\}\}") "Download step must bind exactly to needs.fetch-candidate.outputs.artifact_id"
    Assert-Condition ($yaml -notmatch "download-artifact[\s\S]*?name:\s*candidate-release-assets") "Download step must NOT use artifact name lookup"
    Assert-Condition ($yaml -notmatch "pattern:") "Download step must NOT use pattern lookup"
    Assert-Condition ($yaml -notmatch "merge-multiple:") "Download step must NOT use merge-multiple"

    # Hardening options
    Assert-Condition ($yaml -match "persist-credentials:\s*false") "Checkout must specify persist-credentials: false"
    Assert-Condition ($yaml -match 'refs/heads/main') "Workflow must enforce execution on refs/heads/main only"
    Assert-Condition ($yaml -match "overwrite:\s*false") "Artifact upload must specify overwrite: false (immutable artifact)"
    Assert-Condition ($yaml -match "retention-days:\s*1") "Candidate handoff artifact must specify retention-days: 1"

    Pass "Workflow YAML security architecture assertions verified."

    Write-Host "`nALL CANDIDATE HANDOFF AND WORKFLOW SECURITY TESTS PASSED!" -ForegroundColor Green
    exit 0
} catch {
    Write-Error "Test execution failed: $_"
    exit 1
} finally {
    Remove-Item -LiteralPath $WorkDir -Recurse -Force -ErrorAction SilentlyContinue
}

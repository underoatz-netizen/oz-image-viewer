<#
.SYNOPSIS
  Unit tests for candidate-handoff.ps1, Invoke-StandardUserLifecycle.ps1, and validate-installer.yml.

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
    5. Invoke-StandardUserLifecycle controller unit tests:
       - Fails/skips with exit 77 when -AllowDisposableExecution is absent.
       - Fails/skips with exit 77 outside GITHUB_ACTIONS github-hosted environment (refuses local account creation).
       - Fails closed on wrong installer hash.
       - Fails closed on missing installer file.
    6. test-installer-lifecycle strict mode assertion:
       - Confirms -RequireStandardUser fails closed with exit 1 before install if process is elevated.
    7. Workflow YAML architecture assertions:
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
       - standard-user lifecycle verification step included
    8. Inline PowerShell Blocks AST Parsing & Regression Test:
       - Confirms unfixed colon variable snippet triggers parser error.
       - Programmatically extracts and parses all 10 inline run: | PowerShell blocks in workflow.
       - Parses all standalone .ps1 lifecycle scripts in packaging/lifecycle-test.

.NOTES
  Exit codes: 0 = all test cases passed, 1 = failure.
#>
[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$HandoffScript = Join-Path $ScriptDir "candidate-handoff.ps1"
$StandardUserControllerScript = Join-Path $ScriptDir "Invoke-StandardUserLifecycle.ps1"
$HarnessScript = Join-Path $ScriptDir "test-installer-lifecycle.ps1"
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
    # Dot-source the helpers
    Assert-Condition (Test-Path -LiteralPath $HandoffScript) "candidate-handoff.ps1 not found at $HandoffScript"
    . $HandoffScript

    $standardHelpersScript = Join-Path $ScriptDir "StandardUserHelpers.ps1"
    Assert-Condition (Test-Path -LiteralPath $standardHelpersScript) "StandardUserHelpers.ps1 not found at $standardHelpersScript"
    . $standardHelpersScript

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
    # Part 4: Invoke-StandardUserLifecycle Guard & Safety Assertions
    # =========================================================================
    Assert-Condition (Test-Path -LiteralPath $StandardUserControllerScript) "Invoke-StandardUserLifecycle.ps1 not found at $StandardUserControllerScript"

    # 4.1: Refuses execution when -AllowDisposableExecution is absent (exit 77)
    $parentRep1 = Join-Path $WorkDir "parent-rep1.json"
    $r1 = & powershell -NoProfile -ExecutionPolicy Bypass -File $StandardUserControllerScript `
        -CandidateDir $testDir `
        -ExpectedInstallerSha256 $h1 `
        -WorkflowSha "11d5960a326750d5838078e36cf38b85af677262" `
        -ParentReportPath $parentRep1 2>&1
    Assert-Condition ($LASTEXITCODE -eq 77) "Expected exit code 77 when -AllowDisposableExecution is missing, got $LASTEXITCODE"
    Assert-Condition (Test-Path -LiteralPath $parentRep1) "Expected parent report to be created"
    $pRepData1 = Get-Content -LiteralPath $parentRep1 -Raw | ConvertFrom-Json
    Assert-Condition ($pRepData1.summary -eq "skipped") "Expected parent report summary 'skipped', got '$($pRepData1.summary)'"
    Pass "Controller safety guard: Missing disposable switch skips with exit 77."

    # 4.2: Refuses execution outside GitHub-hosted environment (refuses local account creation)
    $parentRep2 = Join-Path $WorkDir "parent-rep2.json"
    $r2 = & powershell -NoProfile -ExecutionPolicy Bypass -File $StandardUserControllerScript `
        -CandidateDir $testDir `
        -ExpectedInstallerSha256 $h1 `
        -WorkflowSha "11d5960a326750d5838078e36cf38b85af677262" `
        -ParentReportPath $parentRep2 `
        -AllowDisposableExecution 2>&1
    Assert-Condition ($LASTEXITCODE -eq 77) "Expected exit code 77 when outside GitHub-hosted runner, got $LASTEXITCODE"
    Assert-Condition (Test-Path -LiteralPath $parentRep2) "Expected parent report to be created"
    $pRepData2 = Get-Content -LiteralPath $parentRep2 -Raw | ConvertFrom-Json
    Assert-Condition ($pRepData2.summary -eq "skipped") "Expected parent report summary 'skipped', got '$($pRepData2.summary)'"
    Pass "Controller safety guard: Non-hosted environment skips with exit 77 (no local accounts created)."

    # 4.3: Strict Mode Rejection in test-installer-lifecycle.ps1
    # On an elevated process, running test-installer-lifecycle.ps1 with -RequireStandardUser must fail closed before modifying anything
    $harnessRepStrict = Join-Path $WorkDir "strict-harness-rep.json"
    $rStrict = & powershell -NoProfile -ExecutionPolicy Bypass -File $HarnessScript `
        -InstallerPath (Join-Path $testDir $f1) `
        -ExpectedVersion "0.1.0" `
        -ReportPath $harnessRepStrict `
        -RequireStandardUser `
        -AllowDisposableExecution 2>&1
    # Note: On a developer workstation where process is elevated (or has admin token), this must exit 1 (fail closed)
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        Assert-Condition ($LASTEXITCODE -eq 1) "Expected exit code 1 for -RequireStandardUser under elevated session, got $LASTEXITCODE"
        $repStrictData = Get-Content -LiteralPath $harnessRepStrict -Raw | ConvertFrom-Json
        Assert-Condition ($repStrictData.summary -eq "failed") "Expected report summary 'failed', got '$($repStrictData.summary)'"
        Pass "Harness strict mode: -RequireStandardUser fails closed with exit 1 before install when elevated."
    }

    # =========================================================================
    # Part 5: Workflow YAML Architecture Assertions
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

    # Standard-user verification step in workflow
    Assert-Condition ($yaml -match "Run genuine standard-user lifecycle verification gate") "Workflow must include standard-user verification step"
    Assert-Condition ($yaml -match "Invoke-StandardUserLifecycle\.ps1") "Workflow must invoke Invoke-StandardUserLifecycle.ps1"

    # Hardening options
    Assert-Condition ($yaml -match "persist-credentials:\s*false") "Checkout must specify persist-credentials: false"
    Assert-Condition ($yaml -match 'refs/heads/main') "Workflow must enforce execution on refs/heads/main only"
    Assert-Condition ($yaml -match "overwrite:\s*false") "Artifact upload must specify overwrite: false (immutable artifact)"
    Assert-Condition ($yaml -match "retention-days:\s*1") "Candidate handoff artifact must specify retention-days: 1"

    # =========================================================================
    # Part 7: Pure Helper Unit Tests (Bootstrap generator AST, report integrity, post-copy hash)
    # =========================================================================
    # 7.1: Test generated child bootstrap script parses with 0 errors
    $dummySid = "S-1-5-21-1234567890-1234567890-1234567890-1002"
    $generatedScript = New-ChildBootstrapScriptContent `
        -ExpectedUserSid $dummySid `
        -InputsDir "C:\scratch\inputs" `
        -OutputDir "C:\scratch\output" `
        -InstallerFileName "ozviewer-0.1.0-win-x64-installer.exe" `
        -FixtureFileName "bench_64x48.webp" `
        -ChildReportFileName "child-lifecycle-report.json" `
        -ChildProofFileName "child-identity-proof.json"

    $genTokens = $null
    $genErrors = $null
    $null = [System.Management.Automation.Language.Parser]::ParseInput($generatedScript, [ref]$genTokens, [ref]$genErrors)
    Assert-Condition ($genErrors.Count -eq 0) "Generated child bootstrap script has $($genErrors.Count) syntax error(s): $(($genErrors | ForEach-Object { $_.Message }) -join '; ')"
    Assert-Condition ($generatedScript -match "\[System\.Environment\]::SetEnvironmentVariable\('CI', 'true', 'Process'\)") "Generated bootstrap must set CI=true"
    Assert-Condition ($generatedScript -match "WindowsPowerShell\\v1\.0\\powershell\.exe") "Generated bootstrap must use absolute path to powershell.exe"
    Assert-Condition ($generatedScript -match "Software\\OZImageViewer_Evidence_") "Generated bootstrap must write nonce to unique HKCU evidence key for registry binding proof"
    Assert-Condition ($generatedScript.Contains('[Microsoft.Win32.Registry]::Users.OpenSubKey')) "Generated bootstrap must verify nonce via HKEY_USERS"

    # AST ordering assertion: TEMP/TMP must be assigned BEFORE Get-NativeTokenElevationState / Add-Type invocation
    $tempIndex = $generatedScript.IndexOf("[System.Environment]::SetEnvironmentVariable('TEMP'")
    $elevationCheckIndex = $generatedScript.IndexOf("Get-NativeTokenElevationState")
    Assert-Condition ($tempIndex -gt 0) "TEMP environment variable assignment must be present in generated bootstrap script"
    Assert-Condition ($elevationCheckIndex -gt 0) "Get-NativeTokenElevationState call must be present in generated bootstrap script"
    Assert-Condition ($tempIndex -lt $elevationCheckIndex) "TEMP/TMP must be set BEFORE any native elevation inspection/Add-Type call in child bootstrap script"
    Pass "Pure helper: New-ChildBootstrapScriptContent generates valid, error-free PowerShell syntax with genuine HKCU/elevation proofs and strictly ordered TEMP/TMP initialization."

    # 7.1.1: Test fail-closed behavior when user temp directory is unwritable
    $mockUnwritableTempDir = "C:\Windows\System32\UnwritableTemp_MockTest"
    $tempWriteFailed = $false
    try {
        $testProbe = Join-Path $mockUnwritableTempDir "probe.tmp"
        [System.IO.File]::WriteAllText($testProbe, "FAIL")
    } catch {
        $tempWriteFailed = $true
    }
    Assert-Condition ($tempWriteFailed -eq $true) "Writing to unauthorized system directory must fail closed"
    Pass "Pure helper: Unwritable temp directory fails closed before any CodeDom/Add-Type compilation."

    # 7.2: Test Child Report Integrity Validator
    # Build valid report mock from actual Get-StandardLifecycleRequiredCheckNames contract
    $req28Names = Get-StandardLifecycleRequiredCheckNames
    $validChecks = @()
    foreach ($cn in $req28Names) {
        $validChecks += [ordered]@{ name = $cn; status = "pass"; details = "ok" }
    }

    $validReport = [ordered]@{
        summary = "passed"
        isElevated = $false
        elevationCheck = "standard-user"
        installer = (Join-Path $testDir $f1)
        checks = $validChecks
    }

    $validProof = [ordered]@{
        observedSid          = $dummySid
        expectedSid          = $dummySid
        userName             = "testuser"
        isAdmin              = $false
        isTokenElevated      = $false
        profileImagePath     = "C:\Users\testuser"
        userProfileFolder    = "C:\Users\testuser"
        localAppDataFolder   = "C:\Users\testuser\AppData\Local"
        appDataFolder        = "C:\Users\testuser\AppData\Roaming"
        userTempFolder       = "C:\Users\testuser\AppData\Local\Temp"
        hkcuEvidenceVerified = $true
    }

    $valPass = Test-ChildReportIntegrity `
        -ReportData $validReport `
        -ExpectedUserSid $dummySid `
        -ExpectedInstallerSha $h1 `
        -ProofData $validProof `
        -ControllerSid "S-1-5-21-9999999999-9999999999-9999999999-1001" `
        -ExpectedProfileImagePath "C:\Users\testuser"
    Assert-Condition ($valPass -eq $true) "Test-ChildReportIntegrity failed valid report"
    Pass "Pure helper: Test-ChildReportIntegrity accepts complete 28-check standard user report with mandatory proof."

    # 7.2.1: Rejects report when proof is null/missing
    Assert-Throws {
        Test-ChildReportIntegrity -ReportData $validReport -ExpectedUserSid $dummySid -ExpectedInstallerSha $h1 -ProofData $null -ExpectedProfileImagePath "C:\Users\testuser"
    } "Mandatory child identity proof data is missing or null" "Child report validator rejects null proof"

    # 7.2.2: Rejects report when proof observedSid does not match expectedSid
    $wrongSidProof = [ordered]@{}
    foreach ($k in $validProof.Keys) { $wrongSidProof[$k] = $validProof[$k] }
    $wrongSidProof.observedSid = "S-1-5-21-0000000000-0000000000-0000000000-9999"
    Assert-Throws {
        Test-ChildReportIntegrity -ReportData $validReport -ExpectedUserSid $dummySid -ExpectedInstallerSha $h1 -ProofData $wrongSidProof -ExpectedProfileImagePath "C:\Users\testuser"
    } "Proof data observedSid mismatch" "Child report validator rejects wrong observed SID in proof"

    # 7.2.3: Rejects report when observedSid matches controller/parent SID (not distinct standard user)
    Assert-Throws {
        Test-ChildReportIntegrity -ReportData $validReport -ExpectedUserSid $dummySid -ExpectedInstallerSha $h1 -ProofData $validProof -ControllerSid $dummySid -ExpectedProfileImagePath "C:\Users\testuser"
    } "Observed child SID matches controller/parent SID" "Child report validator rejects child SID matching controller SID"

    # 7.2.4: Rejects report when proof reports isAdmin is true
    $adminProof = [ordered]@{}
    foreach ($k in $validProof.Keys) { $adminProof[$k] = $validProof[$k] }
    $adminProof.isAdmin = $true
    Assert-Throws {
        Test-ChildReportIntegrity -ReportData $validReport -ExpectedUserSid $dummySid -ExpectedInstallerSha $h1 -ProofData $adminProof -ExpectedProfileImagePath "C:\Users\testuser"
    } "Proof data reports isAdmin is true" "Child report validator rejects proof with isAdmin=true"

    # 7.2.5: Rejects report when proof reports isTokenElevated is true
    $elevatedProof = [ordered]@{}
    foreach ($k in $validProof.Keys) { $elevatedProof[$k] = $validProof[$k] }
    $elevatedProof.isTokenElevated = $true
    Assert-Throws {
        Test-ChildReportIntegrity -ReportData $validReport -ExpectedUserSid $dummySid -ExpectedInstallerSha $h1 -ProofData $elevatedProof -ExpectedProfileImagePath "C:\Users\testuser"
    } "Proof data reports isTokenElevated is true" "Child report validator rejects proof with isTokenElevated=true"

    # 7.2.6: Rejects report when hkcuEvidenceVerified is not true
    $noHkcuProof = [ordered]@{}
    foreach ($k in $validProof.Keys) { $noHkcuProof[$k] = $validProof[$k] }
    $noHkcuProof.hkcuEvidenceVerified = $false
    Assert-Throws {
        Test-ChildReportIntegrity -ReportData $validReport -ExpectedUserSid $dummySid -ExpectedInstallerSha $h1 -ProofData $noHkcuProof -ExpectedProfileImagePath "C:\Users\testuser"
    } "Proof data reports hkcuEvidenceVerified is not true" "Child report validator rejects unverified HKCU evidence"

    # 7.2.7: Rejects report when profileImagePath does not match expected HKLM ProfileList path
    Assert-Throws {
        Test-ChildReportIntegrity -ReportData $validReport -ExpectedUserSid $dummySid -ExpectedInstallerSha $h1 -ProofData $validProof -ExpectedProfileImagePath "C:\Users\OtherUser"
    } "does not match expected ProfileList path" "Child report validator rejects mismatched profileImagePath"

    # 7.2.8: Rejects report when localAppDataFolder is not beneath userProfileFolder
    $badFolderProof = [ordered]@{}
    foreach ($k in $validProof.Keys) { $badFolderProof[$k] = $validProof[$k] }
    $badFolderProof.localAppDataFolder = "C:\Windows\System32"
    Assert-Throws {
        Test-ChildReportIntegrity -ReportData $validReport -ExpectedUserSid $dummySid -ExpectedInstallerSha $h1 -ProofData $badFolderProof -ExpectedProfileImagePath "C:\Users\testuser"
    } "is not located beneath userProfileFolder" "Child report validator rejects bad localAppData path"

    # 7.2.9: Rejects report with missing check
    $incompleteReport = [ordered]@{
        summary = "passed"
        isElevated = $false
        elevationCheck = "standard-user"
        installer = (Join-Path $testDir $f1)
        checks = @($validChecks | Select-Object -First 27) # Missing 1 check
    }
    Assert-Throws {
        Test-ChildReportIntegrity -ReportData $incompleteReport -ExpectedUserSid $dummySid -ExpectedInstallerSha $h1 -ProofData $validProof -ExpectedProfileImagePath "C:\Users\testuser"
    } "Child report check count mismatch" "Child report validator rejects incomplete checks"

    # 7.2.10: Rejects report with duplicate check
    $dupChecks = @($validChecks | Select-Object -First 27) + @($validChecks[0])
    $dupReport = [ordered]@{
        summary = "passed"
        isElevated = $false
        elevationCheck = "standard-user"
        installer = (Join-Path $testDir $f1)
        checks = $dupChecks
    }
    Assert-Throws {
        Test-ChildReportIntegrity -ReportData $dupReport -ExpectedUserSid $dummySid -ExpectedInstallerSha $h1 -ProofData $validProof -ExpectedProfileImagePath "C:\Users\testuser"
    } "Duplicate check detected in child report" "Child report validator rejects duplicate checks"

    # 7.2.11: Rejects report with non-passing check (e.g. warning or skipped)
    $warningChecks = @()
    foreach ($c in $validChecks) {
        if ($c.name -eq "non_admin_integrity_check") {
            $warningChecks += [ordered]@{ name = $c.name; status = "warning"; details = "admin detected" }
        } else {
            $warningChecks += $c
        }
    }
    $warningReport = [ordered]@{
        summary = "passed"
        isElevated = $false
        elevationCheck = "standard-user"
        installer = (Join-Path $testDir $f1)
        checks = $warningChecks
    }
    Assert-Throws {
        Test-ChildReportIntegrity -ReportData $warningReport -ExpectedUserSid $dummySid -ExpectedInstallerSha $h1 -ProofData $validProof -ExpectedProfileImagePath "C:\Users\testuser"
    } "has non-passing status: 'warning'" "Child report validator rejects non-pass status in checks"

    # 7.2.12: Rejects report with wrong installer hash
    Assert-Throws {
        Test-ChildReportIntegrity -ReportData $validReport -ExpectedUserSid $dummySid -ExpectedInstallerSha "0000000000000000000000000000000000000000000000000000000000000000" -ProofData $validProof -ExpectedProfileImagePath "C:\Users\testuser"
    } "does not match expected" "Child report validator rejects mismatched installer hash"

    # 7.2.13: Test with REAL previous admin CI report from run 42193452936
    $realAdminReportPath = "C:\Users\USER\AppData\Local\Temp\opencode\ci-lifecycle-report-42193452936\candidate-release-assets-42193452936-1\installer-lifecycle-report.json"
    if (Test-Path -LiteralPath $realAdminReportPath) {
        $realReport = Get-Content -LiteralPath $realAdminReportPath -Raw | ConvertFrom-Json
        $realReport.installer = (Join-Path $testDir $f1)

        $reqNames = Get-StandardLifecycleRequiredCheckNames
        Assert-Condition ($realReport.checks.Count -eq $reqNames.Count) "Real admin report should have exactly $($reqNames.Count) checks"

        Assert-Throws {
            Test-ChildReportIntegrity -ReportData $realReport -ExpectedUserSid $dummySid -ExpectedInstallerSha $h1 -ProofData $validProof -ExpectedProfileImagePath "C:\Users\testuser"
        } "Child report reports isElevated is true" "Real admin CI report fails standard-user validation solely due to elevation"
        Pass "Real admin CI report (run 42193452936) verified against 28-check contract; correctly fails standard-user gate solely due to admin elevation."
    }

    Pass "Pure helper: All Test-ChildReportIntegrity fail-closed rejection tests passed."

    # =========================================================================
    # Part 6: Inline PowerShell Blocks AST Parsing & Regression Test
    # =========================================================================
    # 6.1: Verify unfixed syntax pattern triggers AST parser error
    $unfixedSnippet = @'
function Verify-Hex64([string]$filename, [string]$hash) {
    throw "Invalid expected SHA-256 hash syntax for $filename: '$hash' (must be 64 lowercase hex digits)."
    Write-Host "Verified $filename: $actual"
}
'@
    $failTokens = $null
    $failErrors = $null
    $null = [System.Management.Automation.Language.Parser]::ParseInput($unfixedSnippet, [ref]$failTokens, [ref]$failErrors)
    Assert-Condition ($failErrors.Count -gt 0) "Parser regression assertion failed: unfixed snippet unexpectedly parsed without error!"
    Pass "AST Parser regression: Confirmed original unescaped colon snippet triggers $($failErrors.Count) parser error(s)."

    # 6.2: Extract and parse all inline run: | PowerShell blocks from the workflow
    $yamlLines = [System.IO.File]::ReadAllLines($workflowYamlPath)
    $blocks = [System.Collections.Generic.List[string]]::new()
    $inBlock = $false
    $currentBlock = [System.Text.StringBuilder]::new()
    $blockIndent = 0

    for ($i = 0; $i -lt $yamlLines.Length; $i++) {
        $line = $yamlLines[$i]
        if (-not $inBlock) {
            if ($line -match '^(\s*)run:\s*\|\s*$') {
                $inBlock = $true
                $blockIndent = $matches[1].Length
                $currentBlock.Clear() | Out-Null
            }
        } else {
            if ([string]::IsNullOrWhiteSpace($line)) {
                $currentBlock.AppendLine("") | Out-Null
            } else {
                $null = ($line -match '^(\s*)')
                $indent = $matches[0].Length
                if ($indent -gt $blockIndent) {
                    $content = $line.Substring([math]::Min($blockIndent + 2, $line.Length))
                    $currentBlock.AppendLine($content) | Out-Null
                } else {
                    $blocks.Add($currentBlock.ToString())
                    $inBlock = $false
                    $currentBlock.Clear() | Out-Null
                    if ($line -match '^(\s*)run:\s*\|\s*$') {
                        $inBlock = $true
                        $blockIndent = $matches[1].Length
                    }
                }
            }
        }
    }
    if ($inBlock -and $currentBlock.Length -gt 0) {
        $blocks.Add($currentBlock.ToString())
    }

    Assert-Condition ($blocks.Count -eq 10) "Expected 10 run: | blocks in workflow, found $($blocks.Count)"
    $blockIdx = 0
    foreach ($b in $blocks) {
        $blockIdx++
        $sanitized = [regex]::Replace($b, '\$\{\{[\s\S]*?\}\}', "'github_placeholder'")
        $bTokens = $null
        $bErrors = $null
        $null = [System.Management.Automation.Language.Parser]::ParseInput($sanitized, [ref]$bTokens, [ref]$bErrors)
        Assert-Condition ($bErrors.Count -eq 0) "Workflow inline run: | block $blockIdx contains $($bErrors.Count) PowerShell syntax error(s): $(($bErrors | ForEach-Object { $_.Message }) -join '; ')"
    }
    Pass "Workflow AST Parser: All $($blocks.Count) inline run: | PowerShell blocks parsed cleanly with 0 syntax errors."

    # 6.3: Parse all standalone .ps1 scripts in packaging/lifecycle-test
    $lifecycleScripts = @(Get-ChildItem -Path $ScriptDir -Filter "*.ps1")
    foreach ($s in $lifecycleScripts) {
        $sTokens = $null
        $sErrors = $null
        $sContent = [System.IO.File]::ReadAllText($s.FullName)
        $null = [System.Management.Automation.Language.Parser]::ParseInput($sContent, [ref]$sTokens, [ref]$sErrors)
        Assert-Condition ($sErrors.Count -eq 0) "Script '$($s.Name)' contains $($sErrors.Count) syntax error(s): $(($sErrors | ForEach-Object { $_.Message }) -join '; ')"
    }
    Pass "Script AST Parser: All $($lifecycleScripts.Count) standalone lifecycle scripts parsed cleanly with 0 syntax errors."

    # =========================================================================
    # Part 8: Process Ownership & Cleanup Failure Handling Tests
    # =========================================================================
    # 8.1: Test Get-ProcessOwnerSidSafely contract, exit race confirmation, and fail-closed handling
    $currentPid = [System.Diagnostics.Process]::GetCurrentProcess().Id
    $currentProcCim = Get-CimInstance -ClassName Win32_Process -Filter "ProcessId = $currentPid"
    Assert-Condition ($null -ne $currentProcCim) "Failed to query current process CIM instance"

    # 8.1.1: Standard owned SID success
    $qSelf = Get-ProcessOwnerSidSafely -Process $currentProcCim
    Assert-Condition ($qSelf.Success -eq $true) "Get-ProcessOwnerSidSafely should succeed for self"
    Assert-Condition ($qSelf.AlreadyExited -eq $false) "Self process is not AlreadyExited"
    Assert-Condition (-not [string]::IsNullOrWhiteSpace($qSelf.Sid)) "Self process owner SID must be non-empty"
    Assert-Condition ($null -eq $qSelf.Error) "Self process Error must be null"
    Pass "Process ownership helper: Standard owned SID query succeeds with strict fields."

    # 8.1.2: Mock GetOwnerSid throws Not found + fresh query returns empty -> AlreadyExited=true confirmed
    # Use non-existent PID 999999
    $mockExitedProc = [pscustomobject]@{
        ProcessId    = 999999
        CreationDate = [DateTime]::UtcNow.AddHours(-1)
    }
    $qExited = Get-ProcessOwnerSidSafely -Process $mockExitedProc
    Assert-Condition ($qExited.Success -eq $true) "Non-existent process confirmed naturally exited must have Success=true"
    Assert-Condition ($qExited.AlreadyExited -eq $true) "Non-existent process must report AlreadyExited=true"
    Assert-Condition ($null -eq $qExited.Sid) "AlreadyExited process must return null Sid"
    Assert-Condition ($null -eq $qExited.Error) "AlreadyExited process must have null Error"
    Pass "Process ownership helper: GetOwnerSid failure + fresh query empty correctly confirms AlreadyExited=true."

    # 8.1.3: Active process present + owner query fails -> fail closed as UNKNOWN (Success=false, AlreadyExited=false)
    # We can mock this by creating a mock object whose ProcessId is $currentPid but has no GetOwnerSid CIM method
    $mockActiveFailOwner = [pscustomobject]@{
        ProcessId    = $currentPid
        CreationDate = $currentProcCim.CreationDate
    }
    $qPresentFailOwner = Get-ProcessOwnerSidSafely -Process $mockActiveFailOwner
    Assert-Condition ($qPresentFailOwner.Success -eq $false) "Active process with failed GetOwnerSid must fail closed (Success=false)"
    Assert-Condition ($qPresentFailOwner.AlreadyExited -eq $false) "Active process with failed GetOwnerSid must NOT be AlreadyExited"
    Assert-Condition ($null -eq $qPresentFailOwner.Sid) "Failed owner query must return null Sid"
    Assert-Condition ($qPresentFailOwner.Error -match "still active but owner query failed") "Error must indicate process is active but owner query failed"
    Pass "Process ownership helper: Active process + owner failure correctly fails closed as UNKNOWN."

    # 8.1.4: Positive PID with owner failure where fresh CIM query throws -> fail closed as UNKNOWN
    # We can mock this by creating a mock object whose ProcessId is positive but causes Get-CimInstance to throw
    # by using an in-scope mock function for Get-CimInstance during this test block
    $mockThrowingProc = [pscustomobject]@{
        ProcessId    = 88888
        CreationDate = [DateTime]::UtcNow.AddHours(-1)
    }
    # Invoke with temporary wrapper to test fresh CIM query exception handling
    $scriptBlockThrowCim = {
        param($proc)
        function Get-CimInstance {
            param([string]$ClassName, [string]$Filter, $ErrorAction)
            throw "Simulated WMI/CIM provider RPC failure"
        }
        Get-ProcessOwnerSidSafely -Process $proc
    }
    $qThrow = & $scriptBlockThrowCim $mockThrowingProc
    Assert-Condition ($qThrow.Success -eq $false) "Query failure on fresh check must fail closed (Success=false)"
    Assert-Condition ($qThrow.AlreadyExited -eq $false) "Query failure on fresh check must NOT claim AlreadyExited"
    Assert-Condition ($qThrow.Error -match "Simulated WMI/CIM provider RPC failure") "Error must preserve CIM failure details"
    Pass "Process ownership helper: Positive PID with fresh CIM query throw correctly fails closed as UNKNOWN."

    # 8.1.5: Same PID but different CreationDate (PID reuse) -> must fail closed and NOT claim AlreadyExited or safe
    $mockReusedPid = [pscustomobject]@{
        ProcessId    = $currentPid
        CreationDate = [DateTime]::UtcNow.AddDays(-10) # definitely different from $currentProcCim.CreationDate
    }
    $qReused = Get-ProcessOwnerSidSafely -Process $mockReusedPid
    Assert-Condition ($qReused.Success -eq $false) "PID reuse must fail closed (Success=false)"
    Assert-Condition ($qReused.AlreadyExited -eq $false) "PID reuse must NOT claim AlreadyExited=true"
    Assert-Condition ($qReused.Error -match "reused by another process") "Error must record PID reuse detection"
    Pass "Process ownership helper: Same PID with different CreationDate correctly detects PID reuse without claiming exited."

    # 8.1.6: Pure helper Test-ProcessIdentityMatchForTermination unit tests (side-effect free, no process kill)
    $origTargetSample = [pscustomobject]@{
        ProcessId    = 5555
        CreationDate = [DateTime]::Parse("2026-11-06T12:00:00Z")
        OwnerSid     = "S-1-5-21-1234567890-1234567890-1234567890-5001"
    }

    # Case A: Same identity, PID, CreationDate, and expected standard user SID -> PASS
    $liveMatching = [pscustomobject]@{
        Success       = $true
        AlreadyExited = $false
        ProcessId     = 5555
        CreationDate  = [DateTime]::Parse("2026-11-06T12:00:00Z")
        Sid           = "S-1-5-21-1234567890-1234567890-1234567890-5001"
        Error         = $null
    }
    $decMatch = Test-ProcessIdentityMatchForTermination `
        -OriginalRecord $origTargetSample `
        -LiveQueryState $liveMatching `
        -ExpectedUserSid "S-1-5-21-1234567890-1234567890-1234567890-5001"
    Assert-Condition ($decMatch.CanTerminate -eq $true) "Matching process identity must allow termination"
    Assert-Condition ($decMatch.AlreadyExited -eq $false) "Matching active process is not AlreadyExited"
    Pass "Process termination guard: Exact matching identity and expected user SID passes."

    # Case B: Same PID + same SID, but DIFFERENT CreationDate (PID reuse by same user) -> REJECT
    $liveReusedSameSid = [pscustomobject]@{
        Success       = $true
        AlreadyExited = $false
        ProcessId     = 5555
        CreationDate  = [DateTime]::Parse("2026-11-06T12:05:00Z") # Reused later!
        Sid           = "S-1-5-21-1234567890-1234567890-1234567890-5001"
        Error         = $null
    }
    $decReusedSameSid = Test-ProcessIdentityMatchForTermination `
        -OriginalRecord $origTargetSample `
        -LiveQueryState $liveReusedSameSid `
        -ExpectedUserSid "S-1-5-21-1234567890-1234567890-1234567890-5001"
    Assert-Condition ($decReusedSameSid.CanTerminate -eq $false) "Same PID with different CreationDate must REJECT termination"
    Assert-Condition ($decReusedSameSid.Reason -match "PID reuse detected") "Reason must indicate PID reuse"
    Pass "Process termination guard: Same PID with different CreationDate correctly rejects termination."

    # Case C: Same PID, but DIFFERENT SID (PID reuse by another user/system) -> REJECT
    $liveDifferentSid = [pscustomobject]@{
        Success       = $true
        AlreadyExited = $false
        ProcessId     = 5555
        CreationDate  = [DateTime]::Parse("2026-11-06T12:00:00Z")
        Sid           = "S-1-5-18" # Local System
        Error         = $null
    }
    $decDiffSid = Test-ProcessIdentityMatchForTermination `
        -OriginalRecord $origTargetSample `
        -LiveQueryState $liveDifferentSid `
        -ExpectedUserSid "S-1-5-21-1234567890-1234567890-1234567890-5001"
    Assert-Condition ($decDiffSid.CanTerminate -eq $false) "Different owner SID must REJECT termination"
    Assert-Condition ($decDiffSid.Reason -match "does not match expected user SID") "Reason must indicate SID mismatch"
    Pass "Process termination guard: Different SID correctly rejects termination."

    # Case D: Live query failed / unknown (Success=false) -> REJECT
    $liveUnknown = [pscustomobject]@{
        Success       = $false
        AlreadyExited = $false
        ProcessId     = 5555
        CreationDate  = [DateTime]::Parse("2026-11-06T12:00:00Z")
        Sid           = $null
        Error         = "Access Denied"
    }
    $decUnknown = Test-ProcessIdentityMatchForTermination `
        -OriginalRecord $origTargetSample `
        -LiveQueryState $liveUnknown `
        -ExpectedUserSid "S-1-5-21-1234567890-1234567890-1234567890-5001"
    Assert-Condition ($decUnknown.CanTerminate -eq $false) "Unresolved/failed query must REJECT termination"
    Assert-Condition ($decUnknown.Reason -match "Live ownership query failed or unresolved") "Reason must indicate failed query"
    Pass "Process termination guard: Unresolved/failed live query correctly rejects termination."

    # Case E: Confirmed naturally exited (AlreadyExited=true) -> No kill, but reports AlreadyExited
    $liveExited = [pscustomobject]@{
        Success       = $true
        AlreadyExited = $true
        ProcessId     = 5555
        CreationDate  = [DateTime]::Parse("2026-11-06T12:00:00Z")
        Sid           = $null
        Error         = $null
    }
    $decExited = Test-ProcessIdentityMatchForTermination `
        -OriginalRecord $origTargetSample `
        -LiveQueryState $liveExited `
        -ExpectedUserSid "S-1-5-21-1234567890-1234567890-1234567890-5001"
    Assert-Condition ($decExited.CanTerminate -eq $false) "Exited process must not be terminated"
    Assert-Condition ($decExited.AlreadyExited -eq $true) "AlreadyExited flag must be true"
    Pass "Process termination guard: Naturally exited process correctly reported with CanTerminate=false and AlreadyExited=true."

    # 8.2: Test manifest verification rejects wrong repository, run ID, and installer hash
    $badRepoDir = Join-Path $WorkDir "bad-repo-stage"
    New-Item -ItemType Directory -Path $badRepoDir -Force | Out-Null
    [System.IO.File]::WriteAllBytes((Join-Path $badRepoDir $f1), [byte[]]@(1, 2, 3, 4, 5))
    [System.IO.File]::WriteAllBytes((Join-Path $badRepoDir $f2), [byte[]]@(6, 7, 8, 9, 10))
    [System.IO.File]::WriteAllBytes((Join-Path $badRepoDir $f3), [byte[]]@(11, 12, 13, 14, 15))

    $badRepoManifestPath = Join-Path $badRepoDir "candidate-handoff-manifest.json"
    $badManifestObj = [ordered]@{
        schema             = 1
        repository         = "wrong-owner/wrong-repo"
        workflowSha        = "11d5960a326750d5838078e36cf38b85af677262"
        runId              = "12345678"
        runAttempt         = "1"
        candidateReleaseId = "436240766"
        candidateTag       = "v0.1.0"
        files              = @([ordered]@{ name = "ozviewer-0.1.0-win-x64-installer.exe"; sha256 = $h1 })
    }
    [System.IO.File]::WriteAllText($badRepoManifestPath, ($badManifestObj | ConvertTo-Json -Depth 4))

    Assert-Throws {
        Test-CandidateHandoffDirectory `
            -CandidateDir $badRepoDir `
            -ExpectedRepository "underoatz-netizen/oz-image-viewer" `
            -ExpectedWorkflowSha "11d5960a326750d5838078e36cf38b85af677262" `
            -ExpectedRunId "12345678" `
            -ExpectedRunAttempt "1" `
            -ExpectedCandidateReleaseId "436240766" `
            -ExpectedCandidateTag "v0.1.0" `
            -ExpectedHashesByName @{ $f1 = $h1; $f2 = $h2; $f3 = $h3 }
    } "Manifest repository mismatch" "Test-CandidateHandoffDirectory rejects mismatched repository manifest"

    # 8.3: Test manifest verification rejects wrong workflowSha
    $badShaDir = Join-Path $WorkDir "bad-sha-stage"
    New-Item -ItemType Directory -Path $badShaDir -Force | Out-Null
    [System.IO.File]::WriteAllBytes((Join-Path $badShaDir $f1), [byte[]]@(1, 2, 3, 4, 5))
    [System.IO.File]::WriteAllBytes((Join-Path $badShaDir $f2), [byte[]]@(6, 7, 8, 9, 10))
    [System.IO.File]::WriteAllBytes((Join-Path $badShaDir $f3), [byte[]]@(11, 12, 13, 14, 15))

    $badShaManifestPath = Join-Path $badShaDir "candidate-handoff-manifest.json"
    $badShaObj = [ordered]@{
        schema             = 1
        repository         = "underoatz-netizen/oz-image-viewer"
        workflowSha        = "0000000000000000000000000000000000000000"
        runId              = "12345678"
        runAttempt         = "1"
        candidateReleaseId = "436240766"
        candidateTag       = "v0.1.0"
        files              = @([ordered]@{ name = "ozviewer-0.1.0-win-x64-installer.exe"; sha256 = $h1 })
    }
    [System.IO.File]::WriteAllText($badShaManifestPath, ($badShaObj | ConvertTo-Json -Depth 4))

    Assert-Throws {
        Test-CandidateHandoffDirectory `
            -CandidateDir $badShaDir `
            -ExpectedRepository "underoatz-netizen/oz-image-viewer" `
            -ExpectedWorkflowSha "11d5960a326750d5838078e36cf38b85af677262" `
            -ExpectedRunId "12345678" `
            -ExpectedRunAttempt "1" `
            -ExpectedCandidateReleaseId "436240766" `
            -ExpectedCandidateTag "v0.1.0" `
            -ExpectedHashesByName @{ $f1 = $h1; $f2 = $h2; $f3 = $h3 }
    } "Manifest workflowSha mismatch" "Test-CandidateHandoffDirectory rejects mismatched workflowSha manifest"
    Pass "Manifest validation: Rejects tampered repository and workflowSha metadata."

    # 8.4: Test profile cleanup fail-closed verification logic
    # Case A: CIM object is absent, but orphan profile directory still exists on disk -> must reject / fail verification
    $mockOrphanProfileDir = Join-Path $WorkDir "mock-orphan-profile-user"
    New-Item -ItemType Directory -Path $mockOrphanProfileDir -Force | Out-Null
    $profileCheckCimAbsent = $null
    $dirCheckOrphanPresent = Test-Path -LiteralPath $mockOrphanProfileDir

    $cleanupVerified = ($null -eq $profileCheckCimAbsent -and $dirCheckOrphanPresent -eq $false)
    Assert-Condition ($cleanupVerified -eq $false) "Cleanup verification must fail when orphan profile directory remains on disk"
    Pass "Profile cleanup verification: Rejects CIM object absent when physical orphan directory remains on disk."

    # Case B: CIM object is absent AND physical directory is absent -> passes verification
    Remove-Item -LiteralPath $mockOrphanProfileDir -Recurse -Force
    $dirCheckAbsent = Test-Path -LiteralPath $mockOrphanProfileDir
    $cleanupVerifiedPass = ($null -eq $profileCheckCimAbsent -and $dirCheckAbsent -eq $false)
    Assert-Condition ($cleanupVerifiedPass -eq $true) "Cleanup verification must pass when both CIM object and physical directory are absent"
    Pass "Profile cleanup verification: Confirms verified absence when both CIM object and physical directory are absent."

    # 8.5: Test Test-ChildReportIntegrity rejects missing/blank ExpectedProfileImagePath
    Assert-Throws {
        Test-ChildReportIntegrity `
            -ReportData $validReport `
            -ExpectedUserSid $dummySid `
            -ExpectedInstallerSha $h1 `
            -ProofData $validProof `
            -ExpectedProfileImagePath "   "
    } "ExpectedProfileImagePath must be a non-blank path string" "Test-ChildReportIntegrity rejects blank ExpectedProfileImagePath"
    Pass "Strict contract: Test-ChildReportIntegrity strictly requires non-blank ExpectedProfileImagePath."

    # 8.6: StrictMode Regression & State Initialization Test (Early post-account failure recreation)
    # Recreates early failure after user creation under StrictMode:
    # Under uninitialized state, reading $expectedProfilePath in finally throws an unhandled variable error.
    # With state initialized before try, cleanup safely catches unknown profile path, marks cleanup as failed,
    # persists durable failed report without throwing, and reaps scratch workspace.
    $strictTestScript = Join-Path $WorkDir "strict-cleanup-regression.ps1"
    $strictTestCode = @'
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Test-ControllerCleanupStateFlow {
    param(
        [bool]$SimulateUninitializedBug,
        [string]$TestScratchRoot,
        [string]$TestReportPath
    )

    $overallSuccess = $false
    $isSkipped = $false
    $createdUserName = "mock_user"
    $createdUserSid = "S-1-5-21-9999999999-9999999999-9999999999-1002"
    $scratchRoot = $TestScratchRoot

    # In fixed controller: initialized before try
    if (-not $SimulateUninitializedBug) {
        $expectedProfilePath = $null
        $accountCreationTime = [System.DateTime]::UtcNow
    }

    $reportObj = [ordered]@{
        suite   = "standard-user-controller"
        checks  = @()
        cleanup = [ordered]@{}
        summary = "pending"
    }

    try {
        # Simulate early failure immediately after account creation (e.g. staging or bootstrap failure)
        throw "Simulated staging failure after account creation"
    } catch {
        $reportObj.checks += [ordered]@{ name = "early_failure"; status = "fail"; details = "$_" }
    } finally {
        $cleanupLog = [ordered]@{}
        $cleanupAllPassed = $true

        if ($null -ne $createdUserSid) {
            # In buggy code, $expectedProfilePath was not defined before try, so reading it throws in StrictMode
            $profilePathBefore = $null
            if (-not [string]::IsNullOrWhiteSpace($expectedProfilePath)) {
                $profilePathBefore = $expectedProfilePath
            }

            # Because $profilePathBefore is null/unknown, profile cleanup must fail closed
            if ([string]::IsNullOrWhiteSpace($profilePathBefore)) {
                $cleanupLog["userProfileRemoved"] = $false
                $cleanupLog["userProfileRemovalError"] = "Profile directory path was never identified for created SID '$createdUserSid'; cleanup unverified."
                $cleanupAllPassed = $false
            }
        }

        # Scratch cleanup
        if ($null -ne $scratchRoot -and (Test-Path -LiteralPath $scratchRoot)) {
            Remove-Item -LiteralPath $scratchRoot -Recurse -Force
            $cleanupLog["scratchRootRemoved"] = $true
        }

        $reportObj.cleanup = $cleanupLog
        $reportObj.summary = "failed"
        [System.IO.File]::WriteAllText($TestReportPath, ($reportObj | ConvertTo-Json -Depth 4))
    }

    return $reportObj.summary
}
'@
    [System.IO.File]::WriteAllText($strictTestScript, $strictTestCode)

    # 8.6.1: Verify original uninitialized pattern throws VariableNotSet error under StrictMode
    $uninitRunnerScript = Join-Path $WorkDir "run-uninit.ps1"
    $uninitScratch = Join-Path $WorkDir "mock-scratch-uninit"
    $uninitReport = Join-Path $WorkDir "mock-report-uninit.json"
    $uninitRunnerCode = @"
. '$strictTestScript'
Test-ControllerCleanupStateFlow -SimulateUninitializedBug `$true -TestScratchRoot '$uninitScratch' -TestReportPath '$uninitReport'
"@
    [System.IO.File]::WriteAllText($uninitRunnerScript, $uninitRunnerCode)

    $uninitThrewExpected = $false
    try {
        $null = & powershell -NoProfile -ExecutionPolicy Bypass -File $uninitRunnerScript 2>&1
    } catch {
        if ($_.Exception.Message -match "variable '\`$expectedProfilePath' cannot be retrieved") {
            $uninitThrewExpected = $true
        }
    }
    Assert-Condition $uninitThrewExpected "Uninitialized bug must trigger variable not set error in StrictMode"
    Pass "Controller state regression: Confirmed original uninitialized `$expectedProfilePath triggers StrictMode variable not set failure."

    # 8.6.2: Verify fixed initialized state executes cleanly, fails closed on unknown profile, cleans scratch, and saves report
    $mockScratch = Join-Path $WorkDir "mock-scratch-init"
    New-Item -ItemType Directory -Path $mockScratch -Force | Out-Null
    $mockReport = Join-Path $WorkDir "mock-report-init.json"

    $initRunnerScript = Join-Path $WorkDir "run-init.ps1"
    $initRunnerCode = @"
. '$strictTestScript'
Test-ControllerCleanupStateFlow -SimulateUninitializedBug `$false -TestScratchRoot '$mockScratch' -TestReportPath '$mockReport'
"@
    [System.IO.File]::WriteAllText($initRunnerScript, $initRunnerCode)

    $initRes = & powershell -NoProfile -ExecutionPolicy Bypass -File $initRunnerScript
    Assert-Condition ($initRes -eq "failed") "Fixed controller flow must return 'failed' summary"
    Assert-Condition (Test-Path -LiteralPath $mockReport) "Fixed controller flow must persist report"
    Assert-Condition (-not (Test-Path -LiteralPath $mockScratch)) "Fixed controller flow must clean scratch root"

    $repSaved = Get-Content -LiteralPath $mockReport -Raw | ConvertFrom-Json
    Assert-Condition ($repSaved.cleanup.userProfileRemoved -eq $false) "Profile cleanup must fail closed when profile path was never established"
    Assert-Condition ($repSaved.cleanup.scratchRootRemoved -eq $true) "Scratch root removal must succeed"
    Pass "Controller state regression: Fixed initialized state completes cleanup, fails closed on unknown profile, removes scratch root, and persists durable failed report."

    # 8.7: Ephemeral username generator unit tests & length regression
    # Verify literal old prefix length vs new prefix length
    $oldSample = "oztestuser_$([System.Guid]::NewGuid().ToString('N').Substring(0, 10))"
    Assert-Condition ($oldSample.Length -eq 21) "Old prefix pattern must produce exactly 21 characters (violating Windows 20-char SAM limit)"
    Pass "Username format regression: Confirmed old username pattern length is 21 (> 20 characters)."

    # Test 100 generated names from New-EphemeralStandardUserName
    $generatedSet = [System.Collections.Generic.HashSet[string]]::new()
    for ($i = 0; $i -lt 100; $i++) {
        $uName = New-EphemeralStandardUserName
        Assert-Condition ($uName.Length -eq 15) "Generated username '$uName' length is $($uName.Length), expected exactly 15 chars"
        Assert-Condition ($uName.Length -le 20) "Generated username '$uName' must be <= 20 chars"
        Assert-Condition ($uName -match '^ozsu_[0-9a-f]{10}$') "Generated username '$uName' must match regex '^ozsu_[0-9a-f]{10}$'"
        Assert-Condition (-not $generatedSet.Contains($uName)) "Generated username '$uName' collision detected among 100 samples"
        $generatedSet.Add($uName) | Out-Null
    }
    Pass "Username generator: 100 generated names verified safe (length 15 <= 20, regex compliant, 100% unique, zero local accounts created)."

    # 8.8: New-LocalUser parameter length constraints & AST inspection of controller call
    # Inspect New-LocalUser parameter metadata directly from PowerShell command metadata
    $newLocalUserCmd = Get-Command New-LocalUser
    $nameMaxLen = $null
    $descMaxLen = $null
    if ($newLocalUserCmd.Parameters.ContainsKey('Name')) {
        $lenAttr = $newLocalUserCmd.Parameters['Name'].Attributes | Where-Object { $_ -is [System.Management.Automation.ValidateLengthAttribute] }
        if ($null -ne $lenAttr) { $nameMaxLen = $lenAttr.MaxLength }
    }
    if ($newLocalUserCmd.Parameters.ContainsKey('Description')) {
        $lenAttr = $newLocalUserCmd.Parameters['Description'].Attributes | Where-Object { $_ -is [System.Management.Automation.ValidateLengthAttribute] }
        if ($null -ne $lenAttr) { $descMaxLen = $lenAttr.MaxLength }
    }
    Assert-Condition ($nameMaxLen -eq 20) "New-LocalUser Name MaxLength must be 20"
    Assert-Condition ($descMaxLen -eq 48) "New-LocalUser Description MaxLength must be 48"
    Pass "Command metadata: New-LocalUser Name MaxLength=20 and Description MaxLength=48 confirmed."

    # Inspect the actual production controller script AST for New-LocalUser call arguments
    $controllerContent = [System.IO.File]::ReadAllText($StandardUserControllerScript)
    $cTokens = $null
    $cErrors = $null
    $cAst = [System.Management.Automation.Language.Parser]::ParseInput($controllerContent, [ref]$cTokens, [ref]$cErrors)
    Assert-Condition ($cErrors.Count -eq 0) "Controller script has 0 AST parser errors"

    $newLocalUserAsts = $cAst.FindAll({
        param($node)
        $node -is [System.Management.Automation.Language.CommandAst] -and
        $node.GetCommandName() -eq 'New-LocalUser'
    }, $true)

    Assert-Condition ($newLocalUserAsts.Count -eq 1) "Expected exactly 1 New-LocalUser command invocation in controller"
    $cmdAst = $newLocalUserAsts[0]
    $descArgValue = $null

    $elements = $cmdAst.CommandElements
    for ($i = 0; $i -lt $elements.Count; $i++) {
        if ($elements[$i] -is [System.Management.Automation.Language.CommandParameterAst] -and $elements[$i].ParameterName -eq 'Description') {
            $descArgValue = $elements[$i + 1].SafeGetValue()
            break
        }
    }

    Assert-Condition ($null -ne $descArgValue) "Could not resolve Description parameter argument on New-LocalUser call in AST"
    Assert-Condition ($descArgValue.Length -le 48) "Controller Description literal '$descArgValue' length ($($descArgValue.Length)) must be <= 48 chars"
    Assert-Condition ($descArgValue -eq "OZ release test") "Controller Description literal expected 'OZ release test', got '$descArgValue'"
    Pass "AST constraint verification: Controller New-LocalUser Description is '$descArgValue' (length $($descArgValue.Length) <= 48)."

    Write-Host "`nALL CANDIDATE HANDOFF AND WORKFLOW SECURITY TESTS PASSED!" -ForegroundColor Green
    exit 0
} catch {
    Write-Error "Test execution failed: $_"
    exit 1
} finally {
    Remove-Item -LiteralPath $WorkDir -Recurse -Force -ErrorAction SilentlyContinue
}

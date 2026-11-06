<#
.SYNOPSIS
  Disposable GitHub Actions Windows 2022 runner standard-user controller and child process bootstrapper.

.DESCRIPTION
  This controller implements the genuine standard-user lifecycle verification gate
  on disposable GitHub-hosted Windows runners (windows-2022).
  It must NOT be run on developer workstations.

  Architecture & Safety Constraints:
    - Enforces execution environment guard: strictly requires $env:GITHUB_ACTIONS = "true"
      AND $env:RUNNER_ENVIRONMENT = "github-hosted" AND Windows Server 2022 (build >= 20348)
      along with the mandatory -AllowDisposableExecution switch.
    - Generates a unique ephemeral local standard user with an OS-cryptographic random
      password held strictly in memory as a SecureString/PSCredential (never written to disk,
      never passed via command line arguments, never logged).
    - Ensures the created account belongs to Builtin\Users (SID S-1-5-32-545) and strictly
      asserts it is NOT a member of Builtin\Administrators (SID S-1-5-32-544).
    - Sets up an isolated scratch root under direct SystemDrive (e.g. C:\ozviewer-su-scratch-<guid>)
      outside the git checkout to avoid parent runner profile traversal restrictions.
    - Rebuilds exact DirectorySecurity on scratchRoot, scratch\inputs (read-only for child),
      scratch\output (modify for child), and scratch\parent (parent report), ensuring only
      the created user SID, controller/Admins, and SYSTEM have access.
    - Pure bootstrap generator function creates child script with minimal clean environment:
      sets CI=true (non-secret attestation), reconstructs SystemRoot, ComSpec, USERPROFILE,
      LOCALAPPDATA, APPDATA, TEMP, TMP (derived under the actual new user's LocalAppData\Temp,
      never falling back to system Temp), and absolute powershell.exe on PATH.
    - Child process asserts identity SID matches created account, asserts WindowsPrincipal
      Administrator is false, verifies native token elevation is false, asserts profile directory
      under ProfileList matches created SID, and verifies real HKCU registry binding by writing
      a GUID nonce to HKCU and verifying its exact presence via Registry.Users.OpenSubKey(createdSID).
    - Child runs test-installer-lifecycle.ps1 with -RequireStandardUser without path overrides,
      testing the real new-user default path (%LOCALAPPDATA%\Programs\OZ Image Viewer).
    - Parent monitors execution with a 5-minute wallclock timeout, caches exit code, and disposes
      process handle finally.
    - Validates child report: checks all 28 exact required check names once, strictly requiring 'pass'
      (any fail/unknown/skipped/warning blocks the gate).
    - Performs verified cleanup of child processes (by owner SID), local user account, profile,
      and scratch directory. Cleanup verification results are recorded in the parent report.
    - Final parent durable report is saved AFTER cleanup and before exit. Any report save error
      fails closed with non-zero exit.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$CandidateDir,
    [Parameter(Mandatory = $true)][string]$ExpectedInstallerSha256,
    [Parameter(Mandatory = $true)][string]$WorkflowSha,
    [string]$Repository = "",
    [string]$RunId = "",
    [string]$RunAttempt = "",
    [string]$CandidateReleaseId = "",
    [string]$CandidateTag = "",
    [Parameter(Mandatory = $true)][string]$ParentReportPath,
    [switch]$AllowDisposableExecution
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Dot-source the pure shared standard-user helpers
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$helpersPath = Join-Path $ScriptDir "StandardUserHelpers.ps1"
if (-not (Test-Path -LiteralPath $helpersPath -PathType Leaf)) {
    throw "StandardUserHelpers.ps1 not found in $ScriptDir"
}
. $helpersPath

$script:ParentReport = [ordered]@{
    suite                  = "standard-user-controller"
    timestamp              = [System.DateTime]::UtcNow.ToString("o")
    environmentAttestation = "unknown"
    runnerOs               = [System.Environment]::OSVersion.VersionString
    workflowSha            = $WorkflowSha
    expectedInstallerSha   = $ExpectedInstallerSha256
    createdUserSid         = $null
    createdUserName        = $null
    scratchRoot            = $null
    checks                 = @()
    childReport            = $null
    cleanup                = [ordered]@{}
    summary                = "pending"
}

function Record-ParentCheck {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Status,
        [string]$Details = ""
    )
    $entry = [ordered]@{
        name    = $Name
        status  = $Status
        details = $Details
    }
    $script:ParentReport.checks += $entry
    $color = switch ($Status) {
        "pass"    { "Green" }
        "fail"    { "Red" }
        "warning" { "Yellow" }
        "skipped" { "Cyan" }
        default   { "White" }
    }
    Write-Host "[parent-check] [$Status] $Name - $Details" -ForegroundColor $color
}

function Save-ParentReportInternal {
    param([string]$FinalSummary)
    $script:ParentReport.summary = $FinalSummary
    $pDir = Split-Path -Parent $ParentReportPath
    if (-not [string]::IsNullOrWhiteSpace($pDir) -and -not (Test-Path -LiteralPath $pDir)) {
        New-Item -ItemType Directory -Path $pDir -Force | Out-Null
    }
    $json = $script:ParentReport | ConvertTo-Json -Depth 6
    [System.IO.File]::WriteAllText($ParentReportPath, $json, (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "Standard-user controller report saved to: $ParentReportPath"
}

# --- Execution Boundary ---
$overallSuccess = $false
$isSkipped = $false
$createdUserName = New-EphemeralStandardUserName
$createdUserSid = $null
$localUserObj = $null
$scratchRoot = $null
$childProc = $null
$exitCodeCached = $null
$accountCreationTime = $null
$expectedProfilePath = $null

try {
    # Guard 1: Safety switch & Disposable Runner Environment Attestation
    if (-not $AllowDisposableExecution) {
        Record-ParentCheck -Name "guard_disposable_switch" -Status "skipped" -Details "Refusing execution: -AllowDisposableExecution switch required."
        $isSkipped = $true
        throw "EXIT_SKIPPED_77"
    }

    $isGithubActions = ($env:GITHUB_ACTIONS -eq "true")
    $runnerEnv = $env:RUNNER_ENVIRONMENT
    $osBuild = [System.Environment]::OSVersion.Version.Build

    if (-not ($isGithubActions -and $runnerEnv -eq "github-hosted" -and $osBuild -ge 20348)) {
        $detail = "Refusing execution outside GitHub-hosted Windows Server 2022 runner (GITHUB_ACTIONS=$isGithubActions, RUNNER_ENVIRONMENT=$runnerEnv, OSBuild=$osBuild)."
        Record-ParentCheck -Name "guard_hosted_runner_attestation" -Status "skipped" -Details $detail
        $isSkipped = $true
        throw "EXIT_SKIPPED_77"
    }
    Record-ParentCheck -Name "guard_hosted_runner_attestation" -Status "pass" -Details "Confirmed GitHub-hosted Windows Server 2022 runner environment."
    $script:ParentReport.environmentAttestation = "github-hosted-windows2022"

    # Guard 2: Candidate Assets & Expected Hash Verification
    $installerPath = Join-Path $CandidateDir "ozviewer-0.1.0-win-x64-installer.exe"
    if (-not (Test-Path -LiteralPath $installerPath -PathType Leaf)) {
        Record-ParentCheck -Name "candidate_installer_exists" -Status "fail" -Details "Candidate installer missing: $installerPath"
        throw "Installer missing"
    }
    $actualInstallerHash = (Get-FileHash -LiteralPath $installerPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $expectedCleanHash = $ExpectedInstallerSha256.Trim().ToLowerInvariant()
    if ($actualInstallerHash -ne $expectedCleanHash) {
        Record-ParentCheck -Name "candidate_installer_hash" -Status "fail" -Details "Hash mismatch on candidate installer! Expected '$expectedCleanHash', got '$actualInstallerHash'."
        throw "Installer hash mismatch"
    }
    Record-ParentCheck -Name "candidate_installer_hash" -Status "pass" -Details "Candidate installer SHA-256 verified ($actualInstallerHash)."

    # Setup Scratch Directory Structure directly under SystemDrive (avoid runner profile traversal restrictions)
    $sysDrive = [System.Environment]::GetEnvironmentVariable('SystemDrive')
    if ([string]::IsNullOrWhiteSpace($sysDrive)) { $sysDrive = "C:" }
    $scratchRoot = Join-Path $sysDrive "ozviewer-su-scratch-$([System.Guid]::NewGuid().ToString('N'))"
    $script:ParentReport.scratchRoot = $scratchRoot

    $scratchInputs = Join-Path $scratchRoot "inputs"
    $scratchOutput = Join-Path $scratchRoot "output"
    $scratchParent = Join-Path $scratchRoot "parent"

    New-Item -ItemType Directory -Path $scratchInputs -Force | Out-Null
    New-Item -ItemType Directory -Path $scratchOutput -Force | Out-Null
    New-Item -ItemType Directory -Path $scratchParent -Force | Out-Null

    # Generate OS-Cryptographic Random Password in Memory
    $rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    $tokenBytes = [byte[]]::new(32)
    $rng.GetBytes($tokenBytes)
    $plainPass = [System.Convert]::ToBase64String($tokenBytes) + "A1!z"
    $secPass = ConvertTo-SecureString $plainPass -AsPlainText -Force
    $rng.Dispose()

    $accountCreationTime = [System.DateTime]::UtcNow

    # Create Ephemeral Standard User
    Write-Host "Creating ephemeral local standard user: $createdUserName..."
    $localUserObj = New-LocalUser -Name $createdUserName -Password $secPass -Description "OZ release test" -PasswordNeverExpires
    $createdUserSid = $localUserObj.SID.Value
    $script:ParentReport.createdUserName = $createdUserName
    $script:ParentReport.createdUserSid = $createdUserSid

    $usersGroup = Get-LocalGroup -SID "S-1-5-32-545" # Builtin\Users
    Add-LocalGroupMember -Group $usersGroup -Member $localUserObj -ErrorAction SilentlyContinue

    $adminsGroup = Get-LocalGroup -SID "S-1-5-32-544" # Builtin\Administrators
    $adminMembers = @(Get-LocalGroupMember -Group $adminsGroup | ForEach-Object { $_.SID.Value })
    if ($adminMembers -contains $createdUserSid) {
        throw "Security violation: Created account is in Administrators group!"
    }
    Record-ParentCheck -Name "user_creation_standard_privileges" -Status "pass" -Details "Created user $createdUserName ($createdUserSid) verified as non-admin in Builtin\Users."

    # Copy Reviewed Artifacts & Scripts into Scratch Inputs
    $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
    $copiedFiles = @(
        (Join-Path $scriptDir "test-installer-lifecycle.ps1"),
        (Join-Path $scriptDir "ProcessHelper.ps1"),
        (Join-Path $scriptDir "candidate-handoff.ps1"),
        (Join-Path $scriptDir "StandardUserHelpers.ps1"),
        $installerPath
    )

    $fixtureSrc = Join-Path (Split-Path -Parent (Split-Path -Parent $scriptDir)) "tests\data\webp\bench_64x48.webp"
    if (-not (Test-Path -LiteralPath $fixtureSrc)) {
        $fixtureSrc = Join-Path (Split-Path -Parent (Split-Path -Parent $scriptDir)) "tests\data\phasea\bench_64x48.bmp"
    }
    $copiedFiles += $fixtureSrc

    foreach ($f in $copiedFiles) {
        $dest = Join-Path $scratchInputs (Split-Path -Leaf $f)
        Copy-Item -LiteralPath $f -Destination $dest -Force
    }

    # Manifest copy & strict identity verification (required)
    $manifestSrc = Join-Path $CandidateDir "candidate-handoff-manifest.json"
    if (-not (Test-Path -LiteralPath $manifestSrc -PathType Leaf)) {
        throw "Manifest candidate-handoff-manifest.json missing in CandidateDir: '$CandidateDir'"
    }
    $manifestRaw = Get-Content -LiteralPath $manifestSrc -Raw
    $manifestData = $manifestRaw | ConvertFrom-Json

    # Bind manifest properties
    $expectedRepo = if (-not [string]::IsNullOrWhiteSpace($Repository)) { $Repository } else { $env:GITHUB_REPOSITORY }
    $expectedRun = if (-not [string]::IsNullOrWhiteSpace($RunId)) { $RunId } else { $env:GITHUB_RUN_ID }
    $expectedAttempt = if (-not [string]::IsNullOrWhiteSpace($RunAttempt)) { $RunAttempt } else { $env:GITHUB_RUN_ATTEMPT }
    $expectedRelId = if (-not [string]::IsNullOrWhiteSpace($CandidateReleaseId)) { $CandidateReleaseId } else { $env:CANDIDATE_RELEASE_ID }
    $expectedTag = if (-not [string]::IsNullOrWhiteSpace($CandidateTag)) { $CandidateTag } else { $env:CANDIDATE_TAG }

    if (-not [string]::IsNullOrWhiteSpace($expectedRepo) -and "$($manifestData.repository)".Trim() -ne $expectedRepo.Trim()) {
        throw "Manifest repository mismatch: expected '$expectedRepo', got '$($manifestData.repository)'"
    }
    if ("$($manifestData.workflowSha)".Trim().ToLowerInvariant() -ne $WorkflowSha.Trim().ToLowerInvariant()) {
        throw "Manifest workflowSha mismatch: expected '$WorkflowSha', got '$($manifestData.workflowSha)'"
    }
    if (-not [string]::IsNullOrWhiteSpace($expectedRun) -and "$($manifestData.runId)".Trim() -ne "$expectedRun".Trim()) {
        throw "Manifest runId mismatch: expected '$expectedRun', got '$($manifestData.runId)'"
    }
    if (-not [string]::IsNullOrWhiteSpace($expectedAttempt) -and "$($manifestData.runAttempt)".Trim() -ne "$expectedAttempt".Trim()) {
        throw "Manifest runAttempt mismatch: expected '$expectedAttempt', got '$($manifestData.runAttempt)'"
    }
    if (-not [string]::IsNullOrWhiteSpace($expectedRelId) -and "$($manifestData.candidateReleaseId)".Trim() -ne "$expectedRelId".Trim()) {
        throw "Manifest candidateReleaseId mismatch: expected '$expectedRelId', got '$($manifestData.candidateReleaseId)'"
    }
    if (-not [string]::IsNullOrWhiteSpace($expectedTag) -and "$($manifestData.candidateTag)".Trim() -ne "$expectedTag".Trim()) {
        throw "Manifest candidateTag mismatch: expected '$expectedTag', got '$($manifestData.candidateTag)'"
    }

    # Bind installer in manifest to expected installer hash
    $manifestInstallerEntry = @($manifestData.files | Where-Object { $_.name -eq "ozviewer-0.1.0-win-x64-installer.exe" })
    if ($manifestInstallerEntry.Count -ne 1) {
        throw "Manifest does not contain exactly one entry for 'ozviewer-0.1.0-win-x64-installer.exe' (count: $($manifestInstallerEntry.Count))"
    }
    if ("$($manifestInstallerEntry[0].sha256)".Trim().ToLowerInvariant() -ne $expectedCleanHash) {
        throw "Manifest installer hash mismatch! Manifest reports '$($manifestInstallerEntry[0].sha256)', expected '$expectedCleanHash'"
    }

    $script:ParentReport["manifest"] = $manifestData

    Copy-Item -LiteralPath $manifestSrc -Destination (Join-Path $scratchInputs "candidate-handoff-manifest.json") -Force

    # Rehash copied installer to ensure exact byte identity after copy
    $stagedInstaller = Join-Path $scratchInputs (Split-Path -Leaf $installerPath)
    $postCopyHash = (Get-FileHash -LiteralPath $stagedInstaller -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($postCopyHash -ne $expectedCleanHash) {
        throw "Post-copy hash mismatch on installer! Expected '$expectedCleanHash', got '$postCopyHash'"
    }
    Record-ParentCheck -Name "scratch_inputs_prepared" -Status "pass" -Details "Verified reviewed scripts and candidate assets staged and rehashed in $scratchInputs."

    # Rebuild Restrictive DirectorySecurity on Scratch Dirs
    $secRoot = New-Object System.Security.AccessControl.DirectorySecurity
    $secRoot.SetAccessRuleProtection($true, $false)
    $ruleSystem = New-Object System.Security.AccessControl.FileSystemAccessRule([System.Security.Principal.SecurityIdentifier]"S-1-5-18", "FullControl", "ContainerInherit,ObjectInherit", "None", "Allow")
    $ruleAdmins = New-Object System.Security.AccessControl.FileSystemAccessRule([System.Security.Principal.SecurityIdentifier]"S-1-5-32-544", "FullControl", "ContainerInherit,ObjectInherit", "None", "Allow")
    $ruleUserTraverse = New-Object System.Security.AccessControl.FileSystemAccessRule($localUserObj.SID, "ReadAndExecute", "None", "None", "Allow")
    $secRoot.AddAccessRule($ruleSystem)
    $secRoot.AddAccessRule($ruleAdmins)
    $secRoot.AddAccessRule($ruleUserTraverse)
    $dInfo = [System.IO.DirectoryInfo]::new($scratchRoot)
    $dInfo.SetAccessControl($secRoot)

    # Inputs: ReadAndExecute for child
    $secInputs = New-Object System.Security.AccessControl.DirectorySecurity
    $secInputs.SetAccessRuleProtection($true, $false)
    $ruleUserRead = New-Object System.Security.AccessControl.FileSystemAccessRule($localUserObj.SID, "ReadAndExecute", "ContainerInherit,ObjectInherit", "None", "Allow")
    $secInputs.AddAccessRule($ruleSystem)
    $secInputs.AddAccessRule($ruleAdmins)
    $secInputs.AddAccessRule($ruleUserRead)
    [System.IO.DirectoryInfo]::new($scratchInputs).SetAccessControl($secInputs)

    # Output: Modify for child
    $secOutput = New-Object System.Security.AccessControl.DirectorySecurity
    $secOutput.SetAccessRuleProtection($true, $false)
    $ruleUserModify = New-Object System.Security.AccessControl.FileSystemAccessRule($localUserObj.SID, "Modify", "ContainerInherit,ObjectInherit", "None", "Allow")
    $secOutput.AddAccessRule($ruleSystem)
    $secOutput.AddAccessRule($ruleAdmins)
    $secOutput.AddAccessRule($ruleUserModify)
    [System.IO.DirectoryInfo]::new($scratchOutput).SetAccessControl($secOutput)

    # Parent: No access for child
    $secParent = New-Object System.Security.AccessControl.DirectorySecurity
    $secParent.SetAccessRuleProtection($true, $false)
    $secParent.AddAccessRule($ruleSystem)
    $secParent.AddAccessRule($ruleAdmins)
    [System.IO.DirectoryInfo]::new($scratchParent).SetAccessControl($secParent)

    Record-ParentCheck -Name "scratch_acls_applied" -Status "pass" -Details "DirectorySecurity applied: root traverse only, inputs read-only, output modify, parent protected."

    # Generate Child Bootstrap Script
    $childBootstrapScript = Join-Path $scratchInputs "child-bootstrap.ps1"
    $childReportPath = Join-Path $scratchOutput "child-lifecycle-report.json"
    $childProofPath = Join-Path $scratchOutput "child-identity-proof.json"
    $childStdoutPath = Join-Path $scratchOutput "child-stdout.log"
    $childStderrPath = Join-Path $scratchOutput "child-stderr.log"

    $bootstrapCode = New-ChildBootstrapScriptContent `
        -ExpectedUserSid $createdUserSid `
        -InputsDir $scratchInputs `
        -OutputDir $scratchOutput `
        -InstallerFileName (Split-Path -Leaf $installerPath) `
        -FixtureFileName (Split-Path -Leaf $fixtureSrc) `
        -ChildReportFileName "child-lifecycle-report.json" `
        -ChildProofFileName "child-identity-proof.json"

    [System.IO.File]::WriteAllText($childBootstrapScript, $bootstrapCode, (New-Object System.Text.UTF8Encoding($false)))

    # Launch Child Process
    $cred = New-Object System.Management.Automation.PSCredential($createdUserName, $secPass)
    $powershellExe = Join-Path $env:SystemRoot "System32\WindowsPowerShell\v1.0\powershell.exe"

    Write-Host "Launching standard-user child process..."
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $powershellExe
    $psi.Arguments = "-NoProfile -NonInteractive -ExecutionPolicy Bypass -File `"$childBootstrapScript`""
    $psi.WorkingDirectory = $scratchInputs
    $psi.UserName = $cred.UserName
    $psi.Password = $cred.Password
    $psi.LoadUserProfile = $true
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true

    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $childProc = [System.Diagnostics.Process]::Start($psi)

    $timeoutSeconds = 300
    $outTask = $childProc.StandardOutput.ReadToEndAsync()
    $errTask = $childProc.StandardError.ReadToEndAsync()

    $completed = $childProc.WaitForExit($timeoutSeconds * 1000)
    $sw.Stop()

    if (-not $completed) {
        Write-Warning "Child process timed out after $timeoutSeconds seconds. Initiating process termination..."
        try { $childProc.Kill() } catch {}
        Record-ParentCheck -Name "child_process_execution" -Status "fail" -Details "Child process timed out after $timeoutSeconds seconds."
        throw "Child process timeout"
    }

    $exitCodeCached = $childProc.ExitCode
    $stdout = if ($outTask.IsCompleted) { $outTask.Result } else { "" }
    $stderr = if ($errTask.IsCompleted) { $errTask.Result } else { "" }
    [System.IO.File]::WriteAllText($childStdoutPath, $stdout)
    [System.IO.File]::WriteAllText($childStderrPath, $stderr)

    Write-Host "Child process finished with exit code $exitCodeCached in $([math]::Round($sw.Elapsed.TotalSeconds, 2))s."

    if ($exitCodeCached -ne 0) {
        Record-ParentCheck -Name "child_process_exit_code" -Status "fail" -Details "Child process exited with non-zero code: $exitCodeCached. StdErr: $stderr"
        throw "Child process exit failure"
    }
    Record-ParentCheck -Name "child_process_exit_code" -Status "pass" -Details "Child process exited with code 0."

    # Validate Child Report and Proof
    if (-not (Test-Path -LiteralPath $childReportPath -PathType Leaf)) {
        Record-ParentCheck -Name "child_report_exists" -Status "fail" -Details "Child lifecycle report missing at $childReportPath!"
        throw "Child report missing"
    }
    Record-ParentCheck -Name "child_report_exists" -Status "pass" -Details "Child report file exists."

    if (-not (Test-Path -LiteralPath $childProofPath -PathType Leaf)) {
        Record-ParentCheck -Name "child_proof_exists" -Status "fail" -Details "Child identity proof missing at $childProofPath!"
        throw "Child identity proof missing"
    }
    Record-ParentCheck -Name "child_proof_exists" -Status "pass" -Details "Child identity proof file exists."

    $childReportData = Get-Content -LiteralPath $childReportPath -Raw | ConvertFrom-Json
    $script:ParentReport.childReport = $childReportData

    $childProofData = Get-Content -LiteralPath $childProofPath -Raw | ConvertFrom-Json
    $script:ParentReport["childProof"] = $childProofData

    $controllerSid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    $expectedProfilePath = $null
    try {
        $pKey = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey("SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList\$createdUserSid")
        if ($null -ne $pKey) {
            $expectedProfilePath = "$($pKey.GetValue('ProfileImagePath'))"
            $pKey.Close()
        }
    } catch {
        throw "Failed to query HKLM ProfileList for created user SID '$createdUserSid': $_"
    }

    if ([string]::IsNullOrWhiteSpace($expectedProfilePath)) {
        throw "HKLM ProfileList lookup returned empty ProfileImagePath for created user SID '$createdUserSid'!"
    }
    $script:ParentReport["expectedProfileImagePath"] = $expectedProfilePath

    $integrityPassed = Test-ChildReportIntegrity `
        -ReportData $childReportData `
        -ExpectedUserSid $createdUserSid `
        -ExpectedInstallerSha $expectedCleanHash `
        -ProofData $childProofData `
        -ExpectedProfileImagePath $expectedProfilePath `
        -ControllerSid $controllerSid
    if ($integrityPassed -ne $true) {
        throw "Child report integrity failed"
    }
    Record-ParentCheck -Name "child_report_integrity" -Status "pass" -Details "All 28 required lifecycle checks verified strictly 'pass', identity, proof, and installer hash bound."

    $overallSuccess = $true
} catch {
    if ($_.Exception.Message -ne "EXIT_SKIPPED_77") {
        Record-ParentCheck -Name "unhandled_controller_exception" -Status "fail" -Details "$_"
    }
} finally {
    # Child process disposal
    if ($null -ne $childProc) {
        $childProc.Dispose()
    }

    # Verified Cleanup Phase
    Write-Host "Initiating verified cleanup of ephemeral user and processes..."
    $cleanupLog = [ordered]@{}
    $cleanupAllPassed = $true

    if ($null -ne $createdUserSid) {
        # 1. Terminate remaining processes owned by created user SID
        # Restrict inspection to processes created after this controller created the ephemeral account
        # Bounded ownership query helper: any query failure counts as UNKNOWN/unresolved
        try {
            $cimProcs = Get-CimInstance Win32_Process
            $targetsToKill = [System.Collections.Generic.List[object]]::new()
            $unresolvedRelevant = $false

            foreach ($p in $cimProcs) {
                $pCreation = $p.CreationDate
                if ($null -ne $pCreation -and $null -ne $accountCreationTime) {
                    if ($pCreation -lt $accountCreationTime) {
                        # Pre-existing process created before account existed; cannot be owned by new user
                        continue
                    }
                }

                $queryRes = Get-ProcessOwnerSidSafely -Process $p
                if ($queryRes.AlreadyExited) {
                    # Process confirmed naturally exited between snapshot and query; safe to skip
                    continue
                }
                if ($queryRes.Success) {
                    if ($queryRes.Sid -eq $createdUserSid) {
                        $targetsToKill.Add([pscustomobject]@{
                            ProcessId    = [int]$p.ProcessId
                            CreationDate = $pCreation
                            OwnerSid     = $queryRes.Sid
                        })
                    }
                } else {
                    $unresolvedRelevant = $true
                    $cleanupLog["processOwnerQueryFailed_PID_$($p.ProcessId)"] = $queryRes.Error
                }
            }

            $killedCount = 0
            foreach ($origTarget in $targetsToKill) {
                $tPid = $origTarget.ProcessId
                # Before Stop-Process, query fresh live process and verify identity match via Test-ProcessIdentityMatchForTermination
                try {
                    $verifyList = @(Get-CimInstance -ClassName Win32_Process -Filter "ProcessId = $tPid" -ErrorAction Stop)
                    if ($verifyList.Count -eq 0) {
                        # Process naturally exited before kill attempt; confirmed gone
                        $cleanupLog["killSkippedAlreadyGone_PID_$tPid"] = "Process confirmed naturally exited before kill attempt"
                        continue
                    }

                    $liveProc = $verifyList[0]
                    $vQuery = Get-ProcessOwnerSidSafely -Process $liveProc
                    $termDecision = Test-ProcessIdentityMatchForTermination `
                        -OriginalRecord $origTarget `
                        -LiveQueryState $vQuery `
                        -ExpectedUserSid $createdUserSid

                    if ($termDecision.CanTerminate -eq $true) {
                        Stop-Process -Id $tPid -Force -ErrorAction Stop
                        $killedCount++
                    } elseif ($termDecision.AlreadyExited -eq $true) {
                        $cleanupLog["killSkippedAlreadyGone_PID_$tPid"] = $termDecision.Reason
                    } else {
                        $cleanupLog["killSkippedIdentityMismatch_PID_$tPid"] = $termDecision.Reason
                        $cleanupAllPassed = $false
                    }
                } catch {
                    $cleanupLog["killError_PID_$tPid"] = "$_"
                    $cleanupAllPassed = $false
                }
            }
            $cleanupLog["processesTerminated"] = $killedCount
            if ($unresolvedRelevant) {
                $cleanupLog["unresolvedRelevantProcessesObserved"] = $true
                $cleanupAllPassed = $false
            }
        } catch {
            $cleanupLog["processTerminationError"] = "$_"
            $cleanupAllPassed = $false
        }

        # Bounded poll to verify zero processes remain owned by created user SID (using same helper)
        $pollSeconds = 0
        $procsRemaining = $true
        while ($procsRemaining -and ($pollSeconds -lt 15)) {
            $remainingOwned = [System.Collections.Generic.List[int]]::new()
            $pollHadUnknown = $false
            try {
                $checkProcs = Get-CimInstance Win32_Process
                foreach ($cp in $checkProcs) {
                    if ($null -ne $cp.CreationDate -and $null -ne $accountCreationTime -and $cp.CreationDate -lt $accountCreationTime) {
                        continue
                    }
                    $qRes = Get-ProcessOwnerSidSafely -Process $cp
                    if ($qRes.AlreadyExited) {
                        continue
                    }
                    if ($qRes.Success) {
                        if ($qRes.Sid -eq $createdUserSid) {
                            $remainingOwned.Add($cp.ProcessId)
                        }
                    } else {
                        $pollHadUnknown = $true
                    }
                }
            } catch {
                $pollHadUnknown = $true
            }

            if ($remainingOwned.Count -eq 0 -and -not $pollHadUnknown) {
                $procsRemaining = $false
            } else {
                Start-Sleep -Seconds 1
                $pollSeconds++
            }
        }
        if ($procsRemaining) {
            $cleanupLog["processesRemainingAfterWait"] = $true
            $cleanupAllPassed = $false
        }

        # 2. Delete local user account and verify
        try {
            Remove-LocalUser -Name $createdUserName -ErrorAction Stop
            $userCheck = Get-LocalUser -Name $createdUserName -ErrorAction SilentlyContinue
            if ($null -eq $userCheck) {
                $cleanupLog["localUserRemoved"] = $true
            } else {
                $cleanupLog["localUserRemoved"] = $false
                $cleanupAllPassed = $false
            }
        } catch {
            $cleanupLog["localUserRemoved"] = $false
            $cleanupLog["localUserRemovalError"] = "$_"
            $cleanupAllPassed = $false
        }

        # 3. Remove user profile and verify both Win32_UserProfile and physical ProfileImagePath
        # Capture ProfileImagePath from expectedProfilePath, HKLM ProfileList, or validated childProof
        $profilePathBefore = $null
        if (-not [string]::IsNullOrWhiteSpace($expectedProfilePath)) {
            $profilePathBefore = $expectedProfilePath
        } else {
            try {
                $pKey = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey("SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList\$createdUserSid")
                if ($null -ne $pKey) {
                    $profilePathBefore = "$($pKey.GetValue('ProfileImagePath'))"
                    $pKey.Close()
                }
            } catch {}
        }
        if ([string]::IsNullOrWhiteSpace($profilePathBefore) -and $null -ne $script:ParentReport["childProof"]) {
            $profilePathBefore = "$($script:ParentReport['childProof'].profileImagePath)"
        }

        try {
            $profile = Get-CimInstance Win32_UserProfile | Where-Object { $_.SID -eq $createdUserSid }
            if ($null -ne $profile) {
                if ([string]::IsNullOrWhiteSpace($profilePathBefore)) {
                    $profilePathBefore = "$($profile.LocalPath)"
                }
                Remove-CimInstance -InputObject $profile -ErrorAction Stop
            }
            $profileCheck = Get-CimInstance Win32_UserProfile | Where-Object { $_.SID -eq $createdUserSid }
            $dirCheck = if (-not [string]::IsNullOrWhiteSpace($profilePathBefore)) { Test-Path -LiteralPath $profilePathBefore } else { $null }

            # Profile cleanup verification:
            # If child started/profile was required but profile path is completely unknown -> fail closed (unverified)
            if ([string]::IsNullOrWhiteSpace($profilePathBefore)) {
                $cleanupLog["userProfileRemoved"] = $false
                $cleanupLog["userProfileRemovalError"] = "Profile directory path was never identified for created SID '$createdUserSid'; cleanup unverified."
                $cleanupAllPassed = $false
            } elseif ($null -eq $profileCheck -and $dirCheck -eq $false) {
                # Valid: CIM object removed AND physical directory absent on disk
                $cleanupLog["userProfileRemoved"] = $true
            } else {
                # Mismatch: CIM still present OR orphan physical directory remains
                $cleanupLog["userProfileRemoved"] = $false
                $cleanupLog["userProfileCimPresent"] = ($null -ne $profileCheck)
                $cleanupLog["userProfileDirPresent"] = $dirCheck
                $cleanupLog["userProfilePath"] = $profilePathBefore
                $cleanupAllPassed = $false
            }
        } catch {
            $cleanupLog["userProfileRemoved"] = $false
            $cleanupLog["userProfileRemovalError"] = "$_"
            $cleanupAllPassed = $false
        }
    }

    # 4. Remove scratch directory and verify
    if ($null -ne $scratchRoot) {
        try {
            if (Test-Path -LiteralPath $scratchRoot) {
                Remove-Item -LiteralPath $scratchRoot -Recurse -Force -ErrorAction Stop
            }
            if (-not (Test-Path -LiteralPath $scratchRoot)) {
                $cleanupLog["scratchRootRemoved"] = $true
            } else {
                $cleanupLog["scratchRootRemoved"] = $false
                $cleanupAllPassed = $false
            }
        } catch {
            $cleanupLog["scratchRootRemoved"] = $false
            $cleanupLog["scratchRootRemovalError"] = "$_"
            $cleanupAllPassed = $false
        }
    }

    $script:ParentReport.cleanup = $cleanupLog

    # Record cleanup status check if account was created
    if ($null -ne $createdUserSid -or $null -ne $scratchRoot) {
        if ($cleanupAllPassed) {
            Record-ParentCheck -Name "controller_cleanup_verified" -Status "pass" -Details "Ephemeral account, profile, processes, and scratch workspace cleanly removed and verified."
        } else {
            Record-ParentCheck -Name "controller_cleanup_verified" -Status "fail" -Details "Cleanup verification failed or incomplete."
            $overallSuccess = $false
        }
    }

    # Save final durable parent report
    $finalVerdict = if ($isSkipped) {
        "skipped"
    } elseif ($overallSuccess) {
        "passed"
    } else {
        "failed"
    }
    Save-ParentReportInternal -FinalSummary $finalVerdict
}

if ($isSkipped) {
    exit 77
} elseif ($overallSuccess) {
    exit 0
} else {
    exit 1
}

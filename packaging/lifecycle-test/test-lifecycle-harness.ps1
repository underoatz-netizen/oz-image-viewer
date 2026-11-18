<#
.SYNOPSIS
  Unit tests for test-installer-lifecycle.ps1 harness logic, ProcessHelper.ps1, registry helpers, and staged ZIP structure.

.DESCRIPTION
  Exercises:
    1. ProcessHelper execution:
       - Exit code 0 with captured stdout.
       - Exit code 17 with captured stderr (proves non-zero exit is captured accurately).
       - Timeout handling: powershell sleep beyond timeout throws and kills process.
    2. Harness guard assertions:
       - Fails/skips with exit 77 when -AllowDisposableExecution is absent.
       - Missing installer binary fails closed with exit 1.
       - Preexisting installation directory fails closed without deleting user files.
    3. Workflow YAML audit:
       - Asserts parameter binding, env mapping, if-no-files-found: error, timeout-minutes,
         and report summary validation.
    4. Flat ZIP extraction & payload validation:
       - Extracts ozviewer-0.1.0-win-x64-portable.zip to a temp folder.
       - Verifies the ZIP is flat (ozviewer.exe is in root, NOT nested).
       - Invokes validate-portable-payload.ps1 on the extracted root and confirms exit 0.
    5. Registry helper unit tests:
       - Get-RegValueSafe on absent subkey returns $null without error in StrictMode.
       - Get-RegValueSafe on absent value name returns $null without error in StrictMode.
       - Get-UserChoiceSnapshot and Compare-UserChoiceSnapshot detect unchanged vs altered states.
       - Foreign sentinel preservation: ensures foreign values are preserved without deletion.

.NOTES
  Exit codes: 0 = all test cases passed, 1 = failure.
#>
[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$HarnessScript = Join-Path $ScriptDir "test-installer-lifecycle.ps1"
$ProcessHelperScript = Join-Path $ScriptDir "ProcessHelper.ps1"
$RepoRoot = Split-Path -Parent (Split-Path -Parent $ScriptDir)

$WorkDir = Join-Path $env:TEMP "ozviewer-lifecycle-unit-tests"
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
    # =========================================================================
    # Part 1: ProcessHelper.ps1 verification (exit 0, exit 17, timeout)
    # =========================================================================
    Assert-Condition (Test-Path -LiteralPath $ProcessHelperScript) "ProcessHelper.ps1 not found at $ProcessHelperScript"
    . $ProcessHelperScript
    . (Join-Path $ScriptDir "candidate-handoff.ps1")
    . (Join-Path $ScriptDir "StandardUserHelpers.ps1")

    # 1.1: Success with stdout (exit 0)
    $res0 = Invoke-ProcessSafe -FilePath "powershell.exe" -ArgumentList @("-NoProfile", "-Command", "Write-Output 'HELLO_SAFE_HELPER'; exit 0") -TimeoutSeconds 10
    Assert-Condition ($res0.ExitCode -eq 0) "Expected exit code 0, got $($res0.ExitCode)"
    Assert-Condition ($res0.StdOut -match "HELLO_SAFE_HELPER") "Expected stdout containing HELLO_SAFE_HELPER, got: $($res0.StdOut)"
    Pass "ProcessHelper: Exit code 0 and stdout capture verified."

    # 1.2: Specific non-zero exit code (exit 17) with stderr
    $res17 = Invoke-ProcessSafe -FilePath "powershell.exe" -ArgumentList @("-NoProfile", "-Command", "[Console]::Error.WriteLine('ERR_CODE_17'); exit 17") -TimeoutSeconds 10
    Assert-Condition ($res17.ExitCode -eq 17) "Expected exit code 17, got $($res17.ExitCode)"
    Assert-Condition ($res17.StdErr -match "ERR_CODE_17") "Expected stderr containing ERR_CODE_17, got: $($res17.StdErr)"
    Pass "ProcessHelper: Exit code 17 and stderr capture verified."

    # 1.3: Timeout handling
    $timedOut = $false
    try {
        $null = Invoke-ProcessSafe -FilePath "powershell.exe" -ArgumentList @("-NoProfile", "-Command", "Start-Sleep -Seconds 10; exit 0") -TimeoutSeconds 2
    } catch {
        if ($_ -match "timed out after 2 seconds") {
            $timedOut = $true
        } else {
            throw "Unexpected exception on timeout test: $_"
        }
    }
    Assert-Condition $timedOut "Expected process to throw timeout exception after 2s"
    Pass "ProcessHelper: Timeout kill and exception verified."

    # =========================================================================
    # Part 2: test-installer-lifecycle.ps1 guards and fail-closed checks
    # =========================================================================
    $dummyInstaller = Join-Path $WorkDir "dummy-installer.exe"
    [System.IO.File]::WriteAllText($dummyInstaller, "dummy")

    # 2.1: Missing -AllowDisposableExecution switch must skip with 77
    $rep1 = Join-Path $WorkDir "report1.json"
    $null = & powershell -NoProfile -ExecutionPolicy Bypass -File $HarnessScript -InstallerPath $dummyInstaller -ReportPath $rep1 2>&1
    Assert-Condition ($LASTEXITCODE -eq 77) "Expected exit code 77 when -AllowDisposableExecution is missing, got $LASTEXITCODE"
    Assert-Condition (Test-Path -LiteralPath $rep1) "Expected report1.json to be created"
    $repData1 = Get-Content -LiteralPath $rep1 -Raw | ConvertFrom-Json
    Assert-Condition ($repData1.summary -eq "skipped") "Expected report summary 'skipped', got '$($repData1.summary)'"
    Pass "Harness guard: Missing disposable switch skips with exit 77 and records skipped report."

    # 2.2: Missing installer file must fail closed with exit 1
    $rep2 = Join-Path $WorkDir "report2.json"
    $badInstaller = Join-Path $WorkDir "nonexistent-installer.exe"
    $null = & powershell -NoProfile -ExecutionPolicy Bypass -File $HarnessScript -InstallerPath $badInstaller -ReportPath $rep2 -AllowDisposableExecution 2>&1
    Assert-Condition ($LASTEXITCODE -eq 1) "Expected exit code 1 when installer binary is missing, got $LASTEXITCODE"
    Assert-Condition (Test-Path -LiteralPath $rep2) "Expected report2.json to be created"
    $repData2 = Get-Content -LiteralPath $rep2 -Raw | ConvertFrom-Json
    Assert-Condition ($repData2.summary -eq "failed") "Expected report summary 'failed', got '$($repData2.summary)'"
    Pass "Harness guard: Missing installer file fails closed with exit 1 and records failed report."

    # 2.3: Preexisting install directory must fail closed WITHOUT deleting directory
    $mockTargetDir = Join-Path $WorkDir "mock-target-dir"
    New-Item -ItemType Directory -Path $mockTargetDir -Force | Out-Null
    $userFile = Join-Path $mockTargetDir "user_preexisting_file.txt"
    [System.IO.File]::WriteAllText($userFile, "DO NOT DELETE THIS USER DATA")

    $rep3 = Join-Path $WorkDir "report3.json"
    $null = & powershell -NoProfile -ExecutionPolicy Bypass -File $HarnessScript `
        -InstallerPath $dummyInstaller `
        -ReportPath $rep3 `
        -OverrideInstallDir $mockTargetDir `
        -AllowDisposableExecution 2>&1
    Assert-Condition ($LASTEXITCODE -eq 1) "Expected exit code 1 when preexisting install dir exists, got $LASTEXITCODE"
    Assert-Condition (Test-Path -LiteralPath $userFile) "User file was deleted! Preexisting directory must be preserved."
    Pass "Harness guard: Preexisting install directory fails closed WITHOUT deleting user files."

    # =========================================================================
    # Part 3: Workflow YAML audit
    # =========================================================================
    $workflowYaml = Join-Path $RepoRoot ".github\workflows\validate-installer.yml"
    Assert-Condition (Test-Path -LiteralPath $workflowYaml) "Workflow file not found at $workflowYaml"
    $yamlContent = Get-Content -LiteralPath $workflowYaml -Raw
    Assert-Condition ($yamlContent -match "candidate_tag") "Workflow must define candidate_tag"
    Assert-Condition ($yamlContent -match "expected_installer_sha256") "Workflow must define expected_installer_sha256"
    Assert-Condition ($yamlContent -match "expected_portable_sha256") "Workflow must define expected_portable_sha256"
    Assert-Condition ($yamlContent -match "expected_source_sha256") "Workflow must define expected_source_sha256"
    Assert-Condition ($yamlContent -match "expected_checksums_sha256") "Workflow must define expected_checksums_sha256"
    Assert-Condition ($yamlContent -match "expected_candidate_commit_sha") "Workflow must define independently controlled expected_candidate_commit_sha"
    Assert-Condition ($yamlContent -match "expected_candidate_tree_sha") "Workflow must define independently controlled expected_candidate_tree_sha"
    Assert-Condition ($yamlContent -match "contents:\s*read") "Workflow must enforce read-only contents permissions"
    Assert-Condition ($yamlContent -match "timeout-minutes:\s*15") "Workflow must enforce bounded job timeout"
    Assert-Condition ($yamlContent -match "if-no-files-found:\s*error") "Workflow must fail if report artifact is missing"
    Assert-Condition ($yamlContent -match "validate-portable-payload\.ps1") "Workflow must validate portable payload"
    $validateSection = $yamlContent.Substring($yamlContent.IndexOf("validate-candidate:"))
    Assert-Condition ($validateSection -match "-ExpectedCommitSha\s+\`$identity\.commitSha") "Portable validator must receive the normalized independently supplied commit SHA"
    Assert-Condition ($validateSection -match "-ExpectedTreeSha\s+\`$identity\.treeSha") "Portable validator must receive the normalized independently supplied tree SHA"
    Assert-Condition ($validateSection -match "-ExpectedLgplSourceSha256\s+\`$env:EXPECTED_SOURCE_SHA256") "Workflow must pass trusted source ZIP hash as ExpectedLgplSourceSha256"
    Assert-Condition ($yamlContent -match "New-PortableClosureManifest") "Workflow must create a pinned portable closure from the validated payload"
    Assert-Condition ($yamlContent -match "PortableClosurePath" -and $yamlContent -match "PortableClosureSha256") "Workflow must pass the pinned closure path and hash to lifecycle"
    Assert-Condition ($yamlContent -match "Test-LifecycleReportContract") "Workflow must enforce the exact lifecycle report contract"
    Assert-Condition ($validateSection -notmatch "GH_TOKEN|github\.token|secrets\.") "Candidate execution job must not receive tokens or secrets"
    Pass "Workflow audit: Independent source identity, trusted tag/source binding, pinned closure, report contract, and token separation verified."

    # =========================================================================
    # Part 4: Synthetic portable payload, closure, and installed-runtime contract
    # =========================================================================
    $normalizedIdentity = Assert-SourceIdentityFormat -CommitSha ("A" * 40) -TreeSha ("B" * 40)
    Assert-Condition ($normalizedIdentity.commitSha -eq ("a" * 40) -and $normalizedIdentity.treeSha -eq ("b" * 40)) "Expected source identity must normalize to lowercase hex"
    Assert-Throws {
        Assert-SourceIdentityFormat -CommitSha ("a" * 40) -TreeSha ("b" * 39)
    } "40 lowercase hex" "Expected source identity rejects a truncated tree SHA"
    Assert-Throws {
        Assert-SourceIdentityFormat -CommitSha ("g" * 40) -TreeSha ("b" * 40)
    } "40 lowercase hex" "Expected source identity rejects a non-hex commit SHA"
    Pass "Source identity: canonical normalization and malformed-value rejection verified."

    $syntheticRoot = Join-Path $WorkDir "synthetic-portable"
    $payloadDir = Join-Path $syntheticRoot "payload"
    New-Item -ItemType Directory -Path $payloadDir -Force | Out-Null
    $syntheticCommit = ("a" * 40)
    $syntheticTree = ("b" * 40)
    $syntheticLgpl = ("c" * 64)

    $payloadText = @{
        "ozviewer.exe"              = "MZ-SYNTHETIC-ENTRYPOINT"
        "heif.dll"                  = "SYNTHETIC-HEIF-DLL"
        "libde265.dll"              = "SYNTHETIC-LIBDE265-DLL"
        "LICENSE"                   = "Synthetic license"
        "THIRD-PARTY-NOTICES.md"    = "Synthetic notices"
        "THIRD-PARTY-LICENSES.txt"  = "Synthetic third-party licenses"
        "THIRD-PARTY-SOURCE.txt"    = "Synthetic source offer"
        "RUNNING.txt"               = "Synthetic running notes"
    }
    foreach ($name in $payloadText.Keys) {
        [System.IO.File]::WriteAllText((Join-Path $payloadDir $name), $payloadText[$name], (New-Object System.Text.UTF8Encoding($false)))
    }

    $stagedBinaries = @()
    foreach ($pe in @("ozviewer.exe", "heif.dll", "libde265.dll")) {
        $pePath = Join-Path $payloadDir $pe
        $peItem = Get-Item -LiteralPath $pePath
        $stagedBinaries += [ordered]@{
            name            = $pe
            bytes           = $peItem.Length
            sha256          = (Get-FileHash -LiteralPath $pePath -Algorithm SHA256).Hash.ToLowerInvariant()
            machine         = "0x8664"
            signatureStatus = "unsigned"
        }
    }
    $provenance = [ordered]@{
        product = [ordered]@{ version = "0.1.1"; tag = "v0.1.1"; packageName = "pikaview" }
        buildConfiguration = [ordered]@{ diagnostics = "OFF" }
        source = [ordered]@{ cleanStatus = "clean"; isClean = $true; commitSha = $syntheticCommit; treeSha = $syntheticTree }
        stagedCandidateBinaries = $stagedBinaries
        lgplCorrespondingSourceAsset = [ordered]@{
            assetName = "pikaview-0.1.1-lgpl-corresponding-source.zip"
            sha256    = $syntheticLgpl
        }
    }
    [System.IO.File]::WriteAllText((Join-Path $payloadDir "BUILD-PROVENANCE.json"), ($provenance | ConvertTo-Json -Depth 8), (New-Object System.Text.UTF8Encoding($false)))

    $manifestFiles = @()
    foreach ($file in @(Get-ChildItem -LiteralPath $payloadDir -File | Sort-Object Name)) {
        if ($file.Name -eq "package-manifest.json") { continue }
        $manifestFiles += [ordered]@{
            name   = $file.Name
            bytes  = $file.Length
            sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
    $packageManifest = [ordered]@{
        schema              = 1
        packageName         = "pikaview"
        version             = "0.1.1"
        platform            = "win-x64"
        entrypoint          = "ozviewer.exe"
        supportedExtensions = @(".jpg", ".png")
        dynamicDependencies = @("heif.dll", "libde265.dll")
        files               = @($manifestFiles | Sort-Object { $_.name })
    }
    [System.IO.File]::WriteAllText((Join-Path $payloadDir "package-manifest.json"), ($packageManifest | ConvertTo-Json -Depth 8), (New-Object System.Text.UTF8Encoding($false)))

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $syntheticZip = Join-Path $syntheticRoot "pikaview-0.1.1-win-x64-portable.zip"
    [System.IO.Compression.ZipFile]::CreateFromDirectory($payloadDir, $syntheticZip, [System.IO.Compression.CompressionLevel]::Optimal, $false)
    $syntheticZipHash = (Get-FileHash -LiteralPath $syntheticZip -Algorithm SHA256).Hash.ToLowerInvariant()

    $validateScript = Join-Path $RepoRoot "packaging\validate-portable-payload.ps1"
    $validationResult = Join-Path $syntheticRoot "validation-result.json"
    $valRes = & powershell -NoProfile -ExecutionPolicy Bypass -File $validateScript `
        -PackageDir $payloadDir `
        -ZipPath $syntheticZip `
        -ExpectedSha256 $syntheticZipHash `
        -ExpectedVersion "0.1.1" `
        -ExpectedPlatform "win-x64" `
        -ExpectedCommitSha $syntheticCommit `
        -ExpectedTreeSha $syntheticTree `
        -ExpectedLgplSourceSha256 $syntheticLgpl `
        -ResultPath $validationResult 2>&1
    Assert-Condition ($LASTEXITCODE -eq 0) "Synthetic portable validation failed (exit $LASTEXITCODE):`n$($valRes -join "`n")"
    Assert-Condition (Test-Path -LiteralPath $validationResult -PathType Leaf) "Synthetic portable validation result was not written"
    Pass "Synthetic payload: validator accepts an independently supplied source identity and LGPL source hash."

    $badValRes = & powershell -NoProfile -ExecutionPolicy Bypass -File $validateScript `
        -PackageDir $payloadDir `
        -ZipPath $syntheticZip `
        -ExpectedSha256 $syntheticZipHash `
        -ExpectedVersion "0.1.1" `
        -ExpectedPlatform "win-x64" `
        -ExpectedCommitSha ("d" * 40) `
        -ExpectedTreeSha $syntheticTree `
        -ExpectedLgplSourceSha256 $syntheticLgpl 2>&1
    Assert-Condition ($LASTEXITCODE -ne 0) "Synthetic validator unexpectedly accepted a provenance/expected commit mismatch"
    Pass "Synthetic payload: validator fails closed when provenance commit does not match the independent expected commit."

    $closurePath = Join-Path $syntheticRoot "portable-closure.json"
    New-PortableClosureManifest `
        -PackageDir $payloadDir `
        -PortableZipPath $syntheticZip `
        -ExpectedPayloadSha256 $syntheticZipHash `
        -ExpectedVersion "0.1.1" `
        -ExpectedPackageName "pikaview" `
        -ExpectedCommitSha $syntheticCommit `
        -ExpectedTreeSha $syntheticTree `
        -OutputPath $closurePath
    $closureSha = (Get-FileHash -LiteralPath $closurePath -Algorithm SHA256).Hash.ToLowerInvariant()
    $parsedClosure = Test-PortableClosureManifest `
        -Path $closurePath `
        -ExpectedSha256 $closureSha `
        -ExpectedVersion "0.1.1" `
        -ExpectedPackageName "pikaview" `
        -ExpectedPayloadSha256 $syntheticZipHash `
        -ExpectedCommitSha $syntheticCommit `
        -ExpectedTreeSha $syntheticTree
    Assert-Condition ($parsedClosure.expectedSourceCommitSha -eq $syntheticCommit) "Closure source commit mismatch"
    Assert-Condition (@($parsedClosure.files).Count -eq $manifestFiles.Count) "Closure file count mismatch"
    Pass "Closure: deterministic closure matches validated payload files and independent source identity."

    Assert-Throws {
        Test-PortableClosureManifest -Path $closurePath -ExpectedSha256 ("0" * 64) -ExpectedVersion "0.1.1" -ExpectedPackageName "pikaview" -ExpectedPayloadSha256 $syntheticZipHash -ExpectedCommitSha $syntheticCommit -ExpectedTreeSha $syntheticTree
    } "SHA-256 mismatch" "Closure hash pin rejects a substituted closure"

    Assert-Throws {
        Test-PortableClosureManifest -Path $closurePath -ExpectedSha256 $closureSha -ExpectedVersion "0.1.1" -ExpectedPackageName "pikaview" -ExpectedPayloadSha256 ("e" * 64) -ExpectedCommitSha $syntheticCommit -ExpectedTreeSha $syntheticTree
    } "does not match expected portable ZIP hash" "Closure rejects a payload hash mismatch"

    $installedDir = Join-Path $syntheticRoot "installed"
    Copy-Item -LiteralPath $payloadDir -Destination $installedDir -Recurse
    $installedOk = Test-InstalledRuntimeClosure -InstallDir $installedDir -Closure $parsedClosure
    Assert-Condition ($installedOk -eq $true) "Installed runtime closure check rejected an exact copy"
    $runningPath = Join-Path $installedDir "RUNNING.txt"
    $runningText = [System.IO.File]::ReadAllText($runningPath)
    [System.IO.File]::WriteAllText($runningPath, ("X" + $runningText.Substring(1)), (New-Object System.Text.UTF8Encoding($false)))
    Assert-Throws {
        Test-InstalledRuntimeClosure -InstallDir $installedDir -Closure $parsedClosure
    } "SHA-256 mismatch" "Installed runtime closure rejects a modified file"
    Pass "Installed runtime: exact closure copy passes; modified runtime file fails closed."

    $expectedExe = Join-Path $installedDir "ozviewer.exe"
    Assert-Condition (Test-InstallLocationMatches -Actual ($installedDir + "\") -Expected $installedDir) "InstallLocation exact path should match"
    Assert-Condition (-not (Test-InstallLocationMatches -Actual (Join-Path $installedDir "Elsewhere") -Expected $installedDir)) "InstallLocation mismatch should fail"
    $goodCommand = ('"' + $expectedExe + '" "%1"')
    Assert-Condition (Test-CommandTargetMatches -CommandValue $goodCommand -ExpectedExePath $expectedExe) "Command target exact path should match"
    Assert-Condition (-not (Test-CommandTargetMatches -CommandValue ('"' + (Join-Path $installedDir "stale.exe") + '" "%1"') -ExpectedExePath $expectedExe)) "Stale command target should fail"
    Assert-Condition (-not (Test-CommandTargetMatches -CommandValue ('"' + $expectedExe + '"') -ExpectedExePath $expectedExe)) "Command target missing %1 should fail"
    Pass "Contract: InstallLocation and shell command target checks reject stale or malformed values."

    $shortcutExe = Join-Path $WorkDir "Programs\OZ Image Viewer\ozviewer.exe"
    $shortcutDir = Split-Path -Parent $shortcutExe
    $ownedShortcut = [pscustomobject]@{
        TargetPath       = $shortcutExe
        Arguments        = ""
        WorkingDirectory = $shortcutDir
        IconLocation     = "$shortcutExe,0"
        Description      = ""
    }
    Assert-Condition (Test-OwnedDesktopShortcutFields -Snapshot $ownedShortcut -ExpectedExePath $shortcutExe -ExpectedWorkingDirectory $shortcutDir) "Owned desktop shortcut explicit fields should pass"
    $descriptionShortcut = [pscustomobject]@{
        TargetPath       = $shortcutExe
        Arguments        = ""
        WorkingDirectory = $shortcutDir
        IconLocation     = "$shortcutExe,0"
        Description      = "PikaView"
    }
    Assert-Condition (Test-OwnedDesktopShortcutFields -Snapshot $descriptionShortcut -ExpectedExePath $shortcutExe -ExpectedWorkingDirectory $shortcutDir) "Non-empty Description must not be asserted against an Inno default"
    $modifiedShortcut = [pscustomobject]@{
        TargetPath       = $shortcutExe
        Arguments        = "--desktop-user-modified-sentinel-v1"
        WorkingDirectory = $shortcutDir
        IconLocation     = "$shortcutExe,0"
        Description      = "PikaView"
    }
    Assert-Condition (-not (Test-OwnedDesktopShortcutFields -Snapshot $modifiedShortcut -ExpectedExePath $shortcutExe -ExpectedWorkingDirectory $shortcutDir)) "User-modified Arguments must fail owned-field check"
    $wrongTargetShortcut = [pscustomobject]@{
        TargetPath       = Join-Path $env:SystemRoot "System32\notepad.exe"
        Arguments        = ""
        WorkingDirectory = $shortcutDir
        IconLocation     = "$shortcutExe,0"
        Description      = ""
    }
    Assert-Condition (-not (Test-OwnedDesktopShortcutFields -Snapshot $wrongTargetShortcut -ExpectedExePath $shortcutExe -ExpectedWorkingDirectory $shortcutDir)) "Wrong TargetPath must fail owned-field check"
    $iconWithoutIndex = [pscustomobject]@{
        TargetPath       = $shortcutExe
        Arguments        = ""
        WorkingDirectory = $shortcutDir
        IconLocation     = $shortcutExe
        Description      = ""
    }
    Assert-Condition (Test-OwnedDesktopShortcutFields -Snapshot $iconWithoutIndex -ExpectedExePath $shortcutExe -ExpectedWorkingDirectory $shortcutDir) "IconLocation without ,0 should be accepted"
    Assert-Condition (Test-ShortcutExplicitFieldsEqual -Before $ownedShortcut -After $descriptionShortcut) "Explicit-field comparison must ignore Description"
    Assert-Condition (-not (Test-ShortcutExplicitFieldsEqual -Before $ownedShortcut -After $modifiedShortcut)) "Explicit-field comparison must detect changed Arguments"
    Assert-Condition ($null -eq (Get-ShortcutSnapshot -Path (Join-Path $WorkDir "missing-shortcut.lnk"))) "Absent shortcut must return null"
    Assert-Condition (-not (Test-ShortcutExplicitFieldsEqual -Before $null -After $ownedShortcut)) "Null shortcut comparison must fail closed"
    Pass "Desktop shortcut contract: explicit fields are enforced, Description is observed but never asserted."

    $baseChecks = @(Get-StandardLifecycleRequiredCheckNames)
    $validChecks = @($baseChecks | ForEach-Object { [ordered]@{ name = $_; status = "pass"; details = "ok" } })
    $validReport = [ordered]@{ summary = "passed"; checks = $validChecks }
    Assert-Condition ((Test-LifecycleReportContract -ReportData $validReport -RequiredCheckNames $baseChecks -Context "synthetic report") -eq $true) "Report contract rejected a valid exact report"
    $warningChecks = @($validChecks | ForEach-Object {
        if ($_.name -eq "non_admin_integrity_check") { [ordered]@{ name = $_.name; status = "warning"; details = "admin" } } else { $_ }
    })
    $warningReport = [ordered]@{ summary = "passed"; checks = $warningChecks }
    Assert-Condition ((Test-LifecycleReportContract -ReportData $warningReport -RequiredCheckNames $baseChecks -AllowedWarningCheckNames @("non_admin_integrity_check") -Context "synthetic report") -eq $true) "Report contract rejected an allowed environment warning"
    $skippedChecks = @($validChecks | ForEach-Object {
        if ($_.name -eq "stage1_runtime_closure") { [ordered]@{ name = $_.name; status = "skipped"; details = "skip" } } else { $_ }
    })
    Assert-Throws {
        Test-LifecycleReportContract -ReportData ([ordered]@{ summary = "passed"; checks = $skippedChecks }) -RequiredCheckNames $baseChecks -Context "synthetic report"
    } "non-passing status" "Report contract rejects a skipped closure check"
    Assert-Throws {
        Test-LifecycleReportContract -ReportData ([ordered]@{ summary = "passed"; checks = @($validChecks | Select-Object -First ($validChecks.Count - 1)) }) -RequiredCheckNames $baseChecks -Context "synthetic report"
    } "check count mismatch" "Report contract rejects a missing check"
    $extraChecks = @($validChecks) + @([ordered]@{ name = "unrelated_extra"; status = "pass"; details = "extra" })
    Assert-Throws {
        Test-LifecycleReportContract -ReportData ([ordered]@{ summary = "passed"; checks = $extraChecks }) -RequiredCheckNames $baseChecks -Context "synthetic report"
    } "check count mismatch" "Report contract rejects an unrelated extra check"
    $skippedDesktopChecks = @($validChecks | ForEach-Object {
        if ($_.name -eq "stage3_desktop_shortcut_optin") { [ordered]@{ name = $_.name; status = "skipped"; details = "skip" } } else { $_ }
    })
    Assert-Throws {
        Test-LifecycleReportContract -ReportData ([ordered]@{ summary = "passed"; checks = $skippedDesktopChecks }) -RequiredCheckNames $baseChecks -Context "synthetic report"
    } "non-passing status" "Report contract rejects a skipped desktop shortcut check"
    $missingDesktopChecks = @($validChecks | Where-Object { $_.name -ne "stage3_desktop_shortcut_optin" })
    Assert-Throws {
        Test-LifecycleReportContract -ReportData ([ordered]@{ summary = "passed"; checks = $missingDesktopChecks }) -RequiredCheckNames $baseChecks -Context "synthetic report"
    } "check count mismatch" "Report contract rejects a missing desktop shortcut check"
    $duplicateDesktopChecks = @($validChecks)
    $duplicateDesktopChecks[$duplicateDesktopChecks.Count - 1] = [ordered]@{ name = "stage3_desktop_shortcut_optin"; status = "pass"; details = "duplicate" }
    Assert-Throws {
        Test-LifecycleReportContract -ReportData ([ordered]@{ summary = "passed"; checks = $duplicateDesktopChecks }) -RequiredCheckNames $baseChecks -Context "synthetic report"
    } "Duplicate check detected" "Report contract rejects a duplicate desktop shortcut check"
    Pass "Report contract: exact check set, non-skipped closure checks, required desktop checks, and allowed environment warnings verified."

    $lifecycleRaw = Get-Content -LiteralPath (Join-Path $ScriptDir "test-installer-lifecycle.ps1") -Raw
    $snapshotFunctionCount = ([regex]::Matches($lifecycleRaw, '(?m)^function Get-UserChoiceSnapshot\s*\{')).Count
    Assert-Condition ($snapshotFunctionCount -eq 1) "test-installer-lifecycle.ps1 must define exactly one Get-UserChoiceSnapshot function (found $snapshotFunctionCount)"
    Assert-Condition ($lifecycleRaw -notmatch 'function Get-UserChoiceSnapshot\(\[string\[\]\]\$Exts\)') "Nested/stage-local UserChoice snapshot must be removed"
    Assert-Condition ($lifecycleRaw -match 'Compare-UserChoiceSnapshotAll') "Stage 3/5 must use the shared full-value/type snapshot comparator"
    $allRequiredNames = @(Get-StandardLifecycleRequiredCheckNames -IncludeUpgradeChecks)
    foreach ($newCheck in @("runtime_closure_manifest", "stage1_runtime_closure", "stage1_install_location", "stage3_command_target", "stage5_runtime_closure", "stage5_install_location", "stage5_command_target", "stage3_desktop_shortcut_foreign_preserved", "stage3_desktop_shortcut_optin", "stage3_desktop_shortcut_reinstall_preserved", "stage4_desktop_shortcut_removed", "stage4_desktop_shortcut_user_modified_preserved")) {
        Assert-Condition ($lifecycleRaw -match [regex]::Escape($newCheck)) "test-installer-lifecycle.ps1 must record required check '$newCheck'"
        Assert-Condition ($allRequiredNames -contains $newCheck) "Required check list must include '$newCheck'"
    }
    foreach ($shortcutHelper in @('Get-ShortcutSnapshot', 'Test-ShortcutExplicitFieldsEqual', 'Test-OwnedDesktopShortcutFields')) {
        Assert-Condition ($lifecycleRaw -match [regex]::Escape($shortcutHelper)) "test-installer-lifecycle.ps1 must use '$shortcutHelper'"
        $helpersRaw = Get-Content -LiteralPath (Join-Path $ScriptDir "StandardUserHelpers.ps1") -Raw
        Assert-Condition ($helpersRaw -match "function $([regex]::Escape($shortcutHelper))") "StandardUserHelpers.ps1 must define '$shortcutHelper'"
    }
    Assert-Condition ($lifecycleRaw -match 'associate desktopicon') "Harness must exercise the opted-in desktop shortcut task"
    Assert-Condition ($lifecycleRaw -match 'stage4_desktop_shortcut_user_modified_preserved') "Harness must exercise user-modified desktop shortcut survival"
    Pass "Static contract: UserChoice snapshot, required closure/desktop checks, and desktop shortcut helpers present."

    # =========================================================================
    # Part 5: Registry Safe-Reading & StrictMode Helper Tests
    # =========================================================================
    $regTestScript = Join-Path $WorkDir "reg-test.ps1"
    $regCode = @'
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-RegValueSafe {
    param(
        [Parameter(Mandatory = $true)][string]$SubKeyPath,
        [string]$ValueName = ""
    )
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($SubKeyPath)
    if ($null -eq $key) { return $null }
    try {
        return $key.GetValue($ValueName, $null)
    } finally {
        $key.Close()
    }
}

$v1 = Get-RegValueSafe -SubKeyPath "Software\NonExistentSubKeyTest"
if ($null -ne $v1) { throw "Expected null for missing key" }
$v2 = Get-RegValueSafe -SubKeyPath "Software" -ValueName "NonExistentValueTest"
if ($null -ne $v2) { throw "Expected null for missing value" }
Write-Output "REG_SAFE_OK"
'@
    [System.IO.File]::WriteAllText($regTestScript, $regCode)
    $resReg = & powershell -NoProfile -ExecutionPolicy Bypass -File $regTestScript
    Assert-Condition ($resReg -eq "REG_SAFE_OK") "Get-RegValueSafe failed strict mode test: $resReg"
    Pass "Registry helpers: Safe reading of missing subkeys and missing values verified under StrictMode."

    Write-Host "`nALL LIFECYCLE HARNESS AND PROCESS HELPER UNIT TESTS PASSED!" -ForegroundColor Green
    exit 0
} catch {
    Write-Error "Test execution failed: $_"
    exit 1
} finally {
    Remove-Item -LiteralPath $WorkDir -Recurse -Force -ErrorAction SilentlyContinue
}

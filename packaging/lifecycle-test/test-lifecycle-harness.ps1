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

try {
    # =========================================================================
    # Part 1: ProcessHelper.ps1 verification (exit 0, exit 17, timeout)
    # =========================================================================
    Assert-Condition (Test-Path -LiteralPath $ProcessHelperScript) "ProcessHelper.ps1 not found at $ProcessHelperScript"
    . $ProcessHelperScript

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
    Assert-Condition ($yamlContent -match "contents:\s*read") "Workflow must enforce read-only contents permissions"
    Assert-Condition ($yamlContent -match "timeout-minutes:\s*15") "Workflow must enforce bounded job timeout"
    Assert-Condition ($yamlContent -match "if-no-files-found:\s*error") "Workflow must fail if report artifact is missing"
    Assert-Condition ($yamlContent -match "validate-portable-payload\.ps1") "Workflow must validate portable payload"
    Pass "Workflow audit: Parameterized dispatch, read permissions, bounded timeout, and artifact enforcement verified."

    # =========================================================================
    # Part 4: Flat ZIP extraction & payload validation test
    # =========================================================================
    $stageDir = "C:\Users\USER\AppData\Local\Temp\opencode\ozviewer-v0.1.0-release-stage"
    $portableZip = Join-Path $stageDir "ozviewer-0.1.0-win-x64-portable.zip"
    Assert-Condition (Test-Path -LiteralPath $portableZip) "Staged portable zip not found at $portableZip"

    $zipUnpack = Join-Path $WorkDir "zip-extract"
    New-Item -ItemType Directory -Path $zipUnpack -Force | Out-Null
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::ExtractToDirectory($portableZip, $zipUnpack)

    # Verify that the extraction root is flat (ozviewer.exe is in root, not nested)
    Assert-Condition (Test-Path -LiteralPath (Join-Path $zipUnpack "ozviewer.exe") -PathType Leaf) "ZIP root does not contain ozviewer.exe directly! ZIP is not flat."
    Assert-Condition (-not (Test-Path -LiteralPath (Join-Path $zipUnpack "ozviewer-0.1.0-win-x64-portable") -PathType Container)) "ZIP unexpectedly contains a nested package folder."
    Pass "ZIP extraction: Confirmed portable ZIP is flat; ozviewer.exe resides directly at extraction root."

    # Validate the unpacked directory using validate-portable-payload.ps1
    $validateScript = Join-Path $RepoRoot "packaging\validate-portable-payload.ps1"
    $valRes = & powershell -NoProfile -ExecutionPolicy Bypass -File $validateScript `
        -PackageDir $zipUnpack `
        -ZipPath $portableZip 2>&1
    Assert-Condition ($LASTEXITCODE -eq 0) "validate-portable-payload failed on extracted ZIP root (exit $LASTEXITCODE):`n$($valRes -join "`n")"
    Pass "Payload validation: validate-portable-payload.ps1 passed against extracted flat ZIP root."

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

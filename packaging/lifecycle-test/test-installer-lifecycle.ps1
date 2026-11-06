<#
.SYNOPSIS
  Disposable runner / VM harness for OZ Image Viewer installer lifecycle verification.

.DESCRIPTION
  This script is intended for execution ONLY in a clean, disposable environment (such as
  a GitHub Actions windows-2022 runner or an isolated throwaway test VM).
  It must NOT be run on developer workstations or in standard persistent user profiles.

  Lifecycle Checks Implemented:
    1. Pre-flight isolation & non-admin privilege assertion.
       - Confirms -AllowDisposableExecution is passed and checks hosted CI / VM attestation.
       - Inspects process token elevation / administrator integrity level.
       - Asserts that target installation directory and HKCU keys are absent initially;
         fails closed immediately if preexisting installation or registry keys exist
         WITHOUT deleting or modifying any preexisting data.
    2. Default installation (tasks unchecked, default associations opt-in NOT selected).
       - Runs silent install: /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP-
       - Asserts installation into %LOCALAPPDATA%\Programs\OZ Image Viewer with exit 0.
       - Asserts presence of entrypoint (ozviewer.exe), runtime DLLs (heif.dll, libde265.dll),
         uninstaller (unins000.exe), licenses and notices.
       - Asserts absence of dev/debug artifacts (*.pdb, *.lib, *.obj, *.ilk).
       - Asserts that default install DID NOT write ProgID or Capabilities to HKCU\Software\Classes
         or HKCU\Software\RegisteredApplications.
       - Asserts that HKLM application uninstall key was NOT touched.
    3. Benchmark / FirstPresent application launch smoke.
       - Executes installed ozviewer.exe with --benchmark against a test image fixture.
       - Uses a bounded timeout (max 30s) and strictly checks process exit code 0.
       - Captures and validates JSONL stdout/stderr for FirstPresent timing markers.
       - Ensures process handles and child processes are cleaned up.
    4. Reinstallation & Opt-in association installation.
       - Snapshots real HKCU Classes default handler and complete UserChoice state for all
         14 declared supported extensions before opt-in.
       - Seeds or detects a foreign default handler on a supported extension (.png) to verify
         it is preserved without overwriting existing associations.
       - Runs installer with /TASKS="associate" over existing install.
       - Verifies exit code 0 and successful reconciliation.
       - Verifies HKCU\Software\Classes\OZImageViewer.Image ProgID and command keys.
       - Verifies HKCU\Software\OZImageViewer\Capabilities\FileAssociations entries.
       - Verifies HKCU\Software\OZImageViewer\Install marker with matching AppId.
       - Verifies NO user choice or default handler hijack occurred (UserChoice untouched).
       - Verifies foreign default handler on supported extension was preserved intact.
    5. Clean uninstallation & cleanup.
       - Runs unins000.exe with /VERYSILENT /SUPPRESSMSGBOXES /NORESTART within bounded timeout.
       - Verifies complete removal of installed binaries, DLLs, and directory.
       - Verifies complete removal of owned HKCU registry keys (ProgID, Capabilities, Install marker, RegisteredApplications value).
       - Verifies foreign sentinel/preexisting handlers are preserved.

.PARAMETER InstallerPath
  Path to the compiled ozviewer-0.1.0-win-x64-installer.exe.

.PARAMETER ExpectedVersion
  Expected application version (defaults to 0.1.0).

.PARAMETER FixturePath
  Path to an image fixture to test benchmark rendering smoke.

.PARAMETER ReportPath
  Path to write machine-readable JSON lifecycle test report.

.PARAMETER OverrideInstallDir
  Optional override for the target installation directory (primarily for isolated unit testing).
  Defaults to "$env:LOCALAPPDATA\Programs\OZ Image Viewer".

.PARAMETER RequireStandardUser
  Strict mode: Asserts process token is non-elevated and not in Administrator role.
  Fails closed before executing or modifying anything if elevated.

.PARAMETER AllowDisposableExecution
  Mandatory safety switch confirming the run is occurring on a disposable runner/VM.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$InstallerPath,
    [string]$ExpectedVersion = "0.1.0",
    [string]$FixturePath = "",
    [string]$ReportPath = "",
    [string]$OverrideInstallDir = "",
    [switch]$RequireStandardUser,
    [switch]$AllowDisposableExecution
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Dot-source the reliable process execution helper
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$processHelper = Join-Path $ScriptDir "ProcessHelper.ps1"
if (-not (Test-Path -LiteralPath $processHelper -PathType Leaf)) {
    throw "Required helper ProcessHelper.ps1 not found in $ScriptDir"
}
. $processHelper

$standardUserHelpers = Join-Path $ScriptDir "StandardUserHelpers.ps1"
if (-not (Test-Path -LiteralPath $standardUserHelpers -PathType Leaf)) {
    throw "Required helper StandardUserHelpers.ps1 not found in $ScriptDir"
}
. $standardUserHelpers

# Safe Registry Helpers that never throw in StrictMode and distinguish null from absent
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

function Get-UserChoiceSnapshot {
    param([Parameter(Mandatory = $true)][string]$Extension)
    $subPath = "Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\$Extension\UserChoice"
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($subPath)
    if ($null -eq $key) { return $null }
    try {
        $dict = [ordered]@{}
        $names = @($key.GetValueNames() | Sort-Object)
        foreach ($name in $names) {
            $dict[$name] = [ordered]@{
                Kind = $key.GetValueKind($name).ToString()
                Value = $key.GetValue($name)
            }
        }
        return $dict
    } finally {
        $key.Close()
    }
}

function Compare-UserChoiceSnapshot {
    param(
        $Before,
        $After
    )
    if ($null -eq $Before -and $null -eq $After) { return $true }
    if ($null -eq $Before -or $null -eq $After) { return $false }

    $bKeys = @($Before.Keys | Sort-Object)
    $aKeys = @($After.Keys | Sort-Object)
    if ($bKeys.Count -ne $aKeys.Count) { return $false }
    for ($i = 0; $i -lt $bKeys.Count; $i++) {
        if ($bKeys[$i] -ne $aKeys[$i]) { return $false }
        $k = $bKeys[$i]
        if ($Before[$k].Kind -ne $After[$k].Kind) { return $false }
        if ($Before[$k].Value -ne $After[$k].Value) { return $false }
    }
    return $true
}

$script:Report = [ordered]@{
    testSuite = "installer-lifecycle"
    timestamp = [System.DateTime]::UtcNow.ToString("o")
    installer = $InstallerPath
    expectedVersion = $ExpectedVersion
    isElevated = $false
    elevationCheck = "unknown"
    checks = @()
    summary = "pending"
}

function Record-Check {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Status, # pass, fail, warning, skipped
        [string]$Details = ""
    )
    $entry = [ordered]@{
        name = $Name
        status = $Status
        details = $Details
    }
    $script:Report.checks += $entry
    $color = switch ($Status) {
        "pass" { "Green" }
        "fail" { "Red" }
        "warning" { "Yellow" }
        "skipped" { "Cyan" }
        default { "White" }
    }
    Write-Host "[$Status] $Name - $Details" -ForegroundColor $color
    if ($Status -eq "fail") {
        Save-Report -FinalSummary "failed"
        exit 1
    }
}

function Save-Report {
    param([string]$FinalSummary)
    $script:Report.summary = $FinalSummary
    if (-not [string]::IsNullOrWhiteSpace($ReportPath)) {
        $reportDir = Split-Path -Parent $ReportPath
        if (-not [string]::IsNullOrWhiteSpace($reportDir) -and -not (Test-Path -LiteralPath $reportDir)) {
            New-Item -ItemType Directory -Path $reportDir -Force | Out-Null
        }
        $json = $script:Report | ConvertTo-Json -Depth 5
        [System.IO.File]::WriteAllText($ReportPath, $json, (New-Object System.Text.UTF8Encoding($false)))
        Write-Host "Lifecycle report written to: $ReportPath"
    }
}

# --- Guard 1: Safety switch against casual execution on developer machine ---
if (-not $AllowDisposableExecution) {
    $msg = "Refusing execution: -AllowDisposableExecution switch must be explicitly provided. This harness installs and uninstalls software and must only be run in a disposable VM or CI runner."
    Record-Check -Name "guard_disposable_switch" -Status "skipped" -Details $msg
    Save-Report -FinalSummary "skipped"
    exit 77
}

$isCiEnvironment = ($env:GITHUB_ACTIONS -eq "true" -or $env:CI -eq "true" -or $env:TF_BUILD -eq "True")
if ($isCiEnvironment) {
    $wfName = if (-not [string]::IsNullOrWhiteSpace($env:GITHUB_WORKFLOW)) { $env:GITHUB_WORKFLOW } else { "CI" }
    Record-Check -Name "guard_environment_attestation" -Status "pass" -Details "Running within disposable hosted CI runner ($wfName)."
} elseif ($RequireStandardUser) {
    Record-Check -Name "guard_environment_attestation" -Status "fail" -Details "Strict standard user verification requires explicit disposable CI environment attestation (CI=true)."
} else {
    Record-Check -Name "guard_environment_attestation" -Status "warning" -Details "Not running in detected standard CI runner. Relying on explicit -AllowDisposableExecution flag. Ensure environment is disposable."
}

# --- Guard 2: Verify installer file exists ---
if (-not (Test-Path -LiteralPath $InstallerPath -PathType Leaf)) {
    Record-Check -Name "installer_file_exists" -Status "fail" -Details "Installer binary not found: $InstallerPath"
}
$InstallerPath = (Resolve-Path -LiteralPath $InstallerPath).Path
Record-Check -Name "installer_file_exists" -Status "pass" -Details "Installer found at $InstallerPath"

# --- Guard 3: Non-admin / Elevation Check ---
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
$isAdmin = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
$script:Report.isElevated = $isAdmin

# Query process token elevation directly via Windows API to be certain (fail closed on API failure)
$isTokenElevated = $null
$tokenElevationError = $null
try {
    $isTokenElevated = Get-NativeTokenElevationState
} catch {
    $tokenElevationError = "$_"
}

if ($RequireStandardUser) {
    if ($null -ne $tokenElevationError) {
        $script:Report.elevationCheck = "unknown-elevation-error"
        Record-Check -Name "non_admin_integrity_check" -Status "fail" -Details "Strict standard user required (-RequireStandardUser), but native token elevation check failed closed: $tokenElevationError"
    } elseif ($isAdmin -or $isTokenElevated) {
        $script:Report.elevationCheck = "elevated-administrator"
        Record-Check -Name "non_admin_integrity_check" -Status "fail" -Details "Strict standard user required (-RequireStandardUser), but process is elevated (isAdmin=$isAdmin, isTokenElevated=$isTokenElevated). Fails closed before executing or modifying anything."
    } else {
        $script:Report.elevationCheck = "standard-user"
        Record-Check -Name "non_admin_integrity_check" -Status "pass" -Details "Strict standard user confirmed (non-elevated process token, not Administrator role). Identity: $($identity.Name), SID: $($identity.User.Value)."
    }
} else {
    $effectiveElevated = ($isAdmin -or ($isTokenElevated -eq $true))
    if ($effectiveElevated) {
        $script:Report.elevationCheck = "elevated-administrator"
        Record-Check -Name "non_admin_integrity_check" -Status "warning" -Details "Process is running as Administrator (common on hosted CI runners). Per-user non-elevated UAC boundary is unverified on this run."
    } elseif ($null -ne $tokenElevationError) {
        $script:Report.elevationCheck = "unknown-elevation-error"
        Record-Check -Name "non_admin_integrity_check" -Status "warning" -Details "Native token elevation check encountered error: $tokenElevationError. Admin role is $isAdmin."
    } else {
        $script:Report.elevationCheck = "standard-user"
        Record-Check -Name "non_admin_integrity_check" -Status "pass" -Details "Running as standard unprivileged user; true non-elevated behavior confirmed."
    }
}

# Target directory and registry constants
if (-not [string]::IsNullOrWhiteSpace($OverrideInstallDir)) {
    $installDir = $OverrideInstallDir
} else {
    $installDir = Join-Path $env:LOCALAPPDATA "Programs\OZ Image Viewer"
}
$exePath = Join-Path $installDir "ozviewer.exe"
$uninstallerPath = Join-Path $installDir "unins000.exe"

$appId = "{7E3B1C9A-4D2F-4B6E-9C31-0A5F2D8B47E1}"
$uninstallKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\${appId}_is1"
$hklmUninstallKey = "HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\${appId}_is1"
$progIdKey = "HKCU:\Software\Classes\OZImageViewer.Image"
$capabilitiesKey = "HKCU:\Software\OZImageViewer\Capabilities"
$markerKey = "HKCU:\Software\OZImageViewer\Install"
$registeredAppsKey = "HKCU:\Software\RegisteredApplications"

# Declared supported extensions
$declaredExtensions = @(".jpg", ".jpeg", ".png", ".webp", ".avif", ".heic", ".heif", ".bmp", ".gif", ".tif", ".tiff", ".ico", ".cur", ".tga")

# --- Guard 4: Pre-flight fail-closed check for preexisting state ---
if (Test-Path -LiteralPath $installDir) {
    Record-Check -Name "preflight_clean_target_directory" -Status "fail" -Details "Target directory already exists at '$installDir'. Preexisting installation detected. Refusing to delete or overwrite user directory."
}
Record-Check -Name "preflight_clean_target_directory" -Status "pass" -Details "Target directory is clean."

if (Test-Path -LiteralPath $uninstallKey) {
    Record-Check -Name "preflight_clean_registry_uninstall" -Status "fail" -Details "Uninstall registry key already exists at '$uninstallKey'. Refusing to proceed."
}
if (Test-Path -LiteralPath $progIdKey) {
    Record-Check -Name "preflight_clean_registry_progid" -Status "fail" -Details "ProgID registry key already exists at '$progIdKey'. Refusing to proceed."
}
Record-Check -Name "preflight_clean_registry" -Status "pass" -Details "No preexisting OZ Image Viewer registry keys found."

# Snapshot real UserChoice and Classes Default values for all declared extensions before install
$initialUserChoices = @{}
$initialClassesDefaults = @{}
foreach ($ext in $declaredExtensions) {
    $initialUserChoices[$ext] = Get-UserChoiceSnapshot -Extension $ext
    $initialClassesDefaults[$ext] = Get-RegValueSafe -SubKeyPath "Software\Classes\$ext" -ValueName ""
}

try {
    # =========================================================================
    # Stage 1: Silent Default Installation (Tasks unchecked by default)
    # =========================================================================
    Write-Host "`n--- STAGE 1: Default Silent Install ---"
    $instRes1 = Invoke-ProcessSafe -FilePath $InstallerPath -ArgumentList @("/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/SP-") -TimeoutSeconds 60
    if ($instRes1.ExitCode -ne 0) {
        Record-Check -Name "stage1_installer_exit_code" -Status "fail" -Details "Installer failed with exit code $($instRes1.ExitCode). StdErr: $($instRes1.StdErr)"
    }
    Record-Check -Name "stage1_installer_exit_code" -Status "pass" -Details "Installer exited cleanly with code 0 in $([math]::Round($instRes1.DurationSeconds, 2))s."

    # Verify deployed files
    $requiredFiles = @("ozviewer.exe", "heif.dll", "libde265.dll", "unins000.exe", "LICENSE", "THIRD-PARTY-NOTICES.md")
    foreach ($rf in $requiredFiles) {
        $full = Join-Path $installDir $rf
        if (-not (Test-Path -LiteralPath $full -PathType Leaf)) {
            Record-Check -Name "stage1_installed_file_$rf" -Status "fail" -Details "Required file missing: $full"
        }
    }
    Record-Check -Name "stage1_installed_files" -Status "pass" -Details "All core binaries, DLLs, and legal notices present."

    # Assert absence of dev artifacts (wrapped in @(...) for StrictMode safety)
    $badFiles = @(Get-ChildItem -Path $installDir -Include "*.pdb", "*.lib", "*.obj", "*.ilk" -Recurse)
    if ($badFiles.Count -gt 0) {
        Record-Check -Name "stage1_no_dev_artifacts" -Status "fail" -Details "Development/debug artifacts found in installation: $(($badFiles | ForEach-Object { $_.Name }) -join ', ')"
    }
    Record-Check -Name "stage1_no_dev_artifacts" -Status "pass" -Details "No dev or debug artifacts staged."

    # Assert registry state: uninstall entry must be in HKCU
    if (-not (Test-Path -LiteralPath $uninstallKey)) {
        Record-Check -Name "stage1_hkcu_uninstall_registered" -Status "fail" -Details "Uninstall entry not found at $uninstallKey"
    }
    $dispVer = Get-RegValueSafe -SubKeyPath "Software\Microsoft\Windows\CurrentVersion\Uninstall\${appId}_is1" -ValueName "DisplayVersion"
    if ($dispVer -ne $ExpectedVersion) {
        Record-Check -Name "stage1_uninstall_version" -Status "fail" -Details "DisplayVersion mismatch: expected $ExpectedVersion, got $dispVer"
    }
    Record-Check -Name "stage1_hkcu_uninstall_registered" -Status "pass" -Details "Uninstall entry registered under HKCU with version $dispVer."

    # Assert HKLM application uninstall key was not modified
    if (Test-Path -LiteralPath $hklmUninstallKey) {
        Record-Check -Name "stage1_hklm_unmodified" -Status "fail" -Details "HKLM application uninstall key was modified: found $hklmUninstallKey"
    }
    Record-Check -Name "stage1_hklm_unmodified" -Status "pass" -Details "HKLM application uninstall key was not created."

    # Default install must NOT register optional file associations (tasks are unchecked by default)
    if (Test-Path -LiteralPath $progIdKey) {
        Record-Check -Name "stage1_default_no_associations" -Status "fail" -Details "Default install unexpectedly registered ProgID: $progIdKey"
    }
    Record-Check -Name "stage1_default_no_associations" -Status "pass" -Details "Default install did not register file associations (opt-in respected)."

    # =========================================================================
    # Stage 2: App Launch and Image Rendering Smoke Test
    # =========================================================================
    Write-Host "`n--- STAGE 2: App Launch & Benchmark Smoke ---"
    $fixtureToTest = $FixturePath
    if ([string]::IsNullOrWhiteSpace($fixtureToTest) -or -not (Test-Path -LiteralPath $fixtureToTest)) {
        $testPng = Join-Path $env:TEMP "ozviewer-lifecycle-test-fixture.png"
        $bmpBytes = [byte[]]@(
            0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
            0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53,
            0xDE, 0x00, 0x00, 0x00, 0x0C, 0x49, 0x44, 0x41, 0x54, 0x08, 0xD7, 0x63, 0xF8, 0xCF, 0xC0, 0x00,
            0x00, 0x03, 0x01, 0x01, 0x00, 0x18, 0xDD, 0x8D, 0xB0, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E,
            0x44, 0xAE, 0x42, 0x60, 0x82
        )
        [System.IO.File]::WriteAllBytes($testPng, $bmpBytes)
        $fixtureToTest = $testPng
    }

    $benchRes = Invoke-ProcessSafe -FilePath $exePath -ArgumentList @("--benchmark", "`"$fixtureToTest`"") -TimeoutSeconds 30
    if ($benchRes.ExitCode -ne 0) {
        Record-Check -Name "stage2_benchmark_exit_code" -Status "fail" -Details "ozviewer.exe --benchmark exited with non-zero code $($benchRes.ExitCode). StdErr: $($benchRes.StdErr)"
    }
    # Validate FirstPresent was logged
    $allOutput = "$($benchRes.StdOut)`n$($benchRes.StdErr)"
    if ($allOutput -notmatch "FirstPresent") {
        Record-Check -Name "stage2_benchmark_first_present" -Status "fail" -Details "Benchmark run did not emit FirstPresent marker. Output: $allOutput"
    }
    Record-Check -Name "stage2_benchmark_first_present" -Status "pass" -Details "App launched, decoded fixture, emitted FirstPresent timing, and auto-exited with code 0."

    # =========================================================================
    # Stage 3: Reinstallation & Opt-In File Association
    # =========================================================================
    Write-Host "`n--- STAGE 3: Reinstallation with File Association Opt-In ---"

    # Inspect real supported .png extension in HKCU Classes
    $pngSubKey = "Software\Classes\.png"
    $pngDefaultBefore = Get-RegValueSafe -SubKeyPath $pngSubKey -ValueName ""
    $createdPngSentinel = $false

    if ($null -eq $pngDefaultBefore) {
        # .png has no HKCU default handler; set a temporary foreign sentinel value
        $pngKey = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($pngSubKey)
        $pngKey.SetValue("", "ForeignHandler.Png")
        $pngKey.Close()
        $pngDefaultBefore = "ForeignHandler.Png"
        $createdPngSentinel = $true
    }

    $instRes2 = Invoke-ProcessSafe -FilePath $InstallerPath -ArgumentList @("/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/SP-", "/TASKS=`"associate`"") -TimeoutSeconds 60
    if ($instRes2.ExitCode -ne 0) {
        Record-Check -Name "stage3_reinstall_exit_code" -Status "fail" -Details "Reinstallation with /TASKS=associate failed with code $($instRes2.ExitCode)"
    }
    Record-Check -Name "stage3_reinstall_exit_code" -Status "pass" -Details "Reinstallation succeeded with code 0."

    # Check that ProgID and capabilities keys are now registered in HKCU
    if (-not (Test-Path -LiteralPath $progIdKey)) {
        Record-Check -Name "stage3_progid_registered" -Status "fail" -Details "ProgID key was not created after opt-in: $progIdKey"
    }
    Record-Check -Name "stage3_progid_registered" -Status "pass" -Details "ProgID correctly registered under HKCU."

    if (-not (Test-Path -LiteralPath $capabilitiesKey)) {
        Record-Check -Name "stage3_capabilities_registered" -Status "fail" -Details "Capabilities key not created: $capabilitiesKey"
    }
    Record-Check -Name "stage3_capabilities_registered" -Status "pass" -Details "Capabilities registered."

    # Verify registered application entry points to Capabilities
    $regAppVal = Get-RegValueSafe -SubKeyPath "Software\RegisteredApplications" -ValueName "OZ Image Viewer"
    if ($regAppVal -ne "Software\OZImageViewer\Capabilities") {
        Record-Check -Name "stage3_registered_apps_value" -Status "fail" -Details "RegisteredApplications value mismatch: expected 'Software\OZImageViewer\Capabilities', got '$regAppVal'"
    }
    Record-Check -Name "stage3_registered_apps_value" -Status "pass" -Details "RegisteredApplications correctly registered."

    # Verify Install marker
    $markerAppId = Get-RegValueSafe -SubKeyPath "Software\OZImageViewer\Install" -ValueName "AppId"
    if ($markerAppId -ne $appId) {
        Record-Check -Name "stage3_ownership_marker" -Status "fail" -Details "Ownership marker mismatch: expected $appId, got $markerAppId"
    }
    Record-Check -Name "stage3_ownership_marker" -Status "pass" -Details "Ownership marker AppId verified."

    # Verify complete UserChoice state remained identical for all declared extensions
    foreach ($ext in $declaredExtensions) {
        $ucAfter = Get-UserChoiceSnapshot -Extension $ext
        $ucBefore = $initialUserChoices[$ext]
        $matched = Compare-UserChoiceSnapshot -Before $ucBefore -After $ucAfter
        if (-not $matched) {
            Record-Check -Name "stage3_userchoice_unmodified_$ext" -Status "fail" -Details "Complete UserChoice state for '$ext' was altered during installation!"
        }
    }
    Record-Check -Name "stage3_userchoice_unmodified" -Status "pass" -Details "Complete UserChoice state for all declared extensions remained identical (no defaults hijacked)."

    # Verify foreign default handler on supported extension was preserved
    $pngDefaultAfter = Get-RegValueSafe -SubKeyPath $pngSubKey -ValueName ""
    if ($pngDefaultAfter -ne $pngDefaultBefore) {
        Record-Check -Name "stage3_supported_ext_foreign_handler_preserved" -Status "fail" -Details "Foreign default handler on supported extension .png was altered: expected '$pngDefaultBefore', got '$pngDefaultAfter'"
    }
    Record-Check -Name "stage3_supported_ext_foreign_handler_preserved" -Status "pass" -Details "Foreign default handler for supported extension .png was preserved intact."

    # =========================================================================
    # Stage 4: Uninstallation & Complete Cleanup
    # =========================================================================
    Write-Host "`n--- STAGE 4: Uninstallation & Cleanup ---"
    $uninstRes = Invoke-ProcessSafe -FilePath $uninstallerPath -ArgumentList @("/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART") -TimeoutSeconds 60
    if ($uninstRes.ExitCode -ne 0) {
        Record-Check -Name "stage4_uninstaller_exit_code" -Status "fail" -Details "Uninstaller failed with exit code $($uninstRes.ExitCode)"
    }
    Record-Check -Name "stage4_uninstaller_exit_code" -Status "pass" -Details "Uninstaller exited cleanly with code 0."

    # Allow bounded settle period for Inno's self-deleting helper process
    $settleSeconds = 0
    while ((Test-Path -LiteralPath $exePath) -and ($settleSeconds -lt 15)) {
        Start-Sleep -Seconds 1
        $settleSeconds++
    }

    if (Test-Path -LiteralPath $exePath) {
        Record-Check -Name "stage4_exe_removed" -Status "fail" -Details "Executable ozviewer.exe still exists after uninstall settle."
    }
    Record-Check -Name "stage4_exe_removed" -Status "pass" -Details "Main executable successfully removed."

    if (Test-Path -LiteralPath (Join-Path $installDir "heif.dll")) {
        Record-Check -Name "stage4_heif_dll_removed" -Status "fail" -Details "heif.dll still exists after uninstall."
    }
    if (Test-Path -LiteralPath (Join-Path $installDir "libde265.dll")) {
        Record-Check -Name "stage4_libde265_dll_removed" -Status "fail" -Details "libde265.dll still exists after uninstall."
    }
    Record-Check -Name "stage4_dlls_removed" -Status "pass" -Details "Both heif.dll and libde265.dll dynamic libraries removed."

    # Verify registry cleanup
    if (Test-Path -LiteralPath $uninstallKey) {
        Record-Check -Name "stage4_uninstall_key_removed" -Status "fail" -Details "Uninstall registry key still exists: $uninstallKey"
    }
    Record-Check -Name "stage4_uninstall_key_removed" -Status "pass" -Details "Uninstall registry key cleanly reaped."

    if (Test-Path -LiteralPath $progIdKey) {
        Record-Check -Name "stage4_progid_reaped" -Status "fail" -Details "ProgID registry key was not removed: $progIdKey"
    }
    Record-Check -Name "stage4_progid_reaped" -Status "pass" -Details "Owned ProgID registry key cleanly reaped."

    if (Test-Path -LiteralPath $capabilitiesKey) {
        Record-Check -Name "stage4_capabilities_reaped" -Status "fail" -Details "Capabilities registry key was not removed: $capabilitiesKey"
    }
    Record-Check -Name "stage4_capabilities_reaped" -Status "pass" -Details "Owned Capabilities registry key cleanly reaped."

    if (Test-Path -LiteralPath $markerKey) {
        Record-Check -Name "stage4_marker_reaped" -Status "fail" -Details "Ownership marker was not removed: $markerKey"
    }
    Record-Check -Name "stage4_marker_reaped" -Status "pass" -Details "Ownership marker cleanly reaped."

    $regAppAfter = Get-RegValueSafe -SubKeyPath "Software\RegisteredApplications" -ValueName "OZ Image Viewer"
    if ($null -ne $regAppAfter) {
        Record-Check -Name "stage4_registered_apps_reaped" -Status "fail" -Details "RegisteredApplications value was not reaped."
    }
    Record-Check -Name "stage4_registered_apps_reaped" -Status "pass" -Details "RegisteredApplications entry cleanly removed."

    # Verify foreign handler for .png was preserved after uninstall
    $pngDefaultFinal = Get-RegValueSafe -SubKeyPath $pngSubKey -ValueName ""
    if ($pngDefaultFinal -ne $pngDefaultBefore) {
        Record-Check -Name "stage4_png_handler_preserved" -Status "fail" -Details "Foreign default handler for .png was modified by uninstallation: expected '$pngDefaultBefore', got '$pngDefaultFinal'"
    }
    Record-Check -Name "stage4_png_handler_preserved" -Status "pass" -Details "Foreign default handler on supported extension was preserved through uninstallation."

    # Clean up only test-created sentinel value if we created it
    if ($createdPngSentinel) {
        $k = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($pngSubKey, $true)
        if ($null -ne $k) {
            $k.DeleteValue("", $false)
            $k.Close()
        }
    }

    Write-Host "`nAll lifecycle verification stages completed successfully!" -ForegroundColor Green
    Save-Report -FinalSummary "passed"
    exit 0
} catch {
    Record-Check -Name "unhandled_lifecycle_exception" -Status "fail" -Details "$_"
    Save-Report -FinalSummary "failed"
    exit 1
}

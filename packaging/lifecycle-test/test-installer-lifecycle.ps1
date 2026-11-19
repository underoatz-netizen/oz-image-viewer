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
        - Asserts every installed runtime file matches the runner-validated portable closure
          digests (not merely file presence), and that HKCU Uninstall\\InstallLocation points
          at the actual install directory.
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
        - Verifies the actual shell-open command target is the installed ozviewer.exe.
        - Verifies HKCU\Software\OZImageViewer\Capabilities\FileAssociations entries.
        - Verifies HKCU\Software\OZImageViewer\Install marker with matching AppId.
        - Verifies NO user choice or default handler hijack occurred (all 14 extensions,
          full value names/kinds/values).
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

.PARAMETER UpgradeFromInstallerPath
  Path to the pinned historical v0.1.0 installer used for the upgrade scenario.

.PARAMETER PortableClosurePath
  Path to the runner-validated, hash-pinned portable closure manifest. The installed
  runtime after fresh install and after the v0.1.0 -> v0.1.1 upgrade must match every
  declared file digest.

.PARAMETER PortableClosureSha256
  Expected SHA-256 of PortableClosurePath. Required before any install is attempted.

.PARAMETER ExpectedCandidateCommitSha
  Independently supplied expected candidate source commit (40 hex). Never read from provenance.

.PARAMETER ExpectedCandidateTreeSha
  Independently supplied expected candidate source tree (40 hex). Never read from provenance.

.PARAMETER ExpectedPortableSha256
  Expected portable ZIP SHA-256 recorded in the validated closure.

.PARAMETER ExpectedPackageName
  Expected package name (for example pikaview) recorded in the validated closure.

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
    [string]$UpgradeFromInstallerPath = "",
    [string]$PortableClosurePath = "",
    [string]$PortableClosureSha256 = "",
    [string]$ExpectedCandidateCommitSha = "",
    [string]$ExpectedCandidateTreeSha = "",
    [string]$ExpectedPortableSha256 = "",
    [string]$ExpectedPackageName = "",
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

$handoffHelpers = Join-Path $ScriptDir "candidate-handoff.ps1"
if (-not (Test-Path -LiteralPath $handoffHelpers -PathType Leaf)) {
    throw "Required helper candidate-handoff.ps1 not found in $ScriptDir"
}
. $handoffHelpers

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

function Test-RegistryValueExactlyEqual {
    param($Before, $After)
    if ($null -eq $Before -and $null -eq $After) { return $true }
    if ($null -eq $Before -or $null -eq $After) { return $false }
    if ($Before -is [byte[]] -or $After -is [byte[]]) {
        return (([byte[]]$Before) -join ',') -eq (([byte[]]$After) -join ',')
    }
    return [object]::Equals($Before, $After)
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
        if ([string]$Before[$k].Kind -ne [string]$After[$k].Kind) { return $false }
        if (-not (Test-RegistryValueExactlyEqual -Before $Before[$k].Value -After $After[$k].Value)) { return $false }
    }
    return $true
}

function Compare-UserChoiceSnapshotAll {
    param(
        [Parameter(Mandatory = $true)][hashtable]$Before,
        [Parameter(Mandatory = $true)][hashtable]$After,
        [Parameter(Mandatory = $true)][string[]]$Extensions
    )
    $mismatches = [System.Collections.Generic.List[string]]::new()
    foreach ($ext in $Extensions) {
        $b = $null
        $a = $null
        if ($Before.ContainsKey($ext)) { $b = $Before[$ext] }
        if ($After.ContainsKey($ext)) { $a = $After[$ext] }
        if (-not (Compare-UserChoiceSnapshot -Before $b -After $a)) {
            $mismatches.Add($ext)
        }
    }
    return $mismatches.ToArray()
}

function Wait-UninstallComplete {
    param(
        [Parameter(Mandatory = $true)][string]$TargetDir,
        [int]$TimeoutSeconds = 15
    )
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
        $dirExists = Test-Path -LiteralPath $TargetDir
        $iuProcs = @(Get-Process -Name "_iu*" -ErrorAction SilentlyContinue)
        if (-not $dirExists -and $iuProcs.Count -eq 0) {
            break
        }
        Start-Sleep -Milliseconds 200
    }
    Start-Sleep -Milliseconds 300
}

$script:Report = [ordered]@{
    testSuite = "installer-lifecycle"
    timestamp = [System.DateTime]::UtcNow.ToString("o")
    installer = $InstallerPath
    expectedVersion = $ExpectedVersion
    expectedPackageName = $ExpectedPackageName
    portableClosurePath = $PortableClosurePath
    portableClosureSha256 = $PortableClosureSha256
    portablePayloadSha256 = $ExpectedPortableSha256
    expectedCandidateCommitSha = $ExpectedCandidateCommitSha
    expectedCandidateTreeSha = $ExpectedCandidateTreeSha
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
$desktopDir = [Environment]::GetFolderPath("Desktop")
$desktopShortcutName = if ($ExpectedVersion -eq "0.1.0") { "OZ Image Viewer.lnk" } else { "PikaView.lnk" }
$desktopShortcutPath = Join-Path $desktopDir $desktopShortcutName

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

# --- Guard 5: Require the runner-validated, hash-pinned portable closure ---
$portableClosure = $null
if ([string]::IsNullOrWhiteSpace($PortableClosurePath) -or [string]::IsNullOrWhiteSpace($PortableClosureSha256)) {
    Record-Check -Name "runtime_closure_manifest" -Status "fail" -Details "A validated portable closure and its pinned SHA-256 are required before any install is attempted."
}
try {
    $portableClosure = Test-PortableClosureManifest `
        -Path $PortableClosurePath `
        -ExpectedSha256 $PortableClosureSha256 `
        -ExpectedVersion $ExpectedVersion `
        -ExpectedPackageName $ExpectedPackageName `
        -ExpectedPayloadSha256 $ExpectedPortableSha256 `
        -ExpectedCommitSha $ExpectedCandidateCommitSha `
        -ExpectedTreeSha $ExpectedCandidateTreeSha
    Record-Check -Name "runtime_closure_manifest" -Status "pass" -Details "Validated portable closure is pinned by SHA-256 and matches the supplied candidate identity."
} catch {
    Record-Check -Name "runtime_closure_manifest" -Status "fail" -Details "Portable closure validation failed: $($_.Exception.Message)"
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

    # Installed runtime must match the validated portable closure byte-for-byte.
    try {
        Test-InstalledRuntimeClosure -InstallDir $installDir -Closure $portableClosure | Out-Null
        Record-Check -Name "stage1_runtime_closure" -Status "pass" -Details "Installed runtime matches every validated portable closure digest and package-manifest hash."
    } catch {
        Record-Check -Name "stage1_runtime_closure" -Status "fail" -Details "Installed runtime does not match the validated portable closure: $($_.Exception.Message)"
    }

    # Verify HKCU Uninstall\InstallLocation names the actual installation directory.
    $installLocation = Get-RegValueSafe -SubKeyPath "Software\Microsoft\Windows\CurrentVersion\Uninstall\${appId}_is1" -ValueName "InstallLocation"
    if (-not (Test-InstallLocationMatches -Actual $installLocation -Expected $installDir)) {
        Record-Check -Name "stage1_install_location" -Status "fail" -Details "Uninstall InstallLocation mismatch: expected '$installDir', got '$installLocation'."
    }
    Record-Check -Name "stage1_install_location" -Status "pass" -Details "Uninstall InstallLocation matches the actual install directory."

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
    $expectedDispName = if ($ExpectedVersion -in @("0.1.1", "0.1.2")) { "PikaView $ExpectedVersion" } else { "OZ Image Viewer" }
    $dispName = Get-RegValueSafe -SubKeyPath "Software\Microsoft\Windows\CurrentVersion\Uninstall\${appId}_is1" -ValueName "DisplayName"
    if ($dispName -ne $expectedDispName) {
        Record-Check -Name "stage1_uninstall_display_name" -Status "fail" -Details "DisplayName mismatch: expected '$expectedDispName', got '$dispName'"
    }
    Record-Check -Name "stage1_hkcu_uninstall_registered" -Status "pass" -Details "Uninstall entry registered under HKCU with DisplayName '$dispName' and version $dispVer."

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

    # Desktop shortcut collision preflight: never overwrite or delete a real user's shortcut.
    if (Test-Path -LiteralPath $desktopShortcutPath) {
        Record-Check -Name "stage3_desktop_shortcut_foreign_preserved" -Status "fail" -Details "A pre-existing desktop shortcut exists at '$desktopShortcutPath'; refusing to overwrite or delete it."
    }
    $foreignDesktopTarget = Join-Path $env:SystemRoot "System32\notepad.exe"
    $foreignDesktopArgs = "FOREIGN_DESKTOP_SHORTCUT_SENTINEL_V1"
    $foreignDesktopDescription = "FOREIGN_DESKTOP_SHORTCUT_SENTINEL_V1"
    $desktopShell = New-Object -ComObject WScript.Shell
    try {
        $foreignSc = $desktopShell.CreateShortcut($desktopShortcutPath)
        $foreignSc.TargetPath = $foreignDesktopTarget
        $foreignSc.Arguments = $foreignDesktopArgs
        $foreignSc.Description = $foreignDesktopDescription
        $foreignSc.Save()
    } finally {
        [void][System.Runtime.InteropServices.Marshal]::ReleaseComObject($desktopShell)
    }
    $desktopForeignRes = Invoke-ProcessSafe -FilePath $InstallerPath -ArgumentList @("/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/SP-", "/TASKS=`"desktopicon`"") -TimeoutSeconds 120
    $desktopForeignSnapshot = Get-ShortcutSnapshot -Path $desktopShortcutPath
    $desktopForeignDetails = if ($null -eq $desktopForeignSnapshot) {
        "shortcut is absent"
    } else {
        "Target='$($desktopForeignSnapshot.TargetPath)', Args='$($desktopForeignSnapshot.Arguments)', Description='$($desktopForeignSnapshot.Description)'"
    }
    $desktopForeignPreserved = ($null -ne $desktopForeignSnapshot -and
        [string]::Equals("$($desktopForeignSnapshot.TargetPath)".Trim(), $foreignDesktopTarget.Trim(), [System.StringComparison]::OrdinalIgnoreCase) -and
        "$($desktopForeignSnapshot.Arguments)" -ceq $foreignDesktopArgs)
    if ($desktopForeignRes.ExitCode -eq 0 -or -not $desktopForeignPreserved) {
        Record-Check -Name "stage3_desktop_shortcut_foreign_preserved" -Status "fail" -Details "Foreign desktop shortcut collision failed: ExitCode=$($desktopForeignRes.ExitCode), $desktopForeignDetails."
    }
    if ($desktopForeignPreserved) {
        Remove-Item -LiteralPath $desktopShortcutPath -Force -ErrorAction SilentlyContinue
    }
    Record-Check -Name "stage3_desktop_shortcut_foreign_preserved" -Status "pass" -Details "Installer aborted before overwriting or deleting the foreign desktop shortcut; $desktopForeignDetails (Description observed, not asserted)."

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

    $instRes2 = Invoke-ProcessSafe -FilePath $InstallerPath -ArgumentList @("/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/SP-", "/TASKS=`"associate desktopicon`"") -TimeoutSeconds 60
    if ($instRes2.ExitCode -ne 0) {
        Record-Check -Name "stage3_reinstall_exit_code" -Status "fail" -Details "Reinstallation with /TASKS=`"associate desktopicon`" failed with code $($instRes2.ExitCode)"
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
    $expectedRegAppName = if ($ExpectedVersion -in @("0.1.1", "0.1.2")) { "PikaView" } else { "OZ Image Viewer" }
    $regAppVal = Get-RegValueSafe -SubKeyPath "Software\RegisteredApplications" -ValueName $expectedRegAppName
    if ($regAppVal -ne "Software\OZImageViewer\Capabilities") {
        Record-Check -Name "stage3_registered_apps_value" -Status "fail" -Details "RegisteredApplications\$expectedRegAppName value mismatch: expected 'Software\OZImageViewer\Capabilities', got '$regAppVal'"
    }
    Record-Check -Name "stage3_registered_apps_value" -Status "pass" -Details "RegisteredApplications\$expectedRegAppName correctly registered."

    # Verify Install marker
    $markerAppId = Get-RegValueSafe -SubKeyPath "Software\OZImageViewer\Install" -ValueName "AppId"
    if ($markerAppId -ne $appId) {
        Record-Check -Name "stage3_ownership_marker" -Status "fail" -Details "Ownership marker mismatch: expected $appId, got $markerAppId"
    }
    Record-Check -Name "stage3_ownership_marker" -Status "pass" -Details "Ownership marker AppId verified."

    # Verify the actual registered shell-open command targets the installed executable.
    $commandValue = Get-RegValueSafe -SubKeyPath "Software\Classes\OZImageViewer.Image\shell\open\command" -ValueName ""
    if (-not (Test-CommandTargetMatches -CommandValue $commandValue -ExpectedExePath $exePath)) {
        Record-Check -Name "stage3_command_target" -Status "fail" -Details "ProgID shell-open command target mismatch: expected '\"$exePath\" \"%1\"', got '$commandValue'."
    }
    Record-Check -Name "stage3_command_target" -Status "pass" -Details "ProgID shell-open command targets the installed executable with %1."

    # Verify complete UserChoice state remained identical for all declared extensions.
    $ucAfterInstall = @{}
    foreach ($ext in $declaredExtensions) {
        $ucAfterInstall[$ext] = Get-UserChoiceSnapshot -Extension $ext
    }
    $ucMismatches = @(Compare-UserChoiceSnapshotAll -Before $initialUserChoices -After $ucAfterInstall -Extensions $declaredExtensions)
    if ($ucMismatches.Count -gt 0) {
        Record-Check -Name "stage3_userchoice_unmodified" -Status "fail" -Details "Complete UserChoice state was altered for: $($ucMismatches -join ', ')"
    }
    Record-Check -Name "stage3_userchoice_unmodified" -Status "pass" -Details "Complete UserChoice state for all declared extensions remained identical (no defaults hijacked)."

    # Verify foreign default handler on supported extension was preserved
    $pngDefaultAfter = Get-RegValueSafe -SubKeyPath $pngSubKey -ValueName ""
    if ($pngDefaultAfter -ne $pngDefaultBefore) {
        Record-Check -Name "stage3_supported_ext_foreign_handler_preserved" -Status "fail" -Details "Foreign default handler on supported extension .png was altered: expected '$pngDefaultBefore', got '$pngDefaultAfter'"
    }
    Record-Check -Name "stage3_supported_ext_foreign_handler_preserved" -Status "pass" -Details "Foreign default handler for supported extension .png was preserved intact."

    # Desktop shortcut opt-in: observe all fields, assert only explicit installer-owned fields.
    $desktopOptInSnapshot = Get-ShortcutSnapshot -Path $desktopShortcutPath
    $desktopOptInDetails = if ($null -eq $desktopOptInSnapshot) {
        "shortcut is absent"
    } else {
        "Target='$($desktopOptInSnapshot.TargetPath)', Args='$($desktopOptInSnapshot.Arguments)', WorkDir='$($desktopOptInSnapshot.WorkingDirectory)', Icon='$($desktopOptInSnapshot.IconLocation)', Description='$($desktopOptInSnapshot.Description)'"
    }
    if ($null -eq $desktopOptInSnapshot -or
        -not (Test-OwnedDesktopShortcutFields -Snapshot $desktopOptInSnapshot -ExpectedExePath $exePath -ExpectedWorkingDirectory $installDir)) {
        Record-Check -Name "stage3_desktop_shortcut_optin" -Status "fail" -Details "Desktop shortcut opt-in did not create the expected installer-owned shortcut: $desktopOptInDetails."
    }
    Record-Check -Name "stage3_desktop_shortcut_optin" -Status "pass" -Details "Desktop shortcut created with expected explicit fields; $desktopOptInDetails (Description observed, not asserted against an Inno default)."

    # Reinstall with the desktop task still selected must not disturb the untouched shortcut.
    $desktopReinstallRes = Invoke-ProcessSafe -FilePath $InstallerPath -ArgumentList @("/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/SP-", "/TASKS=`"associate desktopicon`"") -TimeoutSeconds 120
    $desktopAfterReinstall = Get-ShortcutSnapshot -Path $desktopShortcutPath
    $desktopReinstallDetails = if ($null -eq $desktopAfterReinstall) {
        "shortcut is absent"
    } else {
        "Target='$($desktopAfterReinstall.TargetPath)', Args='$($desktopAfterReinstall.Arguments)', WorkDir='$($desktopAfterReinstall.WorkingDirectory)', Icon='$($desktopAfterReinstall.IconLocation)', Description='$($desktopAfterReinstall.Description)'"
    }
    $desktopFieldsPreserved = ($desktopReinstallRes.ExitCode -eq 0 -and
        $null -ne $desktopAfterReinstall -and
        (Test-ShortcutExplicitFieldsEqual -Before $desktopOptInSnapshot -After $desktopAfterReinstall))
    if (-not $desktopFieldsPreserved) {
        Record-Check -Name "stage3_desktop_shortcut_reinstall_preserved" -Status "fail" -Details "Installer-created desktop shortcut did not survive its own reinstall with desktopicon selected. ExitCode=$($desktopReinstallRes.ExitCode); after reinstall: $desktopReinstallDetails."
    }
    Record-Check -Name "stage3_desktop_shortcut_reinstall_preserved" -Status "pass" -Details "Untouched desktop shortcut survived reinstall with explicit fields unchanged. Before Description='$($desktopOptInSnapshot.Description)', after Description='$($desktopAfterReinstall.Description)' (Description observed, not asserted)."

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
    Wait-UninstallComplete -TargetDir $installDir -TimeoutSeconds 15

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

    $regAppAfter = Get-RegValueSafe -SubKeyPath "Software\RegisteredApplications" -ValueName $expectedRegAppName
    if ($null -ne $regAppAfter) {
        Record-Check -Name "stage4_registered_apps_reaped" -Status "fail" -Details "RegisteredApplications\$expectedRegAppName value was not reaped."
    }
    Record-Check -Name "stage4_registered_apps_reaped" -Status "pass" -Details "RegisteredApplications\$expectedRegAppName entry cleanly removed."

    # Verify foreign handler for .png was preserved after uninstall
    $pngDefaultFinal = Get-RegValueSafe -SubKeyPath $pngSubKey -ValueName ""
    if ($pngDefaultFinal -ne $pngDefaultBefore) {
        Record-Check -Name "stage4_png_handler_preserved" -Status "fail" -Details "Foreign default handler for .png was modified by uninstallation: expected '$pngDefaultBefore', got '$pngDefaultFinal'"
    }
    Record-Check -Name "stage4_png_handler_preserved" -Status "pass" -Details "Foreign default handler on supported extension was preserved through uninstallation."

    # The untouched installer-created desktop shortcut must be removed by uninstall.
    $desktopAfterUninstall = Get-ShortcutSnapshot -Path $desktopShortcutPath
    if ($null -ne $desktopAfterUninstall) {
        Record-Check -Name "stage4_desktop_shortcut_removed" -Status "fail" -Details "Uninstall left the installer-created desktop shortcut in place: Target='$($desktopAfterUninstall.TargetPath)', Args='$($desktopAfterUninstall.Arguments)', WorkDir='$($desktopAfterUninstall.WorkingDirectory)', Icon='$($desktopAfterUninstall.IconLocation)', Description='$($desktopAfterUninstall.Description)' (Description observed, not asserted)."
    }
    Record-Check -Name "stage4_desktop_shortcut_removed" -Status "pass" -Details "Uninstall removed the untouched installer-created desktop shortcut."

    # Clean up only test-created sentinel value if we created it
    if ($createdPngSentinel) {
        $k = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($pngSubKey, $true)
        if ($null -ne $k) {
            $k.DeleteValue("", $false)
            $k.Close()
        }
    }

    # Stage 4b: a user-modified installer-created desktop shortcut must survive uninstall.
    $desktopUserModInstallRes = Invoke-ProcessSafe -FilePath $InstallerPath -ArgumentList @("/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/SP-", "/TASKS=`"desktopicon`"") -TimeoutSeconds 120
    $desktopUserModInitial = Get-ShortcutSnapshot -Path $desktopShortcutPath
    $desktopUserModInitialDetails = if ($null -eq $desktopUserModInitial) {
        "shortcut is absent"
    } else {
        "Target='$($desktopUserModInitial.TargetPath)', Args='$($desktopUserModInitial.Arguments)', Description='$($desktopUserModInitial.Description)'"
    }
    if ($desktopUserModInstallRes.ExitCode -ne 0 -or
        $null -eq $desktopUserModInitial -or
        -not (Test-OwnedDesktopShortcutFields -Snapshot $desktopUserModInitial -ExpectedExePath $exePath -ExpectedWorkingDirectory $installDir)) {
        Record-Check -Name "stage4_desktop_shortcut_user_modified_preserved" -Status "fail" -Details "Could not create a removable owned desktop shortcut for the user-modified test: ExitCode=$($desktopUserModInstallRes.ExitCode); $desktopUserModInitialDetails."
    }
    $desktopUserModArgs = "--desktop-user-modified-sentinel-v1"
    $desktopShell = New-Object -ComObject WScript.Shell
    try {
        $userModSc = $desktopShell.CreateShortcut($desktopShortcutPath)
        $userModSc.Arguments = $desktopUserModArgs
        $userModSc.Save()
    } finally {
        [void][System.Runtime.InteropServices.Marshal]::ReleaseComObject($desktopShell)
    }
    $desktopUserModified = Get-ShortcutSnapshot -Path $desktopShortcutPath
    if ($null -eq $desktopUserModified -or "$($desktopUserModified.Arguments)" -cne $desktopUserModArgs) {
        Record-Check -Name "stage4_desktop_shortcut_user_modified_preserved" -Status "fail" -Details "Could not mark the test-created desktop shortcut as user-modified."
    }
    $uninstDesktopMod = Join-Path $installDir "unins000.exe"
    if (-not (Test-Path -LiteralPath $uninstDesktopMod -PathType Leaf)) {
        Record-Check -Name "stage4_desktop_shortcut_user_modified_preserved" -Status "fail" -Details "Uninstaller missing for the user-modified desktop shortcut test: $uninstDesktopMod"
    }
    $desktopModUninstRes = Invoke-ProcessSafe -FilePath $uninstDesktopMod -ArgumentList @("/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART") -TimeoutSeconds 120
    Wait-UninstallComplete -TargetDir $installDir -TimeoutSeconds 15
    $desktopAfterModUninstall = Get-ShortcutSnapshot -Path $desktopShortcutPath
    if ($desktopModUninstRes.ExitCode -ne 0 -or
        $null -eq $desktopAfterModUninstall -or
        "$($desktopAfterModUninstall.Arguments)" -cne $desktopUserModArgs -or
        -not [string]::Equals("$($desktopAfterModUninstall.TargetPath)".Trim(), $exePath.Trim(), [System.StringComparison]::OrdinalIgnoreCase) -or
        -not (Test-ShortcutExplicitFieldsEqual -Before $desktopUserModified -After $desktopAfterModUninstall)) {
        $desktopAfterModDetails = if ($null -eq $desktopAfterModUninstall) { "shortcut is absent" } else { "Target='$($desktopAfterModUninstall.TargetPath)', Args='$($desktopAfterModUninstall.Arguments)', Description='$($desktopAfterModUninstall.Description)'" }
        Record-Check -Name "stage4_desktop_shortcut_user_modified_preserved" -Status "fail" -Details "User-modified desktop shortcut was not preserved by uninstall: ExitCode=$($desktopModUninstRes.ExitCode); $desktopAfterModDetails."
    }
    Record-Check -Name "stage4_desktop_shortcut_user_modified_preserved" -Status "pass" -Details "User-modified desktop shortcut survived uninstall unchanged; Description='$($desktopAfterModUninstall.Description)' (observed, not asserted)."
    if ($null -ne $desktopAfterModUninstall -and
        [string]::Equals("$($desktopAfterModUninstall.TargetPath)".Trim(), $exePath.Trim(), [System.StringComparison]::OrdinalIgnoreCase) -and
        "$($desktopAfterModUninstall.Arguments)" -ceq $desktopUserModArgs) {
        Remove-Item -LiteralPath $desktopShortcutPath -Force -ErrorAction SilentlyContinue
    }

    # --- STAGE 5: Separate True v0.1.0 -> v0.1.1 Upgrade Scenario ---
    if (-not [string]::IsNullOrWhiteSpace($UpgradeFromInstallerPath)) {
        Write-Host "`n--- STAGE 5: True v0.1.0 -> v0.1.1 Upgrade Scenario ---" -ForegroundColor Cyan
        if (-not (Test-Path -LiteralPath $UpgradeFromInstallerPath -PathType Leaf)) {
            Record-Check -Name "stage5_upgrade_base_exists" -Status "fail" -Details "Historical v0.1.0 installer not found: $UpgradeFromInstallerPath"
        }
        $histActual = (Get-FileHash -LiteralPath $UpgradeFromInstallerPath -Algorithm SHA256).Hash.ToLowerInvariant()
        $histPinned = "25637adf86ab1eeac9a0e5992918bc492c957c7568116f452de8c6a5e2d3b633"
        if ($histActual -ne $histPinned) {
            Record-Check -Name "stage5_upgrade_base_hash" -Status "fail" -Details "Historical v0.1.0 installer hash mismatch: expected $histPinned, got $histActual"
        }
        Record-Check -Name "stage5_upgrade_base_hash" -Status "pass" -Details "Verified pinned historical v0.1.0 installer ($histActual)."

        $upgradeCustomDir = "${installDir}_UpgradeTest"
        if (Test-Path -LiteralPath $upgradeCustomDir) {
            Record-Check -Name "stage5_upgrade_preflight" -Status "fail" -Details "Upgrade test directory already exists: $upgradeCustomDir"
        }

        # Use the single canonical full-value/type snapshot function for all 14 extensions.
        $ucBefore = @{}
        foreach ($ext in $declaredExtensions) {
            $ucBefore[$ext] = Get-UserChoiceSnapshot -Extension $ext
        }

        # Foreign shortcut collision test at exact {userprograms}\PikaView.lnk using WScript.Shell
        $programsDir = [Environment]::GetFolderPath("Programs")
        $shortcutPath = Join-Path $programsDir "PikaView.lnk"
        $wsh = New-Object -ComObject WScript.Shell
        $foreignTarget = Join-Path $env:SystemRoot "System32\notepad.exe"
        $foreignArgs = "FOREIGN_SHORTCUT_SENTINEL_V1"
        if (-not (Test-Path -LiteralPath $shortcutPath)) {
            $sc = $wsh.CreateShortcut($shortcutPath)
            $sc.TargetPath = $foreignTarget
            $sc.Arguments = $foreignArgs
            $sc.Save()
            $resCol = Invoke-ProcessSafe -FilePath $InstallerPath -ArgumentList @("/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/SP-", "/TASKS=`"!associate`"") -TimeoutSeconds 120
            $scCheck = $wsh.CreateShortcut($shortcutPath)
            if ($resCol.ExitCode -eq 0 -or $scCheck.TargetPath -ne $foreignTarget -or $scCheck.Arguments -ne $foreignArgs) {
                Record-Check -Name "stage5_shortcut_foreign_preserved" -Status "fail" -Details "Foreign shortcut collision failed: ExitCode=$($resCol.ExitCode), Target=$($scCheck.TargetPath), Args=$($scCheck.Arguments)"
            } else {
                Record-Check -Name "stage5_shortcut_foreign_preserved" -Status "pass" -Details "Installer aborted before overwriting foreign PikaView.lnk."
            }
            if (($scCheck.TargetPath -eq $foreignTarget) -and ($scCheck.Arguments -eq $foreignArgs)) {
                Remove-Item -LiteralPath $shortcutPath -Force -ErrorAction SilentlyContinue
            }
        } else {
            Record-Check -Name "stage5_shortcut_foreign_preserved" -Status "fail" -Details "Pre-existing shortcut at '$shortcutPath' prevented clean collision test."
        }

        $resOld = Invoke-ProcessSafe -FilePath $UpgradeFromInstallerPath -ArgumentList @("/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/SP-", "/DIR=`"$upgradeCustomDir`"", "/TASKS=`"associate`"") -TimeoutSeconds 120
        if ($resOld.ExitCode -ne 0) {
            Record-Check -Name "stage5_v010_install" -Status "fail" -Details "v0.1.0 base install failed with exit code $($resOld.ExitCode)"
        }
        $oldVer = Get-RegValueSafe -SubKeyPath "Software\Microsoft\Windows\CurrentVersion\Uninstall\${appId}_is1" -ValueName "DisplayVersion"
        if ($oldVer -ne "0.1.0") {
            Record-Check -Name "stage5_v010_install" -Status "fail" -Details "Expected v0.1.0 DisplayVersion, got '$oldVer'"
        }
        Record-Check -Name "stage5_v010_install" -Status "pass" -Details "Installed v0.1.0 base into custom path '$upgradeCustomDir'."
        $ucAfterOld = @{}
        foreach ($ext in $declaredExtensions) {
            $ucAfterOld[$ext] = Get-UserChoiceSnapshot -Extension $ext
        }

        # Seed foreign .png handler before upgrade
        $upgPngCreated = $false
        $pngBeforeUpg = Get-RegValueSafe -SubKeyPath $pngSubKey -ValueName ""
        if ([string]::IsNullOrWhiteSpace($pngBeforeUpg)) {
            $k = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($pngSubKey)
            $k.SetValue("", "ForeignUpgradeSentinel.png")
            $k.Close()
            $pngBeforeUpg = "ForeignUpgradeSentinel.png"
            $upgPngCreated = $true
        }

        # Run v0.1.1 upgrade over v0.1.0 with associate task EXPLICITLY UNCHECKED (/TASKS="!associate")
        $resUpg = Invoke-ProcessSafe -FilePath $InstallerPath -ArgumentList @("/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/SP-", "/TASKS=`"!associate`"") -TimeoutSeconds 120
        if ($resUpg.ExitCode -ne 0) {
            Record-Check -Name "stage5_v011_upgrade" -Status "fail" -Details "v0.1.1 upgrade failed with exit code $($resUpg.ExitCode)"
        }
        $newVer = Get-RegValueSafe -SubKeyPath "Software\Microsoft\Windows\CurrentVersion\Uninstall\${appId}_is1" -ValueName "DisplayVersion"
        $newName = Get-RegValueSafe -SubKeyPath "Software\Microsoft\Windows\CurrentVersion\Uninstall\${appId}_is1" -ValueName "DisplayName"
        $newLoc = Get-RegValueSafe -SubKeyPath "Software\Microsoft\Windows\CurrentVersion\Uninstall\${appId}_is1" -ValueName "InstallLocation"
        if ($newVer -ne $ExpectedVersion -or $newName -ne "PikaView $ExpectedVersion") {
            Record-Check -Name "stage5_v011_upgrade" -Status "fail" -Details "Upgrade metadata mismatch: DisplayName='$newName', DisplayVersion='$newVer'"
        }
        if (-not (Test-Path -LiteralPath (Join-Path $upgradeCustomDir "ozviewer.exe") -PathType Leaf)) {
            Record-Check -Name "stage5_v011_upgrade_path" -Status "fail" -Details "Upgraded ozviewer.exe missing from custom path '$upgradeCustomDir' (InstallLocation='$newLoc')"
        }
        if (-not (Test-Path -LiteralPath $progIdKey)) {
            Record-Check -Name "stage5_v011_progid" -Status "fail" -Details "ProgID '$progIdKey' missing after upgrade."
        }
        $upgRegAppNew = Get-RegValueSafe -SubKeyPath "Software\RegisteredApplications" -ValueName "PikaView"
        $upgRegAppOld = Get-RegValueSafe -SubKeyPath "Software\RegisteredApplications" -ValueName "OZ Image Viewer"
        if ($upgRegAppNew -ne "Software\OZImageViewer\Capabilities" -or $null -ne $upgRegAppOld) {
            Record-Check -Name "stage5_v011_regapps_migrated" -Status "fail" -Details "RegisteredApplications migration failed: PikaView='$upgRegAppNew', Legacy='$upgRegAppOld'"
        }
        $pngAfterUpg = Get-RegValueSafe -SubKeyPath $pngSubKey -ValueName ""
        if ($pngAfterUpg -ne $pngBeforeUpg) {
            Record-Check -Name "stage5_v011_foreign_preserved" -Status "fail" -Details "Foreign .png handler modified during upgrade: expected '$pngBeforeUpg', got '$pngAfterUpg'"
        }

        # Upgraded runtime must match the same validated portable closure.
        try {
            Test-InstalledRuntimeClosure -InstallDir $upgradeCustomDir -Closure $portableClosure | Out-Null
            Record-Check -Name "stage5_runtime_closure" -Status "pass" -Details "Upgraded runtime matches every validated portable closure digest and package-manifest hash."
        } catch {
            Record-Check -Name "stage5_runtime_closure" -Status "fail" -Details "Upgraded runtime does not match the validated portable closure: $($_.Exception.Message)"
        }

        # Upgrade must keep the original install location and rewrite the command target to it.
        if (-not (Test-InstallLocationMatches -Actual $newLoc -Expected $upgradeCustomDir)) {
            Record-Check -Name "stage5_install_location" -Status "fail" -Details "Upgraded InstallLocation mismatch: expected '$upgradeCustomDir', got '$newLoc'."
        }
        Record-Check -Name "stage5_install_location" -Status "pass" -Details "Upgraded InstallLocation points at the original custom install directory."

        $upgradedExePath = Join-Path $upgradeCustomDir "ozviewer.exe"
        $upgradedCommand = Get-RegValueSafe -SubKeyPath "Software\Classes\OZImageViewer.Image\shell\open\command" -ValueName ""
        if (-not (Test-CommandTargetMatches -CommandValue $upgradedCommand -ExpectedExePath $upgradedExePath)) {
            Record-Check -Name "stage5_command_target" -Status "fail" -Details "Upgraded shell-open command target mismatch: expected '\"$upgradedExePath\" \"%1\"', got '$upgradedCommand'."
        }
        Record-Check -Name "stage5_command_target" -Status "pass" -Details "Upgraded shell-open command targets the upgraded executable."

        $ucAfterUpg = @{}
        foreach ($ext in $declaredExtensions) {
            $ucAfterUpg[$ext] = Get-UserChoiceSnapshot -Extension $ext
        }
        Record-Check -Name "stage5_v011_upgrade" -Status "pass" -Details "Upgraded v0.1.0 -> v0.1.1 in-place at '$upgradeCustomDir' with DisplayName='$newName', migrated RegisteredApplications, same AppId/ProgID, foreign handler preserved."

        # Modify actual installer-created {userprograms}\PikaView.lnk and verify survival across uninstall
        $userModArgs = "--user-modified-sentinel-v1"
        if (Test-Path -LiteralPath $shortcutPath) {
            $scMod = $wsh.CreateShortcut($shortcutPath)
            $scMod.Arguments = $userModArgs
            $scMod.Save()
        }

        # Clean uninstall of upgraded instance
        $uninstUpg = Join-Path $upgradeCustomDir "unins000.exe"
        $resUninstUpg = Invoke-ProcessSafe -FilePath $uninstUpg -ArgumentList @("/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART") -TimeoutSeconds 120
        Wait-UninstallComplete -TargetDir $upgradeCustomDir -TimeoutSeconds 15
        if ($resUninstUpg.ExitCode -ne 0) {
            Record-Check -Name "stage5_v011_uninstall" -Status "fail" -Details "Upgraded uninstall failed with exit code $($resUninstUpg.ExitCode)"
        }
        if (Test-Path -LiteralPath $upgradeCustomDir) {
            Record-Check -Name "stage5_v011_uninstall" -Status "fail" -Details "Upgrade custom directory still exists after uninstall: $upgradeCustomDir"
        }
        if (-not (Test-Path -LiteralPath $shortcutPath)) {
            Record-Check -Name "stage5_shortcut_user_modified_preserved" -Status "fail" -Details "User-modified PikaView.lnk did not survive uninstall."
        } else {
            $scAfter = $wsh.CreateShortcut($shortcutPath)
            if ($scAfter.Arguments -ne $userModArgs) {
                Record-Check -Name "stage5_shortcut_user_modified_preserved" -Status "fail" -Details "User-modified PikaView.lnk arguments lost: '$($scAfter.Arguments)'"
            } else {
                Record-Check -Name "stage5_shortcut_user_modified_preserved" -Status "pass" -Details "User-modified PikaView.lnk survived uninstall with modified arguments."
                Remove-Item -LiteralPath $shortcutPath -Force -ErrorAction SilentlyContinue
            }
        }
        Record-Check -Name "stage5_v011_uninstall" -Status "pass" -Details "Upgraded v0.1.1 instance cleanly uninstalled."

        $ucAfterUninst = @{}
        foreach ($ext in $declaredExtensions) {
            $ucAfterUninst[$ext] = Get-UserChoiceSnapshot -Extension $ext
        }
        $ucMismatches = [System.Collections.Generic.List[string]]::new()
        foreach ($m in @(Compare-UserChoiceSnapshotAll -Before $initialUserChoices -After $ucBefore -Extensions $declaredExtensions)) { $ucMismatches.Add("pre-upgrade:$m") }
        foreach ($m in @(Compare-UserChoiceSnapshotAll -Before $ucBefore -After $ucAfterOld -Extensions $declaredExtensions)) { $ucMismatches.Add("v0.1.0-install:$m") }
        foreach ($m in @(Compare-UserChoiceSnapshotAll -Before $ucBefore -After $ucAfterUpg -Extensions $declaredExtensions)) { $ucMismatches.Add("v0.1.1-upgrade:$m") }
        foreach ($m in @(Compare-UserChoiceSnapshotAll -Before $ucBefore -After $ucAfterUninst -Extensions $declaredExtensions)) { $ucMismatches.Add("uninstall:$m") }
        if ($ucMismatches.Count -gt 0) {
            Record-Check -Name "stage5_userchoice_preserved" -Status "fail" -Details "UserChoice changed for: $($ucMismatches -join ', ')"
        } else {
            Record-Check -Name "stage5_userchoice_preserved" -Status "pass" -Details "Complete UserChoice state (absence/presence + all value names/kinds/values, all 14 extensions) preserved across v0.1.0 install, v0.1.1 upgrade, and uninstall."
        }

        # Fresh opt-out -> reinstall with opt-out
        $resOptOut1 = Invoke-ProcessSafe -FilePath $InstallerPath -ArgumentList @("/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/SP-", "/TASKS=`"!associate`"") -TimeoutSeconds 120
        $resOptOut2 = Invoke-ProcessSafe -FilePath $InstallerPath -ArgumentList @("/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/SP-", "/TASKS=`"!associate`"") -TimeoutSeconds 120
        if ($resOptOut1.ExitCode -ne 0 -or $resOptOut2.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $shortcutPath) -or (Test-Path -LiteralPath $progIdKey)) {
            Record-Check -Name "stage5_fresh_optout_reinstall" -Status "fail" -Details "Fresh opt-out -> reinstall failed: E1=$($resOptOut1.ExitCode), E2=$($resOptOut2.ExitCode)"
        } else {
            Record-Check -Name "stage5_fresh_optout_reinstall" -Status "pass" -Details "Fresh opt-out -> reinstall preserved PikaView.lnk and kept associations absent."
        }
        $uninstOptOut = Join-Path $installDir "unins000.exe"
        if (Test-Path -LiteralPath $uninstOptOut) {
            Invoke-ProcessSafe -FilePath $uninstOptOut -ArgumentList @("/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART") -TimeoutSeconds 120 | Out-Null
            Wait-UninstallComplete -TargetDir $installDir -TimeoutSeconds 15
        }

        if ($upgPngCreated) {
            $k = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($pngSubKey, $true)
            if ($null -ne $k) {
                $k.DeleteValue("", $false)
                $k.Close()
            }
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

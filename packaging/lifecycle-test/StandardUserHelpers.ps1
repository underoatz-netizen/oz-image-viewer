<#
.SYNOPSIS
  Shared standard-user verification helpers for OZ Image Viewer installer lifecycle testing.

.DESCRIPTION
  Provides pure, importable functions for:
    - Exact 28-check lifecycle contract definition.
    - Native token elevation inspection (fail-closed on Win32 API error).
    - Child bootstrap script generation with genuine profile, HKCU nonce, and identity proofs.
    - Child report schema and check completeness validation.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Exact 28 check names matching the actual test-installer-lifecycle.ps1 execution
$script:StandardLifecycleRequiredCheckNames = @(
    "guard_environment_attestation",
    "installer_file_exists",
    "non_admin_integrity_check",
    "preflight_clean_target_directory",
    "preflight_clean_registry",
    "stage1_installer_exit_code",
    "stage1_installed_files",
    "stage1_no_dev_artifacts",
    "stage1_hkcu_uninstall_registered",
    "stage1_hklm_unmodified",
    "stage1_default_no_associations",
    "stage2_benchmark_first_present",
    "stage3_reinstall_exit_code",
    "stage3_progid_registered",
    "stage3_capabilities_registered",
    "stage3_registered_apps_value",
    "stage3_ownership_marker",
    "stage3_userchoice_unmodified",
    "stage3_supported_ext_foreign_handler_preserved",
    "stage4_uninstaller_exit_code",
    "stage4_exe_removed",
    "stage4_dlls_removed",
    "stage4_uninstall_key_removed",
    "stage4_progid_reaped",
    "stage4_capabilities_reaped",
    "stage4_marker_reaped",
    "stage4_registered_apps_reaped",
    "stage4_png_handler_preserved"
)

function Get-StandardLifecycleRequiredCheckNames {
    return $script:StandardLifecycleRequiredCheckNames
}

function New-EphemeralStandardUserName {
    <#
    .SYNOPSIS
      Generates an ephemeral local username strictly compliant with the Windows 20-character SAM account name limit.
    .DESCRIPTION
      Format: 'ozsu_' + 10 lowercase hexadecimal characters from a GUID (15 characters total).
      Validates length <= 20 and regex format ^[a-z0-9_]{1,20}$.
    #>
    [CmdletBinding()]
    param()

    $suffix = [System.Guid]::NewGuid().ToString('N').Substring(0, 10).ToLowerInvariant()
    $name = "ozsu_$suffix"
    if ($name.Length -gt 20 -or $name -notmatch '^[a-z0-9_]{1,20}$') {
        throw "Generated username '$name' violates SAM account name constraints (Length: $($name.Length), max 20)."
    }
    return $name
}

function Get-NativeTokenElevationState {
    <#
    .SYNOPSIS
      Queries TokenElevation via advapi32!GetTokenInformation.
      Fails closed: throws if any Win32 API call fails rather than returning false.
    #>
    [CmdletBinding()]
    param()

    Add-Type -TypeDefinition @"
    using System;
    using System.ComponentModel;
    using System.Runtime.InteropServices;

    public class NativeElevationInspector {
        [DllImport("advapi32.dll", SetLastError = true)]
        public static extern bool OpenProcessToken(IntPtr ProcessHandle, uint DesiredAccess, out IntPtr TokenHandle);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern IntPtr GetCurrentProcess();

        [DllImport("advapi32.dll", SetLastError = true)]
        public static extern bool GetTokenInformation(IntPtr TokenHandle, int TokenInformationClass, IntPtr TokenInformation, uint TokenInformationLength, out uint ReturnLength);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool CloseHandle(IntPtr hObject);

        public static bool QueryIsElevated() {
            IntPtr tokenHandle = IntPtr.Zero;
            if (!OpenProcessToken(GetCurrentProcess(), 0x0008 /* TOKEN_QUERY */, out tokenHandle)) {
                throw new Win32Exception(Marshal.GetLastWin32Error(), "OpenProcessToken failed.");
            }
            try {
                int elevationResult = 0;
                int elevationResultSize = Marshal.SizeOf(elevationResult);
                IntPtr elevationPtr = Marshal.AllocHGlobal(elevationResultSize);
                try {
                    uint returnLength = 0;
                    if (!GetTokenInformation(tokenHandle, 20 /* TokenElevation */, elevationPtr, (uint)elevationResultSize, out returnLength)) {
                        throw new Win32Exception(Marshal.GetLastWin32Error(), "GetTokenInformation failed.");
                    }
                    elevationResult = Marshal.ReadInt32(elevationPtr);
                    return elevationResult != 0;
                } finally {
                    Marshal.FreeHGlobal(elevationPtr);
                }
            } finally {
                CloseHandle(tokenHandle);
            }
        }
    }
"@ -ErrorAction SilentlyContinue

    return [NativeElevationInspector]::QueryIsElevated()
}

function Get-ProcessOwnerSidSafely {
    <#
    .SYNOPSIS
      Queries a Win32_Process instance's owner SID safely.
      Returns:
        @{ Success = $true; Sid = "<sid>" } when successfully resolved.
        @{ Success = $false; Error = "<error>" } when query fails or returns non-zero.
    #>
    param([Parameter(Mandatory = $true)]$Process)

    try {
        $ownerRes = Invoke-CimMethod -InputObject $Process -MethodName GetOwnerSid
        if ($null -ne $ownerRes -and $ownerRes.ReturnValue -eq 0 -and -not [string]::IsNullOrWhiteSpace($ownerRes.Sid)) {
            return [pscustomobject]@{
                Success = $true
                Sid     = $ownerRes.Sid.Trim()
                Error   = $null
            }
        } else {
            $retVal = if ($null -ne $ownerRes) { $ownerRes.ReturnValue } else { "null" }
            return [pscustomobject]@{
                Success = $false
                Sid     = $null
                Error   = "GetOwnerSid returned non-zero ($retVal) or empty SID"
            }
        }
    } catch {
        return [pscustomobject]@{
            Success = $false
            Sid     = $null
            Error   = "$_"
        }
    }
}

function New-ChildBootstrapScriptContent {
    param(
        [Parameter(Mandatory = $true)][string]$ExpectedUserSid,
        [Parameter(Mandatory = $true)][string]$InputsDir,
        [Parameter(Mandatory = $true)][string]$OutputDir,
        [Parameter(Mandatory = $true)][string]$InstallerFileName,
        [Parameter(Mandatory = $true)][string]$FixtureFileName,
        [Parameter(Mandatory = $true)][string]$ChildReportFileName,
        [Parameter(Mandatory = $true)][string]$ChildProofFileName
    )

    $template = @'
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# 1. Scrub parent environment and reconstruct minimal clean standard user environment
$cleanKeys = @('SystemRoot', 'ComSpec', 'PATHEXT')
$envBackup = @{}
foreach ($k in $cleanKeys) {
    $envBackup[$k] = [System.Environment]::GetEnvironmentVariable($k, 'Process')
}

# Clear all environment variables
Get-ChildItem env: | ForEach-Object {
    [System.Environment]::SetEnvironmentVariable($_.Name, $null, 'Process')
}

# Restore basic system variables + non-secret CI=true attestation
foreach ($k in $cleanKeys) {
    [System.Environment]::SetEnvironmentVariable($k, $envBackup[$k], 'Process')
}
[System.Environment]::SetEnvironmentVariable('CI', 'true', 'Process')

# 2. Strict Child Identity & Elevation Assertions
$expectedSid = '__EXPECTED_USER_SID__'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
if ($identity.User.Value -ne $expectedSid) {
    throw "Security policy violation: Child process user SID ($($identity.User.Value)) does not match expected SID '$expectedSid'!"
}

# Assert WindowsPrincipal is NOT Administrator
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
$isAdmin = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if ($isAdmin) {
    throw "Security policy violation: Child process is running in Administrator role!"
}

# Verify native TokenElevation is strictly false (fail closed on API failure)
$inputsDir = '__INPUTS_DIR__'
$standardUserHelpersScript = Join-Path $inputsDir 'StandardUserHelpers.ps1'
if (-not (Test-Path -LiteralPath $standardUserHelpersScript -PathType Leaf)) {
    throw "StandardUserHelpers.ps1 not found in inputs: '$standardUserHelpersScript'"
}
. $standardUserHelpersScript

$isTokenElevated = $null
try {
    $isTokenElevated = Get-NativeTokenElevationState
} catch {
    throw "Security policy violation: Native token elevation check failed closed: $_"
}
if ($isTokenElevated -ne $false) {
    throw "Security policy violation: Child process native token is elevated (isTokenElevated=$isTokenElevated)!"
}

# Verify ProfileList binding in HKLM
$profileKey = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey("SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList\$expectedSid")
if ($null -eq $profileKey) {
    throw "Profile verification failed: SID '$expectedSid' has no entry in HKLM ProfileList!"
}
$profilePath = "$($profileKey.GetValue('ProfileImagePath'))"
$profileKey.Close()
if ([string]::IsNullOrWhiteSpace($profilePath) -or -not (Test-Path -LiteralPath $profilePath)) {
    throw "Profile directory not found on disk for SID '$expectedSid': '$profilePath'"
}

$userProfile = [System.Environment]::GetFolderPath([System.Environment+SpecialFolder]::UserProfile)
if (-not $userProfile.StartsWith($profilePath, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "UserProfile mismatch: special folder '$userProfile' does not match ProfileImagePath '$profilePath'!"
}

$localAppData = [System.Environment]::GetFolderPath([System.Environment+SpecialFolder]::LocalApplicationData)
$appData = [System.Environment]::GetFolderPath([System.Environment+SpecialFolder]::ApplicationData)

# Establish isolated user TEMP under new user's LocalAppData\Temp (never system fallback)
$userTemp = Join-Path $localAppData "Temp"
if (-not (Test-Path -LiteralPath $userTemp)) {
    New-Item -ItemType Directory -Path $userTemp -Force | Out-Null
}

$systemRoot = [System.Environment]::GetEnvironmentVariable('SystemRoot', 'Process')
$system32 = Join-Path $systemRoot "System32"
[System.Environment]::SetEnvironmentVariable('USERPROFILE', $userProfile, 'Process')
[System.Environment]::SetEnvironmentVariable('LOCALAPPDATA', $localAppData, 'Process')
[System.Environment]::SetEnvironmentVariable('APPDATA', $appData, 'Process')
[System.Environment]::SetEnvironmentVariable('TEMP', $userTemp, 'Process')
[System.Environment]::SetEnvironmentVariable('TMP', $userTemp, 'Process')
[System.Environment]::SetEnvironmentVariable('PATH', "$system32;$systemRoot", 'Process')

# 3. Verify actual HKCU registry binding via unique GUID evidence key & HKU\$expectedSid read
$uniqueGuid = [System.Guid]::NewGuid().ToString('D')
$evidenceSubKey = "Software\OZImageViewer_Evidence_$uniqueGuid"
$nonceVal = [System.Guid]::NewGuid().ToString('D')

# Fail closed if evidence key already exists
$existingEvidence = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($evidenceSubKey)
if ($null -ne $existingEvidence) {
    $existingEvidence.Close()
    throw "HKCU binding verification collision: Evidence key '$evidenceSubKey' already exists!"
}

$createdEvidenceKey = $false
try {
    $eKey = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($evidenceSubKey)
    $eKey.SetValue("Nonce", $nonceVal)
    $eKey.Close()
    $createdEvidenceKey = $true

    # Read back via Registry.Users under expectedSid
    $hkuSub = [Microsoft.Win32.Registry]::Users.OpenSubKey("$expectedSid\$evidenceSubKey")
    if ($null -eq $hkuSub) {
        throw "HKCU binding verification failed: Key was not visible under HKEY_USERS\$expectedSid!"
    }
    $readNonce = "$($hkuSub.GetValue('Nonce'))"
    $hkuSub.Close()

    if ($readNonce -ne $nonceVal) {
        throw "HKCU binding verification failed: Nonce mismatch under HKEY_USERS\$expectedSid (expected '$nonceVal', got '$readNonce')!"
    }
} finally {
    if ($createdEvidenceKey) {
        # Delete strictly only the test-created key
        [Microsoft.Win32.Registry]::CurrentUser.DeleteSubKeyTree($evidenceSubKey, $false)
    }
}

# 4. Save structured child identity proof file for parent verification
$outputDir = '__OUTPUT_DIR__'
$childProofPath = Join-Path $outputDir '__CHILD_PROOF_FILE_NAME__'
$proofObj = [ordered]@{
    observedSid             = $identity.User.Value
    expectedSid             = $expectedSid
    userName                = $identity.Name
    isAdmin                 = $isAdmin
    isTokenElevated         = $isTokenElevated
    profileImagePath        = $profilePath
    userProfileFolder       = $userProfile
    localAppDataFolder      = $localAppData
    appDataFolder           = $appData
    userTempFolder          = $userTemp
    hkcuEvidenceVerified    = $true
}
$proofJson = $proofObj | ConvertTo-Json -Depth 4
[System.IO.File]::WriteAllText($childProofPath, $proofJson, (New-Object System.Text.UTF8Encoding($false)))

# 5. Invoke test-installer-lifecycle.ps1 in strict mode
$installerPath = Join-Path $inputsDir '__INSTALLER_FILE_NAME__'
$fixturePath = Join-Path $inputsDir '__FIXTURE_FILE_NAME__'
$childReportPath = Join-Path $outputDir '__CHILD_REPORT_FILE_NAME__'
$harnessScript = Join-Path $inputsDir 'test-installer-lifecycle.ps1'
$powershellExe = Join-Path $system32 'WindowsPowerShell\v1.0\powershell.exe'

& $powershellExe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $harnessScript `
    -InstallerPath $installerPath `
    -ExpectedVersion '0.1.0' `
    -FixturePath $fixturePath `
    -ReportPath $childReportPath `
    -RequireStandardUser `
    -AllowDisposableExecution

exit $LASTEXITCODE
'@

    return $template.
        Replace('__EXPECTED_USER_SID__', $ExpectedUserSid).
        Replace('__INPUTS_DIR__', $InputsDir).
        Replace('__OUTPUT_DIR__', $OutputDir).
        Replace('__INSTALLER_FILE_NAME__', $InstallerFileName).
        Replace('__FIXTURE_FILE_NAME__', $FixtureFileName).
        Replace('__CHILD_REPORT_FILE_NAME__', $ChildReportFileName).
        Replace('__CHILD_PROOF_FILE_NAME__', $ChildProofFileName)
}

function Test-ChildReportIntegrity {
    param(
        [Parameter(Mandatory = $true)]$ReportData,
        [Parameter(Mandatory = $true)][string]$ExpectedUserSid,
        [Parameter(Mandatory = $true)][string]$ExpectedInstallerSha,
        [AllowNull()][object]$ProofData,
        [Parameter(Mandatory = $true)][string]$ExpectedProfileImagePath,
        [string]$ControllerSid = $null
    )

    if ($null -eq $ReportData) {
        throw "Child report data is null"
    }
    if ($ReportData.summary -ne "passed") {
        throw "Child report summary is '$($ReportData.summary)', expected 'passed'"
    }
    if ($ReportData.isElevated -ne $false) {
        throw "Child report reports isElevated is true!"
    }
    if ($ReportData.elevationCheck -ne "standard-user") {
        throw "Child report elevationCheck is '$($ReportData.elevationCheck)', expected 'standard-user'"
    }

    # Verify installer path and SHA match
    $instPath = "$($ReportData.installer)"
    if (-not (Test-Path -LiteralPath $instPath -PathType Leaf)) {
        throw "Child report references non-existent installer: '$instPath'"
    }
    $actualHash = (Get-FileHash -LiteralPath $instPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $ExpectedInstallerSha.Trim().ToLowerInvariant()) {
        throw "Child report installer SHA ($actualHash) does not match expected ($ExpectedInstallerSha)"
    }

    # MANDATORY: Validate structured child identity proof
    if ($null -eq $ProofData) {
        throw "Mandatory child identity proof data is missing or null!"
    }
    if ("$($ProofData.observedSid)".Trim() -ne $ExpectedUserSid.Trim()) {
        throw "Proof data observedSid mismatch: expected '$ExpectedUserSid', got '$($ProofData.observedSid)'"
    }
    if (-not [string]::IsNullOrWhiteSpace($ControllerSid)) {
        if ("$($ProofData.observedSid)".Trim() -eq $ControllerSid.Trim()) {
            throw "Security policy violation: Observed child SID matches controller/parent SID ($ControllerSid)! Child did not run under distinct standard user."
        }
    }
    if ($ProofData.isAdmin -ne $false) {
        throw "Proof data reports isAdmin is true!"
    }
    if ($ProofData.isTokenElevated -ne $false) {
        throw "Proof data reports isTokenElevated is true!"
    }
    if ($ProofData.hkcuEvidenceVerified -ne $true) {
        throw "Proof data reports hkcuEvidenceVerified is not true!"
    }

    # Canonical path checks: ExpectedProfileImagePath is mandatory and non-blank
    if ([string]::IsNullOrWhiteSpace($ExpectedProfileImagePath)) {
        throw "ExpectedProfileImagePath must be a non-blank path string!"
    }
    $expPImg = $ExpectedProfileImagePath.TrimEnd('\')
    $pImg = "$($ProofData.profileImagePath)".TrimEnd('\')
    $uProf = "$($ProofData.userProfileFolder)".TrimEnd('\')
    $lApp = "$($ProofData.localAppDataFolder)".TrimEnd('\')
    $uTemp = "$($ProofData.userTempFolder)".TrimEnd('\')

    if (-not $pImg.Equals($expPImg, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Proof profileImagePath ('$pImg') does not match expected ProfileList path ('$expPImg')!"
    }
    if (-not $uProf.Equals($pImg, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Proof userProfileFolder ('$uProf') does not match profileImagePath ('$pImg')!"
    }
    if (-not $lApp.StartsWith($uProf, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Proof localAppDataFolder ('$lApp') is not located beneath userProfileFolder ('$uProf')!"
    }
    if (-not $uTemp.StartsWith($lApp, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Proof userTempFolder ('$uTemp') is not located beneath localAppDataFolder ('$lApp')!"
    }

    # Verify exact 28 check names
    $requiredNames = Get-StandardLifecycleRequiredCheckNames
    $checks = @($ReportData.checks)
    if ($checks.Count -ne $requiredNames.Count) {
        throw "Child report check count mismatch: expected exactly $($requiredNames.Count) checks, found $($checks.Count)"
    }

    $seenNames = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($check in $checks) {
        $cName = "$($check.name)".Trim()
        $cStatus = "$($check.status)".Trim()
        if ($seenNames.Contains($cName)) {
            throw "Duplicate check detected in child report: '$cName'"
        }
        $seenNames.Add($cName) | Out-Null
        if ($cStatus -ne "pass") {
            throw "Child check '$cName' has non-passing status: '$cStatus'"
        }
    }

    foreach ($req in $requiredNames) {
        if (-not $seenNames.Contains($req)) {
            throw "Missing required check in child report: '$req'"
        }
    }

    return $true
}

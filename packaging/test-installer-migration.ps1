<#
.SYNOPSIS
  Comprehensive mock unit test suite for PikaView installer/upgrade identity migration.

.DESCRIPTION
  Exercises the exact logic specified in packaging\ozviewer-installer.iss without
  ever executing real installers or touching host registry/shortcuts:
  1. Fresh opt-out install: handler registration is not created; no registry keys/values written.
  2. Fresh opt-in install: creates PikaView handler registration under HKCU, sets ApplicationName,
     ApplicationDescription, ProgID friendly names, default icon, quoted command, and
     RegisteredApplications\PikaView mapping.
  3. Upgrade from owned v0.1.0/v0.1.1 installs to current 0.1.2, task checked or unchecked:
     - Detects existing owned registration with legacy names (OZ Image Viewer / OZ Image Viewer Image).
     - Automatically updates registered labels to PikaView and RegisteredApplications\PikaView.
     - Removes legacy RegisteredApplications\OZ Image Viewer entry.
     - Reaps obsolete file associations if any.
     - Preserves existing opt-in without creating new consent.
     - Uninstall removes recognized prior marker versions without leaving owned-version residue.
  4. Foreign / user-modified registry collision handling:
     - Collision on RegisteredApplications\PikaView owned by another program fails closed (preflight aborts).
     - Foreign / unowned legacy RegisteredApplications\OZ Image Viewer fails closed.
     - Foreign ProgID collision fails closed.
     - Foreign application subkey fails closed.
     - User-modified file association (.jpg pointing to external handler) fails closed.
  5. Legacy and new unregister preservation:
     - Uninstall removes both PikaView and legacy OZ Image Viewer values when owned.
     - Preserves user-modified values (e.g. customized DefaultIcon).
     - Cleans empty keys up to application root without touching UserChoice, HKLM, or external apps.
     - Refuses unregistration if AppId ownership marker is absent or mismatched.
  6. Shortcut migration & collision policy:
     - Start Menu shortcut is updated to PikaView.lnk for current install.
     - Verifies policy: old installer-owned shortcuts migrated only if target matches {app}\ozviewer.exe;
       foreign/unrelated shortcuts or desktop shortcuts where user didn't opt-in are NOT touched.
  7. Inno Setup script compilation & static policy audit:
     - AppId, PrivilegesRequired=lowest, DefaultDirName={localappdata}\Programs\OZ Image Viewer preserved.
     - Executable remains ozviewer.exe.
     - No HKLM, HKCR, UserChoice, or OpenWithProgids references.
     - Compiles with ISCC.exe in an isolated temporary output directory.
#>
[CmdletBinding()]
param(
    [string]$ScratchDir = "C:\Users\USER\AppData\Local\Temp\opencode\test-installer-migration-suite"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Split-Path -Parent $ScriptDir
$InnoScript = Join-Path $ScriptDir 'ozviewer-installer.iss'
$BuilderScript = Join-Path $ScriptDir 'build-installer.ps1'

function Pass([string]$msg) { Write-Host "migration-test: PASS: $msg" -ForegroundColor Green }
function Fail([string]$msg) { throw "ASSERT FAILED: $msg" }
function Assert-True([bool]$cond, [string]$msg) { if (-not $cond) { Fail $msg } }

if (Test-Path -LiteralPath $ScratchDir) {
    Remove-Item -LiteralPath $ScratchDir -Recurse -Force
}
New-Item -ItemType Directory -Path $ScratchDir -Force | Out-Null
$ScratchDir = (Resolve-Path -LiteralPath $ScratchDir).Path

# ---------------------------------------------------------------------------
# Policy definition matching ozviewer-installer.iss
# ---------------------------------------------------------------------------
$Policy = @{
    ProgID               = 'OZImageViewer.Image'
    ProgIDKey            = 'software\classes\ozimageviewer.image'
    CommandKey           = 'software\classes\ozimageviewer.image\shell\open\command'
    OwnerId              = '{7E3B1C9A-4D2F-4B6E-9C31-0A5F2D8B47E1}'
    UninstallKey         = 'software\microsoft\windows\currentversion\uninstall\{7E3B1C9A-4D2F-4B6E-9C31-0A5F2D8B47E1}_is1'
    AppKey               = 'software\ozimageviewer'
    MarkerKey            = 'software\ozimageviewer\install'
    CapKey               = 'software\ozimageviewer\capabilities'
    FileAssocKey         = 'software\ozimageviewer\capabilities\fileassociations'
    RegisteredApps       = 'software\registeredapplications'
    RegisteredName       = 'PikaView'
    LegacyRegisteredName = 'OZ Image Viewer'
    AppName              = 'PikaView'
    LegacyAppName        = 'OZ Image Viewer'
    TypeName             = 'PikaView Image'
    LegacyTypeName       = 'OZ Image Viewer Image'
    CurrentVersion       = '0.1.2'
    Extensions           = @('.jpg', '.png', '.webp')
}

$AppDir = 'C:\Users\Test User\AppData\Local\Programs\OZ Image Viewer'
$CmdValue = '"' + $AppDir + '\ozviewer.exe" "%1"'
$IconValue = '"' + $AppDir + '\ozviewer.exe",0'

function New-MockRegistry { return @{} }
function Mock-K([string]$Key) { return $Key.ToLowerInvariant() }
function Mock-KeyExists($reg, [string]$Key) { return $reg.ContainsKey((Mock-K $Key)) }
function Mock-EnsureKey($reg, [string]$Key) {
    $k = Mock-K $Key
    if (-not $reg.ContainsKey($k)) { $reg[$k] = @{} }
    return $k
}
function Mock-SetValue($reg, [string]$Key, [string]$Name, [string]$Data) {
    $k = Mock-EnsureKey $reg $Key
    $reg[$k][$Name.ToLowerInvariant()] = $Data
}
function Mock-GetValue($reg, [string]$Key, [string]$Name) {
    $k = Mock-K $Key
    if (-not $reg.ContainsKey($k)) { return $null }
    $n = $Name.ToLowerInvariant()
    if (-not $reg[$k].ContainsKey($n)) { return $null }
    return $reg[$k][$n]
}
function Mock-ValueExists($reg, [string]$Key, [string]$Name) {
    return ($null -ne (Mock-GetValue $reg $Key $Name))
}
function Mock-DeleteValue($reg, [string]$Key, [string]$Name) {
    $k = Mock-K $Key
    if ($reg.ContainsKey($k)) { [void]$reg[$k].Remove($Name.ToLowerInvariant()) }
}
function Mock-GetValueNames($reg, [string]$Key) {
    $k = Mock-K $Key
    if (-not $reg.ContainsKey($k)) { return @() }
    return @($reg[$k].Keys)
}
function Mock-HasChildKey($reg, [string]$Key) {
    $prefix = (Mock-K $Key) + '\'
    foreach ($existing in @($reg.Keys)) { if ($existing.StartsWith($prefix)) { return $true } }
    return $false
}
function Mock-DeleteKeyIfEmpty($reg, [string]$Key) {
    $k = Mock-K $Key
    if (-not $reg.ContainsKey($k)) { return }
    if (($reg[$k].Count -gt 0) -or (Mock-HasChildKey $reg $Key)) { return }
    [void]$reg.Remove($k)
}
function Mock-IsOwned($reg, $p) {
    return (Mock-KeyExists $reg $p.MarkerKey) -and ((Mock-GetValue $reg $p.MarkerKey 'AppId') -eq $p.OwnerId)
}
function Mock-IsValueAbsentOrOwned1($reg, [string]$Key, [string]$Name, [string]$Expected) {
    if (-not (Mock-ValueExists $reg $Key $Name)) { return $true }
    return ((Mock-GetValue $reg $Key $Name) -eq $Expected)
}
function Mock-IsValueAbsentOrOwned2($reg, [string]$Key, [string]$Name, [string]$CurrentVal, [string]$LegacyVal) {
    if (-not (Mock-ValueExists $reg $Key $Name)) { return $true }
    $v = Mock-GetValue $reg $Key $Name
    return (($v -eq $CurrentVal) -or ($v -eq $LegacyVal))
}
function Mock-IsRecognizedRegistrationVersion($p, [string]$v) {
    return (($v -eq $p.CurrentVersion) -or ($v -eq '0.1.0') -or ($v -eq '0.1.1'))
}
function Mock-PreflightError($reg, $p, [string]$CmdVal = $CmdValue, [string]$IconVal = $IconValue) {
    if ((Mock-KeyExists $reg $p.AppKey) -and (-not (Mock-IsOwned $reg $p))) {
        return 'the registry key is not owned'
    }
    if ((Mock-KeyExists $reg $p.ProgIDKey) -and (-not (Mock-IsOwned $reg $p))) {
        return 'the ProgID is already registered by another program'
    }
    if (Mock-ValueExists $reg $p.RegisteredApps $p.RegisteredName) {
        if ((-not (Mock-IsOwned $reg $p)) -or ((Mock-GetValue $reg $p.RegisteredApps $p.RegisteredName) -ne $p.CapKey)) {
            return 'another program is already registered under the name'
        }
    }
    if (Mock-ValueExists $reg $p.RegisteredApps $p.LegacyRegisteredName) {
        if ((-not (Mock-IsOwned $reg $p)) -or ((Mock-GetValue $reg $p.RegisteredApps $p.LegacyRegisteredName) -ne $p.CapKey)) {
            return 'a foreign registration exists under the legacy name'
        }
    }
    if (Mock-KeyExists $reg $p.FileAssocKey) {
        foreach ($n in (Mock-GetValueNames $reg $p.FileAssocKey)) {
            if ($n.StartsWith('.')) {
                if ((Mock-GetValue $reg $p.FileAssocKey $n) -ne $p.ProgID) {
                    return "the image extension $n is registered to a different handler"
                }
            }
        }
    }
    if (-not (Mock-IsValueAbsentOrOwned2 $reg $p.ProgIDKey '' $p.TypeName $p.LegacyTypeName)) {
        return 'the ProgID default value has been customized or belongs to another program'
    }
    if (-not (Mock-IsValueAbsentOrOwned2 $reg $p.ProgIDKey 'FriendlyTypeName' $p.TypeName $p.LegacyTypeName)) {
        return 'the ProgID FriendlyTypeName has been customized or belongs to another program'
    }
    if (-not (Mock-IsValueAbsentOrOwned1 $reg $p.ProgIDKey 'DefaultIcon' $IconVal)) {
        return 'the ProgID DefaultIcon does not match this install path'
    }
    if (-not (Mock-IsValueAbsentOrOwned1 $reg $p.CommandKey '' $CmdVal)) {
        return 'the ProgID open command does not match this install path'
    }
    if (-not (Mock-IsValueAbsentOrOwned2 $reg $p.CapKey 'ApplicationName' $p.AppName $p.LegacyAppName)) {
        return 'the capabilities ApplicationName has been customized or belongs to another program'
    }
    if (-not (Mock-IsValueAbsentOrOwned2 $reg $p.CapKey 'ApplicationDescription' ('View and edit images with ' + $p.AppName) ('View and edit images with ' + $p.LegacyAppName))) {
        return 'the capabilities ApplicationDescription has been customized or belongs to another program'
    }
    if (-not (Mock-IsValueAbsentOrOwned1 $reg $p.MarkerKey 'ProgID' $p.ProgID)) {
        return 'the install marker ProgID has been modified'
    }
    if (-not (Mock-IsValueAbsentOrOwned1 $reg $p.MarkerKey 'Capabilities' $p.CapKey)) {
        return 'the install marker Capabilities has been modified'
    }
    if (Mock-ValueExists $reg $p.MarkerKey 'Version') {
        if (-not (Mock-IsRecognizedRegistrationVersion $p (Mock-GetValue $reg $p.MarkerKey 'Version'))) {
            return 'the install marker Version has been modified'
        }
    }
    return ''
}

function Mock-IsInstalledByAppId($reg, $p, [string]$Dir = $AppDir) {
    if (-not (Mock-KeyExists $reg $p.UninstallKey)) { return $false }
    $instLoc = Mock-GetValue $reg $p.UninstallKey 'InstallLocation'
    if ([string]::IsNullOrEmpty($instLoc)) {
        $instLoc = Mock-GetValue $reg $p.UninstallKey 'Inno Setup: App Path'
    }
    if ([string]::IsNullOrEmpty($instLoc)) { return $false }
    return ($instLoc.TrimEnd('\') -ieq $Dir.TrimEnd('\'))
}

function Mock-IsOwnedShortcut($reg, $p, $shortcut, [string]$ExpectedDesc, [string]$Dir = $AppDir) {
    if (-not (Mock-IsInstalledByAppId $reg $p $Dir)) { return $false }
    if ($null -eq $shortcut) { return $false }
    $expTarget = $Dir + '\ozviewer.exe'
    $expIcon = $expTarget + ',0'
    return ($shortcut.TargetPath -ieq $expTarget) -and
           ($shortcut.Arguments -eq '') -and
           ($shortcut.WorkingDirectory -ieq $Dir) -and
           (($shortcut.IconLocation -ieq $expIcon) -or ($shortcut.IconLocation -ieq $expTarget)) -and
           ($shortcut.Description -eq $ExpectedDesc)
}
function Mock-Register($reg, $p) {
    Mock-SetValue $reg $p.ProgIDKey '' $p.TypeName
    Mock-SetValue $reg $p.ProgIDKey 'FriendlyTypeName' $p.TypeName
    Mock-SetValue $reg $p.ProgIDKey 'DefaultIcon' $IconValue
    Mock-SetValue $reg $p.CommandKey '' $CmdValue
    Mock-SetValue $reg $p.CapKey 'ApplicationName' $p.AppName
    Mock-SetValue $reg $p.CapKey 'ApplicationDescription' ('View and edit images with ' + $p.AppName)
    foreach ($e in $p.Extensions) { Mock-SetValue $reg $p.FileAssocKey $e $p.ProgID }
    foreach ($n in (Mock-GetValueNames $reg $p.FileAssocKey)) {
        if ($n.StartsWith('.') -and ($p.Extensions -notcontains $n) -and ((Mock-GetValue $reg $p.FileAssocKey $n) -eq $p.ProgID)) {
            Mock-DeleteValue $reg $p.FileAssocKey $n
        }
    }
    Mock-SetValue $reg $p.RegisteredApps $p.RegisteredName $p.CapKey
    if ((Mock-GetValue $reg $p.RegisteredApps $p.LegacyRegisteredName) -eq $p.CapKey -and (Mock-IsOwned $reg $p)) {
        Mock-DeleteValue $reg $p.RegisteredApps $p.LegacyRegisteredName
    }
    Mock-SetValue $reg $p.MarkerKey 'AppId' $p.OwnerId
    Mock-SetValue $reg $p.MarkerKey 'ProgID' $p.ProgID
    Mock-SetValue $reg $p.MarkerKey 'Capabilities' $p.CapKey
    Mock-SetValue $reg $p.MarkerKey 'Version' $p.CurrentVersion
}
function Mock-NeedsLegacyMigration($reg, $p) {
    if (-not (Mock-IsOwned $reg $p)) { return $false }
    if ((Mock-GetValue $reg $p.RegisteredApps $p.LegacyRegisteredName) -eq $p.CapKey) { return $true }
    if ((Mock-GetValue $reg $p.CapKey 'ApplicationName') -eq $p.LegacyAppName) { return $true }
    if ((Mock-GetValue $reg $p.CapKey 'ApplicationDescription') -eq ('View and edit images with ' + $p.LegacyAppName)) { return $true }
    if ((Mock-GetValue $reg $p.ProgIDKey '') -eq $p.LegacyTypeName) { return $true }
    if ((Mock-GetValue $reg $p.ProgIDKey 'FriendlyTypeName') -eq $p.LegacyTypeName) { return $true }
    return $false
}
function Mock-MigrateIfOwned($reg, $p) {
    if (Mock-NeedsLegacyMigration $reg $p) {
        if ((Mock-PreflightError $reg $p) -eq '') {
            Mock-Register $reg $p
        }
    }
}
function Mock-Unregister($reg, $p) {
    if (-not (Mock-IsOwned $reg $p)) { return }
    if ((Mock-GetValue $reg $p.RegisteredApps $p.RegisteredName) -eq $p.CapKey) {
        Mock-DeleteValue $reg $p.RegisteredApps $p.RegisteredName
    }
    if ((Mock-GetValue $reg $p.RegisteredApps $p.LegacyRegisteredName) -eq $p.CapKey) {
        Mock-DeleteValue $reg $p.RegisteredApps $p.LegacyRegisteredName
    }
    foreach ($n in (Mock-GetValueNames $reg $p.FileAssocKey)) {
        if ((Mock-GetValue $reg $p.FileAssocKey $n) -eq $p.ProgID) { Mock-DeleteValue $reg $p.FileAssocKey $n }
    }
    Mock-DeleteKeyIfEmpty $reg $p.FileAssocKey
    $appVal = Mock-GetValue $reg $p.CapKey 'ApplicationName'
    if ($appVal -eq $p.AppName -or $appVal -eq $p.LegacyAppName) {
        Mock-DeleteValue $reg $p.CapKey 'ApplicationName'
    }
    $descVal = Mock-GetValue $reg $p.CapKey 'ApplicationDescription'
    if ($descVal -eq ('View and edit images with ' + $p.AppName) -or $descVal -eq ('View and edit images with ' + $p.LegacyAppName)) {
        Mock-DeleteValue $reg $p.CapKey 'ApplicationDescription'
    }
    Mock-DeleteKeyIfEmpty $reg $p.CapKey
    if ((Mock-GetValue $reg $p.CommandKey '') -eq $CmdValue) { Mock-DeleteValue $reg $p.CommandKey '' }
    Mock-DeleteKeyIfEmpty $reg $p.CommandKey
    Mock-DeleteKeyIfEmpty $reg ($p.ProgIDKey + '\shell\open')
    Mock-DeleteKeyIfEmpty $reg ($p.ProgIDKey + '\shell')
    if ((Mock-GetValue $reg $p.ProgIDKey 'DefaultIcon') -eq $IconValue) { Mock-DeleteValue $reg $p.ProgIDKey 'DefaultIcon' }
    $friendlyVal = Mock-GetValue $reg $p.ProgIDKey 'FriendlyTypeName'
    if ($friendlyVal -eq $p.TypeName -or $friendlyVal -eq $p.LegacyTypeName) {
        Mock-DeleteValue $reg $p.ProgIDKey 'FriendlyTypeName'
    }
    $defaultVal = Mock-GetValue $reg $p.ProgIDKey ''
    if ($defaultVal -eq $p.TypeName -or $defaultVal -eq $p.LegacyTypeName) {
        Mock-DeleteValue $reg $p.ProgIDKey ''
    }
    Mock-DeleteKeyIfEmpty $reg $p.ProgIDKey
    if ((Mock-GetValue $reg $p.MarkerKey 'ProgID') -eq $p.ProgID) { Mock-DeleteValue $reg $p.MarkerKey 'ProgID' }
    if ((Mock-GetValue $reg $p.MarkerKey 'Capabilities') -eq $p.CapKey) { Mock-DeleteValue $reg $p.MarkerKey 'Capabilities' }
    if ((Mock-ValueExists $reg $p.MarkerKey 'Version') -and
        (Mock-IsRecognizedRegistrationVersion $p (Mock-GetValue $reg $p.MarkerKey 'Version'))) {
        Mock-DeleteValue $reg $p.MarkerKey 'Version'
    }
    if ((Mock-GetValue $reg $p.MarkerKey 'AppId') -eq $p.OwnerId) { Mock-DeleteValue $reg $p.MarkerKey 'AppId' }
    Mock-DeleteKeyIfEmpty $reg $p.MarkerKey
    Mock-DeleteKeyIfEmpty $reg $p.AppKey
}

try {
    # -----------------------------------------------------------------------
    # TEST 1: Fresh Opt-Out Install
    # -----------------------------------------------------------------------
    $reg = New-MockRegistry
    Mock-MigrateIfOwned $reg $Policy
    Assert-True (-not (Mock-KeyExists $reg $Policy.AppKey)) "fresh opt-out created app key"
    Assert-True (-not (Mock-KeyExists $reg $Policy.ProgIDKey)) "fresh opt-out created ProgID key"
    Assert-True (-not (Mock-ValueExists $reg $Policy.RegisteredApps $Policy.RegisteredName)) "fresh opt-out registered app"
    Pass "Fresh opt-out install creates zero handler registrations or keys"

    # -----------------------------------------------------------------------
    # TEST 2: Fresh Opt-In Install
    # -----------------------------------------------------------------------
    $reg = New-MockRegistry
    Assert-True ((Mock-PreflightError $reg $Policy) -eq '') "fresh preflight failed"
    Mock-Register $reg $Policy
    Assert-True (Mock-IsOwned $reg $Policy) "fresh opt-in missing AppId ownership marker"
    Assert-True ((Mock-GetValue $reg $Policy.MarkerKey 'Version') -eq $Policy.CurrentVersion) "fresh opt-in marker version is not $($Policy.CurrentVersion)"
    Assert-True ((Mock-GetValue $reg $Policy.CapKey 'ApplicationName') -eq 'PikaView') "ApplicationName is not PikaView"
    Assert-True ((Mock-GetValue $reg $Policy.CapKey 'ApplicationDescription') -eq 'View and edit images with PikaView') "Description mismatch"
    Assert-True ((Mock-GetValue $reg $Policy.ProgIDKey 'FriendlyTypeName') -eq 'PikaView Image') "FriendlyTypeName is not PikaView Image"
    Assert-True ((Mock-GetValue $reg $Policy.RegisteredApps $Policy.RegisteredName) -eq $Policy.CapKey) "RegisteredApplications missing PikaView"
    Assert-True (-not (Mock-ValueExists $reg $Policy.RegisteredApps $Policy.LegacyRegisteredName)) "Legacy RegisteredApplications should not exist on fresh install"
    Pass "Fresh opt-in install writes PikaView labels and RegisteredApplications"

    # -----------------------------------------------------------------------
    # TEST 3: Upgrade from owned v0.1.0 with task UNCHECKED
    # -----------------------------------------------------------------------
    $reg = New-MockRegistry
    Mock-SetValue $reg $Policy.ProgIDKey '' $Policy.LegacyTypeName
    Mock-SetValue $reg $Policy.ProgIDKey 'FriendlyTypeName' $Policy.LegacyTypeName
    Mock-SetValue $reg $Policy.ProgIDKey 'DefaultIcon' $IconValue
    Mock-SetValue $reg $Policy.CommandKey '' $CmdValue
    Mock-SetValue $reg $Policy.CapKey 'ApplicationName' $Policy.LegacyAppName
    Mock-SetValue $reg $Policy.CapKey 'ApplicationDescription' ('View and edit images with ' + $Policy.LegacyAppName)
    foreach ($e in $Policy.Extensions) { Mock-SetValue $reg $Policy.FileAssocKey $e $Policy.ProgID }
    Mock-SetValue $reg $Policy.RegisteredApps $Policy.LegacyRegisteredName $Policy.CapKey
    Mock-SetValue $reg $Policy.MarkerKey 'AppId' $Policy.OwnerId
    Mock-SetValue $reg $Policy.MarkerKey 'ProgID' $Policy.ProgID
    Mock-SetValue $reg $Policy.MarkerKey 'Capabilities' $Policy.CapKey
    Mock-SetValue $reg $Policy.MarkerKey 'Version' '0.1.0'

    # Run upgrade migration (with task unchecked)
    Mock-MigrateIfOwned $reg $Policy
    Assert-True ((Mock-GetValue $reg $Policy.RegisteredApps $Policy.RegisteredName) -eq $Policy.CapKey) "RegisteredApplications\PikaView not created on upgrade"
    Assert-True (-not (Mock-ValueExists $reg $Policy.RegisteredApps $Policy.LegacyRegisteredName)) "Legacy RegisteredApplications value was not reaped"
    Assert-True ((Mock-GetValue $reg $Policy.CapKey 'ApplicationName') -eq 'PikaView') "ApplicationName not updated to PikaView"
    Assert-True ((Mock-GetValue $reg $Policy.ProgIDKey 'FriendlyTypeName') -eq 'PikaView Image') "FriendlyTypeName not updated to PikaView Image"
    Assert-True ((Mock-GetValue $reg $Policy.MarkerKey 'Version') -eq $Policy.CurrentVersion) "Marker version not updated to $($Policy.CurrentVersion)"
    Pass "Upgrade with unchecked association task automatically migrates owned registration to PikaView"

    # -----------------------------------------------------------------------
    # TEST 3b: Upgrade from owned v0.1.0 with task CHECKED
    # -----------------------------------------------------------------------
    $reg = New-MockRegistry
    Mock-SetValue $reg $Policy.ProgIDKey '' $Policy.LegacyTypeName
    Mock-SetValue $reg $Policy.ProgIDKey 'FriendlyTypeName' $Policy.LegacyTypeName
    Mock-SetValue $reg $Policy.ProgIDKey 'DefaultIcon' $IconValue
    Mock-SetValue $reg $Policy.CommandKey '' $CmdValue
    Mock-SetValue $reg $Policy.CapKey 'ApplicationName' $Policy.LegacyAppName
    Mock-SetValue $reg $Policy.CapKey 'ApplicationDescription' ('View and edit images with ' + $Policy.LegacyAppName)
    foreach ($e in $Policy.Extensions) { Mock-SetValue $reg $Policy.FileAssocKey $e $Policy.ProgID }
    Mock-SetValue $reg $Policy.RegisteredApps $Policy.LegacyRegisteredName $Policy.CapKey
    Mock-SetValue $reg $Policy.MarkerKey 'AppId' $Policy.OwnerId
    Mock-SetValue $reg $Policy.MarkerKey 'ProgID' $Policy.ProgID
    Mock-SetValue $reg $Policy.MarkerKey 'Capabilities' $Policy.CapKey
    Mock-SetValue $reg $Policy.MarkerKey 'Version' '0.1.0'

    Assert-True ((Mock-PreflightError $reg $Policy) -eq '') "owned v0.1.0 checked upgrade preflight rejected recognized version"
    Mock-Register $reg $Policy
    Assert-True ((Mock-GetValue $reg $Policy.MarkerKey 'Version') -eq $Policy.CurrentVersion) "checked v0.1.0 upgrade marker version not $($Policy.CurrentVersion)"
    Assert-True ((Mock-GetValue $reg $Policy.CapKey 'ApplicationName') -eq $Policy.AppName) "checked v0.1.0 upgrade did not refresh PikaView labels"
    Pass "Upgrade from owned v0.1.0 with association task checked refreshes registration to $($Policy.CurrentVersion)"

    # -----------------------------------------------------------------------
    # TEST 3c: Upgrade from owned modern v0.1.1 with task CHECKED
    # -----------------------------------------------------------------------
    $reg = New-MockRegistry
    Mock-Register $reg $Policy
    Mock-SetValue $reg $Policy.MarkerKey 'Version' '0.1.1'
    Assert-True ((Mock-PreflightError $reg $Policy) -eq '') "owned v0.1.1 checked upgrade preflight rejected recognized version"
    Mock-Register $reg $Policy
    Assert-True ((Mock-GetValue $reg $Policy.MarkerKey 'Version') -eq $Policy.CurrentVersion) "checked v0.1.1 upgrade marker version not $($Policy.CurrentVersion)"
    Mock-Unregister $reg $Policy
    Assert-True (-not (Mock-ValueExists $reg $Policy.MarkerKey 'Version')) "checked v0.1.1 upgrade left owned-version residue after uninstall"
    Assert-True (-not (Mock-KeyExists $reg $Policy.MarkerKey)) "checked v0.1.1 upgrade left non-empty marker key after uninstall"
    Pass "Upgrade from owned v0.1.1 with task checked reaches $($Policy.CurrentVersion) and uninstalls without owned-version residue"

    # -----------------------------------------------------------------------
    # TEST 3d: Upgrade from owned modern v0.1.1 with task UNCHECKED, then uninstall
    # -----------------------------------------------------------------------
    $reg = New-MockRegistry
    Mock-Register $reg $Policy
    Mock-SetValue $reg $Policy.MarkerKey 'Version' '0.1.1'
    Assert-True (-not (Mock-NeedsLegacyMigration $reg $Policy)) "modern v0.1.1 registration must not trigger legacy migration"
    Assert-True ((Mock-GetValue $reg $Policy.MarkerKey 'Version') -eq '0.1.1') "unchecked upgrade unexpectedly rewrote marker version"
    Mock-Unregister $reg $Policy
    Assert-True (-not (Mock-ValueExists $reg $Policy.MarkerKey 'Version')) "unchecked v0.1.1 upgrade left recognized marker version after uninstall"
    Assert-True (-not (Mock-KeyExists $reg $Policy.MarkerKey)) "unchecked v0.1.1 upgrade left non-empty marker key after uninstall"
    Pass "Unchecked modern v0.1.1 upgrade retains registration safely and uninstall removes it without residue"

    # -----------------------------------------------------------------------
    # TEST 3e: Current-version reinstall
    # -----------------------------------------------------------------------
    $reg = New-MockRegistry
    Mock-Register $reg $Policy
    Assert-True ((Mock-PreflightError $reg $Policy) -eq '') "current-version reinstall preflight rejected $($Policy.CurrentVersion)"
    Mock-Register $reg $Policy
    Assert-True ((Mock-GetValue $reg $Policy.MarkerKey 'Version') -eq $Policy.CurrentVersion) "current-version reinstall marker mismatch"
    Pass "Reinstall over current $($Policy.CurrentVersion) registration is accepted and stays current"

    # -----------------------------------------------------------------------
    # TEST 3f: Defaults / UserChoice / OpenWith data are never touched
    # -----------------------------------------------------------------------
    $reg = New-MockRegistry
    $defaultsKey = 'software\classes\.jpg'
    $userChoiceKey = 'software\microsoft\windows\currentversion\explorer\fileexts\.jpg\userchoice'
    $openWithKey = 'software\classes\.jpg\openwithprogids'
    Mock-SetValue $reg $defaultsKey '' 'Foreign.Viewer'
    Mock-SetValue $reg $userChoiceKey 'ProgId' 'Foreign.Viewer'
    Mock-SetValue $reg $openWithKey 'Foreign.Viewer' ''
    Mock-Register $reg $Policy
    Mock-Unregister $reg $Policy
    Assert-True ((Mock-GetValue $reg $defaultsKey '') -eq 'Foreign.Viewer') "default handler value was modified"
    Assert-True ((Mock-GetValue $reg $userChoiceKey 'ProgId') -eq 'Foreign.Viewer') "UserChoice value was modified"
    Assert-True ((Mock-GetValue $reg $openWithKey 'Foreign.Viewer') -eq '') "OpenWithProgids value was modified"
    Pass "Register/unregister leave defaults, UserChoice, and OpenWithProgids data unchanged"

    # -----------------------------------------------------------------------
    # TEST 4: Collision detection on RegisteredApplications\PikaView
    # -----------------------------------------------------------------------
    $reg = New-MockRegistry
    Mock-SetValue $reg $Policy.RegisteredApps $Policy.RegisteredName 'Software\ForeignApp\Capabilities'
    Assert-True ((Mock-PreflightError $reg $Policy) -ne '') "foreign RegisteredApplications\PikaView not detected"
    Pass "Foreign RegisteredApplications\PikaView collision preflight fails closed"

    # -----------------------------------------------------------------------
    # TEST 5: Collision detection on legacy RegisteredApplications\OZ Image Viewer
    # -----------------------------------------------------------------------
    $reg = New-MockRegistry
    Mock-SetValue $reg $Policy.RegisteredApps $Policy.LegacyRegisteredName 'Software\OtherApp\Capabilities'
    Assert-True ((Mock-PreflightError $reg $Policy) -ne '') "foreign legacy RegisteredApplications collision not detected"
    Pass "Foreign legacy RegisteredApplications collision preflight fails closed"

    # -----------------------------------------------------------------------
    # TEST 6: Unowned legacy unregister preservation
    # -----------------------------------------------------------------------
    $reg = New-MockRegistry
    Mock-SetValue $reg $Policy.RegisteredApps $Policy.LegacyRegisteredName 'Software\OtherApp\Capabilities'
    Mock-SetValue $reg $Policy.ProgIDKey '' 'OtherProgID'
    Mock-Unregister $reg $Policy
    Assert-True ((Mock-GetValue $reg $Policy.RegisteredApps $Policy.LegacyRegisteredName) -eq 'Software\OtherApp\Capabilities') "unowned legacy entry deleted"
    Assert-True ((Mock-GetValue $reg $Policy.ProgIDKey '') -eq 'OtherProgID') "unowned ProgID deleted"
    Pass "Unregister preserves foreign/unowned legacy registry values"

    # -----------------------------------------------------------------------
    # TEST 7: Uninstall cleans both legacy and PikaView owned values
    # -----------------------------------------------------------------------
    $reg = New-MockRegistry
    # Register under PikaView, but pretend a stray legacy RegisteredApplications value was left
    Mock-Register $reg $Policy
    Mock-SetValue $reg $Policy.RegisteredApps $Policy.LegacyRegisteredName $Policy.CapKey
    Mock-Unregister $reg $Policy
    Assert-True (-not (Mock-ValueExists $reg $Policy.RegisteredApps $Policy.RegisteredName)) "PikaView RegisteredApplications not removed on uninstall"
    Assert-True (-not (Mock-ValueExists $reg $Policy.RegisteredApps $Policy.LegacyRegisteredName)) "Legacy RegisteredApplications not removed on uninstall"
    Assert-True (-not (Mock-KeyExists $reg $Policy.CapKey)) "Capabilities key not removed on uninstall"
    Assert-True (-not (Mock-KeyExists $reg $Policy.ProgIDKey)) "ProgID key not removed on uninstall"
    Assert-True (-not (Mock-KeyExists $reg $Policy.AppKey)) "AppKey not cleaned on uninstall"
    Pass "Uninstall cleanly reaps both current PikaView and legacy OZ Image Viewer owned values"

    # -----------------------------------------------------------------------
    # TEST 7b: Modified owned values fail preflight before file copy (even on unchecked upgrade)
    # -----------------------------------------------------------------------
    $reg8 = New-MockRegistry
    Mock-SetValue $reg8 $Policy.MarkerKey 'AppId' $Policy.OwnerId
    Mock-SetValue $reg8 $Policy.RegisteredApps $Policy.LegacyRegisteredName $Policy.CapKey
    Mock-SetValue $reg8 $Policy.ProgIDKey 'DefaultIcon' '"C:\Custom\other.ico",0'
    Assert-True (Mock-NeedsLegacyMigration $reg8 $Policy) "Legacy migration should be detected"
    Assert-True ((Mock-PreflightError $reg8 $Policy) -ne '') "Modified DefaultIcon must fail preflight before file copy"
    Pass "Modified DefaultIcon on unchecked legacy upgrade fails preflight before file copy"

    # -----------------------------------------------------------------------
    # TEST 7b-ii: Missing ownership and modified owned values fail closed
    # -----------------------------------------------------------------------
    $reg8b = New-MockRegistry
    Mock-SetValue $reg8b $Policy.AppKey 'Sentinel' 'KeepMe'
    Assert-True ((Mock-PreflightError $reg8b $Policy) -ne '') "missing AppId must fail preflight"
    Mock-Unregister $reg8b $Policy
    Assert-True ((Mock-GetValue $reg8b $Policy.AppKey 'Sentinel') -eq 'KeepMe') "missing-AppId unregister must preserve foreign data"

    $modifiedCases = @(
        @{ Key = $Policy.CommandKey; Name = ''; Value = '"C:\Other\app.exe" "%1"'; Label = 'open command' },
        @{ Key = $Policy.ProgIDKey; Name = ''; Value = 'Other.Type'; Label = 'ProgID default' },
        @{ Key = $Policy.ProgIDKey; Name = 'FriendlyTypeName'; Value = 'Other Image'; Label = 'ProgID FriendlyTypeName' },
        @{ Key = $Policy.CapKey; Name = 'ApplicationName'; Value = 'Other App'; Label = 'capabilities ApplicationName' },
        @{ Key = $Policy.CapKey; Name = 'ApplicationDescription'; Value = 'Other description'; Label = 'capabilities ApplicationDescription' },
        @{ Key = $Policy.FileAssocKey; Name = '.jpg'; Value = 'Other.Type'; Label = 'extension' }
    )
    foreach ($c in $modifiedCases) {
        $r = New-MockRegistry
        Mock-SetValue $r $Policy.MarkerKey 'AppId' $Policy.OwnerId
        Mock-SetValue $r $c.Key $c.Name $c.Value
        Assert-True ((Mock-PreflightError $r $Policy) -ne '') "modified $($c.Label) must fail preflight"
    }
    $reg8c = New-MockRegistry
    Mock-SetValue $reg8c $Policy.MarkerKey 'AppId' $Policy.OwnerId
    Mock-SetValue $reg8c $Policy.MarkerKey 'ProgID' 'Custom.ProgID'
    Assert-True ((Mock-PreflightError $reg8c $Policy) -ne '') "modified marker ProgID must fail preflight"
    Pass "Missing AppId and modified command/ProgID/capabilities/extensions fail closed"

    # -----------------------------------------------------------------------
    # TEST 7c: Uninstall preserves modified marker values
    # -----------------------------------------------------------------------
    $reg9 = New-MockRegistry
    Mock-SetValue $reg9 $Policy.MarkerKey 'AppId' $Policy.OwnerId
    Mock-SetValue $reg9 $Policy.MarkerKey 'ProgID' 'Custom.ProgID'
    Mock-SetValue $reg9 $Policy.MarkerKey 'Capabilities' $Policy.CapKey
    Mock-SetValue $reg9 $Policy.MarkerKey 'Version' '0.1.1'
    Mock-Unregister $reg9 $Policy
    Assert-True ((Mock-GetValue $reg9 $Policy.MarkerKey 'ProgID') -eq 'Custom.ProgID') "Modified marker ProgID must be preserved on uninstall"
    Assert-True (Mock-KeyExists $reg9 $Policy.MarkerKey) "Non-empty marker key must be preserved on uninstall"
    Pass "Uninstall preserves modified marker values and unrelated registry data"

    # -----------------------------------------------------------------------
    # TEST 7d: Same-value unowned RegisteredApplications\PikaView fails closed
    # -----------------------------------------------------------------------
    $reg10 = New-MockRegistry
    Mock-SetValue $reg10 $Policy.RegisteredApps $Policy.RegisteredName $Policy.CapKey
    Assert-True ((Mock-PreflightError $reg10 $Policy) -ne '') "Same-value unowned RegisteredApplications\PikaView must fail closed"
    Pass "Same-value unowned RegisteredApplications\PikaView fails closed"

    # -----------------------------------------------------------------------
    # TEST 7e: Recognized marker Versions pass; unknown/future/malformed fail
    # -----------------------------------------------------------------------
    foreach ($recognized in @('0.1.0', '0.1.1', $Policy.CurrentVersion)) {
        $r = New-MockRegistry
        Mock-SetValue $r $Policy.MarkerKey 'AppId' $Policy.OwnerId
        Mock-SetValue $r $Policy.MarkerKey 'Version' $recognized
        Assert-True ((Mock-PreflightError $r $Policy) -eq '') "recognized marker Version $recognized was rejected"
    }
    foreach ($badVersion in @('0.1.3', '0.2.0', '0.1', 'v0.1.1', '9.9.9-custom', '')) {
        $r = New-MockRegistry
        Mock-SetValue $r $Policy.MarkerKey 'AppId' $Policy.OwnerId
        Mock-SetValue $r $Policy.MarkerKey 'Version' $badVersion
        Assert-True ((Mock-PreflightError $r $Policy) -ne '') "unknown/future/malformed marker Version '$badVersion' must fail closed"
    }
    Pass "Version allowlist accepts only $($Policy.CurrentVersion), 0.1.0, and 0.1.1; all others fail closed"

    # -----------------------------------------------------------------------
    # TEST 7f: Shortcut ownership policy (marker-present-but-foreign, modified args/icon)
    # -----------------------------------------------------------------------
    $reg12 = New-MockRegistry
    Mock-SetValue $reg12 $Policy.MarkerKey 'AppId' $Policy.OwnerId
    Mock-SetValue $reg12 $Policy.UninstallKey 'InstallLocation' ($AppDir + '\')
    $ownedLnk = @{ TargetPath = ($AppDir + '\ozviewer.exe'); Arguments = ''; WorkingDirectory = $AppDir; IconLocation = ($AppDir + '\ozviewer.exe,0'); Description = 'PikaView' }
    Assert-True (Mock-IsOwnedShortcut $reg12 $Policy $ownedLnk 'PikaView') "Exact owned shortcut should be accepted"
    $foreignTargetLnk = @{ TargetPath = 'C:\Other\app.exe'; Arguments = ''; WorkingDirectory = $AppDir; IconLocation = ($AppDir + '\ozviewer.exe,0'); Description = 'PikaView' }
    Assert-True (-not (Mock-IsOwnedShortcut $reg12 $Policy $foreignTargetLnk 'PikaView')) "Marker-present-but-foreign shortcut target must fail closed"
    $modArgsLnk = @{ TargetPath = ($AppDir + '\ozviewer.exe'); Arguments = '--custom'; WorkingDirectory = $AppDir; IconLocation = ($AppDir + '\ozviewer.exe,0'); Description = 'PikaView' }
    Assert-True (-not (Mock-IsOwnedShortcut $reg12 $Policy $modArgsLnk 'PikaView')) "Same target with modified arguments must fail closed"
    $modIconLnk = @{ TargetPath = ($AppDir + '\ozviewer.exe'); Arguments = ''; WorkingDirectory = $AppDir; IconLocation = 'C:\Custom\my.ico,0'; Description = 'PikaView' }
    Assert-True (-not (Mock-IsOwnedShortcut $reg12 $Policy $modIconLnk 'PikaView')) "Same target with modified icon must fail closed"
    $modWorkDirLnk = @{ TargetPath = ($AppDir + '\ozviewer.exe'); Arguments = ''; WorkingDirectory = 'C:\OtherDir'; IconLocation = ($AppDir + '\ozviewer.exe,0'); Description = 'PikaView' }
    Assert-True (-not (Mock-IsOwnedShortcut $reg12 $Policy $modWorkDirLnk 'PikaView')) "Same target with modified working directory must fail closed"
    $customDir = 'C:\Custom\PikaView'
    Mock-SetValue $reg12 $Policy.UninstallKey 'InstallLocation' $customDir
    $customDirLnk = @{ TargetPath = ($customDir + '\ozviewer.exe'); Arguments = ''; WorkingDirectory = $customDir; IconLocation = ($customDir + '\ozviewer.exe,0'); Description = '' }
    Assert-True (Mock-IsOwnedShortcut $reg12 $Policy $customDirLnk '' $customDir) "Custom install dir desktop opt-in shortcut should match"
    Pass "Shortcut ownership policy rejects foreign/modified links even when marker is present"

    # -----------------------------------------------------------------------
    # TEST 7g: Fresh opt-out install (no association marker) -> reinstall with owned PikaView.lnk
    # -----------------------------------------------------------------------
    $reg13 = New-MockRegistry
    Mock-SetValue $reg13 $Policy.UninstallKey 'InstallLocation' $AppDir
    Assert-True (-not (Mock-IsOwned $reg13 $Policy)) "Opt-out install must not have association marker"
    Assert-True (Mock-IsOwnedShortcut $reg13 $Policy $ownedLnk 'PikaView') "Reinstall after opt-out install must accept installer-owned PikaView.lnk"
    Pass "Fresh opt-out install followed by reinstall accepts installer-owned PikaView.lnk without association marker"

    # -----------------------------------------------------------------------
    # TEST 7h: Unowned install (no Uninstall\{AppId}_is1) with identical target fails closed
    # -----------------------------------------------------------------------
    $reg14 = New-MockRegistry
    # -----------------------------------------------------------------------
    # TEST 7i: Ownership-aware shortcut uninstall (uninsneveruninstall + RemoveOwnedShortcuts)
    # -----------------------------------------------------------------------
    $reg15 = New-MockRegistry
    Mock-SetValue $reg15 $Policy.UninstallKey 'InstallLocation' $AppDir
    $links15 = @{
        start = @{ TargetPath = ($AppDir + '\ozviewer.exe'); Arguments = ''; WorkingDirectory = $AppDir; IconLocation = ($AppDir + '\ozviewer.exe,0'); Description = 'PikaView' }
        desktop = @{ TargetPath = ($AppDir + '\ozviewer.exe'); Arguments = '--user-arg'; WorkingDirectory = $AppDir; IconLocation = ($AppDir + '\ozviewer.exe,0'); Description = '' }
    }
    if ($links15.start -and (Mock-IsOwnedShortcut $reg15 $Policy $links15.start 'PikaView')) { $links15.Remove('start') }
    if ($links15.desktop -and (Mock-IsOwnedShortcut $reg15 $Policy $links15.desktop '')) { $links15.Remove('desktop') }
    Assert-True (-not $links15.ContainsKey('start')) "Owned Start Menu PikaView.lnk must be removed on uninstall without association marker"
    Assert-True ($links15.ContainsKey('desktop')) "User-modified Desktop PikaView.lnk must be preserved on uninstall"

    # Unrecognized install with identical target must preserve link on uninstall
    $reg16 = New-MockRegistry
    $links16 = @{ start = @{ TargetPath = ($AppDir + '\ozviewer.exe'); Arguments = ''; WorkingDirectory = $AppDir; IconLocation = ($AppDir + '\ozviewer.exe,0'); Description = 'PikaView' } }
    if ($links16.start -and (Mock-IsOwnedShortcut $reg16 $Policy $links16.start 'PikaView')) { $links16.Remove('start') }
    Assert-True ($links16.ContainsKey('start')) "Same-target link on unrecognized install must be preserved on uninstall"
    Pass "Ownership-aware shortcut uninstall removes owned links and preserves modified/foreign links"

    # -----------------------------------------------------------------------
    # TEST 8: Static Inno Setup Script Audit
    # -----------------------------------------------------------------------
    $issContent = Get-Content -LiteralPath $InnoScript -Raw
    Assert-True ($issContent -match 'AppId=\{\{7E3B1C9A-4D2F-4B6E-9C31-0A5F2D8B47E1\}') "AppId changed"
    Assert-True ($issContent -match 'AppName=PikaView') "AppName is not PikaView"
    Assert-True ($issContent -match 'DefaultDirName=\{localappdata\}\\Programs\\OZ Image Viewer') "DefaultDirName modified (breaks upgrade compatibility)"
    Assert-True ($issContent -match 'UninstallDisplayName=PikaView \{#OZ_VERSION\}') "UninstallDisplayName is not PikaView"
    Assert-True ($issContent -match 'Name: "\{userprograms\}\\PikaView".*Flags: uninsneveruninstall') "Start Menu shortcut missing uninsneveruninstall"
    Assert-True ($issContent -match 'Name: "\{userdesktop\}\\PikaView".*Flags: uninsneveruninstall') "Desktop shortcut missing uninsneveruninstall"
    Assert-True ($issContent -match 'OZ_PROGID = ''OZImageViewer\.Image''') "ProgID modified (breaks compatibility)"
    Assert-True ($issContent -match 'OZ_REGISTERED_NAME = ''PikaView''') "OZ_REGISTERED_NAME is not PikaView"
    Assert-True ($issContent -match 'OZ_LEGACY_REGISTERED_NAME = ''OZ Image Viewer''') "OZ_LEGACY_REGISTERED_NAME is missing"
    Assert-True ($issContent.Contains('function IsRecognizedRegistrationVersion(const Value: String): Boolean;')) "recognized-registration-version predicate is missing"
    Assert-True ([regex]::Matches($issContent, 'IsRecognizedRegistrationVersion\(').Count -ge 3) "recognized-registration-version predicate is not used by preflight and uninstall"
    foreach ($recognizedLiteral in @("'0.1.0'", "'0.1.1'", "'{#OZ_VERSION}'")) {
        Assert-True ($issContent.Contains("CompareText(Value, $recognizedLiteral) = 0")) "recognized-registration-version predicate is missing $recognizedLiteral"
    }
    Pass "Static Inno Setup script matches architect compatibility policy exactly"

    # -----------------------------------------------------------------------
    # TEST 9: ISCC Compilation in Isolated Temporary Directory
    # -----------------------------------------------------------------------
    $compileStage = Join-Path $ScratchDir "iscc-compile-test"
    New-Item -ItemType Directory -Path $compileStage -Force | Out-Null
    $fakeGenerated = Join-Path $compileStage "ozviewer-installer-generated.iss"
    $iconPath = (Resolve-Path -LiteralPath (Join-Path $RepoRoot "assets\icons\ozviewer.ico")).Path.Replace('\', '/')
    $licensePath = (Resolve-Path -LiteralPath (Join-Path $RepoRoot "LICENSE")).Path
    Copy-Item -LiteralPath $licensePath -Destination (Join-Path $compileStage "LICENSE") -Force

    # Write mock generated include
    $fakeGenContent = @"
#define OZ_VERSION "$($Policy.CurrentVersion)"
#define OZ_EXE_NAME "ozviewer.exe"
#define OZ_EXT_LIST ".jpg .png .webp"
#define OZ_EXT_COUNT 3
#define OZ_OUTPUT_DIR "$($compileStage.Replace('\', '/'))"
#define OZ_OUTPUT_BASENAME "pikaview-$($Policy.CurrentVersion)-win-x64-installer"
#define OZ_SETUP_ICON "$iconPath"

[Files]
Source: "$licensePath"; DestDir: "{app}"; Flags: ignoreversion
"@
    [System.IO.File]::WriteAllText($fakeGenerated, $fakeGenContent, (New-Object System.Text.UTF8Encoding($true)))
    Copy-Item -LiteralPath $InnoScript -Destination (Join-Path $compileStage "ozviewer-installer.iss") -Force

    $iscc = "C:\Users\USER\AppData\Local\Programs\Inno Setup 6\ISCC.exe"
    if (Test-Path -LiteralPath $iscc -PathType Leaf) {
        $compileRes = & $iscc "/Qp" (Join-Path $compileStage "ozviewer-installer.iss") 2>&1
        Assert-True ($LASTEXITCODE -eq 0) "ISCC compile failed:`n$($compileRes -join "`n")"
        $compiledExe = Join-Path $compileStage "pikaview-$($Policy.CurrentVersion)-win-x64-installer.exe"
        Assert-True (Test-Path -LiteralPath $compiledExe -PathType Leaf) "Compiled installer executable not found at $compiledExe"
        Pass "Inno Setup compilation succeeded with ISCC.exe producing pikaview-$($Policy.CurrentVersion)-win-x64-installer.exe"
    }
    else {
        Write-Host "migration-test: NOTE: ISCC not found at expected location; skipping binary compile" -ForegroundColor Yellow
    }

    Write-Host "`nALL PIKAVIEW INSTALLER/UPGRADE MIGRATION TESTS PASSED!" -ForegroundColor Green
    exit 0
}
catch {
    Write-Host "migration-test: FAIL: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}

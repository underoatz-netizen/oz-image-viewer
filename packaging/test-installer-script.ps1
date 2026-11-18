<#
.SYNOPSIS
  Focused tests for the Stage 9.10 installer packaging boundary.

.DESCRIPTION
  Covers, without ever running a real installer or touching the host registry:
    * manifest/hash validation happy path and fail-closed mutations (schema,
      version, platform, entrypoint, extension tokens, unknown/duplicate
      extensions, file SHA-256, missing file, undeclared file, excluded
      artifact, expected payload hash mismatch, missing expected hash);
    * strict required provenance bindings: missing/wrong ExpectedCommitSha,
      ExpectedTreeSha, and ExpectedLgplSourceSha256 fail closed in both the
      validator and build-installer.ps1;
    * installer build generation from a validated payload, including exact
      payload-only [Files] coverage and quoting;
    * static assertions on the Inno script: stable AppId, per-user lowest
      privileges, x64 restriction, {localappdata}\Programs, unchecked opt-in
      association task, quoted icon/open command, ownership marker,
      RegDeleteKeyIfEmpty cleanup, and the absence of prohibited registry
      behavior ([Registry] section, HKLM/HKCR, UserChoice, OpenWithProgids,
      extension default keys, RegDeleteKeyIncludingSubkeys);
    * extension mapping in the generated include exactly matches the validated
      manifest;
    * Unicode/space payload-path handling;
    * a mock-registry model of the ownership collision/upgrade/uninstall policy;
    * the unsigned distribution statement;
    * a real ISCC compile only when Inno Setup is locally available (otherwise
      the precise limitation is recorded).

  The mock-registry model mirrors the [Code] policy; the static assertions
  cover the actual .iss source. Neither substitutes for a real clean-machine
  install/upgrade/uninstall run, which is an explicit evidence gap.

  The payload, BUILD-PROVENANCE.json, and fixture repository are deterministic
  SYNTHETIC contract-test inputs. They are never real build evidence or release
  provenance. package-portable.ps1's clean-source release gate is exercised
  unchanged against the throwaway fixture repository, not weakened.

.PARAMETER WorkDir
  Scratch directory for the synthetic fixture and generated packages.

.NOTES
  Exit codes: 0 = pass, 77 = skipped, 1 = failure.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$WorkDir
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Split-Path -Parent $ScriptDir
$FixtureHelper = Join-Path $ScriptDir 'test-contract-fixtures.ps1'
if (-not (Test-Path -LiteralPath $FixtureHelper -PathType Leaf)) {
    Write-Host "test-installer: SKIP: contract fixture helper not found: $FixtureHelper" -ForegroundColor Yellow
    exit 77
}
. $FixtureHelper
$InnoScript = Join-Path $ScriptDir 'ozviewer-installer.iss'
$DocsPath = Join-Path $RepoRoot 'docs\stage9-installer.md'

function Skip([string]$Message) {
    Write-Host "test-installer: SKIP: $Message" -ForegroundColor Yellow
    exit 77
}
function Pass([string]$Message) { Write-Host "test-installer: PASS: $Message" -ForegroundColor Green }
function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "ASSERT FAILED: $Message" }
}
function Assert-Contains([string]$Text, [string]$Needle, [string]$Message) {
    if ($Text.IndexOf($Needle, [StringComparison]::Ordinal) -lt 0) {
        throw "ASSERT FAILED: $Message (missing: $Needle)"
    }
}
function Assert-NotContains([string]$Text, [string]$Needle, [string]$Message) {
    if ($Text.IndexOf($Needle, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
        throw "ASSERT FAILED: $Message (found: $Needle)"
    }
}

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Skip "git is required to construct the clean contract-test fixture repository"
}

if (-not (Test-Path -LiteralPath $WorkDir -PathType Container)) {
    New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null
}
$WorkDir = (Resolve-Path -LiteralPath $WorkDir).Path
# Unique run scope so repeated CTest invocations never collide with or delete
# a previous test run's artifacts.
$WorkDir = Join-Path $WorkDir ('run-' + [System.Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null

# ---------------------------------------------------------------------------
# Synthetic contract-test fixture setup. Never real build evidence.
# ---------------------------------------------------------------------------
$Fixture = New-TestPackagingFixture -Root (Join-Path $WorkDir 'contract-fixture') `
    -SourceScriptDir $ScriptDir -SourceRepoRoot $RepoRoot
$PayloadDir = Join-Path $WorkDir 'contract-payload'
New-TestPayload -Dir $PayloadDir
$LgplAsset = Join-Path $WorkDir 'pikaview-0.1.1-lgpl-corresponding-source.zip'
$LgplSha256 = New-TestLgplFixtureAsset -Path $LgplAsset
$Provenance = New-TestBuildProvenance -Path (Join-Path $WorkDir 'BUILD-PROVENANCE.json') `
    -CommitSha $Fixture.Head -TreeSha $Fixture.Tree -PayloadDir $PayloadDir -LgplAssetPath $LgplAsset
$Validator = Join-Path $Fixture.PackagingDir 'validate-portable-payload.ps1'
$Builder = Join-Path $Fixture.PackagingDir 'build-installer.ps1'
$PackageScript = Join-Path $Fixture.PackagingDir 'package-portable.ps1'
$ExpectedCommitSha = $Fixture.Head
$ExpectedTreeSha = $Fixture.Tree
$ExpectedLgplSourceSha256 = $LgplSha256
Write-Host "test-installer: NOTE: using synthetic contract-test fixture repo $($Fixture.RepoRoot) at $($Fixture.Head) (not a real build)" -ForegroundColor Yellow

function Invoke-Validator {
    param(
        [string]$Pkg,
        [string]$Zip = "",
        [string]$Expected = "",
        [string]$ResultPath = "",
        [string]$CommitSha = "",
        [string]$TreeSha = "",
        [string]$LgplSha256 = ""
    )
    $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $Validator, '-PackageDir', $Pkg)
    if (-not [string]::IsNullOrWhiteSpace($Zip)) { $a += @('-ZipPath', $Zip) }
    if (-not [string]::IsNullOrWhiteSpace($Expected)) { $a += @('-ExpectedSha256', $Expected) }
    if (-not [string]::IsNullOrWhiteSpace($ResultPath)) { $a += @('-ResultPath', $ResultPath) }
    if (-not [string]::IsNullOrWhiteSpace($CommitSha)) { $a += @('-ExpectedCommitSha', $CommitSha) }
    if (-not [string]::IsNullOrWhiteSpace($TreeSha)) { $a += @('-ExpectedTreeSha', $TreeSha) }
    if (-not [string]::IsNullOrWhiteSpace($LgplSha256)) { $a += @('-ExpectedLgplSourceSha256', $LgplSha256) }
    $o = & powershell @a 2>&1
    return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Output = ($o -join "`n") }
}

function Invoke-BuildInstaller {
    param(
        [string]$Pkg,
        [string]$Out,
        [string]$Zip = "",
        [string]$Expected = "",
        [string]$CommitSha = "",
        [string]$TreeSha = "",
        [string]$LgplSha256 = "",
        [switch]$NoCompile,
        [switch]$RequireCompiler
    )
    $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $Builder,
           '-PackageDir', $Pkg, '-OutDir', $Out)
    if (-not [string]::IsNullOrWhiteSpace($Zip)) { $a += @('-ZipPath', $Zip) }
    if (-not [string]::IsNullOrWhiteSpace($Expected)) { $a += @('-ExpectedSha256', $Expected) }
    if (-not [string]::IsNullOrWhiteSpace($CommitSha)) { $a += @('-ExpectedCommitSha', $CommitSha) }
    if (-not [string]::IsNullOrWhiteSpace($TreeSha)) { $a += @('-ExpectedTreeSha', $TreeSha) }
    if (-not [string]::IsNullOrWhiteSpace($LgplSha256)) { $a += @('-ExpectedLgplSourceSha256', $LgplSha256) }
    if ($NoCompile) { $a += '-NoCompile' }
    if ($RequireCompiler) { $a += '-RequireCompiler' }
    $o = & powershell @a 2>&1
    return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Output = ($o -join "`n") }
}

function New-PayloadCopy {
    param([string]$Name, [switch]$WithoutZip)
    $root = Join-Path $WorkDir $Name
    if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    $pkg = Join-Path $root 'pkg'
    Copy-Item -LiteralPath $StageDir -Destination $pkg -Recurse -Force
    if (-not $WithoutZip) {
        Copy-Item -LiteralPath $ZipPath -Destination "$pkg.zip" -Force
        if (Test-Path -LiteralPath "$ZipPath.sha256" -PathType Leaf) {
            Copy-Item -LiteralPath "$ZipPath.sha256" -Destination "$pkg.zip.sha256" -Force
        }
    }
    return $pkg
}

function Edit-Manifest {
    param([string]$Pkg, [scriptblock]$Mutate)
    $mp = Join-Path $Pkg 'package-manifest.json'
    $m = Get-Content -LiteralPath $mp -Raw | ConvertFrom-Json
    & $Mutate $m
    [System.IO.File]::WriteAllText($mp, ($m | ConvertTo-Json -Depth 8), (New-Object System.Text.UTF8Encoding($false)))
}

function Get-Manifest {
    param([string]$Pkg)
    return (Get-Content -LiteralPath (Join-Path $Pkg 'package-manifest.json') -Raw | ConvertFrom-Json)
}

# ---------------------------------------------------------------------------
# Mock registry model of the [Code] ownership policy (see docs).
# ---------------------------------------------------------------------------
$Policy = @{
    ProgID             = 'OZImageViewer.Image'
    ProgIDKey          = 'software\classes\ozimageviewer.image'
    CommandKey         = 'software\classes\ozimageviewer.image\shell\open\command'
    OwnerId            = '{7E3B1C9A-4D2F-4B6E-9C31-0A5F2D8B47E1}'
    AppKey             = 'software\ozimageviewer'
    MarkerKey          = 'software\ozimageviewer\install'
    CapKey             = 'software\ozimageviewer\capabilities'
    FileAssocKey       = 'software\ozimageviewer\capabilities\fileassociations'
    RegisteredApps     = 'software\registeredapplications'
    RegisteredName     = 'pikaview'
    LegacyRegisteredName = 'oz image viewer'
    AppName            = 'PikaView'
    LegacyAppName      = 'OZ Image Viewer'
    TypeName           = 'pikaview image'
    LegacyTypeName     = 'oz image viewer image'
    Extensions         = @('.jpg', '.png')
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
function Mock-PreflightError($reg, $p) {
    if ((Mock-KeyExists $reg $p.AppKey) -and (-not (Mock-IsOwned $reg $p))) {
        return 'the registry key is not owned'
    }
    if ((Mock-KeyExists $reg $p.ProgIDKey) -and (-not (Mock-IsOwned $reg $p))) {
        return 'the ProgID is already registered by another program'
    }
    if (Mock-ValueExists $reg $p.RegisteredApps $p.RegisteredName) {
        if ((Mock-GetValue $reg $p.RegisteredApps $p.RegisteredName) -ne $p.CapKey) {
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
    return ''
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
    Mock-SetValue $reg $p.MarkerKey 'Version' '0.1.1'
}
function Mock-MigrateIfOwned($reg, $p) {
    if (-not (Mock-IsOwned $reg $p)) { return }
    $needsMigration = $false
    if ((Mock-GetValue $reg $p.RegisteredApps $p.LegacyRegisteredName) -eq $p.CapKey) { $needsMigration = $true }
    if ((Mock-GetValue $reg $p.CapKey 'ApplicationName') -eq $p.LegacyAppName) { $needsMigration = $true }
    if ($needsMigration) {
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
    foreach ($v in @('AppId', 'ProgID', 'Capabilities', 'Version')) { Mock-DeleteValue $reg $p.MarkerKey $v }
    Mock-DeleteKeyIfEmpty $reg $p.MarkerKey
    Mock-DeleteKeyIfEmpty $reg $p.AppKey
}

try {
    # --- 0. stage a validated portable package from the contract fixture ------
    $pkgOut = Join-Path $WorkDir 'portable'
    $pkgArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PackageScript,
                 '-PayloadDir', $PayloadDir, '-OutDir', $pkgOut, '-ProvenancePath', $Provenance.Path)
    $pkgResult = & powershell @pkgArgs 2>&1
    Assert-True ($LASTEXITCODE -eq 0) "package-portable.ps1 failed:`n$($pkgResult -join "`n")"
    $stageLine = @($pkgResult | Where-Object { $_ -like 'PACKAGE_STAGE=*' })
    $zipLine = @($pkgResult | Where-Object { $_ -like 'PACKAGE_ZIP=*' })
    Assert-True ($stageLine.Count -ge 1) "package-portable.ps1 did not report PACKAGE_STAGE"
    Assert-True ($zipLine.Count -ge 1) "package-portable.ps1 did not report PACKAGE_ZIP"
    $StageDir = [string]($stageLine[0] -replace '^PACKAGE_STAGE=', '')
    $ZipPath = [string]($zipLine[0] -replace '^PACKAGE_ZIP=', '')
    Pass "staged synthetic contract-test portable package: $StageDir"
    Assert-True (Test-Path -LiteralPath $StageDir -PathType Container) "staged package not found: $StageDir"
    Assert-True (Test-Path -LiteralPath $ZipPath -PathType Leaf) "portable ZIP not found: $ZipPath"
    $manifest = Get-Manifest -Pkg $StageDir
    $expectedExtensions = @($manifest.supportedExtensions)
    $expectedFiles = @($manifest.files | ForEach-Object { [string]$_.name }) + @('package-manifest.json')

    # --- 1. validator happy path --------------------------------------------
    $resultPath = Join-Path $WorkDir 'validate-ok.json'
    $r = Invoke-Validator -Pkg $StageDir -Zip $ZipPath -ResultPath $resultPath -CommitSha $ExpectedCommitSha -TreeSha $ExpectedTreeSha -LgplSha256 $ExpectedLgplSourceSha256
    Assert-True ($r.ExitCode -eq 0) "validator happy path failed:`n$($r.Output)"
    Assert-True (Test-Path -LiteralPath $resultPath) "validator did not write result evidence"
    $vr = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json
    Assert-True ($vr.entrypoint -eq 'ozviewer.exe') "validator result entrypoint mismatch"
    Assert-True (@($vr.files).Count -eq @($manifest.files).Count) "validator result file count mismatch"
    foreach ($e in $expectedExtensions) {
        Assert-True (@($vr.supportedExtensions) -contains $e) "validator result missing extension $e"
    }
    Pass "validator happy path ($($expectedFiles.Count) files, $(@($expectedExtensions).Count) extensions)"

    # --- 2. fail-closed mutations -------------------------------------------
    $mutations = @(
        @{ Name = 'schema';      Match = 'schema';     Edit = { param($m) $m.schema = 99 } },
        @{ Name = 'version';     Match = 'version';    Edit = { param($m) $m.version = '9.9.9' } },
        @{ Name = 'platform';    Match = 'platform';   Edit = { param($m) $m.platform = 'win-x86' } },
        @{ Name = 'entrypoint';  Match = 'entrypoint'; Edit = { param($m) $m.entrypoint = 'nope.exe' } },
        @{ Name = 'ext-token';   Match = 'extension token'; Edit = { param($m) $m.supportedExtensions[0] = '.TOOLONG' } },
        @{ Name = 'ext-allow';   Match = 'allowlist';  Edit = { param($m) $m.supportedExtensions[0] = '.xyz' } },
        @{ Name = 'ext-dup';     Match = 'duplicate';  Edit = { param($m) $m.supportedExtensions[1] = $m.supportedExtensions[0] } }
    )
    foreach ($case in $mutations) {
        $pkg = New-PayloadCopy -Name ("mut-" + $case.Name)
        Edit-Manifest -Pkg $pkg -Mutate $case.Edit
        $rr = Invoke-Validator -Pkg $pkg -CommitSha $ExpectedCommitSha -TreeSha $ExpectedTreeSha -LgplSha256 $ExpectedLgplSourceSha256
        Assert-True ($rr.ExitCode -ne 0) "validator accepted a $($case.Name) mutation"
        Assert-True ($rr.Output -match [regex]::Escape($case.Match)) "validator error did not mention '$($case.Match)' for $($case.Name):`n$($rr.Output)"
    }
    Pass "validator rejects schema/version/platform/entrypoint/extension mutations"

    # --- 3. hash / set fail-closed mutations --------------------------------
    $pkg = New-PayloadCopy -Name 'mut-hash'
    $target = Join-Path $pkg 'heif.dll'
    $bytes = [System.IO.File]::ReadAllBytes($target)
    $bytes[128] = $bytes[128] -bxor 0xFF
    [System.IO.File]::WriteAllBytes($target, $bytes)
    $rr = Invoke-Validator -Pkg $pkg -CommitSha $ExpectedCommitSha -TreeSha $ExpectedTreeSha -LgplSha256 $ExpectedLgplSourceSha256
    Assert-True ($rr.ExitCode -ne 0) "validator accepted a corrupted payload file"
    Assert-True ($rr.Output -match 'SHA-256 mismatch') "corruption error did not mention SHA-256:`n$($rr.Output)"
    Pass "validator fails closed on a corrupted payload file (exact hash)"

    $pkg = New-PayloadCopy -Name 'mut-missing'
    Remove-Item -LiteralPath (Join-Path $pkg 'RUNNING.txt') -Force
    $rr = Invoke-Validator -Pkg $pkg -CommitSha $ExpectedCommitSha -TreeSha $ExpectedTreeSha -LgplSha256 $ExpectedLgplSourceSha256
    Assert-True ($rr.ExitCode -ne 0) "validator accepted a missing payload file"
    Assert-True ($rr.Output -match 'missing') "missing-file error not reported:`n$($rr.Output)"
    Pass "validator fails closed on a missing payload file"

    $pkg = New-PayloadCopy -Name 'mut-extra'
    [System.IO.File]::WriteAllText((Join-Path $pkg 'stray.bin'), 'undeclared')
    $rr = Invoke-Validator -Pkg $pkg -CommitSha $ExpectedCommitSha -TreeSha $ExpectedTreeSha -LgplSha256 $ExpectedLgplSourceSha256
    Assert-True ($rr.ExitCode -ne 0) "validator accepted an undeclared payload file"
    Assert-True ($rr.Output -match 'undeclared') "undeclared-file error not reported:`n$($rr.Output)"
    Pass "validator fails closed on an undeclared (non-payload) file"

    $pkg = New-PayloadCopy -Name 'mut-excluded'
    $excludedPath = Join-Path $pkg 'debug.pdb'
    Copy-Item -LiteralPath (Join-Path $pkg 'ozviewer.exe') -Destination $excludedPath -Force
    $exItem = Get-Item -LiteralPath $excludedPath
    $exHash = (Get-FileHash -LiteralPath $excludedPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $m = Get-Manifest -Pkg $pkg
    $newEntry = [pscustomobject]@{ name = 'debug.pdb'; bytes = [long]$exItem.Length; sha256 = $exHash }
    $m.files = @($m.files) + $newEntry
    [System.IO.File]::WriteAllText((Join-Path $pkg 'package-manifest.json'), ($m | ConvertTo-Json -Depth 8), (New-Object System.Text.UTF8Encoding($false)))
    $rr = Invoke-Validator -Pkg $pkg -CommitSha $ExpectedCommitSha -TreeSha $ExpectedTreeSha -LgplSha256 $ExpectedLgplSourceSha256
    Assert-True ($rr.ExitCode -ne 0) "validator accepted a declared debug artifact"
    Assert-True ($rr.Output -match 'excluded') "excluded-artifact error not reported:`n$($rr.Output)"
    Pass "validator fails closed on a declared build/debug artifact"

    $rr = Invoke-Validator -Pkg $StageDir -Zip $ZipPath -Expected ('0' * 64) -CommitSha $ExpectedCommitSha -TreeSha $ExpectedTreeSha -LgplSha256 $ExpectedLgplSourceSha256
    Assert-True ($rr.ExitCode -ne 0) "validator accepted a wrong expected payload hash"
    Assert-True ($rr.Output -match 'expected payload hash') "expected-hash mismatch not reported:`n$($rr.Output)"
    Pass "validator fails closed on an expected payload hash mismatch"

    $pkg = New-PayloadCopy -Name 'mut-nohash' -WithoutZip
    $rr = Invoke-Validator -Pkg $pkg -CommitSha $ExpectedCommitSha -TreeSha $ExpectedTreeSha -LgplSha256 $ExpectedLgplSourceSha256
    Assert-True ($rr.ExitCode -ne 0) "validator accepted a payload with no expected hash"
    Assert-True ($rr.Output -match 'expected payload hash') "missing expected-hash error not reported:`n$($rr.Output)"
    Pass "validator fails closed when no expected payload hash is available"

    # --- 3b. strict required provenance bindings ----------------------------
    $rr = Invoke-Validator -Pkg $StageDir -Zip $ZipPath -TreeSha $ExpectedTreeSha -LgplSha256 $ExpectedLgplSourceSha256
    Assert-True ($rr.ExitCode -ne 0) "validator accepted a missing ExpectedCommitSha"
    Assert-True ($rr.Output -match 'ExpectedCommitSha is required') "missing ExpectedCommitSha not reported:`n$($rr.Output)"

    $rr = Invoke-Validator -Pkg $StageDir -Zip $ZipPath -CommitSha $ExpectedCommitSha -LgplSha256 $ExpectedLgplSourceSha256
    Assert-True ($rr.ExitCode -ne 0) "validator accepted a missing ExpectedTreeSha"
    Assert-True ($rr.Output -match 'ExpectedTreeSha is required') "missing ExpectedTreeSha not reported:`n$($rr.Output)"

    $rr = Invoke-Validator -Pkg $StageDir -Zip $ZipPath -CommitSha $ExpectedCommitSha -TreeSha $ExpectedTreeSha
    Assert-True ($rr.ExitCode -ne 0) "validator accepted a missing ExpectedLgplSourceSha256"
    Assert-True ($rr.Output -match 'ExpectedLgplSourceSha256 is required') "missing ExpectedLgplSourceSha256 not reported:`n$($rr.Output)"
    Pass "validator fails closed when any controlled provenance binding is missing"

    $rr = Invoke-Validator -Pkg $StageDir -Zip $ZipPath -CommitSha ('0' * 40) -TreeSha $ExpectedTreeSha -LgplSha256 $ExpectedLgplSourceSha256
    Assert-True ($rr.ExitCode -ne 0) "validator accepted a wrong ExpectedCommitSha"
    Assert-True ($rr.Output -match 'commitSha .* does not match expected') "wrong ExpectedCommitSha not reported:`n$($rr.Output)"

    $rr = Invoke-Validator -Pkg $StageDir -Zip $ZipPath -CommitSha $ExpectedCommitSha -TreeSha ('1' * 40) -LgplSha256 $ExpectedLgplSourceSha256
    Assert-True ($rr.ExitCode -ne 0) "validator accepted a wrong ExpectedTreeSha"
    Assert-True ($rr.Output -match 'treeSha .* does not match expected') "wrong ExpectedTreeSha not reported:`n$($rr.Output)"

    $rr = Invoke-Validator -Pkg $StageDir -Zip $ZipPath -CommitSha $ExpectedCommitSha -TreeSha $ExpectedTreeSha -LgplSha256 ('2' * 64)
    Assert-True ($rr.ExitCode -ne 0) "validator accepted a wrong ExpectedLgplSourceSha256"
    Assert-True ($rr.Output -match 'lgplCorrespondingSourceAsset sha256 .* does not match expected') "wrong ExpectedLgplSourceSha256 not reported:`n$($rr.Output)"
    Pass "validator fails closed on wrong commit/tree/LGPL-source bindings"

    # --- 4. build-installer generation --------------------------------------
    $genOut = Join-Path $WorkDir 'gen-ok'
    $bg = Invoke-BuildInstaller -Pkg $StageDir -Out $genOut -Zip $ZipPath -NoCompile -CommitSha $ExpectedCommitSha -TreeSha $ExpectedTreeSha -LgplSha256 $ExpectedLgplSourceSha256
    Assert-True ($bg.ExitCode -eq 0) "build-installer -NoCompile failed:`n$($bg.Output)"
    $genPath = Join-Path $genOut 'ozviewer-installer-generated.iss'
    Assert-True (Test-Path -LiteralPath $genPath -PathType Leaf) "generated include not produced"
    Assert-True (Test-Path -LiteralPath (Join-Path $genOut 'ozviewer-installer.iss') -PathType Leaf) "installer script not copied"
    $gen = Get-Content -LiteralPath $genPath -Raw
    Assert-Contains $gen "#define OZ_VERSION `"$($manifest.version)`"" "generated include version"
    Assert-Contains $gen "#define OZ_EXE_NAME `"$($manifest.entrypoint)`"" "generated include entrypoint"
    foreach ($f in $expectedFiles) {
        Assert-Contains $gen ("Source: `"" + (Join-Path $StageDir $f)) "generated include [Files] entry for $f"
    }
    Assert-Contains $gen 'DestDir: "{app}"' "generated include DestDir"
    # extension mapping must be exactly the validated manifest
    $extMatch = [regex]::Match($gen, '#define OZ_EXT_LIST "([^"]*)"')
    Assert-True $extMatch.Success "generated include has no OZ_EXT_LIST"
    $genExts = @($extMatch.Groups[1].Value -split '\s+' | Where-Object { $_.Length -gt 0 })
    Assert-True (($genExts -join ' ') -eq ($expectedExtensions -join ' ')) "generated extension mapping does not match the validated manifest"
    Assert-Contains $gen 'Source: "' "generated include quotes Source paths"
    Pass "build-installer generates payload-only [Files] and the exact extension mapping"

    # build-installer must fail closed on invalid payload
    $bg = Invoke-BuildInstaller -Pkg (Join-Path $WorkDir 'mut-hash\pkg') -Out (Join-Path $WorkDir 'gen-bad') -NoCompile -CommitSha $ExpectedCommitSha -TreeSha $ExpectedTreeSha -LgplSha256 $ExpectedLgplSourceSha256
    Assert-True ($bg.ExitCode -ne 0) "build-installer accepted a corrupted payload"
    Pass "build-installer fails closed on an untrusted payload"

    # build-installer must require and forward the controlled provenance inputs.
    $bg = Invoke-BuildInstaller -Pkg $StageDir -Out (Join-Path $WorkDir 'gen-bind-missing-commit') -Zip $ZipPath -NoCompile -TreeSha $ExpectedTreeSha -LgplSha256 $ExpectedLgplSourceSha256
    Assert-True ($bg.ExitCode -ne 0) "build-installer accepted a missing ExpectedCommitSha"
    Assert-True ($bg.Output -match 'ExpectedCommitSha is required') "build-installer missing ExpectedCommitSha not reported:`n$($bg.Output)"

    $bg = Invoke-BuildInstaller -Pkg $StageDir -Out (Join-Path $WorkDir 'gen-bind-missing-tree') -Zip $ZipPath -NoCompile -CommitSha $ExpectedCommitSha -LgplSha256 $ExpectedLgplSourceSha256
    Assert-True ($bg.ExitCode -ne 0) "build-installer accepted a missing ExpectedTreeSha"
    Assert-True ($bg.Output -match 'ExpectedTreeSha is required') "build-installer missing ExpectedTreeSha not reported:`n$($bg.Output)"

    $bg = Invoke-BuildInstaller -Pkg $StageDir -Out (Join-Path $WorkDir 'gen-bind-missing-lgpl') -Zip $ZipPath -NoCompile -CommitSha $ExpectedCommitSha -TreeSha $ExpectedTreeSha
    Assert-True ($bg.ExitCode -ne 0) "build-installer accepted a missing ExpectedLgplSourceSha256"
    Assert-True ($bg.Output -match 'ExpectedLgplSourceSha256 is required') "build-installer missing ExpectedLgplSourceSha256 not reported:`n$($bg.Output)"
    Pass "build-installer fails closed when a controlled provenance binding is missing"

    $bg = Invoke-BuildInstaller -Pkg $StageDir -Out (Join-Path $WorkDir 'gen-bind-wrong-commit') -Zip $ZipPath -NoCompile -CommitSha ('0' * 40) -TreeSha $ExpectedTreeSha -LgplSha256 $ExpectedLgplSourceSha256
    Assert-True ($bg.ExitCode -ne 0) "build-installer accepted a wrong ExpectedCommitSha"
    Assert-True ($bg.Output -match 'does not match expected') "build-installer wrong ExpectedCommitSha not reported:`n$($bg.Output)"

    $bg = Invoke-BuildInstaller -Pkg $StageDir -Out (Join-Path $WorkDir 'gen-bind-wrong-tree') -Zip $ZipPath -NoCompile -CommitSha $ExpectedCommitSha -TreeSha ('1' * 40) -LgplSha256 $ExpectedLgplSourceSha256
    Assert-True ($bg.ExitCode -ne 0) "build-installer accepted a wrong ExpectedTreeSha"
    Assert-True ($bg.Output -match 'does not match expected') "build-installer wrong ExpectedTreeSha not reported:`n$($bg.Output)"

    $bg = Invoke-BuildInstaller -Pkg $StageDir -Out (Join-Path $WorkDir 'gen-bind-wrong-lgpl') -Zip $ZipPath -NoCompile -CommitSha $ExpectedCommitSha -TreeSha $ExpectedTreeSha -LgplSha256 ('2' * 64)
    Assert-True ($bg.ExitCode -ne 0) "build-installer accepted a wrong ExpectedLgplSourceSha256"
    Assert-True ($bg.Output -match 'does not match expected') "build-installer wrong ExpectedLgplSourceSha256 not reported:`n$($bg.Output)"
    Pass "build-installer fails closed on wrong commit/tree/LGPL-source bindings"

    # --- 5. static Inno policy assertions -----------------------------------
    Assert-True (Test-Path -LiteralPath $InnoScript -PathType Leaf) "Inno script not found: $InnoScript"
    $issRaw = Get-Content -LiteralPath $InnoScript -Raw
    $issNoSemicolonComments = (@($issRaw -split "`r?`n" | Where-Object { -not ($_.TrimStart().StartsWith(';')) }) -join "`n")
    # Strip both section comments (';') and Pascal line comments ('//') for the
    # forbidden-token scan only.
    $issCode = [regex]::Replace((@($issRaw -split "`r?`n" | Where-Object {
        $t = $_.TrimStart()
        -not ($t.StartsWith(';') -or $t.StartsWith('//'))
    }) -join "`n"), '(?s)\{[^}]*\}', ' ')

    Assert-Contains $issRaw 'AppId={{7E3B1C9A-4D2F-4B6E-9C31-0A5F2D8B47E1}' "stable AppId"
    Assert-Contains $issRaw 'PrivilegesRequired=lowest' "per-user lowest privileges"
    Assert-Contains $issRaw '{localappdata}\Programs\' "per-user Programs install dir"
    Assert-Contains $issRaw 'ArchitecturesAllowed=x64compatible' "x64 restriction"
    Assert-Contains $issRaw 'ArchitecturesInstallIn64BitMode=x64compatible' "x64 install mode"
    Assert-Contains $issRaw '#include "ozviewer-installer-generated.iss"' "generated include directive"
    Assert-Contains $issRaw 'function IsOwned(): Boolean;' "ownership check present"
    Assert-Contains $issRaw 'RegDeleteKeyIfEmpty' "empty-key cleanup present"
    Assert-Contains $issRaw '+ ExpandConstant(''{app}'')' "quoted open command uses ExpandConstant app path"
    Assert-Contains $issRaw '"%1"' "open command passes the file argument"
    Assert-Contains $issRaw '+ ''",0''' "DefaultIcon value is quoted with an index"
    Assert-Contains $issRaw 'ms-settings:defaultapps' "Default Apps launch"
    Assert-Contains $issRaw "'ms-settings:'" "generic Settings fallback"
    Assert-Contains $issRaw 'OZ_MARKER_SUBKEY' "ownership marker subkey"

    # task is present and initially unchecked
    $assocTask = @($issNoSemicolonComments -split "`r?`n" | Where-Object { $_ -match 'Name:\s*"associate"' })
    Assert-True ($assocTask.Count -ge 1) "association task not found"
    Assert-True ($assocTask[0] -match 'Flags:\s*unchecked') "association task is not initially unchecked"

    # forbidden behavior
    Assert-True ($issNoSemicolonComments -notmatch '(?im)^\s*\[Registry\]\s*$') "declarative [Registry] section is forbidden"
    Assert-NotContains $issCode 'HKLM' "HKLM reference in installer code"
    Assert-NotContains $issCode 'HKEY_LOCAL_MACHINE' "HKEY_LOCAL_MACHINE reference in installer code"
    Assert-NotContains $issCode 'HKEY_CLASSES_ROOT' "HKEY_CLASSES_ROOT reference in installer code"
    Assert-NotContains $issCode 'HKCR' "HKCR reference in installer code"
    Assert-NotContains $issCode 'UserChoice' "UserChoice reference in installer code"
    Assert-NotContains $issCode 'OpenWithProgids' "OpenWithProgids reference in installer code"
    Assert-NotContains $issCode 'RegDeleteKeyIncludingSubkeys' "recursive registry deletion is forbidden"
    Assert-NotContains $issCode 'Software\Classes\.' "extension default keys are forbidden"
    Assert-NotContains $issCode 'RegisteredApplications\OZ' "no per-extension default writes"
    Assert-True ($issCode -match 'HKCU') "installer code must operate on HKCU"
    Assert-True ($issRaw -match '(?i)unsigned') "unsigned distribution statement missing from the Inno script"

    # registry function calls must all be rooted at HKCU
    $regCallLines = @($issNoSemicolonComments -split "`r?`n" | Where-Object {
        $_ -match 'RegWriteStringValue\(|RegWriteDWordValue\(|RegWriteBinaryValue\(|RegDeleteValue\(|RegDeleteKeyIfEmpty\(|RegQueryStringValue\(|RegKeyExists\(|RegValueExists\(|RegGetValueNames\('
    })
    Assert-True ($regCallLines.Count -gt 0) "no registry calls found to audit"
    foreach ($line in $regCallLines) {
        Assert-True ($line -match 'HKCU') "registry call is not rooted at HKCU: $($line.Trim())"
    }
    Pass "static Inno policy: AppId/per-user/x64/opt-in/quoting/ownership and prohibited-key absence ($($regCallLines.Count) registry calls audited)"

    # --- 6. docs / unsigned distribution statement --------------------------
    Assert-True (Test-Path -LiteralPath $DocsPath -PathType Leaf) "installer docs not found: $DocsPath"
    $docs = Get-Content -LiteralPath $DocsPath -Raw
    Assert-True ($docs -match '(?i)unsigned') "unsigned distribution statement missing from docs"
    Assert-True ($docs -match '(?i)never\s+.*(default|UserChoice)') "docs must state that defaults are never changed automatically"
    Pass "installer docs exist with the unsigned distribution statement"

    # --- 7. Unicode / space path handling ------------------------------------
    $unicodeRoot = Join-Path $WorkDir ("caf" + [char]0x00E9 + " p" + [char]0x00E4 + "th")
    if (Test-Path -LiteralPath $unicodeRoot) { Remove-Item -LiteralPath $unicodeRoot -Recurse -Force }
    New-Item -ItemType Directory -Path $unicodeRoot -Force | Out-Null
    $unicodePkg = Join-Path $unicodeRoot 'pkg dir'
    Copy-Item -LiteralPath $StageDir -Destination $unicodePkg -Recurse -Force
    Copy-Item -LiteralPath $ZipPath -Destination "$unicodePkg.zip" -Force
    if (Test-Path -LiteralPath "$ZipPath.sha256" -PathType Leaf) {
        Copy-Item -LiteralPath "$ZipPath.sha256" -Destination "$unicodePkg.zip.sha256" -Force
    }
    $ru = Invoke-Validator -Pkg $unicodePkg -CommitSha $ExpectedCommitSha -TreeSha $ExpectedTreeSha -LgplSha256 $ExpectedLgplSourceSha256
    Assert-True ($ru.ExitCode -eq 0) "validator rejected a Unicode/space payload path:`n$($ru.Output)"
    $ruGen = Join-Path $unicodeRoot 'generated'
    $bu = Invoke-BuildInstaller -Pkg $unicodePkg -Out $ruGen -NoCompile -CommitSha $ExpectedCommitSha -TreeSha $ExpectedTreeSha -LgplSha256 $ExpectedLgplSourceSha256
    Assert-True ($bu.ExitCode -eq 0) "build-installer failed on a Unicode/space path:`n$($bu.Output)"
    $uGenText = Get-Content -LiteralPath (Join-Path $ruGen 'ozviewer-installer-generated.iss') -Raw
    Assert-Contains $uGenText ($unicodePkg) "generated include lost the Unicode/space source path"
    Pass "Unicode/space payload paths preserved through validation and generation"

    # --- 8. mock-registry ownership policy ----------------------------------
    # 8a. fresh register -> owned; uninstall -> fully cleaned
    $reg = New-MockRegistry
    Assert-True ((Mock-PreflightError $reg $Policy) -eq '') "fresh preflight should be clean"
    Mock-Register $reg $Policy
    Assert-True (Mock-IsOwned $reg $Policy) "register did not write an ownership marker"
    Assert-True ((Mock-GetValue $reg $Policy.CommandKey '') -eq $CmdValue) "open command quoting wrong"
    foreach ($e in $Policy.Extensions) {
        Assert-True ((Mock-GetValue $reg $Policy.FileAssocKey $e) -eq $Policy.ProgID) "extension $e not mapped to the ProgID"
    }
    Assert-True ((Mock-GetValue $reg $Policy.RegisteredApps $Policy.RegisteredName) -eq $Policy.CapKey) "RegisteredApplications value wrong"
    Mock-Unregister $reg $Policy
    Assert-True (-not (Mock-KeyExists $reg $Policy.ProgIDKey)) "ProgID key not removed on uninstall"
    Assert-True (-not (Mock-KeyExists $reg $Policy.CapKey)) "Capabilities key not removed on uninstall"
    Assert-True (-not (Mock-ValueExists $reg $Policy.RegisteredApps $Policy.RegisteredName)) "RegisteredApplications value not removed"
    Assert-True (-not (Mock-KeyExists $reg $Policy.AppKey)) "application key not cleaned up"
    Pass "mock policy: register writes owned values, uninstall removes only them"

    # 8b. foreign ProgID collision
    $reg = New-MockRegistry
    Mock-SetValue $reg $Policy.ProgIDKey '' 'Someone Else Image'
    Assert-True ((Mock-PreflightError $reg $Policy) -ne '') "foreign ProgID collision not detected"
    Pass "mock policy: foreign ProgID collision fails closed"

    # 8c. foreign RegisteredApplications value
    $reg = New-MockRegistry
    Mock-SetValue $reg $Policy.RegisteredApps $Policy.RegisteredName 'Software\Other\Capabilities'
    Assert-True ((Mock-PreflightError $reg $Policy) -ne '') "foreign RegisteredApplications value not detected"
    Pass "mock policy: foreign RegisteredApplications value fails closed"

    # 8d. owned upgrade reaps only our own stale extension values
    $reg = New-MockRegistry
    Mock-Register $reg $Policy
    Mock-SetValue $reg $Policy.FileAssocKey '.bmp' $Policy.ProgID          # stale, owned
    Mock-Register $reg $Policy
    Assert-True (-not (Mock-ValueExists $reg $Policy.FileAssocKey '.bmp')) "owned stale extension not reaped on upgrade"
    Assert-True ((Mock-GetValue $reg $Policy.FileAssocKey '.jpg') -eq $Policy.ProgID) "current extension lost on upgrade"
    Pass "mock policy: upgrade reconciles only owned registration (stale extension reaped)"

    # 8e. owned registration with a modified extension value fails closed
    $reg = New-MockRegistry
    Mock-Register $reg $Policy
    Mock-SetValue $reg $Policy.FileAssocKey '.jpg' 'Other.ProgID'
    Assert-True ((Mock-PreflightError $reg $Policy) -ne '') "modified extension value not detected"
    Pass "mock policy: a modified extension value fails closed"

    # 8f. uninstall preserves user-modified values, removes matching ones
    $reg = New-MockRegistry
    Mock-Register $reg $Policy
    Mock-SetValue $reg $Policy.ProgIDKey 'DefaultIcon' 'C:\custom.ico,0'   # user-modified
    Mock-Unregister $reg $Policy
    Assert-True ((Mock-GetValue $reg $Policy.ProgIDKey 'DefaultIcon') -eq 'C:\custom.ico,0') "user-modified DefaultIcon was deleted"
    Assert-True ((Mock-GetValue $reg $Policy.CommandKey '') -eq $null -or -not (Mock-ValueExists $reg $Policy.CommandKey '')) "matching open command not removed"
    Assert-True (-not (Mock-ValueExists $reg $Policy.RegisteredApps $Policy.RegisteredName)) "RegisteredApplications value not removed"
    Pass "mock policy: uninstall preserves user-modified values and never infers defaults"

    # 8g. uninstall without an ownership marker changes nothing
    $reg = New-MockRegistry
    Mock-SetValue $reg $Policy.ProgIDKey '' 'Someone Else Image'
    Mock-SetValue $reg $Policy.RegisteredApps $Policy.RegisteredName 'Software\Other\Capabilities'
    Mock-Unregister $reg $Policy
    Assert-True ((Mock-GetValue $reg $Policy.ProgIDKey '') -eq 'Someone Else Image') "unowned ProgID was deleted"
    Assert-True ((Mock-GetValue $reg $Policy.RegisteredApps $Policy.RegisteredName) -eq 'Software\Other\Capabilities') "unowned RegisteredApplications value was deleted"
    Pass "mock policy: uninstall without ownership proof preserves everything"

    # 8h. upgrade with opt-in task unchecked migrates existing owned v0.1.0 registration
    $reg = New-MockRegistry
    # Simulate existing v0.1.0 registration with old display name & legacy RegisteredApplications
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

    # Upgrade runs with association task unchecked -> calls Mock-MigrateIfOwned
    Mock-MigrateIfOwned $reg $Policy
    Assert-True ((Mock-GetValue $reg $Policy.RegisteredApps $Policy.RegisteredName) -eq $Policy.CapKey) "upgrade did not migrate to PikaView RegisteredApplications"
    Assert-True (-not (Mock-ValueExists $reg $Policy.RegisteredApps $Policy.LegacyRegisteredName)) "legacy RegisteredApplications value not removed on owned upgrade"
    Assert-True ((Mock-GetValue $reg $Policy.CapKey 'ApplicationName') -eq $Policy.AppName) "ApplicationName not updated to PikaView on upgrade"
    Assert-True ((Mock-GetValue $reg $Policy.ProgIDKey 'FriendlyTypeName') -eq $Policy.TypeName) "ProgID FriendlyTypeName not updated on upgrade"
    # Now uninstall: should cleanly remove all PikaView values without strays
    Mock-Unregister $reg $Policy
    Assert-True (-not (Mock-KeyExists $reg $Policy.ProgIDKey)) "ProgID key not removed on uninstall after upgrade"
    Assert-True (-not (Mock-KeyExists $reg $Policy.CapKey)) "Capabilities key not removed on uninstall after upgrade"
    Assert-True (-not (Mock-ValueExists $reg $Policy.RegisteredApps $Policy.RegisteredName)) "PikaView RegisteredApplications value not removed on uninstall"
    Assert-True (-not (Mock-ValueExists $reg $Policy.RegisteredApps $Policy.LegacyRegisteredName)) "Legacy RegisteredApplications value still exists after uninstall"
    Assert-True (-not (Mock-KeyExists $reg $Policy.AppKey)) "AppKey not cleaned up after uninstall"
    Pass "mock policy: upgrade with association task unchecked preserves prior opt-in and migrates to PikaView"

    # 8i. foreign collision on legacy name during preflight fails closed
    $reg = New-MockRegistry
    Mock-SetValue $reg $Policy.RegisteredApps $Policy.LegacyRegisteredName 'Software\Foreign\Capabilities'
    Assert-True ((Mock-PreflightError $reg $Policy) -ne '') "foreign collision on legacy RegisteredApplications name not detected"
    Pass "mock policy: foreign collision on legacy RegisteredApplications fails closed"

    # 8j. fresh opt-out install does not register handlers
    $reg = New-MockRegistry
    # When task is unchecked on fresh install, neither Mock-Register nor Mock-MigrateIfOwned writes anything
    Mock-MigrateIfOwned $reg $Policy
    Assert-True (-not (Mock-KeyExists $reg $Policy.ProgIDKey)) "fresh opt-out install wrote ProgID"
    Assert-True (-not (Mock-KeyExists $reg $Policy.CapKey)) "fresh opt-out install wrote Capabilities"
    Assert-True (-not (Mock-ValueExists $reg $Policy.RegisteredApps $Policy.RegisteredName)) "fresh opt-out install wrote RegisteredApplications"
    Pass "mock policy: fresh install with opt-out task unchecked creates no handler registration"

    # --- 9. real compile when Inno Setup is available ------------------------
    $compileOut = Join-Path $WorkDir 'compile'
    $bc = Invoke-BuildInstaller -Pkg $StageDir -Out $compileOut -Zip $ZipPath -CommitSha $ExpectedCommitSha -TreeSha $ExpectedTreeSha -LgplSha256 $ExpectedLgplSourceSha256
    if ($bc.ExitCode -eq 0) {
        $exe = @(Get-ChildItem -LiteralPath $compileOut -Filter '*.exe' -ErrorAction SilentlyContinue)
        Assert-True ($exe.Count -ge 1) "build-installer reported success without an installer executable"
        Pass "Inno Setup compile succeeded: $($exe[0].Name)"
    }
    elseif ($bc.ExitCode -eq 77) {
        Assert-True ($bc.Output -match 'INSTALLER_SKIP=INNO_SETUP_NOT_FOUND') "compiler-unavailable skip did not record the precise limitation"
        Write-Host "test-installer: NOTE: Inno Setup is not installed; compile was skipped (INSTALLER_SKIP=INNO_SETUP_NOT_FOUND)" -ForegroundColor Yellow
    }
    else {
        throw "build-installer failed unexpectedly (exit $($bc.ExitCode)):`n$($bc.Output)"
    }

    Write-Host "test-installer: ALL CASES PASSED" -ForegroundColor Green
    exit 0
}
catch {
    Write-Host "test-installer: FAIL: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}

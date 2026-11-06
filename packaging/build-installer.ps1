<#
.SYNOPSIS
  Build the Stage 9.10 unsigned per-user x64 installer from a validated Stage 9.9
  portable payload.

.DESCRIPTION
  This is a packaging boundary only. It does not build, sign, discover, or
  rebuild any binary. It:

    1. validates the staged portable payload with
       packaging\validate-portable-payload.ps1 (manifest schema, version,
       platform, entrypoint, extension allowlist/tokens, exact per-file hashes,
       exact on-disk file set, and the expected payload ZIP hash);
    2. generates "ozviewer-installer-generated.iss" from the validated manifest
       (version, entrypoint, supported extensions, output path, and exactly the
       validated payload files);
    3. copies packaging\ozviewer-installer.iss next to the generated include;
    4. compiles it with Inno Setup's ISCC.exe when a local compiler is
       available. When no compiler is found it does not fail silently: it
       records the precise limitation and exits 77 (skipped).

  The installer is per-user and unsigned. It never sets a default handler and
  never writes HKLM; see packaging\ozviewer-installer.iss and
  docs\stage9-installer.md.

.PARAMETER PackageDir
  Staged portable directory produced by package-portable.ps1.

.PARAMETER ZipPath
  Portable ZIP that the staged directory came from. Defaults to the sibling
  "<PackageDir>.zip".

.PARAMETER ExpectedSha256
  Expected SHA-256 of the portable ZIP. Defaults to "<ZipPath>.sha256". The
  build fails closed when neither is available.

.PARAMETER OutDir
  Output directory for the generated include, the copied script, and (when
  compiled) the installer executable.

.PARAMETER InnoScript
  Inno script to compile. Defaults to ozviewer-installer.iss next to this
  script.

.PARAMETER IsccPath
  Explicit path to ISCC.exe. When omitted the script auto-detects it.

.PARAMETER MetadataPath
  Packaging metadata JSON. Defaults to portable-package.json next to this
  script.

.PARAMETER NoCompile
  Validate and generate only; never invoke ISCC. Intended for tests.

.PARAMETER RequireCompiler
  Fail (exit 1) when no local Inno Setup compiler is found instead of skipping
  with exit 77.

.NOTES
  Exit codes: 0 = built (or generated with -NoCompile), 77 = generated but no
  local Inno Setup compiler, 1 = validation/build failure.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$PackageDir,
    [string]$ZipPath = "",
    [string]$ExpectedSha256 = "",
    [Parameter(Mandatory = $true)][string]$OutDir,
    [string]$InnoScript = "",
    [string]$IsccPath = "",
    [string]$MetadataPath = "",
    [switch]$NoCompile,
    [switch]$RequireCompiler
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Split-Path -Parent $ScriptDir
$Validator = Join-Path $ScriptDir 'validate-portable-payload.ps1'
if ([string]::IsNullOrWhiteSpace($MetadataPath)) {
    $MetadataPath = Join-Path $ScriptDir 'portable-package.json'
}
if ([string]::IsNullOrWhiteSpace($InnoScript)) {
    $InnoScript = Join-Path $ScriptDir 'ozviewer-installer.iss'
}

function Fail([string]$Message) {
    Write-Host "build-installer: ERROR: $Message" -ForegroundColor Red
    exit 1
}
function Info([string]$Message) { Write-Host "build-installer: $Message" }

function Convert-ToForwardSlashes([string]$Path) {
    return $Path.Replace('\', '/')
}

function Find-Iscc([string]$Explicit) {
    if (-not [string]::IsNullOrWhiteSpace($Explicit)) {
        if (Test-Path -LiteralPath $Explicit -PathType Leaf) { return (Resolve-Path -LiteralPath $Explicit).Path }
        return ""
    }
    $cmd = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($cmd -and $cmd.Source -and (Test-Path -LiteralPath $cmd.Source -PathType Leaf)) {
        return $cmd.Source
    }
    $candidates = @()
    if ($env:ProgramFiles) { $candidates += (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe') }
    if (${env:ProgramFiles(x86)}) { $candidates += (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe') }
    if ($env:LOCALAPPDATA) {
        $candidates += (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe')
        $candidates += (Join-Path $env:LOCALAPPDATA 'Inno Setup 6\ISCC.exe')
    }
    foreach ($c in $candidates) {
        if (Test-Path -LiteralPath $c -PathType Leaf) { return $c }
    }
    foreach ($hive in @('HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall',
                       'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall',
                       'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall')) {
        $entries = Get-ChildItem -LiteralPath $hive -ErrorAction SilentlyContinue
        foreach ($e in @($entries)) {
            $props = Get-ItemProperty -LiteralPath $e.PSPath -ErrorAction SilentlyContinue
            if (-not $props) { continue }
            $displayName = if ($props.PSObject.Properties['DisplayName']) { [string]$props.DisplayName } else { '' }
            $installLocation = if ($props.PSObject.Properties['InstallLocation']) { [string]$props.InstallLocation } else { '' }
            if ($displayName -like 'Inno Setup*' -and -not [string]::IsNullOrWhiteSpace($installLocation)) {
                $cand = Join-Path $installLocation 'ISCC.exe'
                if (Test-Path -LiteralPath $cand -PathType Leaf) { return $cand }
            }
        }
    }
    return ""
}

if (-not (Test-Path -LiteralPath $Validator -PathType Leaf)) { Fail "validator not found: $Validator" }
if (-not (Test-Path -LiteralPath $InnoScript -PathType Leaf)) { Fail "Inno script not found: $InnoScript" }
if (-not (Test-Path -LiteralPath $MetadataPath -PathType Leaf)) { Fail "metadata not found: $MetadataPath" }

$iconPath = Join-Path $RepoRoot 'assets\icons\ozviewer.ico'
if (-not (Test-Path -LiteralPath $iconPath -PathType Leaf)) { Fail "installer icon not found: $iconPath" }

if (-not (Test-Path -LiteralPath $OutDir -PathType Container)) {
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
}
$OutDir = (Resolve-Path -LiteralPath $OutDir).Path
$resultPath = Join-Path $OutDir 'validated-payload.json'

# --- 1. validate the staged payload (fail closed) --------------------------
$validatorArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $Validator,
                   '-PackageDir', $PackageDir,
                   '-ResultPath', $resultPath,
                   '-MetadataPath', $MetadataPath)
if (-not [string]::IsNullOrWhiteSpace($ZipPath)) { $validatorArgs += @('-ZipPath', $ZipPath) }
if (-not [string]::IsNullOrWhiteSpace($ExpectedSha256)) { $validatorArgs += @('-ExpectedSha256', $ExpectedSha256) }
$validatorOutput = & powershell @validatorArgs 2>&1
if ($LASTEXITCODE -ne 0) {
    Fail "payload validation failed:`n$($validatorOutput -join "`n")"
}
foreach ($line in @($validatorOutput)) { Info $line }
if (-not (Test-Path -LiteralPath $resultPath -PathType Leaf)) { Fail "validator did not produce a result file" }
$payload = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json

$packageDirResolved = (Resolve-Path -LiteralPath $PackageDir).Path
$version = [string]$payload.version
$entrypoint = [string]$payload.entrypoint
$extensions = @($payload.supportedExtensions | ForEach-Object { [string]$_ })
$files = @($payload.files | ForEach-Object { [string]$_ })
if ($extensions.Count -eq 0) { Fail "validated payload reported no supported extensions" }
if ($files.Count -eq 0) { Fail "validated payload reported no files" }

# --- 2. generate the include ------------------------------------------------
$generatedName = 'ozviewer-installer-generated.iss'
$generatedPath = Join-Path $OutDir $generatedName
$outputBase = "ozviewer-$version-win-x64-installer"

$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("; Generated by build-installer.ps1 from a validated Stage 9.9 portable payload.")
[void]$sb.AppendLine("; Do not edit. Source payload: $packageDirResolved")
[void]$sb.AppendLine("; Payload ZIP SHA-256: $([string]$payload.payloadSha256)")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("#define OZ_VERSION `"$version`"")
[void]$sb.AppendLine("#define OZ_EXE_NAME `"$entrypoint`"")
[void]$sb.AppendLine("#define OZ_EXT_LIST `"$($extensions -join ' ')`"")
[void]$sb.AppendLine("#define OZ_EXT_COUNT $($extensions.Count)")
[void]$sb.AppendLine("#define OZ_OUTPUT_DIR `"$(Convert-ToForwardSlashes $OutDir)`"")
[void]$sb.AppendLine("#define OZ_OUTPUT_BASENAME `"$outputBase`"")
[void]$sb.AppendLine("#define OZ_SETUP_ICON `"$(Convert-ToForwardSlashes (Resolve-Path -LiteralPath $iconPath).Path)`"")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("[Files]")
$allFiles = @($files) + @('package-manifest.json')
foreach ($name in ($allFiles | Sort-Object)) {
    $source = Join-Path $packageDirResolved $name
    [void]$sb.AppendLine("Source: `"$source`"; DestDir: `"{app}`"; Flags: ignoreversion")
}
[System.IO.File]::WriteAllText($generatedPath, $sb.ToString(), (New-Object System.Text.UTF8Encoding($true)))

# --- 3. copy the script next to the include --------------------------------
$scriptCopy = Join-Path $OutDir 'ozviewer-installer.iss'
Copy-Item -LiteralPath (Resolve-Path -LiteralPath $InnoScript).Path -Destination $scriptCopy -Force

# Copy LICENSE file to $OutDir so Inno's LicenseFile=LICENSE resolves during compilation
$licenseSrc = Join-Path $packageDirResolved 'LICENSE'
if (-not (Test-Path -LiteralPath $licenseSrc -PathType Leaf)) {
    $licenseSrc = Join-Path $RepoRoot 'LICENSE'
}
if (Test-Path -LiteralPath $licenseSrc -PathType Leaf) {
    Copy-Item -LiteralPath $licenseSrc -Destination (Join-Path $OutDir 'LICENSE') -Force
}

Info "validated payload: $entrypoint $version, $($files.Count) files, $($extensions.Count) extensions"
Info "generated include: $generatedPath"
Info "installer script:  $scriptCopy"

if ($NoCompile) {
    Write-Output "INSTALLER_STAGE=$OutDir"
    Write-Output "INSTALLER_GENERATED_ISS=$generatedPath"
    Write-Output "INSTALLER_SCRIPT=$scriptCopy"
    Write-Output "INSTALLER_RESULT=GENERATED_NO_COMPILE"
    exit 0
}

# --- 4. compile if a local Inno Setup compiler is available ----------------
$iscc = Find-Iscc -Explicit $IsccPath
if ([string]::IsNullOrWhiteSpace($iscc)) {
    $limitation = "Inno Setup compiler (ISCC.exe) is not installed or not on PATH; the validated script and generated include were produced but no installer executable could be compiled. Install Inno Setup 6 (https://jrsoftware.org/isdl.php) and re-run build-installer.ps1."
    Write-Output "INSTALLER_STAGE=$OutDir"
    Write-Output "INSTALLER_GENERATED_ISS=$generatedPath"
    Write-Output "INSTALLER_SCRIPT=$scriptCopy"
    Write-Output "INSTALLER_SKIP=INNO_SETUP_NOT_FOUND"
    Write-Output "INSTALLER_LIMITATION=$limitation"
    if ($RequireCompiler) { Fail $limitation }
    Write-Host "build-installer: SKIPPED: $limitation" -ForegroundColor Yellow
    exit 77
}

Info "using Inno Setup compiler: $iscc"
& $iscc "/Qp" $scriptCopy
if ($LASTEXITCODE -ne 0) {
    Fail "ISCC failed with exit code $LASTEXITCODE"
}

$installerPath = Join-Path $OutDir "$outputBase.exe"
if (-not (Test-Path -LiteralPath $installerPath -PathType Leaf)) {
    Fail "ISCC reported success but the installer was not produced: $installerPath"
}
$installerHash = (Get-FileHash -LiteralPath $installerPath -Algorithm SHA256).Hash.ToLowerInvariant()
[System.IO.File]::WriteAllText("$installerPath.sha256", "$installerHash *$outputBase.exe`n", (New-Object System.Text.UTF8Encoding($false)))

Info "installer: $installerPath"
Info "installer SHA256: $installerHash"
Write-Output "INSTALLER_STAGE=$OutDir"
Write-Output "INSTALLER_GENERATED_ISS=$generatedPath"
Write-Output "INSTALLER_SCRIPT=$scriptCopy"
Write-Output "INSTALLER_EXE=$installerPath"
Write-Output "INSTALLER_SHA256=$installerHash"
exit 0

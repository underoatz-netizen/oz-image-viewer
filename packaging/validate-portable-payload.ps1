<#
.SYNOPSIS
  Validate a staged Stage 9.9 portable payload before it is trusted as installer
  input (Stage 9.10).

.DESCRIPTION
  Fail-closed validation of the staged portable directory produced by
  package-portable.ps1. This is the "minimal reproducible packaging boundary":
  the installer build must not rebuild, discover, or infer any binary; it may
  only consume a payload that passes every check below.

  Checks:
    * package-manifest.json exists, is valid JSON, and has the required fields;
    * schema equals the packaging metadata schema;
    * packageName / version / platform match the packaging metadata (and any
      explicit -ExpectedVersion / -ExpectedPlatform);
    * entrypoint matches the metadata and exists in the payload;
    * supportedExtensions are well-formed ".token" strings, unique, and every
      token is in the metadata allowlist (knownImageExtensions);
    * every declared file is a bare filename (no path traversal), unique, and
      its byte length and SHA-256 match the on-disk file exactly;
    * the on-disk file set is exactly the manifest file set plus
      package-manifest.json (no extra, no missing files);
    * dynamicDependencies are all present files;
    * no excluded build/debug artifact extension is present;
    * an expected payload hash is available (from -ExpectedSha256 or the
      portable ZIP .sha256 sidecar) and matches the ZIP's computed SHA-256, and
      every ZIP entry agrees with the manifest.

  The expected payload hash binds the payload ZIP. A build that cannot obtain
  one fails closed; pass -ZipPath and/or -ExpectedSha256.

.PARAMETER PackageDir
  Staged portable directory (contains ozviewer.exe + package-manifest.json).

.PARAMETER ZipPath
  Portable ZIP that the staged directory came from. Defaults to the sibling
  "<PackageDir>.zip" when it exists.

.PARAMETER ExpectedSha256
  Expected SHA-256 of the portable ZIP. When omitted it is read from
  "<ZipPath>.sha256"; when neither is available validation fails closed.

.PARAMETER MetadataPath
  Packaging metadata JSON. Defaults to portable-package.json next to this
  script.

.PARAMETER ExpectedVersion
  Required version; defaults to the version in the packaging metadata.

.PARAMETER ExpectedPlatform
  Required platform; defaults to win-x64 and must also match the metadata.

.PARAMETER AllowedExtensions
  Extension allowlist (without dots). Defaults to the metadata
  knownImageExtensions.

.PARAMETER ResultPath
  Optional path for a JSON validation-evidence file on success.

.NOTES
  Exit codes: 0 = valid, 1 = invalid/untrusted input.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$PackageDir,
    [string]$ZipPath = "",
    [string]$ExpectedSha256 = "",
    [string]$MetadataPath = "",
    [string]$ExpectedVersion = "",
    [string]$ExpectedPlatform = "win-x64",
    [string[]]$AllowedExtensions = @(),
    [string]$ResultPath = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrWhiteSpace($MetadataPath)) {
    $MetadataPath = Join-Path $ScriptDir 'portable-package.json'
}

function Fail([string]$Message) {
    Write-Host "validate-portable: ERROR: $Message" -ForegroundColor Red
    exit 1
}

function Require-Property([object]$Object, [string]$Name, [string]$Context) {
    if (-not ($Object.PSObject.Properties.Name -contains $Name)) {
        Fail "$Context is missing required field '$Name'"
    }
}

$manifestFileName = 'package-manifest.json'
$sha256Pattern = '^[0-9a-f]{64}$'

if (-not (Test-Path -LiteralPath $MetadataPath -PathType Leaf)) {
    Fail "metadata file not found: $MetadataPath"
}
try {
    $metadata = Get-Content -LiteralPath $MetadataPath -Raw | ConvertFrom-Json
}
catch {
    Fail "metadata file is not valid JSON: $($_.Exception.Message)"
}

if (-not (Test-Path -LiteralPath $PackageDir -PathType Container)) {
    Fail "staged payload directory not found: $PackageDir"
}
$PackageDir = (Resolve-Path -LiteralPath $PackageDir).Path

$manifestPath = Join-Path $PackageDir $manifestFileName
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    Fail "staged payload is missing $manifestFileName in $PackageDir"
}
try {
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
}
catch {
    Fail "$manifestFileName is not valid JSON: $($_.Exception.Message)"
}

foreach ($field in @('schema', 'packageName', 'version', 'platform', 'entrypoint', 'supportedExtensions', 'dynamicDependencies', 'files')) {
    Require-Property -Object $manifest -Name $field -Context $manifestFileName
}

# --- schema / identity -----------------------------------------------------
if ([int]$manifest.schema -ne [int]$metadata.schema) {
    Fail "manifest schema $($manifest.schema) does not match packaging schema $($metadata.schema)"
}
if ([string]$manifest.packageName -ne [string]$metadata.packageName) {
    Fail "manifest packageName '$($manifest.packageName)' does not match '$($metadata.packageName)'"
}

$requiredVersion = if (-not [string]::IsNullOrWhiteSpace($ExpectedVersion)) { $ExpectedVersion } else { [string]$metadata.version }
if ([string]$manifest.version -ne $requiredVersion) {
    Fail "manifest version '$($manifest.version)' does not match expected '$requiredVersion'"
}
if ([string]$manifest.platform -ne [string]$ExpectedPlatform) {
    Fail "manifest platform '$($manifest.platform)' does not match expected '$ExpectedPlatform'"
}
if ([string]$metadata.platform -ne [string]$ExpectedPlatform) {
    Fail "packaging metadata platform '$($metadata.platform)' does not match expected '$ExpectedPlatform'"
}

# --- entrypoint ------------------------------------------------------------
$entrypoint = [string]$manifest.entrypoint
if ([string]::IsNullOrWhiteSpace($entrypoint)) { Fail "manifest entrypoint is empty" }
if ([System.IO.Path]::GetFileName($entrypoint) -ne $entrypoint) {
    Fail "manifest entrypoint '$entrypoint' is not a bare filename"
}
if ([string]$manifest.entrypoint -ne [string]$metadata.entrypoint) {
    Fail "manifest entrypoint '$entrypoint' does not match packaging entrypoint '$($metadata.entrypoint)'"
}
$entrypointPath = Join-Path $PackageDir $entrypoint
if (-not (Test-Path -LiteralPath $entrypointPath -PathType Leaf)) {
    Fail "entrypoint '$entrypoint' is missing from the staged payload"
}

# --- extension allowlist / tokens ------------------------------------------
$allowed = New-Object System.Collections.Generic.HashSet[string]
if ($AllowedExtensions.Count -gt 0) {
    foreach ($a in $AllowedExtensions) { [void]$allowed.Add(([string]$a).TrimStart('.').ToLowerInvariant()) }
}
else {
    foreach ($a in @($metadata.knownImageExtensions)) { [void]$allowed.Add(([string]$a).ToLowerInvariant()) }
}
if ($allowed.Count -eq 0) { Fail "no extension allowlist available (pass -AllowedExtensions or fix metadata)" }

$extensions = @($manifest.supportedExtensions)
if ($extensions.Count -eq 0) { Fail "manifest supportedExtensions is empty" }
$seenExt = New-Object System.Collections.Generic.HashSet[string]
foreach ($ext in $extensions) {
    $token = [string]$ext
    if ($token -notmatch '^\.[a-z0-9]{1,6}$') {
        Fail "unsupported extension token '$token' (expected '.<lowercase alphanumeric 1-6>')"
    }
    $bare = $token.TrimStart('.')
    if (-not $allowed.Contains($bare)) {
        Fail "extension token '$token' is not in the packaging allowlist"
    }
    if (-not $seenExt.Add($bare)) {
        Fail "duplicate extension token '$token'"
    }
}

# --- declared files --------------------------------------------------------
$declared = @{}
$files = @($manifest.files)
if ($files.Count -eq 0) { Fail "manifest files list is empty" }
foreach ($entry in $files) {
    foreach ($field in @('name', 'bytes', 'sha256')) {
        Require-Property -Object $entry -Name $field -Context "manifest file entry"
    }
    $name = [string]$entry.name
    if ([string]::IsNullOrWhiteSpace($name) -or [System.IO.Path]::GetFileName($name) -ne $name -or $name -eq '.' -or $name -eq '..') {
        Fail "manifest file name '$name' is not a safe bare filename"
    }
    if ($declared.ContainsKey($name)) { Fail "manifest file '$name' is listed more than once" }
    if ($name -eq $manifestFileName) { Fail "manifest must not list itself as a payload file" }

    $hash = ([string]$entry.sha256).ToLowerInvariant()
    if ($hash -notmatch $sha256Pattern) { Fail "manifest file '$name' has an invalid SHA-256 field" }

    $path = Join-Path $PackageDir $name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        Fail "manifest file '$name' is missing from the staged payload"
    }
    $item = Get-Item -LiteralPath $path
    if ([long]$entry.bytes -ne $item.Length) {
        Fail "manifest file '$name' size $($entry.bytes) does not match on-disk size $($item.Length)"
    }
    $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $hash) {
        Fail "manifest file '$name' SHA-256 mismatch (manifest $hash, on-disk $actual)"
    }
    $declared[$name] = $hash
}

if (-not $declared.ContainsKey($entrypoint)) {
    Fail "entrypoint '$entrypoint' is not listed in the manifest files"
}

foreach ($dep in @($manifest.dynamicDependencies)) {
    if (-not $declared.ContainsKey([string]$dep)) {
        Fail "dynamic dependency '$dep' is not a declared manifest file"
    }
}

# --- exact on-disk set -----------------------------------------------------
$expectedNames = New-Object System.Collections.Generic.HashSet[string]
foreach ($n in $declared.Keys) { [void]$expectedNames.Add($n) }
[void]$expectedNames.Add($manifestFileName)

$onDisk = @{}
foreach ($f in (Get-ChildItem -LiteralPath $PackageDir -File)) { $onDisk[$f.Name] = $f }
foreach ($name in $expectedNames) {
    if (-not $onDisk.ContainsKey($name)) { Fail "payload file '$name' is missing from the staged directory" }
}
foreach ($name in $onDisk.Keys) {
    if (-not $expectedNames.Contains($name)) {
        Fail "staged payload contains undeclared file '$name' (payload-only packaging is required)"
    }
}

# --- excluded artifacts ----------------------------------------------------
$excludedExt = New-Object System.Collections.Generic.HashSet[string]
foreach ($e in @($metadata.excludedExtensions)) { [void]$excludedExt.Add(([string]$e).ToLowerInvariant()) }
foreach ($name in $onDisk.Keys) {
    $ext = [System.IO.Path]::GetExtension($name).ToLowerInvariant()
    if ($excludedExt.Contains($ext)) {
        Fail "excluded build/debug artifact present in staged payload: $name"
    }
}

# Hash of the manifest itself (it is installed as payload evidence but the
# manifest does not list itself; it still must match between directory and ZIP).
$expectedHashes = @{}
foreach ($n in $declared.Keys) { $expectedHashes[$n] = $declared[$n] }
$expectedHashes[$manifestFileName] = (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash.ToLowerInvariant()

# --- expected payload hash + ZIP binding -----------------------------------
if ([string]::IsNullOrWhiteSpace($ZipPath)) {
    $siblingZip = "$PackageDir.zip"
    if (Test-Path -LiteralPath $siblingZip -PathType Leaf) { $ZipPath = $siblingZip }
}
if ([string]::IsNullOrWhiteSpace($ZipPath)) {
    Fail "expected payload hash cannot be verified: pass -ZipPath and/or -ExpectedSha256"
}
if (-not (Test-Path -LiteralPath $ZipPath -PathType Leaf)) {
    Fail "portable ZIP not found: $ZipPath"
}
$ZipPath = (Resolve-Path -LiteralPath $ZipPath).Path

$sidecar = "$ZipPath.sha256"
$sidecarHash = ""
if (Test-Path -LiteralPath $sidecar -PathType Leaf) {
    $line = (Get-Content -LiteralPath $sidecar -Raw).Trim()
    if ($line -match '^([0-9a-fA-F]{64})\b') { $sidecarHash = $Matches[1].ToLowerInvariant() }
    else { Fail "portable ZIP hash sidecar is malformed: $sidecar" }
}

if ([string]::IsNullOrWhiteSpace($ExpectedSha256)) {
    if ([string]::IsNullOrWhiteSpace($sidecarHash)) {
        Fail "no expected payload hash available (pass -ExpectedSha256 or provide $sidecar)"
    }
    $ExpectedSha256 = $sidecarHash
}
$ExpectedSha256 = $ExpectedSha256.Trim().ToLowerInvariant()
if ($ExpectedSha256 -notmatch $sha256Pattern) { Fail "expected payload hash is not a SHA-256 hex string" }
if ((-not [string]::IsNullOrWhiteSpace($sidecarHash)) -and ($sidecarHash -ne $ExpectedSha256)) {
    Fail "expected payload hash does not match the ZIP hash sidecar ($ExpectedSha256 vs $sidecarHash)"
}

$zipHash = (Get-FileHash -LiteralPath $ZipPath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($zipHash -ne $ExpectedSha256) {
    Fail "portable ZIP SHA-256 mismatch (expected $ExpectedSha256, computed $zipHash)"
}

# --- ZIP entries must agree with the manifest ------------------------------
Add-Type -AssemblyName System.IO.Compression | Out-Null
Add-Type -AssemblyName System.IO.Compression.FileSystem | Out-Null
$archive = [System.IO.Compression.ZipFile]::OpenRead($ZipPath)
try {
    $zipEntries = @{}
    foreach ($entry in $archive.Entries) {
        $n = $entry.FullName
        if ([string]::IsNullOrWhiteSpace($n) -or $n.EndsWith('/')) { continue }
        if ([System.IO.Path]::GetFileName($n) -ne $n) { Fail "ZIP entry '$n' is not a bare filename" }
        if ($zipEntries.ContainsKey($n)) { Fail "ZIP contains duplicate entry '$n'" }
        $stream = $entry.Open()
        try {
            $sha = [System.Security.Cryptography.SHA256]::Create()
            try { $hashBytes = $sha.ComputeHash($stream) } finally { $sha.Dispose() }
        }
        finally { $stream.Dispose() }
        $zipEntries[$n] = [pscustomobject]@{
            bytes  = [long]$entry.Length
            sha256 = [BitConverter]::ToString($hashBytes).Replace('-', '').ToLowerInvariant()
        }
    }
    foreach ($name in $expectedNames) {
        if (-not $zipEntries.ContainsKey($name)) { Fail "portable ZIP is missing '$name'" }
        if ($zipEntries[$name].sha256 -ne $expectedHashes[$name]) {
            Fail "portable ZIP entry '$name' does not match the manifest SHA-256"
        }
    }
    foreach ($name in $zipEntries.Keys) {
        if (-not $expectedNames.Contains($name)) { Fail "portable ZIP contains undeclared entry '$name'" }
    }
}
finally { $archive.Dispose() }

# --- evidence --------------------------------------------------------------
$result = [ordered]@{
    schema              = [int]$manifest.schema
    packageName         = [string]$manifest.packageName
    version             = [string]$manifest.version
    platform            = [string]$manifest.platform
    entrypoint          = $entrypoint
    supportedExtensions = @($extensions | ForEach-Object { [string]$_ })
    dynamicDependencies = @($manifest.dynamicDependencies | ForEach-Object { [string]$_ })
    files               = @($declared.Keys | Sort-Object)
    payloadSha256       = $zipHash
    sourceDir           = $PackageDir
    sourceZip           = $ZipPath
}
if (-not [string]::IsNullOrWhiteSpace($ResultPath)) {
    $resultDir = Split-Path -Parent $ResultPath
    if (-not [string]::IsNullOrWhiteSpace($resultDir) -and -not (Test-Path -LiteralPath $resultDir -PathType Container)) {
        New-Item -ItemType Directory -Path $resultDir -Force | Out-Null
    }
    [System.IO.File]::WriteAllText($ResultPath, ($result | ConvertTo-Json -Depth 6), (New-Object System.Text.UTF8Encoding($false)))
}

Write-Host "validate-portable: OK: $entrypoint $($manifest.version) $($manifest.platform), $($declared.Count) files, sha256 $zipHash"
exit 0

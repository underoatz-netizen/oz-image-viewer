# inventory-jpeg.ps1 - headers-only JPEG inventory for the reference benchmark.
#
# Scans a root folder for JPEG files, reads ONLY the file headers (no decode,
# no copy, no write to source images), and selects three distinct files whose
# pixel counts are closest to the requested megapixel targets (default 2, 12,
# 24 MP). Ties are broken by the lexicographically smallest lowercase SHA-256.
#
# Emits a sealed private manifest (schema oz.manifest.v1) plus a sibling seal
# file "<OutPrivate>.sha256" containing the lowercase hex SHA-256 of the
# manifest bytes. The collector rejects the manifest on any seal mismatch
# (tamper detection). Private source paths appear ONLY in the manifest, never
# in console output or public results.
#
# Exact invocation:
#   powershell -NoProfile -ExecutionPolicy Bypass -File benchmark\scripts\inventory-jpeg.ps1 `
#     -Root 'E:\Pictures\n้ำอิง' -TargetsMP 2,12,24 -OutPrivate benchmark\manifests\selection.private.json
param(
    [Parameter(Mandatory = $true)][string]$Root,
    [string]$TargetsMP = "2,12,24",
    [string]$OutPrivate = ""
)
$ErrorActionPreference = 'Stop'

# Headers-only JPEG dimension scan: parse SOF0/SOF1/SOF2 markers within the
# first 64 KiB. Returns a hashtable with Width/Height, or $null when the file
# is not a JPEG or the header cannot be read. Never decodes or writes images.
function Get-JpegDimensions {
    param([string]$Path)
    $fs = [System.IO.File]::OpenRead($Path)
    try {
        $buf = New-Object byte[] 65536
        $read = $fs.Read($buf, 0, $buf.Length)
        if ($read -lt 4) { return $null }
        if ($buf[0] -ne 0xFF -or $buf[1] -ne 0xD8) { return $null }  # not JPEG
        $i = 2
        while ($i + 9 -lt $read) {
            if ($buf[$i] -ne 0xFF) { $i++; continue }
            $marker = $buf[$i + 1]
            if ($marker -eq 0xD8 -or $marker -eq 0x01 -or
                ($marker -ge 0xD0 -and $marker -le 0xD7)) {
                $i += 2; continue  # standalone markers
            }
            if ($i + 4 -gt $read) { break }
            # NB: -shl on a [byte] yields 0 in PowerShell; cast to [int] first.
            $len = ([int]$buf[$i + 2] -shl 8) -bor $buf[$i + 3]
            if ($marker -ge 0xC0 -and $marker -le 0xCF -and
                $marker -ne 0xC4 -and $marker -ne 0xC8 -and $marker -ne 0xCC) {
                # SOF0..SOF15 (excluding DHT/JPG/DAC): precision(1) + height(2) + width(2)
                if ($i + 9 -gt $read) { break }
                $height = ([int]$buf[$i + 5] -shl 8) -bor $buf[$i + 6]
                $width = ([int]$buf[$i + 7] -shl 8) -bor $buf[$i + 8]
                if ($width -gt 0 -and $height -gt 0) {
                    return @{ Width = $width; Height = $height }
                }
                return $null
            }
            $i += 2 + $len
        }
    } finally {
        $fs.Dispose()
    }
    return $null
}

if (-not (Test-Path -LiteralPath $Root)) {
    Write-Error "Root folder not found: $Root"
    exit 1
}

$targets = @($TargetsMP -split ',' | ForEach-Object { [double]$_ })
if ($targets.Count -eq 0) {
    Write-Error "TargetsMP must contain at least one target"
    exit 1
}

Write-Host "Scanning $Root for JPEG headers (no decode, no writes)..."
$files = Get-ChildItem -LiteralPath $Root -Recurse -File -Include *.jpg, *.jpeg -ErrorAction SilentlyContinue
$entries = @()
foreach ($f in $files) {
    $dims = Get-JpegDimensions -Path $f.FullName
    if (-not $dims) { continue }
    $pixels = [int64]$dims.Width * [int64]$dims.Height
    $hash = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    $size = (Get-Item -LiteralPath $f.FullName).Length
    $entries += [pscustomobject]@{
        Path = $f.FullName
        Width = $dims.Width
        Height = $dims.Height
        Pixels = $pixels
        MP = $pixels / 1e6
        Hash = $hash
        Size = $size
    }
}
Write-Host "Found $($entries.Count) readable JPEG files."

# Select one distinct file per target: closest megapixel count, ties broken by
# the lexicographically smallest lowercase SHA-256.
$selected = @()
foreach ($t in $targets) {
    $candidates = $entries |
        Where-Object { $_.Path -notin @($selected | ForEach-Object { $_.Path }) } |
        Sort-Object @{ Expression = { [math]::Abs($_.MP - $t) } }, @{ Expression = { $_.Hash } }
    if ($candidates.Count -eq 0) {
        Write-Warning "no distinct JPEG left for target ${t} MP"
        continue
    }
    $pick = $candidates[0]
    $pick | Add-Member -NotePropertyName TargetMP -NotePropertyValue $t
    $pick | Add-Member -NotePropertyName Deviation -NotePropertyValue ([math]::Abs($pick.MP - $t))
    $selected += $pick
}

if ($selected.Count -eq 0) {
    Write-Error "no JPEG files found under $Root"
    exit 1
}

# Console output: public-safe aliases only (no source paths).
foreach ($s in $selected) {
    $alias = "jpeg-{0}mp" -f $s.TargetMP
    Write-Output ("alias={0} width={1} height={2} pixels={3} target_mp={4} deviation_mp={5} bytes={6} sha256={7}" -f `
        $alias, $s.Width, $s.Height, $s.Pixels, $s.TargetMP,
        [math]::Round($s.Deviation, 4), $s.Size, $s.Hash)
}

if ($OutPrivate) {
    $manifest = [ordered]@{
        schema = 'oz.manifest.v1'
        id = 'selection-' + (Get-Date -Format 'yyyyMMdd-HHmmss')
        images = @($selected | ForEach-Object {
            [ordered]@{
                alias = ("jpeg-{0}mp" -f $_.TargetMP)
                path = $_.Path
                sha256 = $_.Hash
                width = $_.Width
                height = $_.Height
                size_bytes = $_.Size
                pixels = $_.Pixels
                target_mp = $_.TargetMP
                deviation_mp = [math]::Round($_.Deviation, 4)
            }
        })
    }
    $json = $manifest | ConvertTo-Json -Depth 5
    $dir = Split-Path -Parent $OutPrivate
    if ($dir -and -not (Test-Path -LiteralPath $dir)) {
        New-Item -ItemType Directory -Path $dir -Force | Out-Null
    }
    Set-Content -LiteralPath $OutPrivate -Value $json -Encoding UTF8
    $seal = (Get-FileHash -LiteralPath $OutPrivate -Algorithm SHA256).Hash.ToLowerInvariant()
    Set-Content -LiteralPath "$OutPrivate.sha256" -Value $seal -Encoding ASCII
    Write-Host "Private manifest written to $OutPrivate (sealed: $seal)"
} else {
    Write-Host "No -OutPrivate given; manifest not written."
}
# generate-ozviewer-icon.ps1
#
# Authoring-time generator for the OZ Image Viewer application icon.
#
# This script is NOT part of the build. It is committed only to document the
# provenance of the checked-in binary asset `ozviewer.ico` and to make the
# artwork reproducible/reversible. It depends solely on System.Drawing from
# the .NET Framework that ships with Windows, so it adds no runtime or build
# dependency to the project.
#
# Design: "OZ + image" monogram. A deep-navy rounded application tile carries a
# cyan rounded frame (the image/viewport motif) around a high-contrast white
# "OZ" monogram drawn from geometric primitives. The letters stay readable down
# to 16 px; the frame is omitted at tiny sizes so the monogram can grow.
#
# Usage:  powershell -ExecutionPolicy Bypass -File generate-ozviewer-icon.ps1
#
# Output: ozviewer.ico (multi-resolution, 32-bit straight alpha) containing
#         16, 20, 24, 32, 40, 48, 64 (classic BMP/DIB entries) and 256 (PNG).

[CmdletBinding()]
param(
    [string]$OutputPath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

if ([string]::IsNullOrEmpty($OutputPath)) {
    $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
    if ([string]::IsNullOrEmpty($scriptDir)) { $scriptDir = (Get-Location).Path }
    $OutputPath = Join-Path $scriptDir 'ozviewer.ico'
}

$script:Sizes = @(16, 20, 24, 32, 40, 48, 64, 256)

function New-RoundedRectPath {
    param([single]$X, [single]$Y, [single]$W, [single]$H, [single]$Radius)

    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $Radius * 2.0
    $path.AddArc($X, $Y, $d, $d, 180.0, 90.0)
    $path.AddArc($X + $W - $d, $Y, $d, $d, 270.0, 90.0)
    $path.AddArc($X + $W - $d, $Y + $H - $d, $d, $d, 0.0, 90.0)
    $path.AddArc($X, $Y + $H - $d, $d, $d, 90.0, 90.0)
    $path.CloseFigure()
    return $path
}

# Renders the mark at one square resolution with straight (non-premultiplied)
# 32-bit alpha. Geometry is expressed as fractions of the canvas so every size
# shares one definition.
function New-IconBitmap {
    param([int]$Size)

    $s = [single]$Size
    $bmp = New-Object System.Drawing.Bitmap -ArgumentList @(
        $Size, $Size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)

    # --- Application tile: deep navy diagonal gradient, fully rounded. --------
    $tile = New-RoundedRectPath 0.0 0.0 $s $s (0.20 * $s)
    $bgRect = New-Object System.Drawing.RectangleF -ArgumentList @(0.0, 0.0, $s, $s)
    $bgTop = [System.Drawing.Color]::FromArgb(255, 30, 58, 138)   # #1E3A8A
    $bgBot = [System.Drawing.Color]::FromArgb(255, 11, 18, 32)    # #0B1220
    $bgBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush -ArgumentList @(
        $bgRect, $bgTop, $bgBot, [single]45.0)
    $g.FillPath($bgBrush, $tile)
    $bgBrush.Dispose()

    # --- Image/viewport frame: cyan rounded outline. Omitted below 24 px so
    #     the monogram can claim the extra pixels. -----------------------------
    if ($Size -ge 24) {
        $inset = 0.10 * $s
        $frameSide = $s - 2.0 * $inset
        $frame = New-RoundedRectPath $inset $inset $frameSide $frameSide (0.06 * $s)
        $frameWidth = [single][Math]::Max(1.0, 0.032 * $s)
        $framePen = New-Object System.Drawing.Pen -ArgumentList @(
            [System.Drawing.Color]::FromArgb(255, 56, 189, 248), $frameWidth)  # #38BDF8
        $framePen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
        $g.DrawPath($framePen, $frame)
        $framePen.Dispose()
        $frame.Dispose()
    }

    # --- "OZ" monogram: white, geometric, high contrast. ----------------------
    $letterWidth = 0.285 * $s
    $letterGap = 0.11 * $s
    $left = 0.16 * $s
    $oLeft = $left
    $zLeft = $left + $letterWidth + $letterGap
    $top = 0.28 * $s
    $height = 0.44 * $s
    $stroke = [single][Math]::Max(1.75, 0.085 * $s)

    $pen = New-Object System.Drawing.Pen -ArgumentList @(
        [System.Drawing.Color]::White, $stroke)
    $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
    $g.DrawEllipse($pen, $oLeft, $top, $letterWidth, $height)

    $zPoints = @(
        (New-Object System.Drawing.PointF -ArgumentList @($zLeft, $top)),
        (New-Object System.Drawing.PointF -ArgumentList @(($zLeft + $letterWidth), $top)),
        (New-Object System.Drawing.PointF -ArgumentList @($zLeft, ($top + $height))),
        (New-Object System.Drawing.PointF -ArgumentList @(($zLeft + $letterWidth), ($top + $height)))
    )
    $g.DrawLines($pen, $zPoints)
    $pen.Dispose()

    $g.Dispose()
    return $bmp
}

function Get-BitmapBgraBytes {
    param([System.Drawing.Bitmap]$Bitmap)

    $w = $Bitmap.Width
    $h = $Bitmap.Height
    $rect = New-Object System.Drawing.Rectangle -ArgumentList @(0, 0, $w, $h)
    $data = $Bitmap.LockBits(
        $rect,
        [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    try {
        $stride = $data.Stride
        $buffer = New-Object byte[] ($stride * $h)
        [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $buffer, 0, $buffer.Length)
        return [pscustomobject]@{ Bytes = $buffer; Stride = $stride; Width = $w; Height = $h }
    }
    finally {
        $Bitmap.UnlockBits($data)
    }
}

# Classic 32-bit BMP/DIB ICO entry: BITMAPINFOHEADER (height doubled to cover
# the XOR bitmap plus the 1-bpp AND mask) followed by bottom-up BGRA pixels and
# a zeroed AND mask. Straight alpha is preserved in the XOR bitmap.
function ConvertTo-DibEntry {
    param([System.Drawing.Bitmap]$Bitmap)

    $info = Get-BitmapBgraBytes -Bitmap $Bitmap
    $w = $info.Width
    $h = $info.Height

    $ms = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter -ArgumentList @($ms)
    try {
        $bw.Write([uint32]40)               # biSize
        $bw.Write([int32]$w)                # biWidth
        $bw.Write([int32]($h * 2))          # biHeight (XOR + AND)
        $bw.Write([uint16]1)                # biPlanes
        $bw.Write([uint16]32)               # biBitCount
        $bw.Write([uint32]0)                # biCompression = BI_RGB
        $bw.Write([uint32]0)                # biSizeImage
        $bw.Write([int32]0)                 # biXPelsPerMeter
        $bw.Write([int32]0)                 # biYPelsPerMeter
        $bw.Write([uint32]0)                # biClrUsed
        $bw.Write([uint32]0)                # biClrImportant

        for ($row = $h - 1; $row -ge 0; $row--) {
            $bw.Write($info.Bytes, $row * $info.Stride, $w * 4)
        }

        $maskStride = [int]((([Math]::Floor(($w + 31) / 32)) * 4))
        $zeroMask = New-Object byte[] ($maskStride * $h)
        $bw.Write($zeroMask, 0, $zeroMask.Length)

        $bw.Flush()
        return $ms.ToArray()
    }
    finally {
        $bw.Dispose()
        $ms.Dispose()
    }
}

function ConvertTo-PngEntry {
    param([System.Drawing.Bitmap]$Bitmap)

    $ms = New-Object System.IO.MemoryStream
    try {
        $Bitmap.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
        return $ms.ToArray()
    }
    finally {
        $ms.Dispose()
    }
}

# --- Build every frame and assemble one ICONDIR container. --------------------
$frames = @()
foreach ($size in $script:Sizes) {
    $bmp = New-IconBitmap -Size $size
    try {
        # PNG compression is only universally parsed by the shell for 256 px;
        # smaller frames use classic 32-bit DIB entries.
        if ($size -ge 256) {
            $payload = ConvertTo-PngEntry -Bitmap $bmp
        }
        else {
            $payload = ConvertTo-DibEntry -Bitmap $bmp
        }
        $frames += [pscustomobject]@{ Size = $size; Bytes = $payload }
    }
    finally {
        $bmp.Dispose()
    }
}

$out = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter -ArgumentList @($out)
try {
    $bw.Write([uint16]0)                 # reserved
    $bw.Write([uint16]1)                 # type = icon
    $bw.Write([uint16]$frames.Count)     # image count

    $offset = 6 + (16 * $frames.Count)
    foreach ($frame in $frames) {
        $dim = if ($frame.Size -ge 256) { 0 } else { $frame.Size }
        $bw.Write([byte]$dim)            # width (0 => 256)
        $bw.Write([byte]$dim)            # height (0 => 256)
        $bw.Write([byte]0)               # palette count
        $bw.Write([byte]0)               # reserved
        $bw.Write([uint16]1)             # color planes
        $bw.Write([uint16]32)            # bits per pixel
        $bw.Write([uint32]$frame.Bytes.Length)
        $bw.Write([uint32]$offset)
        $offset += $frame.Bytes.Length
    }
    foreach ($frame in $frames) {
        $bw.Write($frame.Bytes, 0, $frame.Bytes.Length)
    }
    $bw.Flush()
    [System.IO.File]::WriteAllBytes($OutputPath, $out.ToArray())
}
finally {
    $bw.Dispose()
    $out.Dispose()
}

Write-Host ("Wrote {0} ({1} bytes, {2} frames: {3})" -f `
    $OutputPath, (Get-Item -LiteralPath $OutputPath).Length, `
    $frames.Count, (($script:Sizes) -join ', '))

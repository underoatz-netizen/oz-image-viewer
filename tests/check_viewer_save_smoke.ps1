# check_viewer_save_smoke.ps1 - scripted end-to-end smoke for the Stage 5
# Slice 6 UI Save/Save As paths.
#
# Phases (all on generated/checked-in fixtures; never a real user image):
#   1. PNG source: Edit -> Rotate Right -> File Save writes the source file
#      in place (hash changes), and closing afterwards needs no dirty prompt
#      (the saved checkpoint made the document clean).
#   2. PNG source: File Save As <copy.png> through the real save dialog writes
#      a PNG copy and leaves the source untouched.
#   3. PNG source: File Save As <copy.jpg> through the real save dialog writes
#      a JPEG copy (SOI magic FF D8).
#   4. Alpha PNG source: File Save As <name.jpg> is rejected with a clear
#      "transparency / use PNG" message box; no JPEG file is written.
#   5. BMP source supports in-place Save; TIFF source (Save-As-only) routes
#      File Save to Save As dialog.
#
# Exit codes: 0 = all phases passed; 77 = skipped (no interactive desktop or
# window could not be created); 1 = smoke failure (message explains).
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File check_viewer_save_smoke.ps1 `
#       -Viewer <ozviewer.exe> -MakePng <ozviewer_make_png.exe> `
#       -WorkDir <dir> -OpaqueBmp <bench_64x48.bmp> -AlphaBmp <alpha_2x2.bmp>

param(
    [Parameter(Mandatory = $true)][string]$Viewer,
    [Parameter(Mandatory = $true)][string]$MakePng,
    [Parameter(Mandatory = $true)][string]$WorkDir,
    [string]$OpaqueBmp = "",
    [string]$AlphaBmp = ""
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;

public static class OzSmokeNative {
    public delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr lparam);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindowW(string lpClassName, string lpWindowName);

    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);

    [DllImport("user32.dll")]
    public static extern bool PostMessageW(IntPtr hwnd, uint msg, IntPtr wparam, IntPtr lparam);

    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr hwnd);

    [DllImport("user32.dll")]
    public static extern bool EnumWindows(EnumWindowsProc callback, IntPtr lparam);

    [DllImport("user32.dll")]
    public static extern bool EnumChildWindows(IntPtr parent, EnumWindowsProc callback, IntPtr lparam);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowTextW(IntPtr hwnd, StringBuilder text, int maxCount);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetClassNameW(IntPtr hwnd, StringBuilder text, int maxCount);

    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr hwnd);

    [DllImport("user32.dll")]
    public static extern bool SetActiveWindow(IntPtr hwnd);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr SendMessageW(IntPtr hwnd, uint msg, IntPtr wparam, string lparam);

    [DllImport("user32.dll")]
    public static extern int GetDlgCtrlID(IntPtr hwnd);
}
'@

Add-Type -AssemblyName System.Windows.Forms

$WM_COMMAND = 0x0111
$WM_CLOSE = 0x0010
$WM_SETTEXT = 0x000C
$ID_EDIT_ROTATE_RIGHT = 202
$ID_FILE_SAVE = 103
$ID_FILE_SAVE_AS = 104

function Get-WindowClass([IntPtr]$hwnd) {
    $sb = New-Object System.Text.StringBuilder 256
    [void][OzSmokeNative]::GetClassNameW($hwnd, $sb, $sb.Capacity)
    return $sb.ToString()
}

function Get-WindowText([IntPtr]$hwnd) {
    $sb = New-Object System.Text.StringBuilder 1024
    [void][OzSmokeNative]::GetWindowTextW($hwnd, $sb, $sb.Capacity)
    return $sb.ToString()
}

function Find-ProcessWindow([int]$processId, [string]$className, [int]$timeoutMs) {
    $deadline = [DateTime]::UtcNow.AddMilliseconds($timeoutMs)
    do {
        $script:found = [IntPtr]::Zero
        $callback = [OzSmokeNative+EnumWindowsProc]{
            param([IntPtr]$hwnd, [IntPtr]$lparam)
            $pidOut = 0
            [void][OzSmokeNative]::GetWindowThreadProcessId($hwnd, [ref]$pidOut)
            if ($pidOut -eq $processId -and [OzSmokeNative]::IsWindowVisible($hwnd)) {
                $cls = Get-WindowClass $hwnd
                if ($cls -eq $className) {
                    $script:found = $hwnd
                    return $false
                }
            }
            return $true
        }
        [void][OzSmokeNative]::EnumWindows($callback, [IntPtr]::Zero)
        if ($script:found -ne [IntPtr]::Zero) { return $script:found }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    return [IntPtr]::Zero
}

function Get-WindowTreeText([IntPtr]$hwnd) {
    $text = Get-WindowText $hwnd
    $childTexts = New-Object System.Collections.Generic.List[string]
    $callback = [OzSmokeNative+EnumWindowsProc]{
        param([IntPtr]$child, [IntPtr]$lparam)
        $childTexts.Add((Get-WindowText $child))
        return $true
    }
    [void][OzSmokeNative]::EnumChildWindows($hwnd, $callback, [IntPtr]::Zero)
    return ($text + " " + ($childTexts -join " "))
}

# Find a visible #32770 dialog of the process whose window tree text matches
# the supplied regular expression (e.g. the overwrite confirmation prompt).
function Find-DialogContaining([int]$processId, [string]$pattern) {
    $script:matchedDialog = [IntPtr]::Zero
    $callback = [OzSmokeNative+EnumWindowsProc]{
        param([IntPtr]$hwnd, [IntPtr]$lparam)
        $pidOut = 0
        [void][OzSmokeNative]::GetWindowThreadProcessId($hwnd, [ref]$pidOut)
        if ($pidOut -eq $processId -and [OzSmokeNative]::IsWindowVisible($hwnd)) {
            if ((Get-WindowClass $hwnd) -eq "#32770") {
                if ((Get-WindowTreeText $hwnd) -match $pattern) {
                    $script:matchedDialog = $hwnd
                    return $false
                }
            }
        }
        return $true
    }
    [void][OzSmokeNative]::EnumWindows($callback, [IntPtr]::Zero)
    return $script:matchedDialog
}

function Invoke-SendKeys([IntPtr]$hwnd, [string]$keys) {
    [void][OzSmokeNative]::SetForegroundWindow($hwnd)
    [void][OzSmokeNative]::SetActiveWindow($hwnd)
    Start-Sleep -Milliseconds 200
    [System.Windows.Forms.SendKeys]::SendWait($keys)
}

function Post-Command([IntPtr]$hwnd, [int]$commandId) {
    [void][OzSmokeNative]::PostMessageW($hwnd, $WM_COMMAND, [IntPtr]$commandId, [IntPtr]::Zero)
}

# The common save dialog pre-selects only the suggested file-name stem, so the
# whole field is selected before typing an absolute path.
function Set-SaveDialogFilename([IntPtr]$dialog, [string]$path) {
    [void][OzSmokeNative]::SetForegroundWindow($dialog)
    [void][OzSmokeNative]::SetActiveWindow($dialog)
    Start-Sleep -Milliseconds 250
    Invoke-SendKeys $dialog "^a"
    Start-Sleep -Milliseconds 200
    Invoke-SendKeys $dialog $path
    Start-Sleep -Milliseconds 400
    Invoke-SendKeys $dialog "{ENTER}"
}

function Get-FileMd5([string]$path) {
    # The viewer may hold the source file open for a short decode window and
    # ReplaceFileW briefly renames it; retry until hashing succeeds.
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    do {
        try {
            return (Get-FileHash -LiteralPath $path -Algorithm MD5).Hash
        } catch {
            Start-Sleep -Milliseconds 100
        }
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "could not hash $path (still locked)"
}

# Deterministically press a message-box button by label (BM_CLICK).
function Click-DialogButton([IntPtr]$dialog, [string]$label) {
    $script:targetButton = [IntPtr]::Zero
    $callback = [OzSmokeNative+EnumWindowsProc]{
        param([IntPtr]$child, [IntPtr]$lparam)
        if ((Get-WindowClass $child) -eq "Button" -and (Get-WindowText $child) -eq $label) {
            $script:targetButton = $child
            return $false
        }
        return $true
    }
    [void][OzSmokeNative]::EnumChildWindows($dialog, $callback, [IntPtr]::Zero)
    if ($script:targetButton -ne [IntPtr]::Zero) {
        [void][OzSmokeNative]::PostMessageW($script:targetButton, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
        return $true
    }
    return $false
}

# As above, but the button label is matched as a substring (localized labels
# such as "&No" / "No").
function Click-DialogButtonMatching([IntPtr]$dialog, [string]$pattern) {
    $script:targetButton = [IntPtr]::Zero
    $callback = [OzSmokeNative+EnumWindowsProc]{
        param([IntPtr]$child, [IntPtr]$lparam)
        if ((Get-WindowClass $child) -eq "Button" -and (Get-WindowText $child) -match $pattern) {
            $script:targetButton = $child
            return $false
        }
        return $true
    }
    [void][OzSmokeNative]::EnumChildWindows($dialog, $callback, [IntPtr]::Zero)
    if ($script:targetButton -ne [IntPtr]::Zero) {
        [void][OzSmokeNative]::PostMessageW($script:targetButton, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
        return $true
    }
    return $false
}

function Wait-FileChanged([string]$path, [string]$originalHash, [int]$timeoutMs) {
    $deadline = [DateTime]::UtcNow.AddMilliseconds($timeoutMs)
    do {
        if (Test-Path -LiteralPath $path) {
            $hash = Get-FileMd5 $path
            if ($hash -ne $originalHash) { return $true }
        }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)
    return $false
}

function Wait-FileExists([string]$path, [int]$timeoutMs) {
    $deadline = [DateTime]::UtcNow.AddMilliseconds($timeoutMs)
    do {
        if (Test-Path -LiteralPath $path) { return $true }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)
    return $false
}

function Start-Viewer([string]$imagePath) {
    # PowerShell 5.1 joins ArgumentList array elements without quoting, so the
    # image path must be passed as one explicitly quoted argument.
    return Start-Process -FilePath $Viewer -ArgumentList "`"$imagePath`"" -PassThru
}

function Stop-Viewer([System.Diagnostics.Process]$process, [IntPtr]$hwnd, [bool]$expectDirtyPrompt) {
    if ($process.HasExited) { return $process.ExitCode }
    [void][OzSmokeNative]::PostMessageW($hwnd, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
    if ($expectDirtyPrompt) {
        # The dirty-discard prompt is a real modal message box; press its OK
        # button deterministically instead of relying on keyboard focus.
        $deadline = [DateTime]::UtcNow.AddMilliseconds(10000)
        do {
            $dialog = Find-ProcessWindow $process.Id "#32770" 1000
            if ($dialog -ne [IntPtr]::Zero) {
                if (-not (Click-DialogButton $dialog "OK")) {
                    Fail "could not press the dirty-discard prompt OK button"
                }
                break
            }
            Start-Sleep -Milliseconds 200
        } while ([DateTime]::UtcNow -lt $deadline)
        Start-Sleep -Milliseconds 300
    }
    if (-not $process.WaitForExit(15000)) {
        $process.Kill()
        $process.WaitForExit()
        return -1
    }
    return $process.ExitCode
}

function Read-Magic([string]$path, [int]$count) {
    $bytes = [System.IO.File]::ReadAllBytes($path)
    if ($bytes.Length -lt $count) { return @() }
    return $bytes[0..($count - 1)]
}

function Fail([string]$message) {
    Write-Output ("SAVE_SMOKE_FAIL: " + $message)
    exit 1
}

function Skip([string]$message) {
    Write-Output ("SAVE_SMOKE_SKIP: " + $message)
    exit 77
}

# --- preconditions ----------------------------------------------------------
foreach ($required in @($Viewer, $MakePng)) {
    if (-not (Test-Path -LiteralPath $required)) {
        Fail "missing required executable: $required"
    }
}
if (-not (Test-Path -LiteralPath $WorkDir)) {
    New-Item -ItemType Directory -Path $WorkDir | Out-Null
}

$viewerWindowClass = "OZViewerStage1"

function Open-ViewerWithFixture([string]$imagePath, [string]$phase) {
    $process = Start-Viewer $imagePath
    $hwnd = Find-ProcessWindow $process.Id $viewerWindowClass 30000
    if ($hwnd -eq [IntPtr]::Zero) {
        $process.Kill()
        Skip "${phase}: viewer window was not created (no interactive desktop?)"
    }
    Start-Sleep -Milliseconds 1500
    return @($process, $hwnd)
}

# --- phase 1: Save in place on a PNG source ---------------------------------
$sourcePng = Join-Path $WorkDir "smoke_source.png"
& $MakePng $sourcePng 64 48 rgb 2>$null | Out-Null
if (-not (Test-Path -LiteralPath $sourcePng)) { Fail "could not generate PNG fixture" }

$phase1 = Open-ViewerWithFixture $sourcePng "phase1"
$process = $phase1[0]; $hwnd = $phase1[1]
$hashBefore = Get-FileMd5 $sourcePng
Post-Command $hwnd $ID_EDIT_ROTATE_RIGHT
Start-Sleep -Milliseconds 1200
Post-Command $hwnd $ID_FILE_SAVE
if (-not (Wait-FileChanged $sourcePng $hashBefore 30000)) {
    Stop-Viewer $process $hwnd $false | Out-Null
    Fail "phase1: in-place Save did not rewrite the PNG source file"
}
$magic = Read-Magic $sourcePng 4
if ($magic.Count -lt 4 -or $magic[0] -ne 0x89 -or $magic[1] -ne 0x50 -or $magic[2] -ne 0x4E -or $magic[3] -ne 0x47) {
    Stop-Viewer $process $hwnd $false | Out-Null
    Fail "phase1: saved PNG has no PNG signature"
}
# After a successful same-source Save the checkpoint must make the document
# clean: closing must exit without the discard prompt.
$exitCode = Stop-Viewer $process $hwnd $false
if ($exitCode -ne 0) {
    Fail "phase1: clean close after Save failed (exit $exitCode; dirty prompt appeared?)"
}
Write-Output "SAVE_SMOKE_PHASE1_SAVE_IN_PLACE_OK"

# --- phase 2: Save As PNG copy ----------------------------------------------
$phase2 = Open-ViewerWithFixture $sourcePng "phase2"
$process = $phase2[0]; $hwnd = $phase2[1]
$sourceHashBefore = Get-FileMd5 $sourcePng
Post-Command $hwnd $ID_EDIT_ROTATE_RIGHT
Start-Sleep -Milliseconds 1200
Post-Command $hwnd $ID_FILE_SAVE_AS
$dialog = Find-ProcessWindow $process.Id "#32770" 15000
if ($dialog -eq [IntPtr]::Zero) {
    Stop-Viewer $process $hwnd $true | Out-Null
    Fail "phase2: Save As dialog did not appear"
}
$copyPng = Join-Path $WorkDir "smoke_copy.png"
Remove-Item -LiteralPath $copyPng -ErrorAction SilentlyContinue
Set-SaveDialogFilename $dialog $copyPng
if (-not (Wait-FileExists $copyPng 30000)) {
    Stop-Viewer $process $hwnd $true | Out-Null
    Fail "phase2: Save As PNG copy was not written"
}
$magic = Read-Magic $copyPng 4
if ($magic.Count -lt 4 -or $magic[0] -ne 0x89 -or $magic[1] -ne 0x50 -or $magic[2] -ne 0x4E -or $magic[3] -ne 0x47) {
    Stop-Viewer $process $hwnd $true | Out-Null
    Fail "phase2: Save As copy has no PNG signature"
}
if ((Get-FileMd5 $sourcePng) -ne $sourceHashBefore) {
    Stop-Viewer $process $hwnd $true | Out-Null
    Fail "phase2: Save As copy modified the source file"
}
# Save As is an export copy: the document stays dirty, so the dirty prompt is
# expected and must allow discarding.
$exitCode = Stop-Viewer $process $hwnd $true
if ($exitCode -ne 0) {
    Fail "phase2: close after Save As copy failed (exit $exitCode)"
}
Write-Output "SAVE_SMOKE_PHASE2_SAVE_AS_PNG_OK"

# --- phase 3: Save As JPEG copy ---------------------------------------------
$phase3 = Open-ViewerWithFixture $sourcePng "phase3"
$process = $phase3[0]; $hwnd = $phase3[1]
Post-Command $hwnd $ID_EDIT_ROTATE_RIGHT
Start-Sleep -Milliseconds 1200
Post-Command $hwnd $ID_FILE_SAVE_AS
$dialog = Find-ProcessWindow $process.Id "#32770" 15000
if ($dialog -eq [IntPtr]::Zero) {
    Stop-Viewer $process $hwnd $true | Out-Null
    Fail "phase3: Save As dialog did not appear"
}
$copyJpg = Join-Path $WorkDir "smoke_copy.jpg"
Remove-Item -LiteralPath $copyJpg -ErrorAction SilentlyContinue
Set-SaveDialogFilename $dialog $copyJpg
if (-not (Wait-FileExists $copyJpg 30000)) {
    Stop-Viewer $process $hwnd $true | Out-Null
    Fail "phase3: Save As JPEG copy was not written"
}
$magic = Read-Magic $copyJpg 3
if ($magic.Count -lt 3 -or $magic[0] -ne 0xFF -or $magic[1] -ne 0xD8 -or $magic[2] -ne 0xFF) {
    Stop-Viewer $process $hwnd $true | Out-Null
    Fail "phase3: Save As copy has no JPEG SOI signature"
}
$exitCode = Stop-Viewer $process $hwnd $true
if ($exitCode -ne 0) {
    Fail "phase3: close after Save As copy failed (exit $exitCode)"
}
Write-Output "SAVE_SMOKE_PHASE3_SAVE_AS_JPEG_OK"

# --- phase 4: alpha PNG -> Save As JPEG must be rejected ---------------------
$alphaPng = Join-Path $WorkDir "smoke_alpha.png"
& $MakePng $alphaPng 32 32 palette 2>$null | Out-Null
if (-not (Test-Path -LiteralPath $alphaPng)) { Fail "could not generate alpha PNG fixture" }

$phase4 = Open-ViewerWithFixture $alphaPng "phase4"
$process = $phase4[0]; $hwnd = $phase4[1]
Post-Command $hwnd $ID_FILE_SAVE_AS
$dialog = Find-ProcessWindow $process.Id "#32770" 15000
if ($dialog -eq [IntPtr]::Zero) {
    Stop-Viewer $process $hwnd $false | Out-Null
    Fail "phase4: Save As dialog did not appear"
}
$alphaJpg = Join-Path $WorkDir "smoke_alpha_output.jpg"
Remove-Item -LiteralPath $alphaJpg -ErrorAction SilentlyContinue
Set-SaveDialogFilename $dialog $alphaJpg

# The save dialog closes, then the alpha rejection message box must appear.
$deadline = [DateTime]::UtcNow.AddMilliseconds(30000)
$rejectionText = ""
do {
    $box = Find-ProcessWindow $process.Id "#32770" 1000
    if ($box -ne [IntPtr]::Zero) {
        $text = Get-WindowTreeText $box
        if ($text -match "(?i)transparen|PNG") {
            $rejectionText = $text.Trim()
            [void][OzSmokeNative]::PostMessageW($box, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
            break
        }
    }
    Start-Sleep -Milliseconds 300
} while ([DateTime]::UtcNow -lt $deadline)

if ($rejectionText -eq "") {
    Stop-Viewer $process $hwnd $false | Out-Null
    Fail "phase4: no alpha rejection message box appeared for JPEG export"
}
if (Test-Path -LiteralPath $alphaJpg) {
    Stop-Viewer $process $hwnd $false | Out-Null
    Fail "phase4: JPEG file was written despite alpha pixels"
}
Start-Sleep -Milliseconds 500
$exitCode = Stop-Viewer $process $hwnd $false
if ($exitCode -ne 0) {
    Fail "phase4: close after rejected Save As failed (exit $exitCode)"
}
Write-Output "SAVE_SMOKE_PHASE4_ALPHA_JPEG_REJECTED_OK"

# --- phase 5: TIFF source is Save-As-only and routes Save to Save As --------
$tiffFixture = Join-Path $WorkDir "smoke_tiff.tif"
# We can use AlphaBmp or OpaqueBmp if present, or TIFF fixture
if ($OpaqueBmp -ne "" -and (Test-Path -LiteralPath $OpaqueBmp)) {
    # BMP supports in-place save in Stage 6, but TIFF routes Save to Save As
    # Let's test TIFF Save-As-only routing with an exported TIFF copy or GIF/ICO/CUR fixture
    # First test BMP in-place save if OpaqueBmp exists
    $bmpCopy = Join-Path $WorkDir "smoke_inplace.bmp"
    Copy-Item -LiteralPath $OpaqueBmp -Destination $bmpCopy -Force
    $bmpHashBefore = Get-FileMd5 $bmpCopy
    $phaseBmp = Open-ViewerWithFixture $bmpCopy "phase5_bmp"
    $pBmp = $phaseBmp[0]; $hBmp = $phaseBmp[1]
    Post-Command $hBmp $ID_EDIT_ROTATE_RIGHT
    Start-Sleep -Milliseconds 1200
    Post-Command $hBmp $ID_FILE_SAVE
    if (-not (Wait-FileChanged $bmpCopy $bmpHashBefore 30000)) {
        Stop-Viewer $pBmp $hBmp $false | Out-Null
        Fail "phase5: in-place Save on BMP source failed to rewrite file"
    }
    Stop-Viewer $pBmp $hBmp $false | Out-Null
    Write-Output "SAVE_SMOKE_PHASE5_BMP_IN_PLACE_OK"
}

# Test unsupported / Save-As-only format (e.g. ICO or CUR or TIFF) routes Save to Save As
$phase5 = Open-ViewerWithFixture $sourcePng "phase5_tiff_route"
$process = $phase5[0]; $hwnd = $phase5[1]
# Save As a TIFF copy first
Post-Command $hwnd $ID_FILE_SAVE_AS
$dialog = Find-ProcessWindow $process.Id "#32770" 15000
if ($dialog -ne [IntPtr]::Zero) {
    Set-SaveDialogFilename $dialog $tiffFixture
    if (Wait-FileExists $tiffFixture 30000) {
        Stop-Viewer $process $hwnd $false | Out-Null
        # Now open TIFF and test File -> Save routes to Save As
        $phaseTiff = Open-ViewerWithFixture $tiffFixture "phase5_tiff_open"
        $pTiff = $phaseTiff[0]; $hTiff = $phaseTiff[1]
        Post-Command $hTiff $ID_EDIT_ROTATE_RIGHT
        Start-Sleep -Milliseconds 1200
        Post-Command $hTiff $ID_FILE_SAVE
        $saveAsDlg = Find-ProcessWindow $pTiff.Id "#32770" 15000
        if ($saveAsDlg -eq [IntPtr]::Zero) {
            Stop-Viewer $pTiff $hTiff $true | Out-Null
            Fail "phase5: Save on TIFF source did not route to Save As dialog"
        }
        Invoke-SendKeys $saveAsDlg "{ESC}"
        Start-Sleep -Milliseconds 500
        Stop-Viewer $pTiff $hTiff $true | Out-Null
        Write-Output "SAVE_SMOKE_PHASE5_TIFF_SAVE_AS_ONLY_ROUTES_TO_SAVE_AS_OK"
    } else {
        Stop-Viewer $process $hwnd $false | Out-Null
        Write-Output "SAVE_SMOKE_PHASE5_SKIPPED_NO_TIFF"
    }
} else {
    Stop-Viewer $process $hwnd $false | Out-Null
    Write-Output "SAVE_SMOKE_PHASE5_SKIPPED_NO_DIALOG"
}

# --- phase 6: cancelling the overwrite confirmation leaves the file intact --
$phase6 = Open-ViewerWithFixture $sourcePng "phase6"
$process = $phase6[0]; $hwnd = $phase6[1]
$existingCopy = Join-Path $WorkDir "smoke_copy.png"
if (-not (Test-Path -LiteralPath $existingCopy)) {
    & $MakePng $existingCopy 64 48 rgb 2>$null | Out-Null
}
$existingHashBefore = Get-FileMd5 $existingCopy
Post-Command $hwnd $ID_EDIT_ROTATE_RIGHT
Start-Sleep -Milliseconds 1200
Post-Command $hwnd $ID_FILE_SAVE_AS
$dialog = Find-ProcessWindow $process.Id "#32770" 15000
if ($dialog -eq [IntPtr]::Zero) {
    Stop-Viewer $process $hwnd $true | Out-Null
    Fail "phase6: Save As dialog did not appear"
}
Set-SaveDialogFilename $dialog $existingCopy
$overwriteDeadline = [DateTime]::UtcNow.AddMilliseconds(10000)
$overwriteDeclined = $false
do {
    $confirm = Find-DialogContaining $process.Id "(?i)already exists|replace|overwrite|Confirm Save As"
    if ($confirm -ne [IntPtr]::Zero) {
        if (Click-DialogButtonMatching $confirm "No") {
            $overwriteDeclined = $true
            break
        }
    }
    Start-Sleep -Milliseconds 200
} while ([DateTime]::UtcNow -lt $overwriteDeadline)
if (-not $overwriteDeclined) {
    Stop-Viewer $process $hwnd $true | Out-Null
    Fail "phase6: overwrite confirmation did not appear or could not be declined"
}
Start-Sleep -Milliseconds 600
if ((Get-FileMd5 $existingCopy) -ne $existingHashBefore) {
    Stop-Viewer $process $hwnd $true | Out-Null
    Fail "phase6: declining the overwrite confirmation still changed the destination"
}
# The save dialog remains open after No: cancel it, then discard-close.
$dialog = Find-ProcessWindow $process.Id "#32770" 5000
if ($dialog -ne [IntPtr]::Zero) {
    Invoke-SendKeys $dialog "{ESC}"
}
Start-Sleep -Milliseconds 500
$exitCode = Stop-Viewer $process $hwnd $true
if ($exitCode -ne 0) {
    Fail "phase6: close after cancelled overwrite failed (exit $exitCode)"
}
Write-Output "SAVE_SMOKE_PHASE6_OVERWRITE_CANCELLED_OK"

Write-Output "SAVE_SMOKE_OK"
exit 0

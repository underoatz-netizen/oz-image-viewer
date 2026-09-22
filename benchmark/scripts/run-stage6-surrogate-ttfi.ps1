param(
    [string]$EvidenceDir = "",
    [switch]$Help
)

$ErrorActionPreference = "Stop"

if ($Help) {
    Write-Host @"
run-stage6-surrogate-ttfi.ps1 - Automated execution of the Stage 6 surrogate WebP encoder TTFI protocol

Usage:
  .\benchmark\scripts\run-stage6-surrogate-ttfi.ps1 [-EvidenceDir <path>]

Description:
  Executes the OPENAI_SOL-approved Stage 6 surrogate TTFI control protocol comparing:
    - Candidate: static WebP decoder+demux + editing + static WebP encoder (OZVIEWER_ENABLE_WEBP_ENCODER=ON)
    - Control:   static WebP decoder+demux + editing + NO WebP encoder (OZVIEWER_ENABLE_WEBP_ENCODER=OFF)
  Uses deterministic TurboJPEG synthetic gradient corpus (2MP, 12MP, 24MP),
  1 warmup + 20 measured fresh-process runs per cell, interleaved execution order
  (2MP Cand->Ctrl, 12MP Ctrl->Cand, 24MP Cand->Ctrl), FirstPresent relative_ns metric,
  and hard <=5.00% regression gate.
"@
    exit 0
}

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")
Set-Location -LiteralPath $RepoRoot

$UtcTag = (Get-Date).ToUniversalTime().ToString("yyyyMMdd-HHmmss")
if (-not $EvidenceDir) {
    $EvidenceDir = Join-Path $RepoRoot "benchmark\evidence\stage6-surrogate-$UtcTag"
}

Write-Host "=== Stage 6 Surrogate WebP Encoder TTFI Benchmark ==="
Write-Host "Repo Root:    $RepoRoot"
Write-Host "Evidence Dir: $EvidenceDir"
Write-Host "UTC Tag:      $UtcTag"

$CandidateDir = Join-Path $RepoRoot "build\stage6-surrogate-candidate"
$ControlDir   = Join-Path $RepoRoot "build\stage6-surrogate-control"

$CandidateViewer = Join-Path $CandidateDir "src\viewer\Release\ozviewer.exe"
$ControlViewer   = Join-Path $ControlDir   "src\viewer\Release\ozviewer.exe"
$Runner          = Join-Path $CandidateDir "src\runner\Release\ozbench.exe"
$JpegGen         = Join-Path $CandidateDir "tests\Release\ozviewer_make_jpeg.exe"

# 1. Preflight tool existence
foreach ($tool in @($CandidateViewer, $ControlViewer, $Runner, $JpegGen)) {
    if (-not (Test-Path -LiteralPath $tool)) {
        throw "Required binary not found: $tool. Please build Release targets first."
    }
}

# 2. Directory structure setup
$DirBuildProv = Join-Path $EvidenceDir "build-provenance"
$DirCorpus    = Join-Path $EvidenceDir "corpus"
$DirPlans     = Join-Path $EvidenceDir "plans"
$DirRawCand   = Join-Path $EvidenceDir "raw\candidate"
$DirRawCtrl   = Join-Path $EvidenceDir "raw\control"
$DirSummCand  = Join-Path $EvidenceDir "summaries\candidate"
$DirSummCtrl  = Join-Path $EvidenceDir "summaries\control"

$allDirs = @(
    $EvidenceDir,
    $DirBuildProv,
    (Join-Path $DirBuildProv "candidate"),
    (Join-Path $DirBuildProv "control"),
    $DirCorpus,
    $DirPlans,
    (Join-Path $EvidenceDir "raw"),
    $DirRawCand,
    $DirRawCtrl,
    (Join-Path $EvidenceDir "summaries"),
    $DirSummCand,
    $DirSummCtrl
)
foreach ($d in $allDirs) {
    if (-not (Test-Path -LiteralPath $d)) {
        New-Item -ItemType Directory -Path $d -Force | Out-Null
    }
}

# Helper: Compute SHA-256 lowercase hex
function Get-FileSha256 {
    param([string]$Path)
    $hash = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    return $hash
}

# 3. Generate deterministic synthetic corpus
Write-Host "`n[1/5] Generating deterministic synthetic corpus..."
$CorpusSpecs = @(
    @{ Alias = "jpeg-2mp";  Width = 2000; Height = 1000; File = "jpeg-2mp.jpg";  TargetMp = 2.0  },
    @{ Alias = "jpeg-12mp"; Width = 4000; Height = 3000; File = "jpeg-12mp.jpg"; TargetMp = 12.0 },
    @{ Alias = "jpeg-24mp"; Width = 6000; Height = 4000; File = "jpeg-24mp.jpg"; TargetMp = 24.0 }
)

$CorpusManifestItems = @()
$TempVerifyDir = Join-Path $EvidenceDir "corpus_verify_temp"
New-Item -ItemType Directory -Path $TempVerifyDir -Force | Out-Null

try {
    foreach ($spec in $CorpusSpecs) {
        $outPath = Join-Path $DirCorpus $spec.File
        $tempPath = Join-Path $TempVerifyDir $spec.File
        $cmd = "& `"$JpegGen`" `"$outPath`" $($spec.Width) $($spec.Height)"
        
        Write-Host "  Generating $($spec.File) ($($spec.Width)x$($spec.Height))..."
        & $JpegGen $outPath $spec.Width $spec.Height
        if ($LASTEXITCODE -ne 0) { throw "ozviewer_make_jpeg failed for $outPath" }

        # Determinism check
        & $JpegGen $tempPath $spec.Width $spec.Height
        if ($LASTEXITCODE -ne 0) { throw "ozviewer_make_jpeg failed for temp verification" }

        $sha1 = Get-FileSha256 $outPath
        $sha2 = Get-FileSha256 $tempPath
        if ($sha1 -ne $sha2) {
            throw "FATAL: Non-deterministic generator output detected for $($spec.File) ($sha1 vs $sha2)"
        }

        $fi = Get-Item -LiteralPath $outPath
        $pixels = [int64]$spec.Width * [int64]$spec.Height
        $actualMp = [math]::Round($pixels / 1000000.0, 4)
        $deviation = [math]::Round([math]::Abs($actualMp - $spec.TargetMp), 4)

        $CorpusManifestItems += [ordered]@{
            alias = $spec.Alias
            filename = $spec.File
            relative_path = "corpus/$($spec.File)"
            width = $spec.Width
            height = $spec.Height
            total_pixels = $pixels
            target_mp = $spec.TargetMp
            actual_mp = $actualMp
            mp_deviation = $deviation
            size_bytes = $fi.Length
            sha256 = $sha1
            generation_command = "ozviewer_make_jpeg $($spec.File) $($spec.Width) $($spec.Height)"
            sampling = "TJSAMP_420"
            quality = 90
            dct = "TJFLAG_FASTDCT"
        }
    }
} finally {
    if (Test-Path -LiteralPath $TempVerifyDir) {
        Remove-Item -LiteralPath $TempVerifyDir -Recurse -Force
    }
}

$ManifestObj = [ordered]@{
    schema = "oz.manifest.v1"
    purpose = "Stage 6 surrogate encoder-capability control; not a historical Stage 5 baseline."
    created_utc = (Get-Date).ToUniversalTime().ToString("o")
    generator = [ordered]@{
        executable = "ozviewer_make_jpeg.exe"
        sha256 = (Get-FileSha256 $JpegGen)
        source_file = "tests/make_jpeg.cpp"
    }
    corpus = $CorpusManifestItems
}

$ManifestPath = Join-Path $DirCorpus "oz.manifest.v1.json"
$ManifestJson = ($ManifestObj | ConvertTo-Json -Depth 10)
[System.IO.File]::WriteAllText($ManifestPath, $ManifestJson, [System.Text.Encoding]::UTF8)

# Seal sibling SHA-256
$ManifestHash = Get-FileSha256 $ManifestPath
$SealPath = Join-Path $DirCorpus "oz.manifest.v1.json.sha256"
[System.IO.File]::WriteAllText($SealPath, "$ManifestHash *oz.manifest.v1.json`r`n", [System.Text.Encoding]::UTF8)
Write-Host "  Corpus generated and sealed: $ManifestHash"

# 4. Generate plans
Write-Host "`n[2/5] Creating benchmark plans for candidate and control..."
$CandidateViewerNorm = $CandidateViewer.Replace("\", "/")
$ControlViewerNorm   = $ControlViewer.Replace("\", "/")

function Create-PlanObj {
    param(
        [string]$PlanName,
        [string]$SubjectName,
        [string]$ExePath,
        [array]$CorpusItems
    )

    $planImages = @()
    foreach ($item in $CorpusItems) {
        $absImgPath = (Join-Path $DirCorpus $item.filename).Replace("\", "/")
        $planImages += [ordered]@{
            path = $absImgPath
            alias = $item.alias
            format = "jpg"
            width = $item.width
            height = $item.height
            size_bytes = $item.size_bytes
            provenance = "Deterministic synthetic TurboJPEG gradient ($($item.width)x$($item.height), q90, 420, fastdct)"
        }
    }

    $obj = [ordered]@{
        schema = "oz.plan.v1"
        name = $PlanName
        note = "Stage 6 surrogate encoder-capability control; not a historical Stage 5 baseline."
        subject = [ordered]@{
            name = $SubjectName
            executable = $ExePath
            version_hint = "Release-x64"
            enabled = $true
            skip_reason = ""
        }
        collector = "markers"
        adapter = "oz_markers"
        endpoint = "FirstPresent"
        cache = "warm"
        timeout_ms = 60000
        repetitions = 20
        warmups = 1
        args = @("--benchmark", "{{image_path}}")
        images = $planImages
    }
    return $obj
}

$PlanCandObj = Create-PlanObj -PlanName "stage6-surrogate-candidate-ttfi" -SubjectName "stage6-encoder-on" -ExePath $CandidateViewerNorm -CorpusItems $CorpusManifestItems
$PlanCtrlObj = Create-PlanObj -PlanName "stage6-surrogate-control-ttfi"   -SubjectName "stage6-surrogate-encoder-off" -ExePath $ControlViewerNorm   -CorpusItems $CorpusManifestItems

$PlanCandPath = Join-Path $DirPlans "plan-candidate.json"
$PlanCtrlPath = Join-Path $DirPlans "plan-control.json"

[System.IO.File]::WriteAllText($PlanCandPath, ($PlanCandObj | ConvertTo-Json -Depth 10), [System.Text.Encoding]::UTF8)
[System.IO.File]::WriteAllText($PlanCtrlPath, ($PlanCtrlObj | ConvertTo-Json -Depth 10), [System.Text.Encoding]::UTF8)

# Validate plans with runner
Write-Host "  Validating plans with neutral runner..."
& $Runner validate $PlanCandPath
if ($LASTEXITCODE -ne 0) { throw "Plan validation failed for candidate" }
& $Runner validate $PlanCtrlPath
if ($LASTEXITCODE -ne 0) { throw "Plan validation failed for control" }

# 5. Capture Conditions and Build Provenance
Write-Host "`n[3/5] Recording conditions and build provenance..."
$CandidateHash = Get-FileSha256 $CandidateViewer
$ControlHash   = Get-FileSha256 $ControlViewer

$ConditionsObj = [ordered]@{
    purpose = "Stage 6 surrogate encoder-capability control; not a historical Stage 5 baseline."
    start_utc = (Get-Date).ToUniversalTime().ToString("o")
    host = $env:COMPUTERNAME
    user = $env:USERNAME
    os = (Get-CimInstance Win32_OperatingSystem).Caption + " " + (Get-CimInstance Win32_OperatingSystem).Version
    processor = (Get-CimInstance Win32_Processor).Name
    total_physical_memory_gb = [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB, 2)
    power_line_status = "AC/Static Session Required"
    isolate_conditions = "No concurrent builds, debuggers, downloads, or external browsers active"
    candidate_binary = [ordered]@{
        path = $CandidateViewer
        size_bytes = (Get-Item -LiteralPath $CandidateViewer).Length
        sha256 = $CandidateHash
        macro_OZVIEWER_HAVE_WEBP_ENCODER = 1
    }
    control_binary = [ordered]@{
        path = $ControlViewer
        size_bytes = (Get-Item -LiteralPath $ControlViewer).Length
        sha256 = $ControlHash
        macro_OZVIEWER_HAVE_WEBP_ENCODER = 0
    }
    runner_binary = [ordered]@{
        path = $Runner
        sha256 = (Get-FileSha256 $Runner)
    }
}
$ConditionsPath = Join-Path $EvidenceDir "conditions.json"
[System.IO.File]::WriteAllText($ConditionsPath, ($ConditionsObj | ConvertTo-Json -Depth 10), [System.Text.Encoding]::UTF8)

# Copy map files to build provenance
$CandidateMapSrc = Join-Path $CandidateDir "src\viewer\build\candidate-ozviewer.map"
$ControlMapSrc   = Join-Path $ControlDir   "src\viewer\build\control-ozviewer.map"
if (Test-Path -LiteralPath $CandidateMapSrc) {
    Copy-Item -LiteralPath $CandidateMapSrc -Destination (Join-Path $DirBuildProv "candidate\candidate-ozviewer.map") -Force
}
if (Test-Path -LiteralPath $ControlMapSrc) {
    Copy-Item -LiteralPath $ControlMapSrc -Destination (Join-Path $DirBuildProv "control\control-ozviewer.map") -Force
}

# 6. Execute benchmark protocol
Write-Host "`n[4/5] Executing benchmark protocol (1 warmup + 20 repetitions per cell)..."
Write-Host "Temporal execution order:"
Write-Host "  Cell 1: 2MP Candidate -> Cell 2: 2MP Control"
Write-Host "  Cell 3: 12MP Control  -> Cell 4: 12MP Candidate"
Write-Host "  Cell 5: 24MP Candidate -> Cell 6: 24MP Control"

$ExecutionSchedule = @(
    @{ Scenario = "jpeg-2mp";  Subject = "candidate"; Plan = $PlanCandPath; OutDir = $DirRawCand },
    @{ Scenario = "jpeg-2mp";  Subject = "control";   Plan = $PlanCtrlPath; OutDir = $DirRawCtrl },
    @{ Scenario = "jpeg-12mp"; Subject = "control";   Plan = $PlanCtrlPath; OutDir = $DirRawCtrl },
    @{ Scenario = "jpeg-12mp"; Subject = "candidate"; Plan = $PlanCandPath; OutDir = $DirRawCand },
    @{ Scenario = "jpeg-24mp"; Subject = "candidate"; Plan = $PlanCandPath; OutDir = $DirRawCand },
    @{ Scenario = "jpeg-24mp"; Subject = "control";   Plan = $PlanCtrlPath; OutDir = $DirRawCtrl }
)

$ExecutionLog = @()
$step = 1
foreach ($item in $ExecutionSchedule) {
    $outFile = Join-Path $item.OutDir "$($item.Scenario).jsonl"
    Write-Host "`n  Step $step/6: Running $($item.Subject) on $($item.Scenario)..."
    Write-Host "    Plan: $($item.Plan)"
    Write-Host "    Output: $outFile"
    
    $startCellUtc = (Get-Date).ToUniversalTime().ToString("o")
    & $Runner run $item.Plan --scenario $item.Scenario --warmups 1 --repetitions 20 --out $outFile
    $rc = $LASTEXITCODE
    $endCellUtc = (Get-Date).ToUniversalTime().ToString("o")

    if ($rc -ne 0) {
        throw "Runner failed with exit code $rc on $($item.Subject) - $($item.Scenario)"
    }

    $ExecutionLog += [ordered]@{
        step = $step
        scenario = $item.Scenario
        subject = $item.Subject
        raw_output = $outFile
        exit_code = $rc
        start_utc = $startCellUtc
        end_utc = $endCellUtc
    }
    $step++
}

# 7. Summarize outputs with runner
Write-Host "`n[5/5] Generating runner summaries and analyzing metrics..."
foreach ($scen in @("jpeg-2mp", "jpeg-12mp", "jpeg-24mp")) {
    $rawCand = Join-Path $DirRawCand "$scen.jsonl"
    $rawCtrl = Join-Path $DirRawCtrl "$scen.jsonl"
    
    $sumCandJson = Join-Path $DirSummCand "$scen-summary.json"
    $sumCandCsv  = Join-Path $DirSummCand "$scen-summary.csv"
    $sumCtrlJson = Join-Path $DirSummCtrl "$scen-summary.json"
    $sumCtrlCsv  = Join-Path $DirSummCtrl "$scen-summary.csv"

    & $Runner summarize $rawCand --format json --out $sumCandJson
    & $Runner summarize $rawCand --format csv  --out $sumCandCsv
    & $Runner summarize $rawCtrl --format json --out $sumCtrlJson
    & $Runner summarize $rawCtrl --format csv  --out $sumCtrlCsv
}

# Helper: Parse raw JSONL FirstPresent relative_ns
function Parse-RawCellMetrics {
    param([string]$JsonlPath)
    
    $lines = Get-Content -LiteralPath $JsonlPath
    $warmups = @()
    $measured = @()

    foreach ($line in $lines) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        $obj = $line | ConvertFrom-Json
        
        # Check run_end records for success/issues/timeouts
        if ($obj.kind -eq "run_end") {
            if (-not $obj.launched) {
                throw ("Process failed to launch in {0}: {1}" -f $JsonlPath, $obj.error_message)
            }
            if ($obj.timed_out) {
                throw ("Process timed out in {0}" -f $JsonlPath)
            }
            if ($obj.exit_code -ne 0) {
                throw ("Process exited non-zero ({0}) in {1}" -f $obj.exit_code, $JsonlPath)
            }
        }
        
        # Collect FirstPresent relative_ns marker
        if ($obj.kind -eq "marker") {
            if ($obj.marker -eq "FirstPresent") {
                $relNs = [int64]$obj.relative_ns
                if ($obj.warmup) {
                    $warmups += $relNs
                } else {
                    $measured += $relNs
                }
            }
            if ($obj.marker -match "FolderEnumeration") {
                throw ("Folder enumeration marker detected in benchmark run! {0}" -f $JsonlPath)
            }
        }
    }

    if ($measured.Count -ne 20) {
        throw "Expected exactly 20 measured samples in $JsonlPath, found $($measured.Count)"
    }
    if ($warmups.Count -ne 1) {
        throw "Expected exactly 1 warmup sample in $JsonlPath, found $($warmups.Count)"
    }

    # Nearest rank calculation for N=20:
    # Sorted 0-indexed:
    # Median (50%): ceil(20*0.50) = 10 -> index 9 (10th value)
    # P95 (95%):    ceil(20*0.95) = 19 -> index 18 (19th value)
    # P99 (99%):    ceil(20*0.99) = 20 -> index 19 (20th value)
    $sorted = $measured | Sort-Object
    $medianNs = $sorted[9]
    $p95Ns    = $sorted[18]
    $p99Ns    = $sorted[19]
    $minNs    = $sorted[0]
    $maxNs    = $sorted[19]

    return [ordered]@{
        measured_count = $measured.Count
        warmup_count = $warmups.Count
        raw_samples_ns = $measured
        median_ns = $medianNs
        median_ms = [math]::Round($medianNs / 1e6, 3)
        p95_ns = $p95Ns
        p95_ms = [math]::Round($p95Ns / 1e6, 3)
        p99_ns = $p99Ns
        p99_ms = [math]::Round($p99Ns / 1e6, 3)
        min_ms = [math]::Round($minNs / 1e6, 3)
        max_ms = [math]::Round($maxNs / 1e6, 3)
    }
}

# Perform regression analysis and gate verification
Write-Host "`n=== CELL ANALYSIS & REGRESSION GATE CHECK ==="
$Scenarios = @("jpeg-2mp", "jpeg-12mp", "jpeg-24mp")
$AnalysisCells = @()
$GatePassed = $true

foreach ($scen in $Scenarios) {
    $rawCand = Join-Path $DirRawCand "$scen.jsonl"
    $rawCtrl = Join-Path $DirRawCtrl "$scen.jsonl"

    $candMetrics = Parse-RawCellMetrics -JsonlPath $rawCand
    $ctrlMetrics = Parse-RawCellMetrics -JsonlPath $rawCtrl

    # Delta % = 100 * (cand - ctrl) / ctrl
    $medianDeltaPct = [math]::Round(100.0 * ($candMetrics.median_ns - $ctrlMetrics.median_ns) / $ctrlMetrics.median_ns, 2)
    $p95DeltaPct    = [math]::Round(100.0 * ($candMetrics.p95_ns - $ctrlMetrics.p95_ns) / $ctrlMetrics.p95_ns, 2)
    $medianDeltaMs  = [math]::Round($candMetrics.median_ms - $ctrlMetrics.median_ms, 3)

    # Gate: candidate median must be <= +5.00% slower than control
    $cellPass = ($medianDeltaPct -le 5.00)
    if (-not $cellPass) {
        $GatePassed = $false
    }

    $AnalysisCells += [ordered]@{
        scenario = $scen
        control_median_ms = $ctrlMetrics.median_ms
        candidate_median_ms = $candMetrics.median_ms
        delta_median_ms = $medianDeltaMs
        delta_median_pct = $medianDeltaPct
        control_p95_ms = $ctrlMetrics.p95_ms
        candidate_p95_ms = $candMetrics.p95_ms
        delta_p95_pct = $p95DeltaPct
        gate_threshold_pct = 5.00
        pass_gate = $cellPass
        control_details = $ctrlMetrics
        candidate_details = $candMetrics
    }

    $passStr = if ($cellPass) { "PASS" } else { "FAIL (REGRESSION > 5%)" }
    Write-Host ("  Scenario {0,-10}: Control = {1,6} ms | Candidate = {2,6} ms | Delta = {3,6}% ({4,6} ms) -> {5}" -f `
        $scen, $ctrlMetrics.median_ms, $candMetrics.median_ms, $medianDeltaPct, $medianDeltaMs, $passStr)
}

$AnalysisObj = [ordered]@{
    purpose = "Stage 6 surrogate encoder-capability control; not a historical Stage 5 baseline."
    analysis_utc = (Get-Date).ToUniversalTime().ToString("o")
    gate_rule = "All 3 candidate medians must be <= +5.00% relative to control (FirstPresent relative_ns)"
    overall_gate_passed = $GatePassed
    cells = $AnalysisCells
    execution_log = $ExecutionLog
}

$AnalysisPath = Join-Path $EvidenceDir "analysis.json"
[System.IO.File]::WriteAllText($AnalysisPath, ($AnalysisObj | ConvertTo-Json -Depth 10), [System.Text.Encoding]::UTF8)

# Write README.md in evidence dir
$ReadmeContent = @"
# Stage 6 Surrogate WebP-Encoder TTFI Evidence

> **NOTICE**: This evidence is a **Stage 6 surrogate encoder-capability control**; it is **NOT** a historical Stage 5 baseline and must never be labeled or claimed as one.

## Conditions and Methodology
- **Candidate**: Release x64 `ozviewer.exe` built with `OZVIEWER_ENABLE_WEBP=ON`, `OZVIEWER_ENABLE_EDITING=ON`, `OZVIEWER_ENABLE_WEBP_ENCODER=ON` (SHA256: $CandidateHash)
- **Control**: Release x64 `ozviewer.exe` built with `OZVIEWER_ENABLE_WEBP=ON`, `OZVIEWER_ENABLE_EDITING=ON`, `OZVIEWER_ENABLE_WEBP_ENCODER=OFF` (SHA256: $ControlHash)
- **Neutral Runner**: Release x64 `ozbench.exe` (SHA256: $(Get-FileSha256 $Runner))
- **Corpus**: Deterministic synthetic TurboJPEG gradient fixtures (`jpeg-2mp.jpg`, `jpeg-12mp.jpg`, `jpeg-24mp.jpg`) generated by candidate Release `ozviewer_make_jpeg.exe` (sealed in `corpus/oz.manifest.v1.json`).
- **Endpoint**: `FirstPresent` relative nanoseconds (ProcessStart-to-FirstPresent) via internal markers.
- **Samples**: 1 discarded warmup + 20 measured fresh-process runs per cell (6 cells total = 120 measured launches).
- **Execution Order**:
  1. 2MP Candidate
  2. 2MP Control
  3. 12MP Control
  4. 12MP Candidate
  5. 24MP Candidate
  6. 24MP Control

## Summary Gate Table

| Scenario | Control Median (ms) | Candidate Median (ms) | Delta Median (%) | Control P95 (ms) | Candidate P95 (ms) | Delta P95 (%) | Gate Status (<= 5.00%) |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
$(
foreach ($c in $AnalysisCells) {
    "| $($c.scenario) | $($c.control_median_ms) | $($c.candidate_median_ms) | $($c.delta_median_pct)% | $($c.control_p95_ms) | $($c.candidate_p95_ms) | $($c.delta_p95_pct)% | $(if ($c.pass_gate) { 'PASS' } else { 'FAIL' }) |"
}
)

**Overall Gate Status**: $(if ($GatePassed) { 'PASS' } else { 'FAIL' })
"@

$ReadmePath = Join-Path $EvidenceDir "README.md"
[System.IO.File]::WriteAllText($ReadmePath, $ReadmeContent, [System.Text.Encoding]::UTF8)

Write-Host "`nEvidence generation complete."
Write-Host "Evidence directory: $EvidenceDir"
Write-Host "Overall Gate Result: $(if ($GatePassed) { 'PASS' } else { 'BLOCKED/FAIL' })"

if (-not $GatePassed) {
    exit 1
}
exit 0

<#
.SYNOPSIS
  Reliable process execution helper with async stream reading and bounded timeouts.

.DESCRIPTION
  Executes an external process safely without deadlock risks from standard streams:
  uses ProcessStartInfo with redirected stdout/stderr and asynchronous ReadToEndAsync()
  tasks, enforces a bounded timeout with Process.Kill() fallback, and disposes process
  resources in a finally block.
#>

function Invoke-ProcessSafe {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$ArgumentList = @(),
        [int]$TimeoutSeconds = 60,
        [string]$WorkingDirectory = ""
    )

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $FilePath
    if ($ArgumentList.Count -gt 0) {
        $psi.Arguments = ($ArgumentList -join " ")
    }
    if (-not [string]::IsNullOrWhiteSpace($WorkingDirectory)) {
        $psi.WorkingDirectory = $WorkingDirectory
    }
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true

    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $proc = [System.Diagnostics.Process]::Start($psi)
    if ($null -eq $proc) {
        throw "Failed to start process: '$FilePath'"
    }

    try {
        $outTask = $proc.StandardOutput.ReadToEndAsync()
        $errTask = $proc.StandardError.ReadToEndAsync()

        $exited = $proc.WaitForExit($TimeoutSeconds * 1000)
        $sw.Stop()

        if (-not $exited) {
            try {
                $proc.Kill()
            } catch { }
            throw "Process '$FilePath' timed out after $TimeoutSeconds seconds."
        }

        # Wait on stream read tasks to ensure all output is flushed
        $null = [System.Threading.Tasks.Task]::WaitAll(@($outTask, $errTask), 5000)

        $stdout = if ($outTask.IsCompleted) { $outTask.Result } else { "" }
        $stderr = if ($errTask.IsCompleted) { $errTask.Result } else { "" }

        return [pscustomobject]@{
            ExitCode = $proc.ExitCode
            StdOut = $stdout
            StdErr = $stderr
            DurationSeconds = $sw.Elapsed.TotalSeconds
            TimedOut = $false
        }
    } finally {
        $proc.Dispose()
    }
}

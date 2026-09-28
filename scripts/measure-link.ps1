#requires -version 7.0
<#
  Linker launcher used inside link-semaphore.ps1. OLO_LINK_METRICS_DIR selects
  the output directory; each invocation gets its own JSON file. The permit is
  held during measurement. No compiler flags or cache keys change.

  Peak working set is the OS high-water mark observed while the process lives.
  Thread counts are sampled, so their maximum is a lower bound. This measures
  the direct linker, not its launcher, descendants, or total host memory.
#>
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($args.Count -eq 0) { throw 'No linker command supplied' }
if (-not $env:OLO_LINK_METRICS_DIR) { throw 'OLO_LINK_METRICS_DIR is required' }
$start = [Diagnostics.ProcessStartInfo]::new()
$start.FileName = $args[0]
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
foreach ($argument in $args | Select-Object -Skip 1) { $start.ArgumentList.Add($argument) }
$responses = @(
    foreach ($argument in $args) {
        if ($argument.StartsWith('@')) {
            $path = $argument.Substring(1)
            try {
                $path = [IO.Path]::GetFullPath($path)
                [ordered]@{ path = $path; sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
                            content = [IO.File]::ReadAllText($path) }
            } catch {
                Write-Warning "[measure-link] failed to capture response file: $($_.Exception.Message)"
                [ordered]@{ path = $path; error = $_.Exception.Message }
            }
        }
    }
)
$process = [Diagnostics.Process]::new()
$process.StartInfo = $start
$startedUtc = [DateTime]::UtcNow.ToString('o')
$watch = [Diagnostics.Stopwatch]::StartNew()
if (-not $process.Start()) { throw 'Linker failed to start' }
$stdoutCopy = $process.StandardOutput.BaseStream.CopyToAsync([Console]::OpenStandardOutput())
$stderrCopy = $process.StandardError.BaseStream.CopyToAsync([Console]::OpenStandardError())
$processId = $process.Id
$peakBytes = 0L
$peakThreads = 0
$samples = 0
$errors = [Collections.Generic.List[string]]::new()
try {
    do {
        try {
            $process.Refresh()
            $peakBytes = [Math]::Max($peakBytes, $process.PeakWorkingSet64)
            $peakThreads = [Math]::Max($peakThreads, $process.Threads.Count)
            $samples++
        } catch {
            $errors.Add($_.Exception.Message)
        }
    } while (-not $process.WaitForExit(20))
    $watch.Stop()
    $code = $process.ExitCode
    $stdoutCopy.GetAwaiter().GetResult()
    $stderrCopy.GetAwaiter().GetResult()
    $record = [ordered]@{
        schemaVersion = 1
        command = @($args)
        responseFiles = $responses
        workingDirectory = (Get-Location).Path
        startedUtc = $startedUtc
        processId = $processId
        exitCode = $code
        wallMs = $watch.Elapsed.TotalMilliseconds
        observedPeakWorkingSetBytes = $peakBytes
        sampledPeakThreads = $peakThreads
        sampleIntervalMs = 20
        samples = $samples
        limitations = 'Observed OS working-set high-water mark; a final unsampled peak can be missed. Thread maximum is sampled. Direct process only.'
        errors = @($errors.ToArray())
    }
    try {
        $directory = [IO.Path]::GetFullPath($env:OLO_LINK_METRICS_DIR)
        [IO.Directory]::CreateDirectory($directory) | Out-Null
        $record | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $directory "link-$processId-$([Guid]::NewGuid().ToString('N')).json")
    } catch {
        Write-Warning "[measure-link] failed to write metrics: $($_.Exception.Message)"
    }
} finally {
    $process.Dispose()
}
exit $code

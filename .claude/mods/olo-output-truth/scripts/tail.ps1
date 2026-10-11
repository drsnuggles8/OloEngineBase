<#
.SYNOPSIS
  Prints the last -Bytes bytes of a file as UTF-8, for mods reading a log too big for $.fs.read.

.DESCRIPTION
  `$.process.run` runs an argument vector with no shell, so `tail` is only there when some
  install put a tail.exe on PATH. pwsh is always present on this box (the build lock needs it).
  Opens with FileShare.ReadWrite so a log still being written is readable. A cut inside a
  multi-byte character at the start is dropped by the decoder, which is harmless for a log.
#>
param(
    [Parameter(Mandatory = $true)][string] $Path,
    [Parameter(Mandatory = $true)][long] $Bytes
)

$ErrorActionPreference = 'Stop'
$fs = [System.IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
try {
    $take = [Math]::Min($fs.Length, $Bytes)
    [void] $fs.Seek(-$take, 'End')
    $buffer = New-Object byte[] $take
    $read = 0
    while ($read -lt $take) {
        $n = $fs.Read($buffer, $read, $take - $read)
        if ($n -le 0) { break }
        $read += $n
    }
    $stdout = [Console]::OpenStandardOutput()
    $stdout.Write($buffer, 0, $read)
    $stdout.Flush()
} finally {
    $fs.Dispose()
}

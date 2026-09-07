<#
.SYNOPSIS
  Find (and optionally kill) the processes preventing a directory from being deleted, then remove it.

.DESCRIPTION
  Used by /cleanup-worktree §3b. `git worktree remove` deletes a worktree's CONTENTS but
  leaves the directory itself when some process holds it as its CURRENT WORKING DIRECTORY —
  the symptom is "Permission denied" from git and "Device or resource busy" from rmdir on a
  directory that is already empty.

  The usual culprit is not the git fsmonitor daemon (killing those does NOT help) but leftover
  `bash.exe` / `sleep.exe` / `pwsh.exe` processes from the worktree's own agent session — a
  background monitor loop that outlived it. This script reads each process's real working
  directory out of its PEB (there is no Win32 API for another process's cwd) and reports or
  kills only the processes actually sitting in the target paths.

.PARAMETER Path
  One or more directories to free. Matching is by path prefix, so a parent frees its children.

.PARAMETER Kill
  Kill the holding processes and then remove the directories. Without it the script only
  reports what holds them and changes nothing.

.EXAMPLE
  pwsh -NoProfile -File .claude/scripts/free-locked-dir.ps1 -Path C:\repos\OloEngine-foo
  pwsh -NoProfile -File .claude/scripts/free-locked-dir.ps1 -Path C:\repos\OloEngine-foo -Kill
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string[]] $Path,
    [switch] $Kill
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -Language CSharp -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class ProcCwd
{
    [DllImport("ntdll.dll")]
    static extern int NtQueryInformationProcess(IntPtr h, int cls, ref PBI pbi, int len, out int ret);
    [DllImport("kernel32.dll")]
    static extern IntPtr OpenProcess(int access, bool inherit, int pid);
    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll")]
    static extern bool ReadProcessMemory(IntPtr h, IntPtr addr, byte[] buf, int size, out IntPtr read);

    [StructLayout(LayoutKind.Sequential)]
    struct PBI
    {
        public IntPtr Reserved1;
        public IntPtr PebBaseAddress;
        public IntPtr Reserved2_0;
        public IntPtr Reserved2_1;
        public IntPtr UniqueProcessId;
        public IntPtr Reserved3;
    }

    static IntPtr ReadPtr(IntPtr h, IntPtr addr)
    {
        byte[] b = new byte[8];
        IntPtr read;
        if (!ReadProcessMemory(h, addr, b, 8, out read)) return IntPtr.Zero;
        return (IntPtr)BitConverter.ToInt64(b, 0);
    }

    // PEB+0x20 -> RTL_USER_PROCESS_PARAMETERS; +0x38 -> CurrentDirectory UNICODE_STRING (64-bit).
    public static string Get(int pid)
    {
        IntPtr h = OpenProcess(0x0410 /* QUERY_INFORMATION | VM_READ */, false, pid);
        if (h == IntPtr.Zero) return null;
        try
        {
            PBI pbi = new PBI();
            int ret;
            if (NtQueryInformationProcess(h, 0, ref pbi, Marshal.SizeOf(pbi), out ret) != 0) return null;

            IntPtr parms = ReadPtr(h, (IntPtr)(pbi.PebBaseAddress.ToInt64() + 0x20));
            if (parms == IntPtr.Zero) return null;

            byte[] us = new byte[16];
            IntPtr read;
            if (!ReadProcessMemory(h, (IntPtr)(parms.ToInt64() + 0x38), us, 16, out read)) return null;

            int len = BitConverter.ToUInt16(us, 0);
            IntPtr buf = (IntPtr)BitConverter.ToInt64(us, 8);
            if (len <= 0 || buf == IntPtr.Zero) return null;

            byte[] s = new byte[len];
            if (!ReadProcessMemory(h, buf, s, len, out read)) return null;
            return System.Text.Encoding.Unicode.GetString(s);
        }
        catch { return null; }
        finally { CloseHandle(h); }
    }
}
'@

$targets = $Path | ForEach-Object { $_.TrimEnd('\', '/') }

# Never kill this session: collect our own process and every ancestor, so a target path that
# happens to contain the running agent's own shell is reported but never terminated.
$protected = [System.Collections.Generic.HashSet[int]]::new()
$walk = $PID
for ($i = 0; $i -lt 32 -and $walk -gt 0; $i++) {
    [void]$protected.Add($walk)
    $procInfo = Get-CimInstance Win32_Process -Filter "ProcessId=$walk" -ErrorAction SilentlyContinue
    if (-not $procInfo) { break }
    $parent = $procInfo.ParentProcessId
    if (-not $parent -or $parent -eq $walk) { break }
    $walk = $parent
}

$holders = @()
foreach ($proc in Get-Process) {
    if ($protected.Contains($proc.Id)) { continue }
    $cwd = $null
    try { $cwd = [ProcCwd]::Get($proc.Id) } catch { }
    if (-not $cwd) { continue }
    $norm = $cwd.TrimEnd('\', '/')
    foreach ($t in $targets) {
        if ($norm -eq $t -or $norm.StartsWith("$t\", [StringComparison]::OrdinalIgnoreCase)) {
            $holders += [pscustomobject]@{ Id = $proc.Id; Name = $proc.Name; Cwd = $cwd; Target = $t }
            break
        }
    }
}

if ($holders.Count -eq 0) {
    Write-Output "No process has any target path as its working directory."
} else {
    Write-Output "Processes holding a target path as their working directory:"
    $holders | Sort-Object Target, Name, Id | Format-Table -AutoSize -Wrap | Out-String -Width 220 | Write-Output
}

if (-not $Kill) {
    Write-Output "(report only; re-run with -Kill to terminate these and remove the directories)"
    return
}

foreach ($h in $holders) {
    try { Stop-Process -Id $h.Id -Force -ErrorAction Stop; Write-Output "killed $($h.Id) $($h.Name)" }
    catch { Write-Output "already gone: $($h.Id) $($h.Name)" }
}

if ($holders.Count -gt 0) { Start-Sleep -Milliseconds 1500 }

$failed = 0
foreach ($t in $targets) {
    if (-not (Test-Path $t)) { Write-Output "already absent: $t"; continue }
    try { Remove-Item -Recurse -Force $t -ErrorAction Stop; Write-Output "REMOVED $t" }
    catch { $failed++; Write-Output "STILL HELD: $t -- $($_.Exception.Message)" }
}

if ($failed -gt 0) { exit 1 }

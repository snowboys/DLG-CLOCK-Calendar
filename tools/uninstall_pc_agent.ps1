# Disable / remove the PC-side resident BLE pusher.
# The device is now a standalone calendar: no PC agent is needed at boot.
#
# Usage (no admin required):
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\uninstall_pc_agent.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\uninstall_pc_agent.ps1 -Keep
#
# -Keep only disables the scheduled task instead of unregistering it.
# ASCII-only on purpose: Windows PowerShell 5.1 reads .ps1 as ANSI otherwise.
param(
    [switch]$Keep
)

$ErrorActionPreference = "Continue"
$task = "PcMonitorAgent"

$t = Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue
if (-not $t) {
    Write-Output "Scheduled task $task not found, nothing to do."
} else {
    try { Stop-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue } catch {}
    if ($Keep) {
        Disable-ScheduledTask -TaskName $task | Out-Null
        Write-Output "Scheduled task $task disabled (kept, can be re-enabled)."
    } else {
        Unregister-ScheduledTask -TaskName $task -Confirm:$false | Out-Null
        Write-Output "Scheduled task $task unregistered."
    }
}

# Kill any still-running pc_agent.py process (only matching command lines).
$procs = Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -like '*pc_agent.py*' -and $_.Name -match '^python' }
foreach ($p in $procs) {
    try {
        Stop-Process -Id $p.ProcessId -Force -ErrorAction Stop
        Write-Output "Stopped background process PID $($p.ProcessId)"
    } catch {}
}

Write-Output ""
Write-Output "Done. The tag now runs fully offline."
Write-Output "To calibrate time manually (one-shot, not resident):"
Write-Output "  C:\Python314\python.exe tools\sync_time.py"
Write-Output "Or write the 0x91 packet with a phone via nRF Connect:"
Write-Output "  C:\Python314\python.exe tools\sync_time.py --nrf-help"

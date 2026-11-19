# 安装"PC 端 BLE 推送"登录自启任务 (不需要管理员权限)
#
# 作用: 开机登录后自动在后台运行 tools\pc_agent.py, 断线会自动重连。
#
# 用法:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\install_pc_agent.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\install_pc_agent.ps1 -Addr 18:BC:5A:5A:87:81
#
# 卸载:
#   Unregister-ScheduledTask -TaskName PcMonitorAgent -Confirm:$false
#
# 日志: %LOCALAPPDATA%\pcmonitor\pc_agent.log
param(
    [string]$Addr = ""          # 设备 MAC, 填了就不用每次扫描 (更快)
)

$ErrorActionPreference = "Stop"

$agent = Join-Path $PSScriptRoot "pc_agent.py"
if (-not (Test-Path $agent)) {
    Write-Error "pc_agent.py not found: $agent"
    exit 1
}

# 解析真实解释器路径 (跳过 WindowsApps 的 python.exe 别名)
$py = (Get-Command python -ErrorAction SilentlyContinue).Source
if (-not $py) {
    Write-Error "python not found in PATH"
    exit 1
}
$real = & $py -c "import sys; print(sys.executable)" 2>$null
$pyw = Join-Path (Split-Path $real) "pythonw.exe"
if (-not (Test-Path $pyw)) { $pyw = $real }     # 没有 pythonw 就用 python

# 日志目录 (pc_agent.py 在 pythonw 下会自己写到这个文件)
$dir = Join-Path $env:LOCALAPPDATA "pcmonitor"
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$log = Join-Path $dir "pc_agent.log"

# 直接用 pythonw 作为任务动作: GUI 子系统, 不会弹任何控制台窗口
$argLine = '-u "' + $agent + '"'
if ($Addr) { $argLine += " " + $Addr }

$action = New-ScheduledTaskAction -Execute $pyw -Argument $argLine -WorkingDirectory (Split-Path $PSScriptRoot -Parent)
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
$settings = New-ScheduledTaskSettingsSet -StartWhenAvailable `
    -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1) `
    -ExecutionTimeLimit ([TimeSpan]::Zero)
Register-ScheduledTask -TaskName "PcMonitorAgent" -Action $action -Trigger $trigger `
    -Settings $settings -Description "DLG-CLOCK BLE PC monitor agent" -Force | Out-Null

Start-ScheduledTask -TaskName "PcMonitorAgent"
Start-Sleep -Seconds 10

Write-Output "Task registered: PcMonitorAgent"
Write-Output "  python : $pyw"
Write-Output "  script : $agent $Addr"
Write-Output "  log    : $log"
Write-Output ""
if (Test-Path $log) {
    Write-Output "--- tail ---"
    Get-Content $log -Tail 6
}
Write-Output ""
Write-Output "Uninstall: Unregister-ScheduledTask -TaskName PcMonitorAgent -Confirm:`$false"

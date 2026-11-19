# 安装"无界面 CPU 温度守护"(需以管理员身份运行本脚本)
#
# 用法:
#   右键"以管理员身份运行 PowerShell", 然后:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\install_temp_daemon.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\install_temp_daemon.ps1 -LibDir "D:\Tools\LibreHardwareMonitor"
#
# 卸载:
#   schtasks /Delete /TN PcMonitorTemp /F
#
# 说明: 任务动作用 wscript.exe + VBS 启动器 (GUI 子系统),
#       彻底避免 powershell 控制台窗口弹出。
param(
    [string]$LibDir = "F:\AI_PROJECTS\DA14585\LibreHardwareMonitor"
)

$isAdmin = ([Security.Principal.WindowsPrincipal] `
    [Security.Principal.WindowsIdentity]::GetCurrent()
    ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Error "请以管理员身份运行 (读取传感器需要驱动权限)"
    exit 1
}
if (-not (Test-Path (Join-Path $LibDir "LibreHardwareMonitorLib.dll"))) {
    Write-Error "找不到 LibreHardwareMonitorLib.dll, 请先下载解压 LibreHardwareMonitor 到 $LibDir"
    exit 1
}

# 守护脚本复制到 LibDir, 避免依赖仓库路径
$daemon = Join-Path $LibDir "temp_daemon.ps1"
Copy-Item (Join-Path $PSScriptRoot "temp_daemon.ps1") $daemon -Force

# 生成 VBS 静默启动器: wscript 无控制台, Run 第 1 个参数 0 = 隐藏窗口
$vbs = Join-Path $LibDir "run_hidden.vbs"
@"
' 静默启动 CPU 温度守护 (由计划任务 PcMonitorTemp 调用)
Set sh = CreateObject("WScript.Shell")
sh.Run "powershell.exe -NoProfile -ExecutionPolicy Bypass -File ""$daemon"" -LibDir ""$LibDir""", 0, False
"@ | Set-Content -Path $vbs -Encoding ascii

$tr = '"wscript.exe" "' + $vbs + '"'
schtasks /Create /TN "PcMonitorTemp" /TR $tr /SC ONLOGON /RL HIGHEST /F

# 清掉可能残留的旧守护进程, 避免重复
Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -like "*temp_daemon*" } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }

schtasks /Run /TN "PcMonitorTemp"
Start-Sleep -Seconds 8

$out = Join-Path $env:ProgramData "pcmonitor_cputemp.txt"
if (Test-Path $out) {
    "已安装并启动, 当前 CPU 核心温度: " + (Get-Content $out -Raw) + " C"
} else {
    "任务已注册, 但暂时读不到温度; 稍等几秒再看 $out"
}

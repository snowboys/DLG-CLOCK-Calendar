# pcmonitor 无界面 CPU 温度守护 (可选组件)
#
# 由 install_temp_daemon.ps1 注册成"登录时以最高权限运行"的计划任务,
# 全程没有窗口/托盘图标; 每 4 秒把 CPU 核心最高温度写入
#   %ProgramData%\pcmonitor_cputemp.txt
# 供 tools/pc_agent.py 读取。
param(
    [string]$LibDir = "F:\AI_PROJECTS\DA14585\LibreHardwareMonitor"
)

$dll = Join-Path $LibDir "LibreHardwareMonitorLib.dll"
if (-not (Test-Path $dll)) {
    Write-Error "找不到 $dll, 请先解压 LibreHardwareMonitor 或用 -LibDir 指定目录"
    exit 1
}

Add-Type -Path $dll

$out = Join-Path $env:ProgramData "pcmonitor_cputemp.txt"
$computer = New-Object LibreHardwareMonitor.Hardware.Computer
$computer.IsCpuEnabled = $true
$computer.Open()

try {
    while ($true) {
        $max = -1
        foreach ($hw in $computer.Hardware) {
            $hw.Update()
            foreach ($s in $hw.Sensors) {
                if ($s.SensorType -eq 'Temperature' -and $s.Value -and
                    $s.Name -match 'Core|Package') {
                    if ($s.Value -gt $max) { $max = $s.Value }
                }
            }
        }
        if ($max -gt 0 -and $max -lt 120) {
            [int][Math]::Round($max) | Out-File -FilePath $out -Encoding ascii -NoNewline
        }
        Start-Sleep -Seconds 4
    }
}
finally {
    $computer.Close()
}

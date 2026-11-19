@echo off
rem 右键 -> "以管理员身份运行" 即可把 PcMonitorTemp 换成 VBS 静默启动
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install_temp_daemon.ps1" -LibDir "F:\AI_PROJECTS\DA14585\LibreHardwareMonitor"
echo.
pause

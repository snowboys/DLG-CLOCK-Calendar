# Build the GUI into a single-file EXE (double-click to run).
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\build_gui_exe.ps1
#   ... -Python "F:\path\to\python.exe"    # specify Python
#
# Output: dist\PcMonitorGUI.exe
param(
    [string]$Python = "python"
)

$repo = Split-Path $PSScriptRoot -Parent
$entry = Join-Path $PSScriptRoot "pc_monitor_gui.py"

if (-not (Test-Path $entry)) {
    Write-Error "entry script not found: $entry"
    exit 1
}

& $Python -m PyInstaller --noconfirm --clean --onefile --windowed `
    --name PcMonitorGUI `
    --collect-submodules bleak `
    --collect-submodules winrt `
    --collect-submodules PIL `
    --distpath (Join-Path $repo "dist") `
    --workpath (Join-Path $repo "build\gui") `
    --specpath (Join-Path $repo "build") `
    $entry

if ($LASTEXITCODE -eq 0) {
    Write-Output ""
    Write-Output "OK: $repo\dist\PcMonitorGUI.exe"
    Write-Output "First launch takes 2-5 seconds (self-extract)."
}

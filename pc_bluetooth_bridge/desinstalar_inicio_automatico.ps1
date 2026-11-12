$ErrorActionPreference = "SilentlyContinue"

$TaskName = "TurtleRobotBluetooth"
& schtasks.exe /Delete /TN $TaskName /F | Out-Null
Write-Host "[OK] Tarea programada '$TaskName' eliminada."

$StartupDir = [Environment]::GetFolderPath("Startup")
$VbsPath = Join-Path $StartupDir "TurtleRobotBluetooth.vbs"
if (Test-Path $VbsPath) {
    Remove-Item $VbsPath -Force
    Write-Host "[OK] Acceso de Inicio de Windows eliminado: $VbsPath"
}

Get-CimInstance Win32_Process -Filter "Name = 'pythonw.exe'" | Where-Object {
    $_.CommandLine -like "*bt_notifier.py*"
} | ForEach-Object {
    Stop-Process -Id $_.ProcessId -Force
    Write-Host "[OK] Proceso en segundo plano detenido (PID $($_.ProcessId))."
}

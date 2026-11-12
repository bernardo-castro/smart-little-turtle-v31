$ErrorActionPreference = "SilentlyContinue"

$TaskName = "TurtleRobotBluetooth"
& schtasks.exe /Delete /TN $TaskName /F | Out-Null
Write-Host "[OK] Tarea programada '$TaskName' eliminada."

$RunKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
Remove-ItemProperty -Path $RunKey -Name "TurtleRobotBluetooth" -ErrorAction SilentlyContinue
Write-Host "[OK] Entrada de Inicio con Windows eliminada."

Get-CimInstance Win32_Process -Filter "Name = 'pythonw.exe'" | Where-Object {
    $_.CommandLine -like "*bt_notifier.py*"
} | ForEach-Object {
    Stop-Process -Id $_.ProcessId -Force
    Write-Host "[OK] Proceso en segundo plano detenido (PID $($_.ProcessId))."
}

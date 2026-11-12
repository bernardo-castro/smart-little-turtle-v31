$ErrorActionPreference = "Stop"

$BridgeDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ScriptPy = Join-Path $BridgeDir "bt_notifier.py"

$PythonW = Join-Path $env:LOCALAPPDATA "Programs\Python\Python310\pythonw.exe"
if (-not (Test-Path $PythonW)) {
    $cmd = Get-Command pythonw.exe -ErrorAction SilentlyContinue
    if ($cmd) {
        $PythonW = $cmd.Source
    } else {
        throw "No se encontró pythonw.exe. Instala Python 3.10+ primero."
    }
}

$Fso = New-Object -ComObject Scripting.FileSystemObject
$ShortPyW = $Fso.GetFile($PythonW).ShortPath
$ShortScript = $Fso.GetFile($ScriptPy).ShortPath

# 1) Registrar inicio de sesión de Windows (HKCU Run estándar, sin ventanas)
$RunKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
Set-ItemProperty -Path $RunKey -Name "TurtleRobotBluetooth" -Value "`"$PythonW`" `"$ScriptPy`" --auto"
Write-Host "[OK] Inicio con Windows configurado en HKCU Run."

# 2) Registrar tarea programada cada 10 minutos (invocando pythonw.exe directo)
$TaskName = "TurtleRobotBluetooth"
& schtasks.exe /Create /SC MINUTE /MO 10 /TN $TaskName /TR "$ShortPyW $ShortScript --auto" /F | Out-Null
Write-Host "[OK] Tarea programada '$TaskName' registrada (búsqueda cada 10 minutos)."

# 3) Iniciar ahora mismo si no está corriendo
& schtasks.exe /Run /TN $TaskName | Out-Null
Write-Host "[OK] Puente Bluetooth activo en segundo plano."

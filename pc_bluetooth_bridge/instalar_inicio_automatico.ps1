$ErrorActionPreference = "Stop"

$BridgeDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ScriptPy = Join-Path $BridgeDir "bt_notifier.py"

# Detectar pythonw.exe
$PythonW = Join-Path $env:LOCALAPPDATA "Programs\Python\Python310\pythonw.exe"
if (-not (Test-Path $PythonW)) {
    $cmd = Get-Command pythonw.exe -ErrorAction SilentlyContinue
    if ($cmd) {
        $PythonW = $cmd.Source
    } else {
        throw "No se encontró pythonw.exe. Instala Python 3.10+ primero."
    }
}

# 1) Crear lanzador silencioso (.vbs) en Inicio de Windows (Startup)
$StartupDir = [Environment]::GetFolderPath("Startup")
$VbsPath = Join-Path $StartupDir "TurtleRobotBluetooth.vbs"

$VbsContent = @"
Set WshShell = CreateObject("WScript.Shell")
WshShell.Run """$PythonW"" ""$ScriptPy"" --auto", 0, False
"@

Set-Content -Path $VbsPath -Value $VbsContent -Encoding ASCII
Write-Host "[OK] Inicio de Windows configurado en: $VbsPath"

# 2) Crear Tarea Programada cada 10 minutos (sin ventana emergente y sin requerir Admin)
$TaskName = "TurtleRobotBluetooth"
$LocalVbsPath = Join-Path $BridgeDir "lanzar_silencioso.vbs"
Set-Content -Path $LocalVbsPath -Value $VbsContent -Encoding ASCII

$Action = New-ScheduledTaskAction -Execute "wscript.exe" -Argument "`"$LocalVbsPath`""
$TriggerRepeat = New-ScheduledTaskTrigger -Once -At (Get-Date) -RepetitionInterval (New-TimeSpan -Minutes 10)
$Settings = New-ScheduledTaskSettingsSet `
    -AllowStartIfOnBatteries `
    -DontStopIfGoingOnBatteries `
    -StartWhenAvailable `
    -ExecutionTimeLimit ([TimeSpan]::Zero) `
    -MultipleInstances IgnoreNew

Register-ScheduledTask -TaskName $TaskName -Action $Action -Trigger $TriggerRepeat -Settings $Settings -Force | Out-Null
Write-Host "[OK] Tarea programada '$TaskName' registrada (búsqueda cada 10 minutos)."

# 3) Ejecutar ahora mismo a través del Programador de Tareas (proceso independiente)
Start-ScheduledTask -TaskName $TaskName
Write-Host "[OK] Puente Bluetooth iniciado en segundo plano."

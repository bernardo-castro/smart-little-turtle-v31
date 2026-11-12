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

# 1) Registrar inicio de sesión de Windows (HKCU Run estándar, sin ventanas ni archivos .vbs)
$RunKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
Set-ItemProperty -Path $RunKey -Name "TurtleRobotBluetooth" -Value "`"$PythonW`" `"$ScriptPy`" --auto"
Write-Host "[OK] Inicio con Windows configurado en HKCU Run."

# 2) Registrar tarea programada cada 10 minutos mediante XML (habilitada también en batería)
$TaskName = "TurtleRobotBluetooth"
$StartBoundary = (Get-Date).ToString("yyyy-MM-ddTHH:mm:ss")
$TempXml = Join-Path $env:TEMP "TurtleRobotBluetoothTask.xml"

$XmlContent = @"
<?xml version="1.0" encoding="UTF-16"?>
<Task version="1.2" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">
  <Triggers>
    <TimeTrigger>
      <Repetition>
        <Interval>PT10M</Interval>
        <StopAtDurationEnd>false</StopAtDurationEnd>
      </Repetition>
      <StartBoundary>$StartBoundary</StartBoundary>
      <Enabled>true</Enabled>
    </TimeTrigger>
  </Triggers>
  <Settings>
    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>
    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>
    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>
    <AllowHardTerminate>true</AllowHardTerminate>
    <StartWhenAvailable>true</StartWhenAvailable>
    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>
    <AllowStartOnDemand>true</AllowStartOnDemand>
    <Enabled>true</Enabled>
    <Hidden>false</Hidden>
    <RunOnlyIfIdle>false</RunOnlyIfIdle>
    <WakeToRun>false</WakeToRun>
    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>
    <Priority>7</Priority>
  </Settings>
  <Actions Context="Author">
    <Exec>
      <Command>$PythonW</Command>
      <Arguments>"$ScriptPy" --auto</Arguments>
      <WorkingDirectory>$BridgeDir</WorkingDirectory>
    </Exec>
  </Actions>
</Task>
"@

Set-Content -Path $TempXml -Value $XmlContent -Encoding Unicode
& schtasks.exe /Create /TN $TaskName /XML $TempXml /F | Out-Null
Remove-Item $TempXml -Force -ErrorAction SilentlyContinue
Write-Host "[OK] Tarea programada '$TaskName' registrada (cada 10 min, con o sin cargador)."

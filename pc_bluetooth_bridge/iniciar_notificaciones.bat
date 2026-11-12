@echo off
title Puente Bluetooth Turtle Robot (Gmail + Google Chat)
if exist "%LOCALAPPDATA%\Programs\Python\Python310\python.exe" (
    "%LOCALAPPDATA%\Programs\Python\Python310\python.exe" "%~dp0bt_notifier.py"
) else (
    python "%~dp0bt_notifier.py"
)
pause

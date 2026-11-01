@echo off
title Behringer U-Phoria UM2 Studio Control
cd /d "%~dp0"

if exist "%~dp0release_package\BehringerUM2Panel.exe" (
    start "" "%~dp0release_package\BehringerUM2Panel.exe"
    exit /b 0
)

if exist "%~dp0panel\BehringerUM2Panel.exe" (
    start "" "%~dp0panel\BehringerUM2Panel.exe"
    exit /b 0
)

cd /d "%~dp0panel"
start "" pythonw.exe panel_backend.py
exit /b 0

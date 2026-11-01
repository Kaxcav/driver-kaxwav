@echo off
title Behringer U-Phoria UM2 Studio Control
rem Abre o painel instalado; sem instalacao, roda o painel do repo com o Python.
if exist "%ProgramFiles%\KaxWav\Behringer UM2 ASIO\BehringerUM2Panel.exe" (
    start "" "%ProgramFiles%\KaxWav\Behringer UM2 ASIO\BehringerUM2Panel.exe"
    exit /b 0
)
cd /d "%~dp0..\panel"
start "" pythonw.exe panel_backend.py
exit /b 0

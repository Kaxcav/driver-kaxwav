@echo off
rem Volta para o backup mais recente feito pelo instalador (ou passe a pasta do backup como argumento).
setlocal
set "PS1=%~dp0Rollback_KaxWav_UM2.ps1"
if "%~1"=="" (
    powershell -NoProfile -Command "Start-Process powershell -Verb RunAs -Wait -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File','\"%PS1%\"'"
) else (
    powershell -NoProfile -Command "Start-Process powershell -Verb RunAs -Wait -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File','\"%PS1%\"','-BackupDir','\"%~1\"'"
)
if exist "%ProgramFiles%\KaxWav\Behringer UM2 ASIO\BehringerUM2Panel.exe" (
    start "" "%ProgramFiles%\KaxWav\Behringer UM2 ASIO\BehringerUM2Panel.exe" --headless
)
pause

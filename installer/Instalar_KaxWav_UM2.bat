@echo off
rem Instala o build do repo em Program Files (backup automatico; rollback: Rollback_KaxWav_UM2.bat).
rem O passo elevado roda no PowerShell; o painel volta a rodar sem elevacao no fim.
setlocal
set "PS1=%~dp0Instalar_KaxWav_UM2.ps1"
powershell -NoProfile -Command "Start-Process powershell -Verb RunAs -Wait -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File','\"%PS1%\"'"
if exist "%ProgramFiles%\KaxWav\Behringer UM2 ASIO\BehringerUM2Panel.exe" (
    start "" "%ProgramFiles%\KaxWav\Behringer UM2 ASIO\BehringerUM2Panel.exe" --headless
)
echo Log e backup em %ProgramData%\KaxWav\backups\
pause

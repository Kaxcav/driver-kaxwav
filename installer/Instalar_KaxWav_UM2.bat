@echo off
rem Instala o build do repo em Program Files (backup automatico; rollback: Rollback_KaxWav_UM2.bat).
rem O passo elevado roda no PowerShell; a verificacao do COM e o painel rodam SEM elevacao (como o FL).
setlocal
set "PS1=%~dp0Instalar_KaxWav_UM2.ps1"
set "DEST=%ProgramFiles%\KaxWav\Behringer UM2 ASIO"
set "COMCHECK=%~dp0..\build\Release\test_com_reg.exe"
powershell -NoProfile -Command "Start-Process powershell -Verb RunAs -Wait -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File','\"%PS1%\"'"
echo.
echo Verificacao do COM sem elevacao (o que o FL Studio carrega):
"%COMCHECK%" > "%TEMP%\kaxwav_com_check.txt" 2>&1
type "%TEMP%\kaxwav_com_check.txt"
findstr /C:"LOADED_PATH=%DEST%\BehringerUM2ASIODriver.dll" "%TEMP%\kaxwav_com_check.txt" >nul
if errorlevel 1 (
    echo VERIFICACAO DO COM FALHOU - o host nao carrega a DLL instalada. Rode Rollback_KaxWav_UM2.bat
) else (
    echo INSTALACAO VERIFICADA
)
if exist "%DEST%\BehringerUM2Panel.exe" (
    start "" "%DEST%\BehringerUM2Panel.exe" --headless
)
echo Log e backup em %ProgramData%\KaxWav\backups\
pause

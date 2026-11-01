@echo off
rem Registra no COM/ASIO a DLL INSTALADA em Program Files (nunca a da pasta de build).
rem Precisa de administrador: DllRegisterServer grava HKCR e HKLM\SOFTWARE\ASIO.
set "DLL_PATH=%ProgramFiles%\KaxWav\Behringer UM2 ASIO\BehringerUM2ASIODriver.dll"
if not exist "%DLL_PATH%" (
    echo [ERRO] DLL nao encontrada em: %DLL_PATH%
    echo Instale primeiro com Instalar_KaxWav_UM2.bat
    pause
    exit /b 1
)
powershell -NoProfile -Command "Start-Process regsvr32.exe -Verb RunAs -Wait -ArgumentList '/s','\"%DLL_PATH%\"'"
echo Registro concluido para: %DLL_PATH%
pause

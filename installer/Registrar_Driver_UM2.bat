@echo off
echo ========================================================
echo REGISTRANDO DRIVER ASIO - BEHRINGER U-PHORIA UM2
echo ========================================================

set DLL_PATH=%~dp0build\Release\BehringerUM2ASIODriver.dll
if not exist "%DLL_PATH%" (
    echo [ERRO] DLL nao encontrada em: %DLL_PATH%
    pause
    exit /b 1
)

echo Registrando DLL no Windows COM via regsvr32...
regsvr32.exe /s "%DLL_PATH%"

echo Registrando driver nas chaves ASIO do Windows...
reg.exe add "HKCU\SOFTWARE\ASIO\Behringer UM2 ASIO" /v "CLSID" /t REG_SZ /d "{77DBBCCD-91C6-4856-A900-FC8D007D161C}" /f >nul
reg.exe add "HKCU\SOFTWARE\ASIO\Behringer UM2 ASIO" /v "Description" /t REG_SZ /d "Behringer UM2 ASIO" /f >nul
reg.exe add "HKLM\SOFTWARE\ASIO\Behringer UM2 ASIO" /v "CLSID" /t REG_SZ /d "{77DBBCCD-91C6-4856-A900-FC8D007D161C}" /f >nul 2>&1

echo.
echo ========================================================
echo REGISTRO CONCLUIDO COM SUCESSO!
echo O driver Behringer UM2 ASIO esta disponivel no FL Studio.
echo ========================================================
pause

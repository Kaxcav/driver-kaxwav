@echo off
title Ativar Behringer UM2 no FL Studio
cd /d "%~dp0"
echo ========================================================
echo REGISTRANDO DRIVER ASIO BEHRINGER UM2 NO FL STUDIO (HKLM)
echo ========================================================
echo.
echo Executando o Editor de Registro do Windows...
regedit.exe "%~dp0Registrar_UM2_FL_Studio.reg"
echo.
echo 1. Clique em "Sim" na janela que abrir para confirmar a gravacao.
echo 2. Abra o FL Studio (F10 -> Audio -> Device: Behringer UM2 ASIO).
echo ========================================================
pause

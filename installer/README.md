# installer
Instalação e desinstalação. Regras:
- Nunca `pnputil /delete-driver oemXX.inf` por índice fixo. Localizar o INF pelo provedor/hardware ID.
- Registro COM do ASIO sempre apontando para o binário instalado, não para pasta de dev.
- Autostart do painel com gatilho válido e caminho de instalação.

## Instalar o build do repo
Pré-requisitos: `cmake --build build --config Release` (driver, bridge, `test_com_reg`) e o painel
empacotado (`cd panel; python -m PyInstaller BehringerUM2Panel.spec` → `panel/dist/BehringerUM2Panel.exe`).

| Script | O que faz |
|---|---|
| `Instalar_KaxWav_UM2.bat` | Eleva e roda o `.ps1`; no fim reabre o painel sem elevação (`--headless`) |
| `Instalar_KaxWav_UM2.ps1 [-DryRun]` | Confere artefatos; recusa se um host (DAW) estiver com a DLL; backup dos arquivos + `reg export` + `manifest.json` (MD5) em `%ProgramData%\KaxWav\backups\<data_hora>`; copia para `%ProgramFiles%\KaxWav\Behringer UM2 ASIO`; `regsvr32`; remove a entrada WOW6432Node; verifica MD5, `InprocServer32` e o caminho que o COM carrega (`test_com_reg.exe`) |
| `Rollback_KaxWav_UM2.bat [pasta]` | Volta para o backup mais recente (ou o indicado): arquivos com MD5 conferido e chaves de registro reimportadas |
| `Registrar_Driver_UM2.bat` | Só o `regsvr32` da DLL instalada |
| `Registrar_UM2_FL_Studio.reg` / `Ativar_UM2_No_FL_Studio.bat` | Registro manual equivalente, apontando para Program Files |
| `Iniciar_Painel_UM2.bat` | Abre o painel instalado (ou o do repo, sem instalação) |

# installer
Instalação e desinstalação. Regras:
- Nunca `pnputil /delete-driver oemXX.inf` por índice fixo. Localizar o INF pelo provedor/hardware ID.
- Registro COM do ASIO sempre apontando para o binário instalado, não para pasta de dev.
- Autostart do painel com gatilho válido e caminho de instalação.

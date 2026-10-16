# Instalador e arquivos publicados

Site: https://otavio-dias-leite.github.io/mp3player/

O workflow de firmware compila `cardputer-adv`, monta a imagem completa e publica os binários versionados. Também gera o binário de aplicação separado para M5Launcher e um arquivo de checksums SHA-256.

O workflow Pages publica no site a imagem completa de uma release já publicada, atualiza o manifesto e copia os guias em `docs/`.

- `mp3-player-cardputer-adv-full.bin`: flash completo por USB, endereço 0x0.
- `firmware.bin`: imagem de aplicação produzida pelo PlatformIO.
- `johnny-mp3-cardputer-adv-VERSAO.bin`: cópia versionada da aplicação para M5Launcher.
- `SHA256SUMS.txt`: checksums dos binários distribuídos.

Não use a imagem completa no fluxo de instalação de aplicação do M5Launcher.

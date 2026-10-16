# Instalar o JOHNNY MP3 pelo M5Launcher

O repositório publica um **binário de aplicação ESP32-S3** para o M5Stack Cardputer ADV:

`johnny-mp3-cardputer-adv-1.0.0.bin`

Download direto:

https://github.com/otavio-dias-leite/mp3player/releases/download/v1.0.0/johnny-mp3-cardputer-adv-1.0.0.bin

## Qual arquivo usar

Use somente o arquivo `johnny-mp3-cardputer-adv-1.0.0.bin` no fluxo de aplicações do M5Launcher. Ele é uma cópia da saída `firmware.bin` do PlatformIO.

**Não use** `mp3-player-cardputer-adv-full.bin` no fluxo de app/OTA do M5Launcher. A imagem `full.bin` contém bootloader, tabela de partições, boot_app0 e aplicação e serve para gravação completa da flash por USB, no endereço 0x0.

## Instalação

1. Baixe o binário de aplicação da release e coloque-o no microSD; no M5Launcher, abra o arquivo de firmware local e inicie a instalação.
2. Para usar um Favorite/URL, cadastre o link direto acima e abra esse favorito no M5Launcher. O endereço aponta para o asset versionado da release, não para uma página HTML.
3. Confirme que o destino é Cardputer ADV / ESP32-S3. Mantenha o dispositivo alimentado e não remova o cartão durante operações.

## Atualização

Para uma versão posterior, use o asset de aplicação com a nova versão no nome e na URL. Verifique também o arquivo `SHA256SUMS.txt` correspondente à release.

## Catálogo público

Esta release prepara e disponibiliza o binário para instalação pelo M5Launcher/Favorites. A inclusão em um catálogo de terceiros é uma etapa separada, sujeita ao fluxo de submissão e aprovação dos mantenedores do catálogo.

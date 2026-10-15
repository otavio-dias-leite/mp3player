# JOHNNY — Cardputer ADV

**Alpha 5 · M5Stack Cardputer ADV / ESP32-S3 · 8 MB Flash · sem PSRAM**

Firmware leve com interface preta e vermelha e foco nesta compilação em reprodução MP3 local.

## Recursos implementados nesta versão

- Player MP3 para arquivos `.mp3` no microSD.
- Busca de músicas em diretórios, com profundidade limitada.
- Reprodução em blocos com buffer fixo; nenhuma faixa inteira é carregada na RAM.
- Pausar/continuar, parar e controle de volume.
- Navegador simples do cartão SD.
- Estado do dispositivo: versão, SD e heap livre.
- Splash JOHNNY / WAKE UP SAMURAI com glitches discretos.

## Controles

- `;` / `.`: item anterior / próximo.
- `ENTER`: abrir pasta ou reproduzir faixa.
- `ESC`, crase ou til: voltar; ao sair do player, encerra decoder e libera buffers.
- `SPACE`: pausar/continuar.
- `P`: parar.
- `-` / `+`: volume.

## Hardware

Alvo: **M5Stack Cardputer ADV**. O ADV usa teclado TCA8418 e codec ES8311; não trate drivers do Cardputer original como intercambiáveis sem adaptação. O SD usa SPI: CS GPIO12, SCK GPIO40, MISO GPIO39 e MOSI GPIO14.

## Compilar no Arch Linux

```bash
python -m pip install --user platformio
cd ~/johnny
pio run -e cardputer-adv
```

Para gravar por USB:

```bash
pio run -e cardputer-adv -t upload
```

O GitHub Actions também compila e publica a imagem completa USB e o firmware OTA após push válido em `main`.

## Fora do escopo desta compilação

Sintetizador, afinador, rádio web, equalizador, gravador, Bluetooth, alarmes, relógio, gerenciador de processos, cofre criptográfico e jogos constam como etapas futuras na arquitetura; não são anunciados como recursos funcionais desta alpha.

A compilação CI confirma que o código compila. Ela não substitui testes físicos de boot, áudio, microSD e estabilidade no Cardputer ADV.
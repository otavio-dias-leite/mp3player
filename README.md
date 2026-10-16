# MP3 Player — M5Stack Cardputer ADV

**Versão 8.0.0** — retorno à base anterior do firmware, preservando o site atual e o fluxo de instalação.

## Firmware 8.0.0

- Reprodução de MP3 e suporte opcional a WAV, FLAC e AAC conforme os decoders disponíveis.
- Biblioteca de áudio em microSD e armazenamento interno.
- Controles de reprodução, fila, avanço/retorno, busca, aleatório e repetição.
- Velocidades normal, slowed (0.75x), sped up (1.25x) e Nightcore (1.50x).
- Equalizador de cinco bandas, presets, compressor e limitador suave.
- Visualizador de áudio, capas e navegação de imagens.
- Configurações persistentes, temporizador de sono e brilho.
- Interface azul Ice Blue para M5Stack Cardputer ADV.

## Hardware

- M5Stack Cardputer ADV / ESP32-S3.
- microSD: CS GPIO12, SCK GPIO40, MISO GPIO39, MOSI GPIO14.
- LED RGB GPIO21.

## Build

A fonte oficial usada pelo PlatformIO é `src/main.cpp`.

```bash
pio run -e cardputer-adv
```

O GitHub Actions compila os binários OTA e USB, publica a release e atualiza o site com a versão efetivamente publicada.

## Instalação

Acesse [MP3 Player](https://otavio-dias-leite.github.io/mp3player/) no Chrome ou Edge, conecte o Cardputer ADV por USB em modo de gravação e use o instalador da página.

O arquivo `MP3_Player.ino` na raiz é um snapshot legado e não é usado pelo build oficial.
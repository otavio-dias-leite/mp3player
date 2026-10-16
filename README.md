# MP3 Player — M5Stack Cardputer ADV

**Versão 8.4.0** — firmware de reprodução musical para o M5Stack Cardputer ADV (ESP32-S3), com biblioteca em microSD/LittleFS, reprodução com DSP, leitor de imagens e interface multilíngue.

## Recursos

- Navegação por pastas com duas entradas visíveis por vez, miniaturas de capas e metadados ID3.
- Leitura de capa incorporada em MP3 (ID3 APIC/PIC) e FLAC PICTURE; também procura `cover.jpg`, `folder.jpg`, `front.jpg`, `album.jpg` e `albumart.jpg`.
- Visualizador de imagens JPEG e PNG, com navegação entre arquivos da pasta.
- Reprodução MP3, WAV, FLAC e AAC quando o decoder está disponível no pacote ESP8266Audio utilizado.
- Velocidades: normal (1.00x), slowed (0.75x), sped up (1.25x) e nightcore (1.75x), com transição de taxa gradual e reamostrador FIR polifásico com filtro anti-aliasing.
- EQ de cinco bandas, presets, controle de volume, compressor suave, limitador e realce adaptado ao speaker do Cardputer.
- Fila, próximo/anterior, busca, aleatório, repetição, temporizador de sono e visualizadores de áudio.
- Recuperação do microSD: remonta o barramento SPI em frequências graduais, reabre a faixa no mesmo offset e tenta continuar de uma cópia idêntica em LittleFS quando disponível.
- Cache limitado de miniaturas; decodificação por etapas e buffers de áudio dimensionados pelo maior bloco contíguo de RAM disponível.
- Interface integralmente localizada em inglês, português brasileiro, espanhol, chinês simplificado, japonês, francês, alemão, russo e italiano.
- Temas escuros, grade azul discreta, LED RGB e configurações persistidas.

## Hardware e memória

- M5Stack Cardputer ADV / ESP32-S3FN8, 8 MB de flash e microSD.
- O firmware não pressupõe PSRAM: sprites e buffer de áudio são alocados na RAM interna, com margem reservada para decoder e sistema.
- Pinos SPI do microSD: CS GPIO12, SCK GPIO40, MISO GPIO39, MOSI GPIO14.

## Teclas

- `;` / `.`: mover seleção / volume
- `,` / `/`: página da biblioteca / busca no player
- `ENTER`: abrir pasta, tocar faixa ou abrir imagem
- `SPACE`: pausar/retomar
- `N` / `P`: próxima/anterior
- `TAB`: abrir player
- `S`: configurações; `E`: equalizador; `H`: ajuda
- `M`: alternar SD/memória interna; `C`: copiar arquivo SD para LittleFS
- `7`: slowed; `8`: sped up; `9`: nightcore; `0`: velocidade normal
- No visualizador: `,` / `;` imagem anterior, `.` / `/` próxima, `DEL` voltar

## Build

Fonte principal: `src/main.cpp` (PlatformIO). O arquivo `MP3_Player.ino` na raiz é um snapshot legado e não é usado pelo pipeline oficial.

Com PlatformIO: `pio run -e cardputer-adv`.

O build de CI gera `firmware.bin` para OTA e `mp3-player-cardputer-adv-full.bin` para gravação USB.

## Nota de validação

A compilação automatizada verifica o toolchain. A qualidade final de áudio, estabilidade de cartões específicos, leitura de imagens grandes e consumo de bateria ainda precisam de testes físicos no Cardputer ADV.


## Versão 8.4.0

- Consolida o pipeline de áudio FIR polifásico com anti-aliasing e transição gradual de velocidade.
- Mantém a recuperação de leitura do microSD com distinção entre falha de mídia e fim normal de arquivo.
- Preserva a navegação de biblioteca, metadados, capas, equalizador, armazenamento interno e interface multilíngue.
- Publicação automatizada: GitHub Actions compila o firmware, cria os binários da release e atualiza o GitHub Pages após a release ser publicada.

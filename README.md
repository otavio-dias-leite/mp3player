# JOHNNY — M5Stack Cardputer ADV

Firmware para o M5Stack Cardputer ADV (ESP32-S3, 8 MB Flash, sem PSRAM), com interface roxo/rosa, navegação por teclado, áudio local, ferramentas de sistema e utilitários.

## Menu principal

A tela inicial tem quatro áreas:

- **MUSIC:** SYNTH, TUNER, MUSIC PLAYER, WEB RADIO, EQUALIZER e MIC RECORDER.
- **SYSTEM:** FILES, USB TRANSFER, WI-FI, UPDATE FIRMWARE e DEVICE STATUS.
- **GAMES:** CONWAY LIFE.
- **HACK:** WI-FI RECON, BLE SCANNER, SPAM FX (OFFLINE) e DEVICE / RF INFO.

O jogo disponível nesta compilação é Conway's Game of Life.

## MUSIC PLAYER

A biblioteca lê músicas do microSD e percorre diretórios. O firmware mantém a reprodução em blocos; não carrega uma faixa inteira na RAM.

### Formatos

- MP3
- WAV PCM
- FLAC
- AAC em fluxo compatível com o decoder
- Ogg/Opus (.opus, .oga ou .ogg contendo Opus)

O contêiner .ogg pode carregar codecs diferentes. Ogg/Opus é suportado; Ogg/Vorbis não está habilitado nesta compilação. Arquivos AAC dentro de MP4/M4A não devem ser presumidos compatíveis: o decoder usado espera um fluxo AAC compatível.

### Metadados e capas

- MP3 e AAC com tags ID3v2.3/v2.4: título, artista, álbum e imagem APIC.
- FLAC: comentários Vorbis (TITLE, ARTIST, ALBUM) e bloco de imagem PICTURE.
- Para qualquer formato compatível, o player procura capas laterais no diretório da música: `cover.jpg`, `Cover.jpg`, `folder.jpg`, `nome-da-faixa.jpg` ou `nome-da-faixa.png`. Também procura `/COVERS/nome-da-faixa.jpg`.
- Capas embutidas ou laterais são limitadas a 40 KiB de dados para manter o uso de RAM previsível. Uma imagem maior ou inválida é ignorada e o player mostra a arte padrão.

### Controles do player

- `,` / `/`: faixa anterior / próxima.
- `;` / `.`: voltar / avançar aproximadamente 5% no arquivo.
- `ENTER` ou espaço: pausar / continuar.
- `P`: parar.
- `-` / `+`: volume.
- `S`: alternar aleatório.
- `R`: repetir desligado / repetir lista / repetir faixa.

O indicador de progresso e o seek são aproximados, calculados pela posição no arquivo. Para evitar corromper o estado interno de decoders que exigem cabeçalhos ou páginas desde o início, o seek está habilitado apenas para MP3 e AAC compatível. Em WAV, FLAC e Ogg/Opus, a reprodução, pausa, parada e troca de faixa continuam disponíveis, mas o seek fica desabilitado. Em MP3 VBR, porcentagem de bytes não equivale perfeitamente a tempo decorrido.

O player não é um cliente Spotify e não transmite o catálogo do Spotify. É uma biblioteca local no microSD, com rádio web separado.

## Outros recursos

- **SYNTH:** sintetizador monofônico com timbres clean, overdrive, fuzz, square e guitarra.
- **TUNER:** afinador cromático pelo microfone interno; modos guitarra, violão e ukulele.
- **WEB RADIO:** estações predefinidas ou URL HTTP/ICY MP3, com gravação WAV no cartão.
- **EQUALIZER:** DSP de cinco bandas peaking biquad em 80 Hz, 250 Hz, 1 kHz, 4 kHz e 10 kHz, com ganhos de ±6 dB e perfis Flat, Warm, Vocal e Bass.
- **MIC RECORDER:** gravação PCM mono de 16 kHz em `/RECORDINGS`.
- **FILES:** explorador do microSD.
- **USB TRANSFER:** protocolo serial JOHNNY/2 para enviar e receber arquivos por meio da página de transferência.
- **WI-FI:** varredura e seleção de redes.
- **DEVICE STATUS:** cartão SD, Wi-Fi, BLE e bateria.
- **CONWAY LIFE:** simulação interativa com pausa, passo, edição de células e save em `/JOHNNY/life.sav`.

## HACK — diagnósticos sem hardware adicional

- **WI-FI RECON:** varredura passiva de redes próximas, com SSID, intensidade do sinal, canal e indicação de rede aberta/segura. Não tenta autenticar, desautenticar ou interromper redes.
- **BLE SCANNER:** descoberta passiva de anúncios Bluetooth LE, com nome, sufixo do endereço e RSSI.
- **TCP CONNECT TEST:** testa uma única combinação de IP e porta digitada pelo operador (por exemplo, `192.168.1.1:80`). Não faz varredura de faixas ou listas de portas.
- **DNS LOOKUP:** resolve um hostname digitado e mostra o endereço retornado.
- **HTTP STATUS CHECK:** envia uma requisição HEAD para uma URL `http://` digitada e mostra o código HTTP.
- **SPAM FX / OFFLINE:** procura no microSD os arquivos de áudio para Rickroll ou Never Fade Away; a terceira opção é apenas um efeito visual local. Nenhum áudio protegido é incluído no firmware.
- **NETWORK / RADIO INFO:** IP, MAC, canal, RSSI e gateway da conexão atual.

O HACK foi limitado a observação e diagnóstico. Funções Bruce/Marauder que exigem CC1101, NRF24, PN532, antena adicional ou outro módulo não fazem parte desta compilação. Também não incluí desautenticação, flooding de beacon, captive portal/evil portal, spoofing de pontos de acesso ou injeção de pacotes.

## Armazenamento e memória

O cartão microSD é armazenamento persistente, não RAM. O áudio usa um buffer de entrada pré-alocado de 8 KiB, buffers de saída limitados e uma área limitada para capas. A reprodução não depende de carregar arquivos inteiros na memória. Uma leitura lenta ou um cartão defeituoso ainda pode causar interrupção; use cartão confiável e evite removê-lo durante a reprodução ou gravação.

O hardware de destino não tem PSRAM. Recursos de áudio, imagem, Wi-Fi e BLE precisam compartilhar a RAM interna. Ao iniciar as ferramentas Wi-Fi/BLE, o JOHNNY interrompe a reprodução ativa para reduzir a competição por recursos.

## Visualização web e estado do dispositivo

Depois de conectar o Cardputer à rede Wi-Fi em **SYSTEM → WI-FI**, abra no navegador de outro dispositivo conectado à mesma rede o endereço IP mostrado em **SYSTEM → DEVICE STATUS** (por exemplo, `http://192.168.1.50`). O hostname mDNS `http://johnny.local` também é anunciado quando a rede o permite.

A página local mostra uma captura da tela física em intervalos aproximados de 1,2 segundo e atualiza o estado do dispositivo. A rota `/api/status` entrega JSON com IP, MAC, SSID, RSSI, gateway, subnet, DNS, versão, bateria, estado do microSD, heap livre, PSRAM e uptime. A visualização é somente leitura: teclado e controles continuam no Cardputer. Não é vídeo contínuo nem streaming remoto de baixa latência. O servidor deve ser usado apenas em uma rede local confiável.

## Interface e teclas

A tela inicial usa um cartão grande com ícone. Navegue horizontalmente na ordem **HACK → GAMES → MUSIC → SYSTEM**; os indicadores mostram a categoria atual. A abertura mantém menus próprios para cada área.

- `,` / `/`: categoria anterior/próxima na tela inicial.
- `;` / `.`: navegar pelos itens dos menus e listas.
- `ENTER`: selecionar / confirmar.
- `ESC`, crase (`\``) ou til (`~`): voltar.
- `TAB`: ativar/desativar o protetor de tela; não fecha o aplicativo.
- **SYNTH:** QWERTYU toca as notas; `-` / `+` ajusta volume; `[` / `]` ajusta oitava; esquerda/direita troca oitava; cima/baixo troca timbre.

Na guitarra, o sintetizador usa buffers alternados e canal de áudio independente para evitar que cada nota interrompa a reprodução do alto-falante principal. A compilação não substitui a validação da resposta sonora no aparelho físico.

## Atualização e compilação

A opção SYSTEM → UPDATE FIRMWARE baixa `firmware.bin` da última GitHub Release. É necessária uma conexão Wi-Fi funcional. A atualização usa HTTPS e o mecanismo OTA do ESP32.

Para compilar e gravar com PlatformIO:

```sh
pio run -e cardputer-adv
pio run -e cardputer-adv -t upload
pio device monitor
```

O workflow de firmware compila o ambiente `cardputer-adv` e monta também uma imagem completa para gravação USB. Pushes para `main` geram artefatos de teste; uma GitHub Release versionada só é publicada por uma tag `v<VERSION>` depois de a compilação passar.

## Hardware e validação

- Alvo: M5Stack Cardputer ADV / ESP32-S3.
- Flash: 8 MB; PSRAM: não disponível.
- O áudio usa o alto-falante/codec integrado gerenciado pelo M5Cardputer. Não inicialize outro driver I2S em paralelo.
- O afinador depende do microfone integrado e de ambiente relativamente silencioso.
- A compilação bem-sucedida verifica a construção do binário, mas não substitui teste físico de boot, som, microfone, SD, Wi-Fi e BLE no aparelho.

## Licença e dependências

O código próprio deste repositório mantém a licença indicada em `LICENSE`. O decoder ESP8266Audio 1.9.7 é GPL-3.0; veja `THIRD_PARTY_NOTICES.md`. A integração AAC tem observações adicionais de licença/patentes descritas no arquivo de avisos.

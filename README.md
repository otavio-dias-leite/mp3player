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

## HACK — ferramentas sem fio e FX

- **WI-FI RECON:** lista redes encontradas e força do sinal. É uma ferramenta de observação; não tenta autenticar nem interromper redes.
- **BLE SCANNER:** descoberta passiva de dispositivos Bluetooth LE próximos, com nome, endereço abreviado e RSSI.
- **SPAM FX (OFFLINE):** demos locais. Rickroll procura no microSD um arquivo de áudio cujo nome contenha `rick` ou `never gonna give you up`; Never Fade Away procura `never fade away`. O firmware não inclui faixas protegidas por direitos autorais. A opção visual é um efeito local e não transmite pacotes.
- **DEVICE / RF INFO:** informações e limites das ferramentas.

O firmware não executa inundação de Wi-Fi/Bluetooth e não cria ponto de acesso enganoso com o nome “Free Wi-Fi”. As opções HACK são leitura passiva e demonstrações locais.

## Armazenamento e memória

O cartão microSD é armazenamento persistente, não RAM. O áudio usa um buffer de entrada pré-alocado de 8 KiB, buffers de saída limitados e uma área limitada para capas. A reprodução não depende de carregar arquivos inteiros na memória. Uma leitura lenta ou um cartão defeituoso ainda pode causar interrupção; use cartão confiável e evite removê-lo durante a reprodução ou gravação.

O hardware de destino não tem PSRAM. Recursos de áudio, imagem, Wi-Fi e BLE precisam compartilhar a RAM interna. Ao iniciar as ferramentas Wi-Fi/BLE, o JOHNNY interrompe a reprodução ativa para reduzir a competição por recursos.

## Teclas gerais

- Setas e teclas equivalentes: navegação conforme a tela.
- `ENTER`: selecionar / confirmar.
- `ESC`, crase (`\``) ou til (`~`): voltar.
- `TAB`: ativar/desativar o protetor de tela; não fecha o aplicativo.

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

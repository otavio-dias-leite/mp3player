# JOHNNY — Cardputer-Adv

Firmware para o M5Stack Cardputer-Adv (ESP32-S3), com interface em roxo/rosa, navegação pelo teclado, áudio e utilitários.

## Módulos

- **SYNTH:** sintetizador monofônico com patch de guitarra dedilhada, clean, overdrive, fuzz e square.
- **TUNER:** afinador cromático pelo microfone interno. Modos guitarra, violão e ukulele; mostra a corda mais próxima, a frequência, o desvio em cents e a direção do ajuste.
- **MUSIC:** biblioteca local com player de navegação horizontal e reprodução por blocos.
- **RADIO:** rádio web pela rede Wi-Fi, com estações predefinidas, URL HTTP personalizada e gravação WAV no microSD (`/RECORDINGS`).
- **FILES:** explorador do cartão microSD.
- **WI-FI:** procura redes e permite selecionar e conectar.
- **USB:** transferência de arquivos do microSD por USB serial usando a página web, com blocos confirmados por ACK e downloads em fluxo.
- **EQUALIZER:** DSP de cinco bandas com filtros peaking biquad (80 Hz, 250 Hz, 1 kHz, 4 kHz e 10 kHz), ganho de ±6 dB, compensação de pré-ganho e perfis Flat, Warm, Vocal e Bass.
- **CONWAY LIFE:** autômato celular Conway B3/S23, com edição de células, execução, pausa e passo individual.
- **SYSTEM:** informações do dispositivo e bateria.

## Menus do firmware

A tela inicial organiza os aplicativos em três camadas:

- **MUSIC:** SYNTH, TUNER, MUSIC PLAYER, WEB RADIO, EQUALIZER e MIC RECORDER.
- **SYSTEM:** FILES, USB TRANSFER, WI-FI, BLUETOOTH, UPDATE FIRMWARE e INSTALL APPS.
- **GAMES:** CONWAY LIFE, SNAKE, PONG, PAC-MAN e TETRIS.

Os módulos existentes (sintetizador, afinador, player, rádio, equalizador, arquivos, USB, Wi-Fi e Conway Life) são acessados pelos novos menus. O save do Conway Life é gravado em `/JOHNNY/life.sav` no microSD ao sair do jogo. O gravador de microfone salva WAV PCM mono de 16 kHz em `/RECORDINGS/MICxxxx.WAV`.

**Jogos:** Conway Life, Snake, Pong, Pac-Man e Tetris estão disponíveis no menu GAMES. Bluetooth e catálogo/instalador de aplicativos permanecem não implementados.

**Servidor web local:** ao conectar o Cardputer ao Wi-Fi, abra o IP exibido em SYSTEM → WI-FI no navegador de um dispositivo na mesma rede. A interface permite navegar pelo microSD e baixar arquivos. O espelhamento da tela e upload pelo navegador ainda não estão implementados nesta versão.

## Atualização OTA

A opção SYSTEM → UPDATE FIRMWARE baixa o binário `firmware.bin` da última GitHub Release do repositório. O dispositivo precisa estar conectado ao Wi-Fi. A atualização usa HTTPS e o mecanismo OTA do ESP32.

O workflow `.github/workflows/firmware-release.yml` compila o ambiente `cardputer-adv` em pushes para `main`. Ao publicar uma tag `v*`, ele anexa `firmware.bin` à GitHub Release. Para disponibilizar uma nova versão ao atualizador, publique uma tag/release após validar a compilação e o teste no hardware.

O cartão microSD é armazenamento persistente, não RAM. O firmware usa buffers limitados em RAM para áudio e transferência; não usa o cartão como swap, pois a latência e a vida útil do cartão tornam essa abordagem inadequada para substituir memória de execução.

## Equalização e áudio

O equalizador é um banco de cinco filtros peaking biquad do tipo RBJ Audio EQ Cookbook, aplicado ao PCM decodificado antes da saída ES8311. O perfil Warm vem ativo inicialmente. Os ganhos são limitados a ±6 dB e compensados por pré-ganho para reduzir clipping. A preferência é armazenada no NVS.

## Formatos de áudio

O player usa ESP8266Audio 1.9.7 e saída pelo M5Cardputer.Speaker (codec ES8311). A reprodução não carrega o arquivo inteiro na RAM: usa uma fonte SD/HTTP e um buffer limitado.

Formatos selecionados:
- MP3
- WAV PCM (8 ou 16 bits, conforme o decoder)
- FLAC
- AAC
- Ogg/Opus (.opus, .oga ou .ogg contendo Opus)

**Limitação OGG:** .ogg é um contêiner que pode conter codecs diferentes. Esta compilação decodifica Ogg/Opus; Ogg/Vorbis comum ainda não é suportado. Ogg/Vorbis exigiria outro decoder/runtime e não deve ser confundido com Opus.

O rádio web usa HTTP/ICY MP3. URLs personalizadas devem começar por http://; HTTPS ainda não está habilitado nessa integração. O stream precisa ser compatível com MP3. As estações pré-configuradas dependem de disponibilidade dos servidores e conexão de internet.

## Teclas

- Setas ou , . / ;: navegação conforme a tela.
- Enter: selecionar, iniciar ou pausar.
- Esc: voltar.
- **TAB:** ligar/desligar o screen saver; a reprodução continua em segundo plano.
- **SYNTH:** QWERTYU toca notas; setas ajustam oitava/timbre; - e + ajustam volume; [ e ] ajustam oitava.
- **MUSIC:** na biblioteca, cima/baixo selecionam; no player, esquerda/direita ou cima/baixo trocam faixa; Enter/espaço pausa; P para; -/+ ajustam volume.
- **RADIO:** cima/baixo seleciona estação; Enter conecta; espaço pausa; P para; opção Custom permite informar uma URL HTTP; R inicia/finaliza gravação WAV no cartão SD; o equalizador é acessado exclusivamente pelo menu MUSIC → EQUALIZER.
- **EQUALIZER:** acessível exclusivamente pelo menu MUSIC → EQUALIZER. Cima/baixo escolhe a banda; esquerda/direita altera o ganho em passos de 0,5 dB; P percorre os perfis; 0 seleciona Flat.
- **CONWAY LIFE:** no menu GAMES, selecione LIFE. Espaço executa/pausa; N avança uma geração; R gera uma população aleatória; C limpa; Enter alterna a célula sob o cursor; setas movem; +/- ajustam a velocidade.
- **TUNER:** esquerda/direita alterna guitarra, violão e ukulele. Toque uma corda e mantenha a nota por um instante. A leitura fica visível por até 5 segundos após o sinal desaparecer. “GRAVE DEMAIS / APERTE” indica corda frouxa; “AGUDA DEMAIS / AFROUXE” indica corda apertada. “AFINADA” aparece dentro da tolerância de ±5 cents.

## Compilar e instalar

Requer PlatformIO:

    pio run -e cardputer-adv
    pio run -e cardputer-adv -t upload
    pio device monitor

Use a porta serial correta ao instalar. O cartão microSD deve estar montado e formatado de forma compatível com a biblioteca SD.

## USB e armazenamento

O protocolo USB serial JOHNNY/2 usa blocos de upload de 512 bytes com confirmação individual. O download é lido em blocos de 4 KiB; navegadores compatíveis com File System Access podem gravar diretamente em um arquivo, sem manter todo o conteúdo em RAM. O cartão deve estar montado e ter espaço livre suficiente. Evite remover o cartão durante operações.

## Hardware e validação

- O afinador depende do microfone integrado e de ruído ambiente baixo.
- A saída usa o alto-falante/codec do próprio Cardputer-Adv; não inicialize outro driver I2S em paralelo.
- O firmware foi preparado para o ESP32-S3 sem depender de PSRAM.
- A gravação de rádio salva áudio mono PCM de 16 bits em WAV, depois do equalizador, em `/RECORDINGS/RADIOxxxx.WAV`. A fila de gravação é limitada; cartões lentos ou defeituosos podem interromper a gravação sem interromper intencionalmente o stream.
- Uma compilação bem-sucedida não substitui o teste físico do alto-falante, microfone, Wi-Fi e cartões reais.
- USB é transferência via serial com navegador compatível, não uma unidade USB MSC montada no sistema operacional.

## Licença e dependências

O código próprio deste repositório mantém a licença indicada em LICENSE. A dependência ESP8266Audio 1.9.7 é GPL-3.0; veja THIRD_PARTY_NOTICES.md e a licença original. A distribuição de firmware vinculado à dependência deve respeitar os termos da licença aplicável.

## Segurança

Use apenas redes, dispositivos e streams que você tenha autorização para acessar.


## Versionamento

A versão pública é controlada exclusivamente pelo arquivo `VERSION` (SemVer). O build do PlatformIO gera `include/johnny_version.h` automaticamente a partir desse arquivo, e o firmware mostra a versão na tela inicial. O workflow de publicação compila o firmware e, somente se a compilação passar, cria/atualiza a GitHub Release `v<VERSION>` com `firmware.bin`. O site e o manifesto do instalador web usam o mesmo arquivo `VERSION`. Ao preparar uma atualização de código, incremente `VERSION` (por exemplo, `0.0.0-alpha.2` para `0.0.0-alpha.3`) antes de enviar as alterações; não mantenha versões separadas no código, site ou workflow.

## Jogos

O autômato de Conway permanece no firmware como aplicativo LIFE. DOOM 1993 ainda não está integrado ao binário do JOHNNY.

**Os núcleos de GB/GBC, GBA e DOOM ainda não estão integrados.** O port existente para Cardputer que encontrei usa uma árvore ESP-IDF própria, driver de teclado matricial antigo e estratégias de paginação/mapeamento de memória específicas. O ADV usa o teclado I²C gerenciado pela biblioteca M5Cardputer e este firmware é Arduino/PlatformIO. Copiar esse port diretamente não é compatível nem seguro sem uma adaptação e validação próprias. O processador do ADV também trabalha sem PSRAM, o que exige manter o consumo de RAM sob controle.

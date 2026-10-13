# JOHNNY — Cardputer-Adv

Firmware para o M5Stack Cardputer-Adv (ESP32-S3), com interface em roxo/rosa, navegação pelo teclado, áudio e utilitários.

## Módulos

- **SYNTH:** sintetizador monofônico com patch de guitarra dedilhada, clean, overdrive, fuzz e square.
- **TUNER:** afinador cromático pelo microfone interno. Modos guitarra, violão e ukulele; mostra a corda mais próxima, a frequência, o desvio em cents e a direção do ajuste.
- **MUSIC:** biblioteca local com player de navegação horizontal e reprodução por blocos.
- **RADIO:** rádio web pela rede Wi-Fi, com estações predefinidas e URL HTTP personalizada.
- **FILES:** explorador do cartão microSD.
- **WI-FI:** procura redes e permite selecionar e conectar.
- **USB:** transferência de arquivos do microSD por USB serial usando a página web.
- **SYSTEM:** informações do dispositivo e bateria.

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
- Esc ou apóstrofo: voltar.
- **TAB:** ligar/desligar o screen saver; a reprodução continua em segundo plano.
- **SYNTH:** QWERTYU toca notas; setas ajustam oitava/timbre; - e + ajustam volume; [ e ] ajustam oitava.
- **MUSIC:** na biblioteca, cima/baixo selecionam; no player, esquerda/direita ou cima/baixo trocam faixa; Enter/espaço pausa; P para; -/+ ajustam volume.
- **RADIO:** cima/baixo seleciona estação; Enter conecta; espaço pausa; P para; opção Custom permite informar uma URL HTTP.
- **TUNER:** esquerda/direita alterna guitarra, violão e ukulele. Toque uma corda e mantenha a nota por um instante. A leitura fica visível por até 3 segundos após o sinal desaparecer. “GRAVE DEMAIS / APERTE” indica corda frouxa; “AGUDA DEMAIS / AFROUXE” indica corda apertada. “AFINADA” aparece dentro da tolerância de ±5 cents.

## Compilar e instalar

Requer PlatformIO:

    pio run -e cardputer-adv
    pio run -e cardputer-adv -t upload
    pio device monitor

Use a porta serial correta ao instalar. O cartão microSD deve estar montado e formatado de forma compatível com a biblioteca SD.

## Hardware e validação

- O afinador depende do microfone integrado e de ruído ambiente baixo.
- A saída usa o alto-falante/codec do próprio Cardputer-Adv; não inicialize outro driver I2S em paralelo.
- O firmware foi preparado para o ESP32-S3 sem depender de PSRAM.
- Uma compilação bem-sucedida não substitui o teste físico do alto-falante, microfone, Wi-Fi e cartões reais.
- USB é transferência via serial com navegador compatível, não uma unidade USB MSC montada no sistema operacional.

## Licença e dependências

O código próprio deste repositório mantém a licença indicada em LICENSE. A dependência ESP8266Audio 1.9.7 é GPL-3.0; veja THIRD_PARTY_NOTICES.md e a licença original. A distribuição de firmware vinculado à dependência deve respeitar os termos da licença aplicável.

## Segurança

Use apenas redes, dispositivos e streams que você tenha autorização para acessar.

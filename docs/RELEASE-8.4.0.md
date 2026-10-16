# MP3 Player 8.4.0 — Cardputer ADV

## Base técnica
- ESP32-S3FN8, flash de 8 MB, sem dependência de PSRAM.
- DSP a 44,1 kHz, EQ de cinco bandas, compressor/limitador e reamostrador FIR polifásico com anti-aliasing.
- Mudança gradual de velocidade para reduzir artefatos e estalos.
- Leitura de microSD com tentativas de recuperação, retomada de posição e distinção entre erro de leitura e fim normal.
- Reprodução por etapas para manter teclado e interface responsivos.

## Recursos preservados
Navegador SD/LittleFS, fila, metadados ID3, capas, visualizador de imagens, presets de EQ, modos de reprodução, temporizador, temas, LED RGB e nove idiomas.

## Build e distribuição
O GitHub Actions executa `pio run -e cardputer-adv`, monta a imagem completa para gravação USB e publica os artefatos da release. O workflow de Pages consome a release publicada e atualiza o instalador web.

## Validação
A compilação do CI valida o toolchain e a integração do projeto. Reprodução, estabilidade com cartões específicos, qualidade acústica e consumo devem ser confirmados no hardware físico.

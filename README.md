# JOHNNY — Cardputer ADV

**Alpha 1 — 1.0.0-alpha.1**

Firmware local de mídia para M5Stack Cardputer ADV (ESP32-S3, 8 MB flash, sem PSRAM), com interface de MP3 player preta e vermelha.

## Aplicativos
- **MUSIC PLAYER:** reprodução MP3/WAV pelo microSD, navegação de diretórios, pausa, parada e volume.
- **IMAGES:** visualizador de JPEG/JPG e BMP pelo microSD.
- **EQUALIZER:** equalizador de três bandas com perfis Flat, Bass, Vocal e Treble.
- **FILES:** navegador do microSD que abre os formatos de áudio e imagem suportados.

## Execução
Somente a tela/aplicativo em primeiro plano é processado. Não há reprodução em segundo plano, rede, indexação ou worker dos aplicativos. Ao sair do player, o decoder e o alto-falante são parados, os objetos de stream e arquivo são liberados e os buffers deixam de ser usados. O barramento microSD permanece inicializado como periférico compartilhado do sistema.

O áudio usa buffer fixo de entrada de 4 KiB e buffers de saída estáticos. O equalizador é processado diretamente no caminho de saída, sem tarefa adicional. Cartões lentos, arquivos corrompidos e imagens grandes ainda podem falhar; esta versão alpha precisa ser testada no Cardputer ADV físico.

Build: `pio run -e cardputer-adv`.

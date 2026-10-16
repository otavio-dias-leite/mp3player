# JOHNNY MP3 1.0.0

**Primeiro lançamento público · M5Stack Cardputer ADV / ESP32-S3**

## Base consolidada

A versão 1.0.0 adota como base o caminho de reprodução da linha MP3 Player v12, já integrado ao build PlatformIO e ao pipeline de release do repositório. O firmware v14 enviado durante a preparação foi revisado, mas sua nova tarefa de decoder e a sincronização por mutex com operações de interface/SD exigem validação adicional no aparelho; por isso, não foi promovido automaticamente à base do primeiro lançamento.

A versão de lançamento mantém o decoder em fatias curtas no loop principal. O Speaker recebe blocos PCM pela fila assíncrona do M5Unified. Isso evita introduzir uma segunda tarefa de aplicação que abra/consulte o mesmo volume FAT do microSD em paralelo à interface.

## Recursos

- Reprodução local com MP3 como formato principal.
- Navegação por microSD e LittleFS, pastas, fila e metadados ID3.
- Pausa, retomada, parada, próxima/anterior, busca MP3, repetição e aleatório.
- Buffer e recuperação do cartão com tentativas de SPI em frequência menor.
- EQ de cinco bandas, presets, perfis separados de volume e realce de áudio.
- Velocidades 0,75x, 1,25x, 1,75x e normal.
- Configurações persistentes, nove idiomas, temas, bateria e ajuda.
- Cópia de arquivos para a memória interna e USB mass-storage, quando habilitado.
- Splash JOHNNY: “WAKE UP” sobre “SAMURAI”.

## Artefatos

- `johnny-mp3-cardputer-adv-1.0.0.bin`: aplicação para M5Launcher.
- `mp3-player-cardputer-adv-full.bin`: imagem completa para gravar por USB em 0x0.
- `firmware.bin`: binário da aplicação PlatformIO.
- Componentes bootloader/partitions/boot_app0 e `SHA256SUMS.txt`.

## Build

```bash
pio run -e cardputer-adv
```

O CI executa o build, verifica os arquivos de boot/partição e monta a imagem completa com esptool.

## Testes físicos necessários

A compilação automatizada não substitui os testes no Cardputer ADV real. Antes de afirmar validação completa, executar:

1. Cold boot e splash, com e sem cartão microSD.
2. MP3 CBR/VBR, taxas de bits baixa/alta e faixas longas.
3. Repetição contínua, sequência de faixas, pausa/retomada, parada e busca.
4. Falha de leitura do SD, recuperação e cartões em diferentes velocidades.
5. Alto-falante e fone; verificar o mute físico ao inserir o plugue e selecionar o perfil de volume correto.
6. EQ, presets, volume, idiomas e gravação de preferências.
7. USB mass-storage: montagem no computador, ejeção correta e retorno ao player.
8. Reprodução contínua por pelo menos uma hora, observando falhas de áudio, reinicializações, temperatura e memória.

**Status:** CI pode validar compilação e montagem da imagem. Os testes físicos precisam ser executados no hardware; não são declarados concluídos por este commit.

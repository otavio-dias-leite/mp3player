# JOHNNY MP3 — M5Stack Cardputer ADV

**Versão 1.0.0 · Primeiro lançamento público · ESP32-S3**

O JOHNNY MP3 é um reprodutor de música local para o **M5Stack Cardputer ADV**. A versão 1.0.0 consolida a linha de reprodução já integrada ao build e mantém o áudio deliberadamente simples: leitura do cartão, decodificação em fatias curtas e envio do PCM à fila do alto-falante. A interface não cria uma segunda tarefa de decodificação concorrendo pelo mesmo microSD.

## Recursos

- Reprodução local de MP3 a partir do microSD e, quando disponível, da memória interna.
- Navegação por pastas/subpastas e fila de reprodução.
- Leitura de título e artista em tags ID3.
- Play, pausa, parar, faixa anterior/próxima, busca de 10 segundos e volume.
- Repetição, reprodução aleatória e temporizador.
- Velocidades SLOWED 0,75x, SPED UP 1,25x, Nightcore 1,75x e normal.
- Equalizador de cinco bandas, seis presets e realce de som configurável.
- Perfis de volume independentes: alto-falante 80% e fone 30% por padrão.
- Buffer de áudio, montagem do SD em velocidades SPI alternativas e tentativa de recuperação após falhas.
- Cópia de faixas para a memória interna e modo USB de armazenamento em massa, quando habilitado.
- Interface com nove idiomas, temas, nível de bateria e ajuda integrada.

**Formatos:** MP3 é o formato principal. WAV, FLAC e AAC são ativados somente quando os cabeçalhos dos decodificadores correspondentes estão presentes na versão instalada de ESP8266Audio. M4A, OGG e OPUS não são suportados.

O conector de 3,5 mm do Cardputer ADV desativa fisicamente o amplificador do alto-falante quando um fone é inserido. A seleção do perfil de volume do fone/alto-falante é feita pela tecla `O`; o firmware não afirma detectar por software um pino de presença de fone.

## Controles

| Tela | Tecla | Ação |
|---|---|---|
| Biblioteca | `; / .` | Item anterior / próximo |
| Biblioteca | `, / /` | Página anterior / próxima |
| Biblioteca | Enter | Abrir pasta ou iniciar a fila |
| Biblioteca | Delete | Pasta anterior |
| Biblioteca | Espaço | Pausar / retomar |
| Biblioteca | N / P | Próxima / anterior |
| Biblioteca | Tab | Abrir o player |
| Biblioteca | S / E / H | Ajustes / equalizador / ajuda |
| Biblioteca | R / X | Atualizar biblioteca / parar |
| Biblioteca | M / C / U | Trocar SD/Flash / copiar para Flash / USB |
| Player | Espaço | Pausar / retomar |
| Player | N / P | Próxima / anterior |
| Player | `, / /` | Buscar −10 s / +10 s |
| Player | `; / .` | Volume +5% / −5% |
| Player | O | Alternar perfil de saída |
| Player | S / R / T / V | Aleatório / repetição / timer / visualizador |
| Player | 7 / 8 / 9 / 0 | 0,75x / 1,25x / 1,75x / normal |
| Player | Tab ou E | Equalizador |
| Player | Delete ou ` | Voltar à biblioteca |

Na tela do equalizador, `, / /` seleciona a banda, `; / .` ajusta o ganho e `0` aplica FLAT. Pressione `H` para consultar a ajuda no dispositivo.

## Instalação

### Instalador USB pelo site

1. Abra [o instalador JOHNNY MP3](https://otavio-dias-leite.github.io/mp3player/) em Chrome ou Edge.
2. Conecte o Cardputer ADV por USB mantendo G0 pressionado para entrar no modo de gravação.
3. Escolha a porta serial e inicie a instalação.
4. Não desconecte o aparelho durante a gravação.

### M5Launcher

Use `johnny-mp3-cardputer-adv-1.0.0.bin`, disponível na [release v1.0.0](https://github.com/otavio-dias-leite/mp3player/releases/tag/v1.0.0). Ele é diferente da imagem completa `mp3-player-cardputer-adv-full.bin`. Consulte [docs/M5LAUNCHER.md](docs/M5LAUNCHER.md).

### Recuperação manual

A imagem completa `mp3-player-cardputer-adv-full.bin` é destinada à gravação USB de toda a memória flash, no endereço `0x0`, com esptool para ESP32-S3. Não selecione essa imagem como se fosse o arquivo de aplicação do M5Launcher.

## Compilação

```bash
pio run -e cardputer-adv
```

A versão vem de `VERSION`; `scripts/version.py` gera o cabeçalho usado pelo firmware. `src/main.cpp` é a unidade compilada e `MP3_Player.ino` é mantido sincronizado para Arduino IDE.

## Validação e licenças

O GitHub Actions compila o firmware, monta a imagem completa e publica os binários. Uma compilação bem-sucedida valida o build e a montagem dos artefatos; não substitui ensaios prolongados no Cardputer ADV físico. Consulte [docs/RELEASE-1.0.0.md](docs/RELEASE-1.0.0.md).

O código do projeto usa a licença MIT. O binário também incorpora ESP8266Audio, distribuído sob GPL-3.0; consulte [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

# JOHNNY — Cardputer-Adv

Firmware experimental para o M5Stack Cardputer-Adv (ESP32-S3), com identidade visual roxa e rosa, navegação pelo teclado e ferramentas locais.

## Módulos disponíveis

- **SYNTH:** sintetizador monofônico com notas QWERTYU, oitavas e efeitos.
- **TUNER:** afinador cromático usando o microfone interno.
- **MUSIC:** biblioteca e reprodução de arquivos WAV.
- **FILES:** explorador do cartão microSD.
- **WI-FI:** configuração e conexão a uma rede salva.
- **USB:** transferência de arquivos do microSD por USB serial, usando a página web do projeto.
- **SYSTEM:** informações básicas do dispositivo.

## Compilar

Requer PlatformIO:

```sh
pio run -e cardputer-adv
pio run -e cardputer-adv -t upload
pio device monitor
```

## Teclas

- Setas ou `, . / ;`: navegação, conforme a tela.
- Enter: selecionar/confirmar.
- Esc ou apóstrofo: voltar.
- No sintetizador: QWERTYU toca a escala; `-` e `+` ajustam volume; `[` e `]` ajustam oitava.

## Limitações conhecidas

- O afinador e o sintetizador dependem da validação de áudio no hardware real.
- A biblioteca de música reproduz arquivos WAV compatíveis; MP3 não está implementado.
- USB é transferência via serial com navegador compatível, não uma unidade USB MSC montada no sistema operacional.
- Não considere funções testadas fisicamente apenas porque o firmware compila.

Use os recursos de rádio e armazenamento somente em dispositivos e redes próprios ou com autorização.

## Licença

MIT — consulte `LICENSE`.

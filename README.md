# Johnny — Cardputer-Adv

Firmware experimental para o **M5Stack Cardputer-Adv**, com interface local e módulos de ferramentas nativas.

## Estado do projeto

Este repositório está sendo inicializado. O primeiro firmware fornece a estrutura de aplicação, interface no LCD, navegação pelo teclado e telas iniciais para afinador e bateria. Recursos de áudio dependem da validação do codec ES8311 e dos pinos I2S na revisão física do dispositivo.

## Hardware-alvo

- M5Stack Cardputer-Adv / ESP32-S3
- LCD ST7789V2 240×135
- Teclado 56 teclas via TCA8418
- Codec ES8311, microfone MEMS e alto-falante
- microSD e IMU BMI270

## Compilar

Requer PlatformIO (VS Code ou CLI).

```sh
pio run
pio run -t upload
pio device monitor
```

Configuração USB/serial e procedimento de gravação podem variar conforme cabo, sistema operacional e modo USB do ESP32-S3.

## Gravação pelo navegador

O objetivo é disponibilizar um instalador WebSerial/WebUSB hospedado por HTTPS, com firmware compilado e manifesto de versões. Esta etapa depende de um binário validado para o Cardputer-Adv; não grave imagens de firmware de outra revisão ou do Cardputer original.

## Teclas

- Setas: navegar
- Enter: selecionar
- Esc/Backspace: voltar

## Segurança e limitações

Use somente em hardware próprio e com autorização. Recursos de rádio ou periféricos que não existem no Cardputer-Adv não fazem parte do escopo. O afinador e a bateria serão considerados funcionais somente após testes reais de entrada/saída de áudio.

## Licença

MIT — consulte `LICENSE`.

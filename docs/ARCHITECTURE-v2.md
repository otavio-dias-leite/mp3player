# JOHNNY v2 — plano de arquitetura e critérios de entrega

Este documento governa a reestruturação. Compilação CI não equivale a teste no Cardputer ADV.

## Restrições do hardware
- Alvo: Cardputer ADV / ESP32-S3, Flash de 8 MB, sem PSRAM.
- UI não executa leitura longa do SD, rede, decoder, nem espera indefinida.
- SPI compartilhado: microSD CS GPIO12, SCK GPIO40, MISO GPIO39, MOSI GPIO14.
- Tela SPI externa deve usar CS separado (GPIO5 é candidato); GPIO12 permanece exclusivo do microSD. DC/RESET dependem do modelo exato da tela.
- I2C externo proposto: SDA GPIO8, SCL GPIO9; barramento pode ser compartilhado com periféricos da placa e precisa de arbitragem.
- LED RGB usa GPIO21; GPIO38 está relacionado à alimentação/iluminação da placa. Não oferecer pinos reservados como GPIO arbitrário.
- GPIO é lógica de 3,3 V; nunca conectar sinal de 5 V diretamente.

## Regras operacionais
1. Uma aplicação em primeiro plano por vez. Serviços essenciais têm ciclo de vida próprio.
2. Toda aplicação implementa begin/update/requestStop/stopped. Encerramento é cooperativo.
3. Não deletar à força uma tarefa que possa possuir mutex, arquivo, transação SPI, decoder ou buffer.
4. Reivindicações de recursos são atômicas e associadas a um dono.
5. Timeout obrigatório para rede, SD, mutexes e parada de processos.
6. Buffers e filas têm limites fixos. Falha por falta de memória/filas cheias é reportada sem bloquear a UI.
7. Alteração persistente usa escrita temporária, flush, validação e rename.
8. Nenhuma função promete uptime contínuo sem validar fonte de energia, RTC e wake sources.

## Estrutura-alvo
```
src/
  core/       AppManager, ResourceManager, ProcessRegistry, EventQueue, Watchdog
  storage/    SD/Flash managers, encrypted vault, atomic settings store
  system/     Clock/NTP, alarm, power, Wi-Fi, GPIO and board pin map
  audio/      Audio arbitration, MP3, radio, synth/pads, recorder, rhythm game
  ui/         Theme, icons, components, screensaver, navigation
  apps/       Music, disk, process, alarm, settings, GPIO and games
  main.cpp    hardware composition root and short event loop
```

## Persistência e segurança
- microSD será a persistência principal de Wi-Fi, configurações, alarmes, temas, presets, pads, playlists e dados de apps.
- Conteúdo e metadados protegidos com AES-256-GCM; nonce exclusivo por arquivo/operação e chave derivada de segredo do usuário via KDF revisada.
- Segredo mestre não pode ficar ao lado do ciphertext. A tela/fluxo de desbloqueio precisa ser implementado antes de ativar o cofre como único armazenamento.
- Escrita transacional: arquivo temporário -> flush -> leitura/autenticação -> rename. Manter backup até confirmar migração.
- Não fazer fallback silencioso para NVS em texto aberto. Migrar credenciais antigas somente após desbloqueio e validação; apagar NVS antigo somente depois.
- Flash física, partição de firmware e espaço real de filesystem devem aparecer separadamente.

## Processos e periféricos
- ResourceManager arbitra display, speaker, mic, SD, SPI, I2C, Wi-Fi, BLE e GPIO.
- AppManager bloqueia segunda aplicação enquanto a atual estiver parando.
- ProcessRegistry lista tarefas gerenciadas pelo JOHNNY, dono, estado, prioridade e stack high-water. Tarefas internas do FreeRTOS são somente leitura.
- Fechar tela envia pedido de parada, aguarda confirmação com limite, fecha arquivos/conexões e libera recursos.
- SPI e I2C devem ter um único gerente por barramento; SD e display externo nunca selecionam o barramento ao mesmo tempo.

## Relógio, alarmes e energia
- NTP quando conectado; timezone America/Sao_Paulo; millis para intervalos monotônicos.
- Após perda total de energia, hora confiável requer RTC ou nova sincronização NTP.
- Alarmes independentes da tela, com repetição, snooze e preempção do alto-falante. Mídia pode vir do microSD ou da Flash destinada a arquivos, nunca da partição de firmware.
- Mídia criptografada só toca com o cofre desbloqueado. Manter alarme após reboot sem desbloqueio exige estratégia de chave segura explícita.
- Wi-Fi/BLE/display/audio entram em repouso quando não utilizados. Light sleep só quando wake sources e serviços ativos permitirem; deep sleep será opcional, não padrão.
- Modo de carregamento automático depende de sinal de carregador confiável. Não inferir carregamento apenas pela porcentagem.

## Disco e GPIO
- Exibir capacidade física da Flash, tamanho das partições, espaço utilizável, usado/livre e microSD total/usado/livre.
- Cópia/movimentação é em blocos fixos; validar destino antes de apagar origem.
- Proteger bootloader, tabela de partições e firmware ativo contra exclusão.
- Pin manager consulta mapa da placa e bloqueia pinos de teclado, display, SD, áudio, LED e power.
- GPIO5 é CS candidato para tela SPI; definir DC/RESET de acordo com display. GPIO12 permanece CS do SD.
- I2C 8/9 requer arbitragem e varredura com timeout.

## UI e áudio
- Tema base graphite/roxo, texto lavanda, seleção rosa e sinalização ciano.
- Ícones próprios por primitivas; sem imagens grandes na RAM. Redesenho de regiões alteradas.
- Temas: JOHNNY Purple, Dark, Cyberpunk, Matrix, Amber e Minimal.
- AudioManager é o único dono do speaker. Alarm tem prioridade sobre MP3/radio/synth.
- MP3 por blocos com buffers fixos e stop cooperativo. Pads Z-X-C-V-B-N-M gravam/amostram som. Mapeamento de teclas musicais deve explicitar oitavas e acidentes.
- Rhythm game usa charts leves; arranjos incluídos devem ser domínio público ou fornecidos pelo usuário.

## Etapas e gates
A. UI/base visual e build modular — CI compila; revisão visual no hardware.
B. App/resource lifecycle — 100 ciclos de abrir/fechar; sem recursos presos.
C. Cofre criptográfico — desbloqueio, autenticidade, migração e recuperação após queda de energia.
D. Gerenciadores de disco/processos — contabilidade, cópia segura e inventário de tarefas gerenciadas.
E. Relógio/alarmes — NTP, offline, recorrência, snooze, seleção SD/Flash e preempção.
F. Energia/screensaver — consumo medido, carregador detectado e wake test.
G. Tema/LED/GPIO/display externo — conflitos de pinos e testes de SPI/I2C.
H. Áudio/synth/rhythm — MP3 prolongado, stop/pause/seek, gravação de pads e timing.
I. Release — CI, checksum, guia; depois teste físico de boot, áudio, SD, Wi-Fi, BLE e stress.

Nenhuma etapa é declarada concluída somente por compilar.

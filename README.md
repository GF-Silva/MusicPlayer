# MusicPlayer

Player de MP3 com `ESP32` que lê arquivos de um cartão `microSD`, decodifica em tempo real com `Helix MP3` e transmite o áudio via `Bluetooth Classic A2DP` para uma caixa de som, fone ou TWS.

O projeto foi organizado em módulos para separar bootstrap, Bluetooth, pipeline de áudio, playback, energia e entrada do usuário. Além do fluxo principal de reprodução, ele também trata reconexão automática, pré-buffer, recuperação de falhas de stream e `deep sleep`.

## Visão geral

Este firmware:

- monta o cartão `microSD` via `SDSPI`
- procura arquivos `.mp3` na raiz do cartão
- busca um dispositivo Bluetooth alvo por `MAC address`
- conecta como `A2DP Source`
- decodifica os frames MP3 com `esp-libhelix-mp3`
- envia PCM para um buffer circular e transmite o áudio por Bluetooth
- entra em `deep sleep` por botão ou por inatividade

## Funcionalidades

- Reprodução de arquivos MP3 direto do `microSD`
- Transmissão sem fio via `Bluetooth Classic A2DP Source`
- Reconexão automática quando a conexão cai
- Novo ciclo de discovery quando o Bluetooth fica preso ou falha repetidamente
- Pré-buffer antes de iniciar o stream para reduzir falhas
- Recuperação de `decode stall` avançando para outra faixa
- Seleção aleatória da próxima música
- Controle de volume por sequência de cliques no botão de power
- `Deep sleep` com wakeup pelo botão
- Estrutura modular em C com separação por responsabilidade

## Hardware usado

- `ESP32`
- Cartão `microSD`
- Módulo/leitor SD em modo `SPI`
- Caixa de som, fone ou TWS com `Bluetooth A2DP`
- 1 botão ligado ao pino de power/wakeup
- LED de status na placa

## Pinagem atual

Configuração definida em [`main/main.c`](/home/ferreira/Público/projetos/MusicPlayer/main/main.c):

- `MISO`: GPIO `19`
- `MOSI`: GPIO `23`
- `CLK`: GPIO `18`
- `CS`: GPIO `4`
- `POWER / WAKE`: GPIO `33`
- `LED`: GPIO `2`

## Controles

O controle é feito pelo botão conectado ao GPIO `33`:

- Segurar por aproximadamente `2s`: entra em `deep sleep`
- Clique duplo: aumenta o volume
- Clique triplo: diminui o volume

## Fluxo de funcionamento

1. O sistema inicia e configura wakeup/LED.
2. O cartão SD é montado em `/sdcard`.
3. Os arquivos `.mp3` da raiz do cartão são contados.
4. Os buffers de áudio e o decoder são preparados.
5. O ESP32 procura o dispositivo Bluetooth alvo.
6. Ao conectar, o stream A2DP é iniciado.
7. As músicas são tocadas e, ao terminar, a próxima faixa é escolhida de forma aleatória.

## Estrutura do projeto

```text
main/
  main.c
  module/
    audio/       # buffers, pipeline e callback A2DP
    bootstrap/   # montagem dos contextos e fachada do app
    bt/          # init BT, discovery, callbacks A2DP/AVRCP/GAP
    control/     # fila de comandos e tasks principais
    input/       # leitura do botão e volume
    playback/    # biblioteca de mídia e engine de decodificação
    power/       # LED, wakeup e deep sleep
    storage/     # montagem do cartão SD
components/
  esp-libhelix-mp3/
```

## Requisitos

- `ESP-IDF 5.4.2`
- Target `esp32`
- Toolchain configurado com `idf.py`

O projeto usa a dependência `esp-libhelix-mp3` já incluída no repositório em `components/`.

## Como compilar e gravar

```bash
idf.py set-target esp32
idf.py menuconfig
idf.py build
idf.py -p /dev/SEU_PORTA flash monitor
```

## Como usar

1. Coloque arquivos `.mp3` na raiz do cartão `microSD`.
2. Ajuste no código o `MAC` do dispositivo Bluetooth alvo.
3. Grave o firmware no `ESP32`.
4. Ligue o dispositivo.
5. Aguarde o discovery e a conexão Bluetooth.

## Configuração importante

O dispositivo Bluetooth alvo agora pode ser ajustado via `idf.py menuconfig`, no menu `MusicPlayer Configuration`.

Os campos mais importantes são:

- nome do dispositivo de referência
- `MAC address` do alvo

O projeto continua priorizando a conexão por `MAC address`. Antes de publicar, vale deixar claro no seu post qual caixa de som/TWS foi usada nos testes e qual `MAC` foi configurado.

## Configuração via menuconfig

As principais opções do projeto agora podem ser ajustadas em `idf.py menuconfig`:

- nome do dispositivo Bluetooth de referência
- `MAC address` do alvo
- GPIOs do SD, botão e LED
- volume inicial e passo de volume
- timeouts de sleep, discovery e recovery
- tamanhos dos buffers de áudio

As opções ficam no menu `MusicPlayer Configuration`.

## Comportamento atual e limitações

- Os arquivos `.mp3` são buscados somente na raiz de `/sdcard`
- A próxima faixa é escolhida de forma aleatória
- O projeto depende de um dispositivo Bluetooth específico configurado no código
- A compatibilidade de amostragem é validada para `44.1 kHz` e `48 kHz`
- O auto-sleep ocorre após aproximadamente `1 minuto` sem Bluetooth/áudio
- Não há interface gráfica, display ou menu local

## Arquivos importantes

- Entrada principal: [`main/main.c`](/home/ferreira/Público/projetos/MusicPlayer/main/main.c)
- Gerência Bluetooth: [`main/module/bt/bt_manager.c`](/home/ferreira/Público/projetos/MusicPlayer/main/module/bt/bt_manager.c)
- Callbacks Bluetooth: [`main/module/bt/bt_callbacks.c`](/home/ferreira/Público/projetos/MusicPlayer/main/module/bt/bt_callbacks.c)
- Engine de playback: [`main/module/playback/playback_engine.c`](/home/ferreira/Público/projetos/MusicPlayer/main/module/playback/playback_engine.c)
- Biblioteca de mídia: [`main/module/playback/media_library.c`](/home/ferreira/Público/projetos/MusicPlayer/main/module/playback/media_library.c)
- Entrada do usuário: [`main/module/input/input_manager.c`](/home/ferreira/Público/projetos/MusicPlayer/main/module/input/input_manager.c)
- Energia e sleep: [`main/module/power/sleep_manager.c`](/home/ferreira/Público/projetos/MusicPlayer/main/module/power/sleep_manager.c)

## Ideias para evolução

- Suporte a pastas e playlists
- Próxima/anterior faixa por botões dedicados
- Persistência de volume e última faixa
- Pareamento/configuração sem recompilar
- Indicadores visuais mais completos
- Testes automatizados para módulos puros

## Status

Projeto funcional e pronto para testes em hardware real.

Se for publicar no GitHub, Reddit e LinkedIn, adicionar fotos do protótipo, esquema de ligação e um pequeno vídeo do sistema tocando música vai aumentar bastante a credibilidade do projeto.

## Licença

Este repositório está sob a licença `MIT`. Veja [`LICENSE`](/home/ferreira/Público/projetos/MusicPlayer/LICENSE).

Dependências de terceiros mantêm suas próprias licenças. Em especial, o decoder incluído em `components/esp-libhelix-mp3/` possui arquivos de licença próprios dentro do componente.

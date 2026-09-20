# System Config Spec

## Objetivo

O firmware deve manter arquivos internos em uma pasta reservada do SD:

```text
/.system/config.json
/.system/errors.log
```

No caminho real do ESP32, isso fica dentro de `CONFIG_MUSICPLAYER_MOUNT_POINT`, por exemplo:

```text
/sdcard/.system/config.json
/sdcard/.system/errors.log
```

## Bootstrap

Depois que o SD monta, o firmware garante que `.system` exista.

Se `config.json` nao existir, ele cria um JSON default com:

- dispositivo BT: `TWS`;
- diretorio de musicas: `musics`;
- mount point fisico do SD: valor de `CONFIG_MUSICPLAYER_MOUNT_POINT`;
- valores atuais consumidos pelo firmware em Wi-Fi, audio, Bluetooth e runtime.

Se `errors.log` nao existir, ele cria um arquivo vazio.

## Config JSON

Formato default:

```json
{
  "schema": "musicplayer.config.v1",
  "bluetooth": {
    "device": "TWS",
    "target_mac": "41:42:78:A4:06:97",
    "discovery_timeout_sec": 12,
    "connecting_stuck_ms": 90000
  },
  "storage": {
    "sd_mount_point": "/sdcard",
    "mount_point": "musics"
  },
  "wifi": {
    "ssid": "ESP32-MusicPlayer",
    "password": "music1234",
    "channel": 6,
    "max_connections": 2
  },
  "audio": {
    "default_volume": 30,
    "volume_step": 5,
    "stream_buffer_size": 32278,
    "stream_low_watermark_pct": 30,
    "stream_high_watermark_pct": 60,
    "mp3_read_min": 1024,
    "mp3_read_max": 16384
  },
  "runtime": {
    "auto_sleep_idle_ms": 60000,
    "decode_stall_recovery_ms": 15000
  }
}
```

O diretório `.system` e reservado pelo firmware para os arquivos internos e nao faz parte da configuracao editavel. O campo `storage.mount_point` define onde as musicas ficam: se for relativo, ele e resolvido dentro de `storage.sd_mount_point`; se for absoluto, e usado diretamente.

Depois de reiniciar, o firmware consome os valores do arquivo para Wi-Fi AP, Bluetooth, volume, buffers de audio, timeouts de runtime e diretorio de musicas.

## GET /configs

Retorna o conteudo de `/.system/config.json` em stream.

```http
GET /configs
Accept: application/json
```

Resposta:

Retorna o JSON completo no formato descrito em `Config JSON`.

## PUT /configs

Substitui o arquivo de config por um JSON valido. A rota valida o arquivo antes de gravar:

- `400 Bad Request`: JSON ausente, grande demais ou com formatacao incorreta;
- `422 Unprocessable Entity`: JSON bem formado, mas com schema, estrutura, campo ausente, tipo ou valor de config invalido;
- `500 Internal Server Error`: falha interna ao receber ou salvar.

```http
PUT /configs
Content-Type: application/json

{ ...config JSON completo... }
```

Resposta:

```json
{"ok":true}
```

## PATCH /configs

Alias de atualizacao registrado para o frontend/API. No firmware atual, ele tem o mesmo comportamento de `PUT /configs`: valida o corpo como JSON e substitui o arquivo inteiro.

## GET /errors

Retorna `/.system/errors.log` como NDJSON em stream.

```http
GET /errors
Accept: application/x-ndjson
```

Cada linha segue:

```json
{"ts":0,"tag":"MP3Player","erro":"mensagem"}
```

## Frontend

A primeira tela mostra tres rotas de administracao:

- `Musicas`: consome `GET /musics`, `PUT /musics?nome=...` e `DELETE /musics?nome=...`;
- `Configs`: consome `GET /configs` e `PUT /configs`;
- `Erros`: consome `GET /errors`.

Uploads e remocoes de multiplas musicas sao executados pelo frontend como fila sequencial, sempre uma musica por request.

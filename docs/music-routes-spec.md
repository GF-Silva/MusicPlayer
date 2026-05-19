# Music Routes Spec

## Objetivo

As rotas de musica devem operar com baixo uso de RAM no ESP32:

- upload de uma musica por vez, gravando o corpo recebido em stream no SD;
- listagem das musicas em JSON, enviada em chunks;
- remocao de uma musica por vez.

Todas as rotas operam somente sobre arquivos `.mp3` na raiz do cartao SD montado em `CONFIG_MUSICPLAYER_MOUNT_POINT`.

## Regras de Nome

O nome do arquivo deve ser apenas o basename, sem diretorios.

Valido:

```text
minha musica.mp3
track_01.mp3
artist-song.mp3
```

Invalido:

```text
../track.mp3
pasta/track.mp3
track.wav
```

Caracteres aceitos: letras, numeros, espaco, `.`, `_` e `-`.

## PUT /musics

Grava ou substitui uma unica musica no SD.

O nome pode ser enviado por query string:

```http
PUT /musics?nome=track_01.mp3
Content-Type: audio/mpeg

<stream binario do arquivo>
```

Ou por header:

```http
X-Filename: track_01.mp3
```

Compatibilidade mantida:

```http
PUT /put-musics?nome=track_01.mp3
```

### Comportamento

1. valida o nome;
2. grava o corpo em `<nome>.part`;
3. ao finalizar, substitui `<nome>` por `<nome>.part` via `rename`;
4. responde somente depois que o stream termina.

### Resposta 200

```json
{"ok":true,"nome":"track_01.mp3","bytes":123456}
```

## GET /musics

Lista todas as musicas `.mp3` do SD.

```http
GET /musics
Accept: application/json
```

A resposta e enviada com `httpd_resp_send_chunk`, item por item.

### Resposta 200

```json
{
  "musicas": [
    {
      "logo": "data:image/jpeg;base64,...",
      "nome": "track_01.mp3",
      "author": "Nome do artista"
    }
  ]
}
```

Campos:

- `logo`: imagem APIC do ID3v2 como data URL, ou `null` se nao existir;
- `nome`: nome do arquivo no SD;
- `author`: frame ID3 `TPE1`, ou `null` se nao existir.

## DELETE /musics

Remove uma unica musica do SD.

Nome por query string:

```http
DELETE /musics?nome=track_01.mp3
```

Nome por header:

```http
X-Filename: track_01.mp3
```

Nome por JSON pequeno:

```http
DELETE /musics
Content-Type: application/json

{"nome":"track_01.mp3"}
```

### Resposta 200

```json
{"ok":true,"nome":"track_01.mp3"}
```

## Erros

As rotas retornam JSON simples:

```json
{"erro":"mensagem"}
```

Codigos usados:

- `400`: nome ausente ou invalido;
- `404`: musica nao encontrada no DELETE;
- `500`: falha de acesso ao SD.

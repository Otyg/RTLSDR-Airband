# File Command TCP Protocol (v1)

Detta dokument beskriver kommandoprotokollet för `file_cmd_tcp_server` i RTLSDR-Airband.

Protokollet används för att:
- lista inspelade filer
- starta uppspelning av inspelad fil via befintlig `udp_stream`
- stoppa pågående uppspelning

## Transport
- TCP
- Textbaserat, en rad per kommando
- Radslut: `\n` (CRLF fungerar också)
- Kommandon är case-insensitive

## Serverhälsning
Vid anslutning skickar servern:

```text
OK file_cmd_tcp_server ready
OK COMMANDS HELP PING LIST_FILES PLAY_FILE LOOP_FILE STOP_PLAYBACK
```

## Kommandon

### `HELP`
Visar tillgängliga kommandon.

Exempel svar:

```text
OK COMMANDS HELP PING LIST_FILES PLAY_FILE LOOP_FILE STOP_PLAYBACK
```

### `PING`
Liveness-check.

Exempel svar:

```text
PONG
```

### `LIST_FILES`
Listar filer rekursivt i kanalens konfigurerade inspelningskataloger
(`file`, `flacfile`, `rawfile` outputs).

Svarformat:
- Första rad: `OK LIST_FILES <count>`
- Därefter `<count>` rader med absoluta sökvägar
- Sista rad: `.`

Exempel:

```text
OK LIST_FILES 2
/home/pi/recordings/2026-04-22/TWR_001.mp3
/home/pi/recordings/2026-04-22/TWR_002.flac
.
```

### `PLAY_FILE <absolute-path>`
Spelar en `.mp3` eller `.flac` en gång över kanalens befintliga `udp_stream`-destination.

Exempel svar vid start:

```text
OK PLAY_FILE STARTED
```

Exempel svar när filen spelats klart:

```text
OK PLAYBACK FINISHED
```

### `LOOP_FILE <absolute-path>`
Som `PLAY_FILE`, men filen startar om automatiskt tills `STOP_PLAYBACK` skickas.

Exempel svar vid start:

```text
OK LOOP_FILE STARTED
```

### `STOP_PLAYBACK`
Stoppar aktiv `PLAY_FILE` eller `LOOP_FILE`.

Exempel svar:

```text
OK PLAYBACK STOPPED
```

Om inget spelas:

```text
OK PLAYBACK NOT_ACTIVE
```

## Fel
Exempel på felsvar:

```text
ERR unknown command
ERR missing file path
ERR file not found
ERR file outside configured recording directories
ERR unsupported file extension (use .mp3 or .flac)
ERR no udp_stream output configured on this channel
ERR udp_stream output is not connected
ERR failed to start decoder (ffmpeg required)
ERR playback failed
ERR playback loop restart failed
```

## Viktiga beteenden
- Under filuppspelning pausas live-audio till samma `udp_stream`-output.
- `PLAY_FILE`/`LOOP_FILE` tillåter bara filer under kanalens inspelningskataloger.
- Avkodning sker via `ffmpeg` i runtime-miljön.

## Snabbtest med netcat

```bash
nc <airband-host> 9002
```

Exempelkommandon:

```text
PING
LIST_FILES
PLAY_FILE /home/pi/recordings/2026-04-22/TWR_001.mp3
STOP_PLAYBACK
LOOP_FILE /home/pi/recordings/2026-04-22/TWR_002.flac
STOP_PLAYBACK
```

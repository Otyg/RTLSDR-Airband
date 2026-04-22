# File Command TCP Server Protocol (v1)

`file_cmd_tcp_server` provides a simple line-based TCP protocol.

Connection:
- Server listens on `bind_address:bind_port`
- On connect, server sends: `OK file_cmd_tcp_server ready`

Commands (newline-terminated):
- `HELP` -> returns available commands
- `PING` -> returns `PONG`
- `LIST_FILES` -> recursively lists files from all configured `file`, `flacfile`, and `rawfile` output directories on the same channel
- `PLAY_FILE <absolute-path>` -> plays a `.mp3` or `.flac` file over the existing `udp_stream` destination configured on the same channel
- `LOOP_FILE <absolute-path>` -> same as `PLAY_FILE` but restarts automatically until stopped
- `STOP_PLAYBACK` -> stops current file playback

`LIST_FILES` response format:
- First line: `OK LIST_FILES <count>`
- Then `<count>` lines, one absolute file path per line
- Final line: `.`

Notes:
- Unknown commands return `ERR unknown command`
- Command matching is case-insensitive
- Date-based subdirectories are traversed recursively
- Playback decoding uses `ffmpeg` (must be present in runtime PATH)
- While `PLAY_FILE` is active, live audio for the same `udp_stream` output is temporarily paused

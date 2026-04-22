# qt_scanner_remote

Minimal Qt6 GUI client for RTLSDR-Airband scanner remote mode.

## Features (v1)
- Connects to backend over TCP
- Receives audio as float32 mono (`tcp_stream_server` output)
- Receives scanner metadata as JSON (`scan_meta_tcp_server` output)
- Plays audio locally (Qt push output mode)
- Displays active frequency, label, squelch state and sequence counter
 - Shows signal level, waveform, and waterfall

## Build

```bash
cmake -S . -B build
cmake --build build -j
```

## Run

```bash
./qt_scanner_remote
```

Default connection:
- Backend host: `127.0.0.1`
- Audio: `9000`
- Metadata: `9001`

## Expected metadata payload
See [`docs/scan_meta_protocol.md`](../docs/scan_meta_protocol.md).

## File Command Protocol
See [`file_cmd_tcp_protocol.md`](./file_cmd_tcp_protocol.md).

## WSL/WSLg Audio Notes
If audio is silent in WSL but signal meters move in the GUI:

```bash
sudo apt update
sudo apt install -y pulseaudio-utils libasound2-plugins alsa-utils
```

Set `~/.asoundrc`:

```conf
pcm.!default {
  type pulse
  fallback "sysdefault"
  hint.description "PulseAudio"
}
ctl.!default {
  type pulse
}
```

Set Pulse server for WSLg:

```bash
export PULSE_SERVER=unix:/mnt/wslg/PulseServer
pactl info
```

Quick sound test:

```bash
speaker-test -D pulse -t sine -f 1000 -l 1
```

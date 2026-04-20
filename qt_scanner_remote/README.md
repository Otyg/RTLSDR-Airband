# qt_scanner_remote

Minimal Qt6 GUI client for RTLSDR-Airband scanner remote mode.

## Features (v1)
- Connects to backend over TCP
- Receives audio as float32 mono (`tcp_stream_server` output)
- Receives scanner metadata as JSON (`scan_meta_tcp_server` output)
- Plays audio locally
- Displays active frequency, label, squelch state and sequence counter

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

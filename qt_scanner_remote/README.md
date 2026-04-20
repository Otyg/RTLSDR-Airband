# qt_scanner_remote

Minimal Qt6 GUI client for RTLSDR-Airband scanner remote mode.

## Features (v1)
- Receives audio as UDP float32 mono (`udp_stream` output)
- Receives scanner metadata as UDP JSON (`scan_meta_udp` output)
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

Default ports:
- Audio: `9000`
- Metadata: `9001`

## Expected metadata payload
See [`docs/scan_meta_protocol.md`](../docs/scan_meta_protocol.md).

# Scan Metadata UDP Protocol (v1)

Each UDP datagram contains one UTF-8 JSON object plus a trailing newline.

Example:
`{"v":1,"seq":42,"device":0,"freq_hz":118150000,"squelch_open":true,"label":"Tower"}`

Fields:
- `v` (int): protocol version, currently `1`
- `seq` (int): sender sequence counter, starts at `0`
- `device` (int): device index in RTLSDR-Airband (`-1` when unknown)
- `freq_hz` (int): active frequency in Hz
- `squelch_open` (bool): current squelch state
- `label` (string): label from config, empty string when undefined

Notes:
- Metadata is emitted when scanner activity causes frequency metadata changes.
- If `continuous = true` is set on `scan_meta_udp`, metadata is emitted continuously.

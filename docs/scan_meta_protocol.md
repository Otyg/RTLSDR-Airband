# Scan Metadata UDP/TCP Protocol (v1)

Each UDP datagram / TCP line contains one UTF-8 JSON object plus a trailing newline.

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

## Decoded digital messages

For channels using `modulation = "ais"` or `modulation = "dsc"`, the same endpoint can also emit:

Example:
`{"v":1,"seq":99,"event":"decoded","device":0,"freq_hz":161975000,"label":"AIS 1","modulation":"ais","msg_type":"ais_frame","crc_ok":true,"mmsi":265123456,"payload":"010203..."}`

Fields for decoded events:
- `event` (string): `"decoded"`
- `modulation` (string): `"ais"` or `"dsc"`
- `msg_type` (string): decoder message type (`"ais_frame"` / `"dsc_message"`)
- `crc_ok` (bool): CRC status (`true` for validated AIS frame CRC)
- `mmsi` (int): decoded MMSI when available, else `-1`
- `payload` (string): decoder payload
  - AIS: JSON string with decoded payload fields (always includes `type`, `mmsi`, `raw_hex`)
    - Common decoded fields for supported message types:
      - type 1/2/3: `nav_status`, `rot`, `sog_kn`, `lon`, `lat`, `cog_deg`, `heading`, `timestamp`
      - type 18/19: `sog_kn`, `lon`, `lat`, `cog_deg`, `heading`, `timestamp`
      - type 5: `imo`, `callsign`, `shipname`, `ship_type`, dimensions (`dim_to_*`)
      - type 24: `part_no`, plus vessel identity fields for part A/B when present
  - DSC: JSON string with parsed fields, including:
    - `format_code`, `format`
    - `address` (when present)
    - `category_code`, `category`
    - `self_id`
    - `telecommand1_code`, `telecommand1`
    - `telecommand2_code`, `telecommand2`
    - `eos_code`, `eos`, `ecc`
    - `message_digits` (non-distress calls when available)
    - distress calls additionally include `distress_nature_code`, `distress_nature`, and when present `distress_position`, `distress_utc`

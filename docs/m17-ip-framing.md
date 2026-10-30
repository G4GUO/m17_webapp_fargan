# M17 over IP framing (the `_net` routines)

M17 has **native IP framing** — it was designed so that a radio can talk to a
reflector over UDP without a hotspot or any RF↔IP bridge box. These routines are
that bridge: they convert between the frames the station puts on air and the
frames a reflector expects.

If you are reading the code and wondering what `build_lich_to_net()`,
`m17_fmt_add_stream_frame_fm_net()` or `m17_net_new_rx_data()` are for, it is
this: the same LSF and stream frames as on air, wrapped in a magic header with a
stream ID, and with a CRC over the whole datagram.

## The two directions

    received from air ──► m17_net_new_rx_data() ──► UDP to reflector
                         (rewrites dst to the reflector, adds magic/SID/CRC)

    received from reflector ──► m17_parse_m17_data() ──► m17_fmt_add_*_fm_net()
                                 ──► dibits ──► modulator ──► air

## Frame layout as implemented (54 bytes)

This is what the code actually sends and expects, confirmed against
`net_add_magic()`, `net_add_stream_id()`, `net_add_lich()`, `net_add_fn()`,
`net_add_payload()` and `net_add_crc()` in `m17_net.cpp`.

| bytes | field | notes |
|---|---|---|
| 0–3 | magic | ASCII `"M17 "` = `0x4D 0x31 0x37 0x20`, big endian |
| 4–5 | stream ID (SID) | the frame ID, one value per transmission |
| 6–33 | LSF **contents**, 28 bytes | dst 6–11, src 12–17, type 18–19, META 20–33 |
| 34–35 | FN | frame number; bit 15 set on the last frame |
| 36–51 | payload | 16 bytes, exactly as carried in an RF stream frame |
| 52–53 | CRC16 | over bytes 0–51 (`m17_crc_array_encode(m_tx_buf, 52)`) |

Everything is big endian, matching the document.

## Where this differs from the current specification

The published M17 IP specification describes a **56-byte** frame: its LICH field
is 240 bits, i.e. the 30-byte LSF *including* the LSF's own CRC16. This
implementation carries only the 28 bytes of LSF contents and omits that CRC, so
every field after byte 33 sits two bytes earlier and the length is 54.

Consequences:

* It works with the reflector this station uses, which presumably also uses the
  54-byte form (the older layout, still common).
* A reflector built strictly to the current document would send 56 bytes with the
  LSF CRC present. The magic would match and the connection would come up, but
  FN, payload and the length check would all be misaligned — so the symptom would
  be "connected but garbage frames" rather than a clean rejection.
* If M17 IP interop ever becomes a requirement, this is the first thing to check,
  and the fix is to carry the 30-byte LICH and extend the frame to 56.

## Function map

| function | file | purpose |
|---|---|---|
| `build_lich_to_net()` | `m17_tx_routines.cpp` | pack dst/src/type/META into the IP frame's LSF field |
| `build_lich_from_net()` | `m17_tx_routines.cpp` | read that field back out of a received IP frame |
| `m17_fmt_add_link_setup_frame_fm_net()` | `m17_tx_routines.cpp` | IP frame → air LSF frame (192 dibits) |
| `m17_fmt_add_stream_frame_fm_net()` | `m17_tx_routines.cpp` | IP frame → air stream frame |
| `m17_send_*_to_net()` | `m17_tx_routines.cpp` | air → IP (uplink) |
| `m17_send_*_fm_net()` | `m17_tx_routines.cpp` | IP → air (downlink) |
| `m17_net_new_rx_data()` | `m17_net.cpp` | assemble a 54-byte datagram from a received air frame and send it |
| `m17_parse_m17_data()` | `m17_net.cpp` | dispatch an incoming `"M17 "` datagram |
| `net_fmt_conn/ackn/nack/ping/pong/disc` | `m17_net.cpp` | the control packets |

## Behaviours worth remembering

* **The destination is rewritten on the way up.** `m17_net_new_rx_data()`
  replaces the LSF destination with the reflector's own address
  (`<name> <module>`, e.g. `M17-M17 C`) before sending, because that is who the
  datagram is addressed to. The original destination is not carried.
* **The SID is the frame ID**, so it is per transmission rather than per PTT
  press in the way the document describes.
* **The packet CRC must be recomputed** whenever a frame is translated between RF
  and IP — the air frame's LSF CRC does not cover the IP datagram. `net_add_crc()`
  does this over the 52 bytes preceding it.
* Control packets are a 4-byte ASCII magic plus, where applicable, a 6-byte
  callsign encoded per the address encoding: `CONN` (with module byte), `ACKN`,
  `NACK`, `PING`, `PONG`, `DISC`.

## Known rough edges (not fixed)

* `m17_net_new_rx_data()` does `memcpy(lh, lich, 54)` from a buffer that is only
  30 bytes (`m_lsf[1]`). Only the first 28 bytes are used by `net_add_lich()`, so
  the extra 26 are dead — but they are read out of bounds of `m_lsf`.
* The 54/56 byte discrepancy above.

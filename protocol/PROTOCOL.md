# WaddleOS mesh protocol, version 1

One small binary packet format for LoRa and Wi-Fi, so the units, the gateway
and the Pi server share one parser. The C++ code is `lib/WaddleMesh/`; the
Python code is `server/waddlemesh/packet.py`. Both are tested against
`protocol/vectors.json`.

All numbers are little-endian. A packet is at most **200 bytes** (LoRa
allows 255; the margin keeps airtime short).

## Members

A group has up to 3 members, numbered 1 to 3 at setup. Number 0 is the server
(it speaks through the gateway or over Wi-Fi). `0xFF` as a recipient means
the whole group. Each group has a random 1-byte group id; units drop packets
from other groups without reading them.

## Header (11 bytes, never encrypted)

| Byte | Field | Notes |
|---|---|---|
| 0 | magic | `0x57` ("W") |
| 1 | version and type | high 4 bits = 1, low 4 bits = type (below) |
| 2 | group id | set at setup |
| 3 | hops | low 4 bits: hops left (starts at 3); bit 4: fragmented |
| 4 | sender | 1–3, or 0 = server |
| 5 | recipient | 1–3, 0 = server, `0xFF` = group |
| 6–9 | counter | per sender, see below |
| 10 | fragment | high 4 bits: index, low 4 bits: count (1 when not split) |

Message id = (sender, counter). It is unique for ever: a unit never reuses a
counter. It saves a reserved counter range to flash before using it, so a
crash or reboot skips ahead instead of repeating.

## Types

| Type | Name | Body | Encrypted |
|---|---|---|---|
| 1 | MSG | time, previous hash, text | yes |
| 2 | ACK | acked sender, acked counter, state | no |
| 3 | SYNC_REQ | last MSG counter per member, battery, firmware build | no |
| 4 | POS | position | yes (group) |
| 5 | SOS | position | yes (group) |

There is no SYNC_RESP packet: whoever answers a SYNC_REQ (the server or another
unit) sends the stored MSG packets again, unchanged, oldest first.

### MSG plaintext

| Bytes | Field |
|---|---|
| 0–3 | time, unix seconds (u32) |
| 4–7 | previous hash: first 4 bytes of BLAKE2b-256 of the sender's previous MSG plaintext in the same conversation, zero for the first |
| 8– | text, UTF-8, at most 1000 bytes |

A receiver that has a different previous message from that sender in that
conversation shows a warning: a message is missing or was changed.

### POS and SOS plaintext (16 bytes)

| Bytes | Field |
|---|---|
| 0–3 | fix time, unix seconds |
| 4–7 | latitude, degrees × 10⁷ (i32) |
| 8–11 | longitude, degrees × 10⁷ (i32) |
| 12–13 | altitude, metres (i16) |
| 14 | battery % |
| 15 | flags: bit 0 = no fix (position is the last known one); bit 1 = SOS cancelled |

### ACK body (6 bytes)

acked sender (u8), acked counter (u32), state (u8): 1 = delivered to the unit,
2 = stored on the server, 3 = read. ACKs carry no content and are not
encrypted, so the server (which has no keys) can send them.

A unit sends "delivered" when a message for it arrives and "read" when it is
shown. The server sends "stored" when it has every piece of a message, and again
for a unit's 10 newest stored messages when that unit syncs. Units send ACKs
with hops 3 like other packets.

### SYNC_REQ body (14 bytes)

last MSG counter the sender has from member 1, 2 and 3 (3 × u32), battery %
(u8), firmware build (first byte of the short git hash). The answer is every
stored MSG from member *n* with a counter above the given one.

A unit in SOS sends an SOS packet every 60 seconds until the user cancels it;
cancelling sends one more SOS with the "cancelled" bit set.

Every SOS is answered: a unit that receives one sends an ACK "delivered" for that
SOS packet's counter, and "read" when its user taps "Sett" on the alert; the
server answers "stored". The sender's SOS screen shows who has it and who has
seen it.

## Encryption (MSG, POS, SOS)

ChaCha20-Poly1305 (RFC 8439), 256-bit key, 16-byte tag after the ciphertext.

- **Key:** group packets use the sender's own *sender key* (32 random bytes,
  made by that unit and given to the other members at setup). DMs use the pair
  key: BLAKE2b-256 of `X25519(my secret, their public) || lower public key ||
  higher public key`, the same in both directions.
- **Nonce (12 bytes):** sender, recipient, counter (u32), type, then 5 zero
  bytes. Unique because the counter is.
- **Associated data:** header bytes 0–2 and 4–9 (everything except hops and the
  fragment byte, which change in transit or per piece).

A message too long for one packet is encrypted once, then the ciphertext and tag
are cut into pieces of at most 189 bytes. Every piece has the same header except
the fragment byte, at most 15 pieces. Receivers wait for all pieces, join them
and then decrypt.

The server stores packets exactly as received and can read none of them.

## Relaying

A unit that hears a packet it has not seen (by type, sender, counter and
fragment) and whose hops left is above 0 sends it again with hops − 1 after a
random delay of 0.5–2.5 s, unless it heard another unit relay it first. Units
never relay their own packets.

## Airtime

EU868 sub-band 869.4–869.65 MHz allows 10 % airtime. Units use 869.525 MHz,
bandwidth 250 kHz, spreading factor 10, coding rate 4/5, 14 dBm. Each unit keeps
its own airtime under 6 % per rolling hour and queues what does not fit.

## Server over Wi-Fi (HTTPS)

`POST /api/packets` with the unit's key as `Authorization: Bearer <hex>`. The
body and the answer are packets, each prefixed by its length (u8). The unit
sends its outgoing packets and a SYNC_REQ; the answer holds ACKs and the
packets it missed.

## Gateway serial link (115200 baud, text lines)

- Gateway → Pi: `RX <rssi> <snr> <hex packet>`
- Pi → gateway: `TX <hex packet>`, answered with `OK` or `ERR <reason>`
- Gateway → Pi on start: `HELLO waddle-gateway <version>`

# WaddleOS mesh protocol, version 1

One small binary packet format for LoRa and Wi-Fi, so the units, the gateway
and the Pi server share one parser. The C++ code is `lib/WaddleMesh/`; the
Python code is `server/waddlemesh/packet.py`. Both are tested against
`protocol/vectors.json`.

All numbers are little-endian. A packet is at most **200 bytes** (LoRa
allows 255; the margin keeps airtime short).

## Members

A group has up to 4 members, numbered 1 to 4 at setup. Number 0 is the server
(it speaks through the gateway or over Wi-Fi). `0xFF` as a recipient means
the whole group. `0x80` + a group id means a *group made on a unit* (see
"Groups"). Each mesh has a random 1-byte mesh id (the "group id" byte below,
set at setup); units drop packets from other meshes without reading them.

## Header (12 bytes, never encrypted)

| Byte | Field | Notes |
|---|---|---|
| 0 | magic | `0x57` ("W") |
| 1 | version and type | high 4 bits = 1, low 4 bits = type (below) |
| 2 | group id | set at setup |
| 3 | hops | low 4 bits: hops left (starts at 3); bit 4: fragmented |
| 4 | sender | 1–4, or 0 = server |
| 5 | recipient | 1–4, 0 = server, `0xFF` = everyone, `0x80` + group id = a group |
| 6–9 | counter | per sender, see below |
| 10 | fragment | high 4 bits: index, low 4 bits: count (1 when not split) |
| 11 | key number | a group's key generation (1, 2, …), 0 for everything else |

Message id = (sender, counter). It is unique for ever: a unit never reuses a
counter. It saves a reserved counter range to flash before using it, so a
crash or reboot skips ahead instead of repeating.

## Types

| Type | Name | Body | Encrypted |
|---|---|---|---|
| 1 | MSG | time, previous hash, text | yes |
| 2 | ACK | acked sender, acked counter, state | no |
| 3 | SYNC_REQ | last MSG counter per member, battery, firmware build, flags | no |
| 4 | POS | position | yes (group) |
| 5 | SOS | position | yes (group) |
| 6 | FETCH | sender, after counter, before counter | no |

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

### SYNC_REQ body (19 bytes)

last MSG counter the sender has from member 1, 2, 3 and 4 (4 × u32), battery %
(u8), firmware build (first byte of the short git hash), flags (u8: bit 0 = "På
tur", bit 1 = its SOS is on, bits 2–4 = how often it checks, for the server's
status page). The answer is every stored MSG from member *n* with a counter
above the given one, except DMs between two others. Group messages go to
everyone; only their members can open them.

### FETCH body (9 bytes)

sender (u8), after (u32), before (u32): "send me sender's messages with a
counter between these two". A unit sends it when the chain (MSG "previous hash")
shows a message is missing: it has the sender's message *after* in that
conversation and just got *before*, whose previous hash is not *after*'s. The
server and other units answer like a SYNC_REQ, with only that range.

A unit in SOS sends an SOS packet every 60 seconds until the user cancels it;
cancelling sends one more SOS with the "cancelled" bit set.

Every SOS is answered: a unit that receives one sends an ACK "delivered" for that
SOS packet's counter, and "read" when its user taps "Sett" on the alert; the
server answers "stored". The sender's SOS screen shows who has it and who has
seen it.

## Encryption (MSG, POS, SOS)

ChaCha20-Poly1305 (RFC 8439), 256-bit key, 16-byte tag after the ciphertext.

- **Key:** packets to everyone use the sender's own *sender key* (32 random
  bytes, made by that unit and given to the other members at setup). Groups use
  the group key with the key number in the header (see "Groups"). DMs use the
  pair key: BLAKE2b-256 of `X25519(my secret, their public) || lower public key ||
  higher public key`, the same in both directions.
- **Nonce (12 bytes):** sender, recipient, counter (u32), type, then 5 zero
  bytes. Unique because the counter is.
- **Associated data:** header bytes 0–2, 4–9 and 11 (everything except hops and
  the fragment byte, which change in transit or per piece).

A message too long for one packet is encrypted once, then the ciphertext and tag
are cut into pieces of at most 188 bytes. Every piece has the same header except
the fragment byte, at most 15 pieces. Receivers wait for all pieces, join them
and then decrypt.

The server stores packets exactly as received and can read none of them.

## Groups

Any member can make a group of some of the members, on their unit. There is no
accept step.

- **Id:** creator's member number (bits 4–6) and the creator's own count
  (bits 0–3), so two units never pick the same id. Recipient byte = `0x80 | id`.
- **Key:** the creator makes 32 random bytes, key number 1. It is sent to each
  member in an *invitation*: a DM (MSG to that member, pair key) whose text
  starts with byte `0x00` and then `G`, group id, key number, member bits (u8,
  bit 0 = member 1), the key (32 bytes) and the group's name (UTF-8). Units do
  not show such DMs as messages. Being a DM, it gets ACKs, retries and sync like
  any message.
- **Messages to the group:** MSG to `0x80 | id`, encrypted with the group key,
  header byte 11 = its key number. A unit keeps every key number it has had, so
  it can open older messages too. A message that arrives before its invitation
  is kept and opened when the key comes.
- **Leaving:** a member's unit sends each other member a DM `0x00 'L'` + group
  id, and forgets the group's new keys. The remaining member with the lowest
  number makes the next key number and sends it in invitations (with the new
  member bits) to those who remain. The one who left keeps what they already
  had, but cannot open anything new.
- **Removing** (a lost unit): any member chooses "Fjern"; their unit makes the
  next key number and invites the rest, as above. `waddle mesh setup` without a
  member does the same on every unit for each group that member was in.

## Relaying

A unit that hears a packet it has not seen (by type, sender, counter and
fragment) and whose hops left is above 0 sends it again with hops − 1 after a
random delay of 0.5–2.5 s, unless it heard another unit relay it first. Units
never relay their own packets. The gateway relays too, without the delay: it sits at
home with the best antenna, and units that hear its copy skip their own.

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
- Gateway → Pi on start: `HELLO waddle-gateway <version> <front end>`
- Pi → gateway every 10 s: `PING <group id>`, answered with
  `PONG <kept packets> <version> <front end>`. The gateway saves the group id.

**Standalone.** When the gateway hears no line from the Pi for 30 s (a power
cut; the gateway on a power bank) it carries on alone, and stops sending `RX`
lines. It keeps every MSG, POS, SOS and ACK packet it hears in flash (by type,
sender, counter and piece; 192 at most, the oldest makes room; of POS and SOS
only each member's newest), relays like a unit (no delay), sends ACK "stored"
for a whole MSG or an SOS, and answers SYNC_REQ and FETCH like the server: at
most 12 packets, hops 0, half a second apart. Its own packets use sender 0 with
counters from `0x80000001` up (saved ahead in flash), so they never meet the
server's.

**Handover.** When the Pi is back, the `PONG` shows kept packets. The Pi sends
`DUMP`; the gateway answers `STORED <hex packet>` for each, oldest first, then
`DUMPED <n>`. The Pi stores them (not relayed or answered again: that was done)
and, when all *n* came, sends `CLEAR`; the gateway forgets them and answers
`OK`. When some are missing the Pi does not clear, and asks again at the next
`PONG`.

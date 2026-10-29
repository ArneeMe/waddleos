# WaddleOS mesh protocol, version 3

One small binary packet format for LoRa and Wi-Fi, so the units, the gateway
and the Pi server share one parser. The C++ code is `lib/WaddleMesh/`; the
Python code is `server/waddlemesh/packet.py`. Both are tested against
`protocol/vectors.json`.

All numbers are little-endian. A packet is at most **200 bytes** (LoRa
allows 255; the margin keeps airtime short).

## Members and the network

A *network* has up to **32 members**, numbered 1 to 32, and one *admin* (Arne)
who lets people in. Number 0 is the server (it speaks through the gateway or
over Wi-Fi). Each network has a random 1-byte network id; units drop packets
from other networks without reading them.

Who is in the network is the **roster** (below): a list signed by the admin.
Units, the server and the tools trust a roster only with the admin's signature,
and only a newer one than they have.

## Header (14 bytes, never encrypted)

| Byte | Field | Notes |
|---|---|---|
| 0 | magic | `0x57` ("W") |
| 1 | version and type | high 4 bits = 3, low 4 bits = type (below) |
| 2 | network id | set when the network is made |
| 3 | hops | low 4 bits: hops left (starts at 3); bit 4: fragmented |
| 4 | sender | 1–32, or 0 = server |
| 5 | recipient | 1–32, 0 = server, `0xFE` = a group (bytes 11–12), `0xFF` = everyone |
| 6–9 | counter | per sender, see below |
| 10 | fragment | high 4 bits: index, low 4 bits: count (1 when not split) |
| 11–12 | group id | u16, for recipient `0xFE`: creator's number × 256 + the creator's count; else 0 |
| 13 | key number | which of the sender's keys (to everyone, POS, SOS) or the group's keys (group) sealed it; else 0 |

Message id = (sender, counter). It is unique for ever: a unit never reuses a
counter. It saves a reserved counter range to flash before using it, so a
crash or reboot skips ahead instead of repeating. A unit's counters start above
`counterFloor` (unix seconds when it joined), so a new unit that gets a former
member's number does not repeat their counters.

Packets of another version are dropped, never stored.

## Types

| Type | Name | Body | Encrypted |
|---|---|---|---|
| 1 | MSG | time, previous hash, text | yes |
| 2 | ACK | acked sender, acked counter, state | no |
| 3 | SYNC_REQ | battery, build, flags, roster version, last counter per member | no |
| 4 | POS | position | yes (sender key) |
| 5 | SOS | position | yes (sender key) |
| 6 | FETCH | sender, after counter, before counter | no |
| 7 | ROSTER | the signed roster | yes (network key) |

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

### SYNC_REQ body (5 + 5 × n bytes)

battery % (u8), firmware build (first byte of the short git hash), flags (u8:
bit 0 = "På tur", bit 1 = its SOS is on, bits 2–4 = how often it checks, for the
server's status page), roster version (u16), then for each member it has
messages from: member (u8) and the last MSG counter it has from them (u32). The
answer is every stored MSG with a higher counter (0 for members not listed),
except DMs between two others, and the newest roster as ROSTER packets when the
asker's version is older. Group messages go to everyone; only their members can
open them.

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

## Encryption (MSG, POS, SOS, ROSTER)

ChaCha20-Poly1305 (RFC 8439), 256-bit key, 16-byte tag after the ciphertext.

- **Keys:**
  - *To everyone, POS, SOS:* the sender's own **sender key** (32 random bytes,
    made by that unit, key number in byte 13). The unit gives it to every member
    in a DM (`0x00 'S'`, below). A unit keeps the last 3 sender keys of each
    member, so older messages still open after a member made a new one.
  - *Groups:* the group key, key number in byte 13 (see "Groups").
  - *DMs:* the pair key: BLAKE2b-256 of `X25519(my secret, their public) ||
    lower public key || higher public key`, the same in both directions. Public
    keys come from the roster.
  - *ROSTER:* the network key (32 random bytes made with the network, given to
    each member when they join).
- **Nonce (12 bytes):** sender, recipient, counter (u32), type, then 5 zero
  bytes. Unique because the counter is.
- **Associated data:** header bytes 0–2, 4–9 and 11–13 (everything except hops
  and the fragment byte, which change in transit or per piece).

A message too long for one packet is encrypted once, then the ciphertext and tag
are cut into pieces of at most 186 bytes. Every piece has the same header except
the fragment byte, at most 15 pieces. Receivers wait for all pieces, join them
and then decrypt.

The server stores packets exactly as received and can read none of them.

## Control DMs

A DM whose text starts with byte `0x00` is not shown as a message: it changes
keys or groups. Being a DM, it gets ACKs, retries and sync like any message; the
sender also keeps invitations and notices in flash and sends them again at each
check until that member's ACK comes, also after a restart.

| Text | Meaning |
|---|---|
| `0x00 'S'` key number, key (32) | my sender key |
| `0x00 'G'` group id (u16), key number, flags (bit 0 = a new group), member bits (u32), key (32), name | a group's key |
| `0x00 'L'` group id (u16) | I leave the group |
| `0x00 'R'` group id (u16), member | please take this member out (to the lowest remaining member) |

## The roster

A signed list of the members. Format (bytes):

| Field | Size |
|---|---|
| format (1) | 1 |
| network id | 1 |
| version (goes up by 1 with each change) | 2 |
| count | 1 |
| per member: number, flags (bit 0 = admin), name length, name (UTF-8, at most 20), X25519 public key (32), server token hash (first 16 bytes of SHA-256 of the token's hex text) | 3 + name + 48 |
| Ed25519 signature (RFC 8032) by the admin over everything above | 64 |

It travels to units as ROSTER packets (sender 0, recipient everyone, sealed with
the network key, in pieces), which the admin's tool makes and the server sends
when a unit's sync shows an older version, and over Wi-Fi as
`GET /api/roster` (the same signed bytes).

A unit takes a newer roster when the signature is the admin's:
- **Someone new:** it sends them its sender key (`'S'`). The newcomer sends
  theirs to everyone.
- **Someone gone:** it takes them out of every group (see "Groups"), makes a new
  sender key and sends it to everyone left, so the one who left cannot open new
  messages to everyone.

## Joining

The admin's key pair (Ed25519 for signing, X25519 for joining) and the network
key live on the admin's computer (`~/.config/waddle/`), not on a unit.

1. **Invitation** (`waddle mesh invite Ola` on the admin's computer): a link
   `waddle://join/<base64url>` with: network id (1), invitation id (8), a
   one-time password (16), the admin's X25519 public key (32), the SHA-256 of the
   Pi's certificate (32) and the server's address (UTF-8). Sent to Ola another
   way (Signal). The password makes it safe: only the holder of the link can
   join with it.
2. **Request** (`waddle mesh join <link>` on Ola's computer, his unit on File
   Transfer): the unit derives the join key **K = BLAKE2b-256(X25519(unit secret,
   admin public) || password || unit public || admin public)** and seals
   `{"name": …}` with it (nonce byte 3). The tool sends
   `POST /api/join {"invite", "from": unit public, "box"}` to the server, checking
   the server's certificate against the link.
3. **Approval** (`waddle mesh approve` on the admin's computer): the tool takes
   the waiting requests from the Pi (SSH), opens each with K and shows the name
   and six digits from both public keys (which Ola's unit shows too, to compare on
   the phone if wanted). On yes it gives Ola the lowest free number and a server
   token, signs a new roster, and puts on the server: the roster, its ROSTER
   packets, and a *welcome* sealed with K (nonce byte 4): network id, Ola's
   number, network key, the admin's public keys, server address, certificate and
   token, `counterFloor`, and the roster.
4. **Welcome**: Ola's tool fetches it (`GET /api/join/<invitation>`), gives it to
   the unit, which opens it with K, checks the roster's signature and starts.

An invitation is used once. The server keeps at most 20 waiting requests, each
at most 1 KB, and forgets them after 7 days.

**The pieces in detail**

- *Link:* `waddle://join/` + base64url (no padding) of: network id (1),
  invitation id (8), password (16), admin X25519 public key (32), SHA-256 of the
  server certificate in DER (32), server address (UTF-8, e.g.
  `https://home.example:8443`).
- *Unit, `POST /api/mesh/join`* (File Transfer web server; body sealed like
  setup: `{"tool", "box"}`, setup key, nonce byte 2) with `{"invite": link,
  "name": name}`. The unit keeps the invitation in flash and answers
  `{"from": its X25519 public hex, "box": hex, "code": "123 456"}`: `box` is
  `{"name": …}` sealed with K, nonce byte 3.
- *Server, `POST /api/join`* `{"invite": invitation id hex, "from", "box"}`; 409
  when that invitation already has a request.
- *Admin, on the Pi over SSH:* `python3 -m waddlemesh joins` prints the waiting
  requests as JSON; `python3 -m waddlemesh welcome <invitation id> <hex>` stores
  the sealed welcome; `python3 -m waddlemesh set-roster <roster hex> <ROSTER
  packet hex>...` checks the signature and that the version is newer, then keeps
  both.
- *Welcome* (sealed with K, nonce byte 4), JSON: `network`, `me`, `name`,
  `networkKey` (hex), `adminSign` (Ed25519 public hex), `adminBox` (X25519
  public hex), `server` (`url`, `cert` PEM, `token`), `counterFloor`, `roster`
  (signed roster hex).
- *Server, `GET /api/join/<invitation id>`*: `{"box": hex}` once approved (then
  forgotten), 404 before.
- *Unit, `POST /api/mesh/welcome`* `{"box": hex}`: opens it with the invitation
  it keeps, checks the roster, saves everything in flash and starts.
- *ROSTER packets:* made by the admin's tool: sender 0, recipient everyone,
  counter `0x40000000 + roster version`, type 7, sealed with the network key, in
  pieces like a long message. Units keep the newest; the server sends them on.

## Groups

Any member can make a group of some of the members, on their unit. There is no
accept step.

- **Id:** creator's member number × 256 + the creator's own count (u16), so two
  units never pick the same id. Recipient byte `0xFE`, id in header bytes 11–12.
- **Key:** the creator makes 32 random bytes, key number 1, and sends it to each
  member in a `'G'` DM (flag bit 0 marks a new group: start afresh).
- **Who may change a group:** only the remaining member with the lowest number
  makes a new key, so two never do at once. An invitation for a group a unit
  already has counts only when it comes from a member of that group and carries
  a newer key number than the unit holds; an older one only adds that old key
  (for history) and changes nothing else.
- **Messages to the group:** MSG to `0xFE`, encrypted with the group key, header
  byte 13 = its key number. A unit keeps every key number it has had (the last
  6), so it can open older messages too. A message that arrives before its
  invitation is kept and opened when the key comes.
- **Leaving:** `'L'` to each other member. The remaining member with the lowest
  number makes the next key number and invites those who remain. The one who
  left keeps what they already had, but cannot open anything new.
- **Removing** (a lost unit): any member chooses "Fjern"; their unit sends `'R'`
  to the lowest remaining member, who takes them out and makes the next key. A
  member gone from the roster is taken out the same way.
- **After someone is out:** units drop their group messages, except ones under an
  older key with a counter no higher than the newest one seen from them when
  they went out (late arrivals of what they wrote before).

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

`POST /api/packets` with the unit's token as `Authorization: Bearer <hex>` (the
server knows each member's token hash from the roster). The body and the answer
are packets, each prefixed by its length (u8). The unit sends its outgoing
packets and a SYNC_REQ; the answer holds ACKs, the packets it missed and, when
its roster is older, the ROSTER packets.

`GET /api/roster` (same authorization): the newest signed roster, as bytes.

Joining (no authorization; the sealed contents prove the invitation):
`POST /api/join` and `GET /api/join/<invitation id>` (404 until approved).

## Gateway serial link (115200 baud, text lines)

- Gateway → Pi: `RX <rssi> <snr> <hex packet>`
- Pi → gateway: `TX <hex packet>`, answered with `OK` or `ERR <reason>`
- Gateway → Pi on start: `HELLO waddle-gateway <version> <front end>`
- Pi → gateway every 10 s: `PING <network id>`, answered with
  `PONG <kept packets> <version> <front end>`. The gateway saves the network id.

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

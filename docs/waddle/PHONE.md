# The phone as a second device (plan, not built)

A member can have an Android app that is the same person as their T5. Not
built yet; this file says how it would work and lists the cases it has to
handle, so none are forgotten when it is built. Packet format today:
[protocol/PROTOCOL.md](../../protocol/PROTOCOL.md).

Everyone starts with a T5. The phone is an extra, never the only device.

## The idea in one paragraph

The phone gets a copy of the T5's identity once, at home. After that the two
never talk to each other directly: both read the same mailbox on the Pi, which
already keeps every packet and answers each device's sync on its own. Key
changes one device makes are also sent to its own number, so the other picks
them up from the Pi like any message. The phone reaches the network only through
the Pi (internet); off-grid there is only the T5.

## What "identity" means

A unit's identity is its secret key: 32 random bytes the T5 makes the first time
it starts and keeps in its flash. The roster holds only the public half, next to
the name and number. The secret is what makes Kari's unit "Kari": every DM key
is worked out from it and the other person's public key, the join key comes
from it, and the sealed files on the SD card are locked with a key made from it.
Name and number are labels; the secret is what opens things. Whoever has a copy
(the paired phone, or a thief) can open everything sent to that person.

A **new identity** means the T5 throws the secret away for sending, makes a fresh
one, and Arne approves a roster entry with the new public key. From then on, DMs
to Kari use keys the old secret cannot open.

## Rules

- **R1. Pairing, once.** At home, T5 on File Transfer, phone on the same Wi-Fi.
  Both show the same 6 digits (as when joining); the owner confirms on the T5. The T5
  sends, sealed: its identity secret, the network (network key, admin keys,
  roster, server address, certificate, token), its own sender key, the other
  members' sender keys, its group keys, and a counter floor (now, from the T5's
  clock). A T5 that is not in a network refuses.
- **R2. The device is part of the sender byte.** The T5 sends as its member
  number (1–32), the phone as member number + 128. Everything that counts
  messages uses the sender byte: the nonce (so the two never reuse one), the
  "previous message" chain, duplicates, sync and FETCH. Names, keys and conversations use the person (sender byte − 128).
  Packets are always addressed to the person, so both devices may open them.
  Protocol version 4.
- **R3. Notes to self.** Every key or group change a device makes (new sender
  key `'S'`, group key `'G'`, leaving `'L'`, a new group) also goes as a DM to its
  own member number. Both devices compute the same "pair key with myself".
- **R4. Only the T5 makes keys.** New sender keys and new group keys after
  someone leaves come only from the T5. The phone makes only the first key of a
  group it creates, with group ids 128–255 of the person's 256 (the T5 uses
  0–127).
- **R5. The T5 hands the Pi what it got away from home.** At its next Wi-Fi sync
  the T5 uploads the packets it got over LoRa that the Pi may lack. The Pi drops
  the ones it has. Without this the phone never sees what came while out of the
  gateway's range, keys included.
- **R6. The Pi sends me my other device's messages.** Today a sync skips the
  asker's own messages; with two devices it skips only that device's.
- **R7. The phone waits for a new sender key.** When the roster loses someone,
  the phone stops writing to "Alle" until the note from its T5 with the new
  sender key arrives ("Venter på ny nøkkel fra T5-en"). Otherwise the one who
  left could read it.
- **R8. A lost device means a new identity, under the same number.** The person
  is taken out of the roster (`waddle mesh remove`), the T5 makes a new identity
  by itself when it gets that roster (built), Arne invites them again
  (`waddle mesh invite <same name>`), and `waddle mesh approve` gives them their
  old number back (built). The phone is paired again. The same number keeps every
  conversation in one place on everyone's units (no second "Kari"), and lets the
  Pi send her old DMs without knowing anything new. The new `counterFloor` keeps
  the new counters above the old ones.
- **R9. One phone per person.** Pairing a new phone replaces the old one; its
  counter floor is newer than anything the old phone used.
- **R10. Read on one, read on both** (nice to have). A "read" ACK from my other
  device marks the message read here too.
- **R11. Old keys are kept for reading.** A unit keeps every key it has had: its
  old identity secrets, every group key number and every member's old sender
  keys. The newest few stay in working memory as today (6 per group, 3 per
  member); older ones go to a sealed archive file on the SD card, read only when
  an old message needs one. Old keys are never used to send. This does not help
  a thief: they could already read everything up to the theft; the new identity
  protects what comes after. Size is no concern: all keys together are a few KB
  (a 1 GB card holds a few million messages).
- **R12. History comes back from the Pi.** The Pi keeps every packet, sealed. A
  unit with the old keys (R11) asks for everything from the start and opens what
  it can. Pairing gives the new device the archive too, so a phone can bring
  history to a new T5 (E3). The Pi must keep the packets; it deletes nothing
  today.

## The people in the scenarios

| Who | Number | Devices |
|---|---|---|
| Arne | 1 (admin) | T5 and phone |
| Kari | 2 | T5 and phone |
| Ola | 3 | T5 only |
| Ingrid | 4 | T5 only, joins in D1 |

"Home" means in range of the gateway or on Wi-Fi; "away" means neither.

## A. Setting up

**A1. Kari pairs her phone.** Kari's T5 is in the network. At home she opens
File Transfer, the app finds the T5, both show "482 913", she taps OK on the T5.
→ The phone shows the same conversations from the Pi as the T5 and can write at
once (R1).

**A2. Ola tries the app without a T5 in the network.** → The app has nothing to
pair with and says so. There is no way in without a T5.

**A3. Kari pairs while the T5 has a group invitation waiting to be opened.** →
The phone gets the keys the T5 has; the rest arrives as notes later (R3).

## B. Direct messages

**B1. Ola (away) writes Kari; she reads it on the phone.** Ola's T5 sends over
LoRa; the next unit or the gateway carries it to the Pi. Kari's phone syncs and
opens it (same pair key). She reads it → "read" ACK from sender 130 → Ola sees
"Lest". Kari's T5 gets the same message at its next check and, with R10, shows
it as read.

**B2. Kari writes Ola from the phone, Ola is away.** Phone → Pi → gateway sends
it once over LoRa; Ola does not hear it. Kari sees "Lagret på serveren". Ola's
T5 gets it at its next check in range of the gateway or of a unit that has it.
Kari's T5 shows her sent message too, marked as from the phone (R6).

**B3. Kari and Ola on a trip together, no gateway.** Ola writes, Kari's T5 gets
it over LoRa and ACKs. Her phone has no signal. → Only the T5 has it; the phone
gets it after the T5's next Wi-Fi sync at home (R5).

**B4. A packet is lost.** Ola's message to Kari never arrives. Ola's T5 retries
until an ACK comes, then keeps it in its outbox for the next check. When Ola's
next message arrives first, Kari's devices see the gap in the chain and ask for
the missing one (FETCH). Works today, per device with R2.

**B5. Kari writes Ola from both devices at once.** T5 sends as 2, phone as 130.
Ola sees both in one conversation, by time. No "missing message" warning,
because the chain is per device (R2). Without R2 every switch between devices
would look like a gap.

**B6. Kari's phone is off for three weeks.** On open it asks the Pi for
everything after its last message from each sender. The Pi keeps everything
today; if it ever forgets old packets, a phone off for longer than that misses
them (say so in the app).

**B7. Both of Kari's devices ACK the same message.** Ola gets "delivered" twice
(from 2 and 130) and shows it once. Harmless.

## C. Groups

**C1. Arne makes "Hytta" (Arne, Kari, Ola) on his T5, at home.** Invitations
`'G'` to Kari and Ola, and a note to Arne himself (R3). Arne's phone gets the
key from the Pi. Kari's T5 or phone, whichever syncs first, ACKs; both open it.

**C2. Same, but Arne and Kari are on a trip.** Kari's T5 gets the invitation
over LoRa and ACKs, so Arne's T5 stops sending it. The Pi never had it. → Both
phones learn the group only when the T5s are home again (R5). Until then, group
messages that reach the phones wait unopened (they are kept until the key comes;
works today).

**C3. Kari makes "Middag" (Kari, Ingrid) on her phone.** Group id from 128–255
(R4), key number 1, `'G'` to Ingrid and a note to Kari. Kari's T5 gets the group
at its next check.

**C4. Ola leaves "Hytta".** `'L'` to Arne and Kari. The lowest member left,
Arne, makes key 2, but only on his T5 (R4); his phone gets it as a note. If Arne's
T5 is off, nothing changes until it is on, and Ola can read new "Hytta" messages
until then. That is already true without phones: the lowest member's unit must
be on.

**C5. Kari leaves "Hytta" on her phone.** `'L'` to the others and a note to her
T5, which then shows the group as left too.

**C6. A late message from Ola after he left.** A message he wrote under key 1
arrives after he left. Both of Kari's devices drop it: once out, nothing from him
in that group shows (the counter in it proves nothing).

**C7. A group message reaches Kari's phone before its key.** Kept, opened when
the note or invitation arrives (works today).

## D. Members coming and going

**D1. Ingrid joins.** Every T5 sends her its sender key; the phones do not need
to, they share the T5's key. Ingrid can read "Alle" from Arne's phone at once.

**D2. Arne removes Ola from the network.** Each T5 makes a new sender key, sends
it to everyone left and to itself (R3). Until a phone has its T5's new key it
does not write to "Alle" (R7). If Kari's T5 is off for a week, Kari's phone
waits a week; the app says why.

**D3. Arne's phone writes "Alle" while his T5 just made a new sender key.** The
phone uses key 4, the T5 key 5. Receivers keep 3 keys per member, so both open.

## E. Changing devices

**E1. Kari gets a new phone; the old one is reset and sold.** She pairs the new
one (R1). The old one is wiped in the app ("Glem meg") or by the reset. No
roster change. If the old one somehow still runs, its counters are below the new
floor, so no nonce is reused (R9), but it can still read: if in doubt, treat it
as lost (E2).

**E2. Kari's phone is stolen.** The thief holds her identity. Arne runs
`waddle mesh remove kari`: everyone makes new sender keys, she is taken out of
all groups. Kari's T5 makes a new identity and keeps the old keys for reading
(R11), Arne sends a new invitation, she joins again as number 2 (R8), pairs a new
phone, and is invited into the groups again. Her history stays on the T5; the
new phone gets it from the Pi with the archived keys (R12). The thief's copy
opens nothing sent after the removal, and the stolen phone's token no longer
works on the Pi.

**E3. Kari's T5 breaks (dead screen, still at home).** Nobody else has the
secret, but there is no T5 to pair with any more. Arne removes her, she joins
with a new T5 under number 2 (R8). When the phone is paired with the new T5 it
hands over its keys, old and current, as the archive (R11); the new T5 then gets
her history from the Pi (R12). The phone gets the new identity in the same step.

**E4. Kari's T5 is lost.** As E3, but the finder holds her identity, so the
removal matters: nothing new opens for them. History comes back through the
phone's keys (R12).

**E4b. Kari loses both her T5 and her phone.** Remove, new T5 under number 2.
No device has the old keys, so the old history is gone; new messages work. Her
messages remain on the others' units.

**E5. Kari wants a newer T5 but the old one works.** Same as E3. There is no
move from T5 to T5; one way of doing it is simpler.

**E6. Kari stops using her T5 but keeps the phone.** Not allowed. When her number
leaves the roster the app says "Ikke med i nettverket" and stops.

**E7. Kari wants a second phone.** Not supported (R9).

## F. Something is down

**F1. Kari's T5 is in a drawer for a month.** The phone works for messages. No
new sender key reaches it (D2) and groups where she is lowest do not change
(C4). The app shows "T5-en har ikke vært på nett siden …".

**F2. The Pi is down.** The phone can do nothing; it says so. The T5s work over
LoRa, the gateway keeps what it hears.

**F3. The Pi comes back after a gateway standalone night.** The gateway hands
its packets to the Pi; the phones get them at the next sync. Works today.

**F4. The phone's clock is wrong.** Nothing depends on it: the counter floor
comes from the T5 at pairing (R1), and message times come from the device that
wrote them.

## What to build, roughly in order

1. Done: protocol 4, the device in the sender byte (R2), notes to self (R3), only
   the unit makes keys (R4), the server sends a device its other one's messages
   (R6), the phone waits for its unit's new sender key (R7), group id ranges.
   Tested in the simulator with Kari's unit and phone.
2. The T5: notes to self (R3), only the T5 rotates (R4), upload after trips
   (R5), pairing endpoint (R1), the key archive and history fetch (R11, R12).
   Done: a new identity when taken out, files sealed with a key of their own
   (R8).
3. The Pi and the tool: R6, R5's upload. Done: `waddle mesh approve` giving a
   returning member their old number (R8).
4. The app (`phone/`, built: `waddle-mobil.apk` in each release): pairing (R1),
   conversations, writing, sync on open, on send and every 5 minutes while
   open, R10 (read on one device is read on the other), and R5 (the T5 hands
   the Pi what it heard on a trip). Still to do: new
   groups from the phone, notifications. It reuses the C++ node through the Android NDK rather than a
   Kotlin rewrite, so the group and key rules stay one piece of code
   (ARCHITECTURE.md, "Another device for the same code"); Kotlin does the
   screens, HTTPS and storage paths.
5. The sim: one person with two devices, and the scenarios above as tests.

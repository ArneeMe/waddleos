# WaddleOS plan

What is done and what is next. How the system fits together:
[ARCHITECTURE.md](ARCHITECTURE.md). How to work: [AGENTS.md](../../AGENTS.md).

## Done

- **Install and update:** own build in GitHub Actions (`waddle-latest`),
  `waddle update` over Wi-Fi, and **Innstillinger → Oppdater fastvare** on the
  reader (through arnee.me). Version "WaddleOS 1.<build number>".
- **Look:** penguin boot and sleep screens. A book sleeps on its page with a
  sleeping penguin; elsewhere the penguin screen, waking on Hjem.
- **Hjem:** clock, date, weather (yr.no via arnee.me), the book being read,
  3 unread NRK headlines (hold to mark read), penguins with a random quote,
  icon buttons. Fetches once when opened if older than 30 min; sets the clock.
- **Nyheter:** NRK top stories from the last 36 hours as an EPUB with gray
  lead images, rebuilt by arnee.me (hourly on paper, every 3–6 h in practice).
  Unread first, "LEST" section, Hent nå, read stories marked as they open and
  skipped while paging.
- **Bluesound:** remote for the Node (presets, volume, play/pause, skip).
- **Dictionary:** English Wiktionary on arnee.me, installed with
  `waddle dict get`; the reader handles its 1 GB `.dict.dz`.
- **Computer:** one `waddle` command (`update`, `book`, `add`, `dict get`).
- **Privacy:** everything that talked to outside servers is removed. The
  reader talks only to arnee.me and the Bluesound Node, when Arne opens a screen.
- **Simpler:** over 20,000 lines of CrossPoint removed (see ARCHITECTURE.md,
  "Removed from CrossPoint"). Norwegian and English only.

## Next (ideas Arne has mentioned, not started)

- Cloudflare timer that starts `nyheter.yml` every hour (GitHub skips runs).
  Needs a GitHub token as a Cloudflare secret.
- Back from a headline opened on Hjem returns to Hjem, not the news list.
- Chapter on the Hjem book card ("kapittel 18").
- Hjem fetch in the background so taps are not ignored for a few seconds.
- Reader font sizes below 12 (adds flash per size).
- Read later: send an article link to arnee.me, get it on the reader.
- To-do and shopping list from Oppgaver on arnee.me.
- NRK podcasts played on the Bluesound (arnee.me finds the newest episode).
- Settings that may be pointless now: sleep screen options, XTC status bar,
  font (one choice). Ask Arne before removing.

## Mesh unit

WaddleOS grows a second device: a LilyGO T5 E-Paper S3 Pro (868 MHz LoRa,
GPS) as an off-grid messenger for a network of up to 32 people. Full spec: "Off-Grid Mesh
Messenger – Software Spec" (Claude Docs,
https://claude.ai/artifact/VugwyP8RmFDMwqKaB2qPCX).

Decided:

- One repo, one system. The reader and the mesh unit are two builds
  (`x4pro-waddle`, `t5s3-waddle`) of a shared core; a Heltec V4 gateway and a
  Python server on Arne's Pi live here too.
- Our own OS, not a Meshtastic fork (Meshtastic is a reference). RadioLib for
  the SX1262.
- Privacy as on the reader: nothing leaves by itself. The only servers are
  the Pi and arnee.me. Arne's router can port forward to the Pi.
- Updates come from arnee.me when Arne taps the button. No signing: only Arne
  can publish there. A new build that crashes twice in its first minute goes
  back to the one before by itself.
- Mostly a backup for when other ways to talk are gone: "Hjemme" it checks for
  messages once a day; "På tur" every hour, with GPS and position sharing.
- A network of up to 32 people, which Arne lets in (invite link by Signal,
  approval on his computer); groups made on the units. People in a group can
  pose as each other: accepted, there is no room for a signature per message.

Phases:

1. Done: `src/` split into `core/`, `apps/` and `products/x4pro/` with no
   change in behavior. `services/` (radio, mesh, GPS) comes with phase 3.
2. Written, waiting for the hardware: the T5 build, radio, messaging (group,
   DMs, keys, ACK, sync), gateway and Pi server, map, SOS, position sharing,
   notes. What to check on the devices: [TESTING.md](TESTING.md).
3. After the first tests: fix what the devices show, measure battery days.

Idea: **the phone as a second device for the same person.** An Android app
that is the same member as their T5, through the Pi. Everyone still starts with
a T5. The phone copies the T5's identity once at home (code check, as when
joining); after that both read the same mailbox on the Pi, and every key change
a device makes is also sent to itself. The device is part of the sender byte, so
the two never reuse a message id. Only the T5 makes new keys. A lost phone or T5
means leaving the roster and joining again with a new identity under the same
number; old keys are kept for reading, so history comes back from the Pi. Rules
and the cases it must handle (dropped messages, groups, new or lost phone or
T5): [PHONE.md](PHONE.md).

Next on the unit, before the phone: **seal the message files on the SD card.**
`/.mesh/chat/` holds the conversations as readable text and `/.mesh/packets/`
the packets as received; anyone who takes the card out reads the history
without the T5. Seal them like the key files (`saveSealed`), and keep the old
storage keys (PHONE.md, R11) so history survives a new identity.

Before calling it done: **a screen-by-screen look** (the units, the reader and
the Pi's status page) by Arne, to check that everything reads well for a person,
once the technical work has settled. The screen simulator
(`bash test/screens/run.sh`) gives the T5 screens without a device.

Later, maybe: a small piezo buzzer on the T5's free QWIIC port for a sound on
SOS or a new message (the board has no speaker or vibration motor).

Open: which GPS the unit has (MIA-M10Q or L76K), and whether the home
connection has a public IP (no CGNAT).

## Decided against

- Audio on the reader (no audio hardware, BLE only, no A2DP).
- Estimated battery use per part (only the total is measurable).
- Automatic update checks.

## Notes for Arne's machines

- Arch: `sudo pacman -S esptool`. Serial access: add yourself to `uucp`; if the
  browser still cannot open the port, `sudo chmod a+rw /dev/ttyACM0`.
- Stock firmware backup: `xteink-x4pro-stock.bin` (16 MB, sha256
  `16eea9ef7ab702e39990b4e96d0c0fb62162b8bbad2bd3a0d2147447a681cf92`).
  Backup command: `esptool --chip esp32s3 -p /dev/ttyACM0 -b 921600 read-flash 0 ALL xteink-x4pro-stock.bin`.
- The reader is at `192.168.50.185` (`WADDLE_IP` overrides). If it is
  unreachable: the transfer screen is closed or asleep, or the router isolates
  Wi-Fi clients ("AP Isolated" on ASUS).

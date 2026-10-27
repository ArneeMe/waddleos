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

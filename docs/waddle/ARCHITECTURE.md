# WaddleOS architecture

A map of the system for AI agents (and Arne). Read this before changing
anything; [AGENTS.md](../../AGENTS.md) has the rules for how to work, and
[PLAN.md](PLAN.md) has what is done and what is next.

## The three parts

WaddleOS is more than the firmware. Three parts work together:

| Part | Where | What it does |
|---|---|---|
| Firmware | `ArneeMe/waddleos` (this repo), branch `waddle` | Runs on the Xteink X4 Pro e-reader and the LilyGO T5 mesh units |
| Mesh server | `server/` in this repo, on Arne's Raspberry Pi | Stores and passes on the mesh units' encrypted messages (see "Mesh") |
| arnee.me | `ArneeMe/arneeme` (Astro on Cloudflare Workers) | Builds the NRK paper, weather and dictionary; passes on the firmware and the time |
| `waddle` command | `tools/` in this repo | Runs on Arne's computer (zsh, Linux and macOS): updates, books, dictionary |

```
 Arne's computer                 arnee.me (Cloudflare)               GitHub
 ───────────────                 ─────────────────────               ──────
 waddle update ──Wi-Fi──┐        /api/nrk.epub, nrk.json  ◄── release nrk-siste   (nyheter.yml, hourly)
 waddle book   ──Wi-Fi──┤        /api/hjem.json           ◄── release nrk-siste   (vaer.mjs, yr.no)
 waddle add    ──Wi-Fi──┤        /api/ordbok/<file>       ──► release ordbok       (ordbok.yml, by hand)
 waddle dict get ───────┤        /api/fastvare/<file>     ◄── release waddle-latest (waddle-build.yml)
                        ▼        /api/tid  (current time)
                 ┌────────────┐         ▲
                 │  X4 Pro    │─────────┘  HTTPS, only when Arne opens a screen
                 │  WaddleOS  │─────────►  Bluesound Node, port 11000, local network
                 └────────────┘
```

The reader only talks to arnee.me and the Bluesound Node, and only because
Arne opened a screen. Nothing runs in the background or during sleep.

## Hardware and build

- **Device:** Xteink X4 Pro. ESP32-S3, 512 KB RAM plus 8 MB PSRAM, 16 MB flash
  (two 6.25 MB app slots), 800×480 e-ink (SSD1677, 4 gray levels), GT911
  touch, BM8563 clock, battery gauge, front light with warmth, vibration
  motor absent. Bluetooth Low Energy only: no classic Bluetooth, no audio.
- **Three build targets** in `platformio.ini`: `x4pro-waddle` (the reader),
  `t5s3-waddle` (the mesh unit, LilyGO T5 E-Paper S3 Pro: 960×540 e-ink,
  SX1262 868 MHz LoRa, GPS) and `heltec-gateway` (Heltec V4 on the Pi's USB).
  `WADDLE=1`, `WADDLE_PRIVATE=1` and `WADDLE_OWNER_NAME="Arne"` are always set.
  `CROSSPOINT_BOARD_NAME` tags each image, so a device refuses the other's firmware.
- **Compiling:** in a cloud session run `bash scripts/cloud-setup.sh` once,
  then `pio run -e x4pro-waddle`. GitHub Actions
  (`.github/workflows/waddle-build.yml`) also builds every PR;
  compiler errors appear as annotations on the `build` check run (read them
  with `gh api repos/ArneeMe/waddleos/check-runs/<id>/annotations`).
- **Release:** a push to `waddle` publishes `waddleos-x4pro.bin`,
  `waddleos-t5.bin`, `waddle-gateway.bin` and `version.json` to the
  `waddle-latest` pre-release. CI also runs the mesh tests (`test/mesh/run.sh`
  and `server/tests`).
- **Version:** "WaddleOS 1.<GitHub run number>". The build writes
  `src/core/waddle/WaddleBuild.h` (git-ignored) with `WADDLE_VERSION` and
  `WADDLE_BUILD` (short git hash). Only `WaddleVersion.cpp` includes it, so a
  new number recompiles one file. Never put changing values in `-D` flags:
  that recompiles everything and makes CI slow.
- **FreeInk SDK** (`freeink-sdk/`, a git submodule) holds the drivers and the
  board profile. Do not patch it in place.

## Source layout

`src/` is split so a second device (the T5 S3 Pro mesh unit, see PLAN.md) can
share the base and pick its own apps. Each build lists what it compiles in
`build_src_filter` in `platformio.ini`: `x4pro-waddle` takes `core/`, `apps/`
and `products/x4pro/`; `t5s3-waddle` takes `core/`, `apps/` (not Bluesound),
`services/` and `products/t5s3/`. `core/WaddleMain.cpp` is the one `setup()` and
`loop()`; each product fills in the hooks in `core/Product.h` (board start,
home screen, background loop, sleep, web routes, firmware file name). Includes are written from `src/`
(`"core/activities/Activity.h"`), or as a bare name for a file in the same folder.

```
src/core/               shared by every device
  activities/           Activity, ActivityManager, list screens; boot_sleep/,
                        network/ (Wi-Fi, file transfer, USB), settings/, util/ (dialogs, keyboard, light panel)
  components/           UITheme (Lyra is the only theme), icons, shared UI pieces
  network/              web server for file transfer, HttpDownloader (HTTPS via wolfSSL), firmware flashing
  util/                 helpers (buttons, strings, QR, time zones, screenshots)
  waddle/               WaddleOS base: Wi-Fi, version, update screen, privacy, penguin drawing
  *.h / *.cpp           settings, state, button mapping, Wi-Fi passwords, fonts
src/apps/               one folder per app
  reader/               EPUB/XTC reader, dictionary, bookmarks, clippings, recent books, text settings
  library/              library list and file browser
  news/                 Nyheter (NRK paper)
  bluesound/            Bluesound remote (reader only)
  messages/ map/ sos/ notes/   the mesh unit's apps: Meldinger, Kart, SOS, Notater
src/services/mesh/      the mesh unit's radio, GPS, keys, message store and node
src/products/x4pro/     the e-reader: Product.cpp, Hjem, weather icons, logo
src/products/t5s3/      the mesh unit: Product.cpp (light sleep, SOS button), Hjem
src/products/gateway/   the Heltec V4 gateway firmware (its own small main)
lib/WaddleMesh/         mesh packets, encryption, airtime rules (no Arduino; host-tested)
lib/monocypher/         Monocypher 4.0.2, copied unchanged
protocol/               PROTOCOL.md and vectors.json, shared by firmware and server
server/                 the Pi server (Python standard library only)
lib/                    Epub, GfxRenderer, I18n, hal, fonts, zip, ...
lib/I18n/translations/  norwegian.yaml and english.yaml only
tools/                  the waddle command
assets/waddle/          penguin and weather icon generators
docs/waddle/            this file, PLAN.md, privacy notes
```

Not yet clean: some core files still know about apps (the settings screen opens
the reader's text and status bar screens, `ActivityManager` opens the reader and
Hjem, the web server lists books, the theme draws the reader's status bar, the
SD card font system lives in core). Untangle those when the second device needs it.

### WaddleOS files

| File | What |
|---|---|
| `products/x4pro/WaddleHomeActivity.*` | **Hjem**, the home screen: pixel clock, date, weather, the book being read, top 3 unread NRK headlines, penguins with a random quote, 5 icon buttons. Fetches weather, news and time when opened if the data is older than 30 min (once per visit) |
| `apps/news/NewsActivity.*` | **Nyheter**: headline list, unread first then "LEST", Forrige / Hent nå / Neste, hold a row to toggle read |
| `apps/news/NewsPaper.*` | The NRK paper on the SD card: load the list, read state, `sync()` download, `openStory()`, `enterChapter()` (marks stories read and skips read ones while paging) |
| `apps/bluesound/NodeActivity.*`, `BluOsClient.*` | **Bluesound** remote: presets, volume, play/pause, next/previous, a singing penguin |
| `core/waddle/UpdateActivity.*` | **Oppdater fastvare**: check `version.json`, download, hand the file to `SdFirmwareUpdateActivity` |
| `core/waddle/WaddleVersion.*` | `waddle::version()` and `waddle::build()` |
| `core/waddle/WaddleWifi.*` | `joinSaved()` (last used saved network) and `off()`, shared by every Wi-Fi screen |
| `core/waddle/WaddleFirmwareApi.*` | `POST /api/firmware/apply` on the file transfer web server, used by `waddle update` |
| `core/waddle/WaddlePrivacy.*` | Neutral hostname and user agent ("reader"), hotspot name and password |
| `core/waddle/WaddleDraw.*` | Penguin, music notes, speaker icon, pixel font for the clock, boot/sleep splash |
| `core/waddle/WaddlePenguin.h`, `products/x4pro/WaddleWeatherIcons.h` | Generated bitmaps (`assets/waddle/*.py`) |

### Hooks in CrossPoint code

These upstream files call into WaddleOS code. Upstream is no longer followed,
so they may be edited freely.

| File (under `src/`) | Hook |
|---|---|
| `core/activities/ActivityManager.cpp` | `goHome()` opens `WaddleHomeActivity`; leaving the NRK book returns to `NewsActivity` |
| `apps/reader/EpubReaderActivity.cpp` | `renderBook()` calls `waddle::news::enterChapter()` for the NRK book |
| `apps/reader/ReaderActivity.cpp` | The NRK book is not added to recent books |
| `core/activities/boot_sleep/*` | Penguin boot and sleep screens; a book sleeps on its page with a sleeping penguin |
| `core/WaddleMain.cpp` | `enterDeepSleep()`: keep the page only when sleeping from the reader; `product::sleep()` lets the mesh unit light-sleep instead |
| `core/activities/settings/SettingsActivity.cpp` | "Oppdater fastvare" opens `UpdateActivity` |
| `core/network/CrossPointWebServer.cpp` | Registers `/api/firmware/apply`; reports `waddle::version()` |
| `lib/hal/HalClock.*` | `setUtc()`, used by Hjem to set the clock from arnee.me |

## Settings

One settings system in core, and each app brings its own settings.

- **Storage:** everything is in `/.crosspoint/settings.json`, one key per
  setting. A device reads the keys it knows and ignores the rest, so the same
  file works on every device. Never rename a key that has shipped; new app keys
  get a prefix (`mesh.channel`).
- **Core** (`core/CrossPointSettings.*`, `SETTINGS`): settings every device has
  (sleep, night mode, language, clock, buttons, frontlight, Bluetooth).
  `core/SettingsList.cpp` lists their rows. Hardware-only rows are shown only
  where the board has the hardware (`BoardConfig`, `FREEINK_CAP_*`).
- **Apps** (`apps/reader/ReaderSettings.*`, `READER_SETTINGS`): an app keeps
  its own settings and hands core a `SettingsModule` (`core/SettingsModule.h`):
  its rows, extra keys to save and load, and changes to the rows at runtime
  (fonts, dictionaries).
- **Products** pick which apps' settings exist: `settingsModules()` in
  `products/x4pro/ProductSettings.cpp`. A new device lists its own apps.
- **Screens:** the Settings screen and the web settings page are built from
  core's rows followed by each module's rows, grouped by category.

To add a setting: put the field in the struct that owns it (core or the app),
add a row with a new key to that owner's list, and add the Norwegian text in
`lib/I18n/translations/norwegian.yaml`.

## Mesh

Three mesh units (one per person), a gateway and the Pi. Full spec: "Off-Grid
Mesh Messenger – Software Spec" (Claude Docs). Packet format: `protocol/PROTOCOL.md`.

```
 T5 unit ◄──LoRa 869.525 MHz──► T5 unit ◄──LoRa──► Heltec V4 ──USB serial──► Pi: waddlemesh
    │        (relays, 3 hops)                       (gateway)                 SQLite, :8443
    └───────────── HTTPS (pinned certificate, port forward on the router) ──────────┘
```

- **Privacy:** messages and positions are encrypted on the unit
  (ChaCha20-Poly1305; per-member sender keys for everyone, X25519 for DMs, and
  a key per group, with a key number in the header).
  The server stores packets as received and has no keys. Units talk only to each
  other, the Pi and arnee.me (updates).
- **The network** (protocol/PROTOCOL.md, "The roster", "Joining"): up to 32
  members and one admin (Arne), whose keys and the network key live on his
  computer (`~/.config/waddle/admin.json`). Who is in is the *roster*, signed by
  the admin; units and the Pi trust only that signature. Joining: `waddle mesh
  invite <name>` makes a link sent by Signal; the newcomer runs `waddle mesh
  join <link>` with their unit on File Transfer; Arne runs `waddle mesh approve`.
  The unit gets a sealed welcome (its number, the network key, the server, the
  roster). `waddle mesh remove <name>` takes someone out.
- **Keys on the unit:** its own secret and sender key in NVS flash; the network,
  the roster, groups and the others' sender keys on the SD card in files sealed
  with a storage key from a secret of its own in flash (`MeshConfig.cpp`,
  `saveSealed`). A unit taken out of the network makes a new secret and sender
  key (`newIdentity`); the files stay readable. Sender
  keys travel to every member as DMs, sent again until confirmed. When someone
  leaves the roster, every unit makes a new sender key. The messages
  (`/.mesh/chat/`, `/.mesh/packets/`), positions and read counts are sealed
  too, record by record (`appendSealed`, `SealedLog`). A paired
  phone as a second device: [PHONE.md](PHONE.md) (plan).
- **Groups** (`services/mesh/MeshGroups.cpp`): made on a unit; the key goes to
  each member in an invitation DM. Leaving or "Fjern" makes the next key number
  for those left (only the lowest member makes it); every unit keeps the old
  ones for the history.
- **Node** (`services/mesh/MeshNode.cpp`): one loop for radio and Wi-Fi alike:
  decrypt, store (`/.mesh/`), ACK (delivered, read), relay with a random delay,
  retry after 30 s, 2 min, 8 min, then try the server over Wi-Fi. SYNC_REQ on
  start and wake; the server and other units answer with what is missing. A gap
  in a sender's chain is fetched on purpose (FETCH).
- **Sleep:** the T5 light-sleeps with the radio listening (`product::sleep()` in
  `products/t5s3/Product.cpp`): packets are handled without waking the screen;
  a button or an incoming SOS wakes it.
- **GPS** shares the radio's power switch, so it is told to sleep (u-blox
  backup mode) instead of switched off. Which GPS ships is not known yet.
- **Server** (`server/`): `python3 -m waddlemesh serve`, systemd units and
  `install.sh` in `server/`. Status page from the home network only.
- **Two modes:** *full*, the Pi on: it stores and answers, and shows a status
  page (`https://<pi>:8443/`, Norwegian, no keys: SOS, each unit's last
  contact, signal, battery, "Hjemme"/"På tur", the gateway); *low power*, the
  Pi off: after 30 s without a `PING` the gateway keeps packets in flash and
  answers syncs itself (`lib/WaddleMesh/src/GatewayStore.*`), and hands them
  over when the Pi is back (protocol/PROTOCOL.md, "Gateway serial link").

## How a screen works

Read `.agents/rules/ui-activities.md` for details. The short version:

- A screen is an `Activity` with `onEnter()`, `loop()`, `render(RenderLock&&)`
  and `onExit()`. `loop()` runs on the main task; `render()` runs on a
  separate render task when `requestUpdate()` was called.
- Data that `render()` reads must be changed under `RenderLock lock(*this)`.
  The lock is not recursive: never call a function that takes it while
  holding it.
- Open a screen with `activityManager.replaceActivity(...)` (or `goHome()`,
  `goToReader(path)`, ...). Open one on top with `startActivityForResult()`
  and close it with `finish()`.
- Touch: `mappedInput.wasScreenTapped(x, y)`, `wasScreenLongPress(x, y)`,
  `wasSwipe()`. Buttons: `wasReleased(Button::Confirm)` and so on. Do not
  build features on holding a button: on Arne's reader a hold toggles the
  front light (home button actions in `main.cpp`). Use an on-screen button.
- `preventAutoSleep()` returning true resets the auto-sleep timer every loop.
  Only return true for short work (a fetch, a download). A screen that keeps
  starting work never sleeps; that was a real bug on Hjem.
- Blocking network calls inside `loop()` freeze input for their duration.
  Keep them short and rare.
- Display refresh: `displayBuffer()` (default), `FAST_REFRESH` for small
  changes, `HALF_REFRESH` now and then to clear ghosting.

## Network

- Join Wi-Fi with `waddle::wifi::joinSaved(*this)`, poll `WiFi.status()` in
  `loop()` with a ~15 s timeout, and call `waddle::wifi::off()` when done and
  in `onExit()`.
- HTTPS goes through `HttpDownloader` (`fetchUrl`, `downloadToFile`). Check
  `MIN_TLS_FREE_HEAP` / `MIN_TLS_MAX_ALLOC` first. `downloadToFile` writes to
  `<dest>.part` and renames on success.
- Allowed hosts: `arnee.me` and the Bluesound Node. Anything else needs Arne's
  approval (privacy goal 1). There is no NTP: the clock comes from
  `arnee.me/api/tid`.

## Files on the SD card

| Path | What |
|---|---|
| `/Nyheter/NRK.epub`, `/Nyheter/nrk.json` | The current NRK paper and its headline list |
| `/Nyheter/lest.txt` | Ids of read stories, one per line, only for the current paper |
| `/.waddle/hjem.json`, `/.waddle/hjem-hentet.txt` | Weather, and when Hjem last fetched (local minutes since 1970) |
| `/.waddle/node-ip.txt` | Optional Bluesound Node IP (default 192.168.50.239) |
| `/dictionaries/english/` | Wiktionary StarDict files, put there by `waddle dict get` |
| `/waddleos-x4pro.bin` | Firmware downloaded by Oppdater fastvare |
| `/.crosspoint/` | Reader caches; `epub_<hash>/progress.bin` holds a book's position |
| `/.crosspoint/settings.json` (and friends) | Settings, saved as JSON by key |

## arnee.me

| Endpoint | Source | Format |
|---|---|---|
| `/api/nrk.epub` | release `nrk-siste`, `nyheter/build.mjs` | EPUB: front page, then one chapter per story (story i is chapter i + 1), gray lead image |
| `/api/nrk.json` | same | `{tittel, bygd, saker, liste: [{id, t, kl}]}`, `liste` in chapter order |
| `/api/hjem.json` | `nyheter/vaer.mjs` (yr.no, Oslo) | `{bygd, sted, temp, min, max, ikon, tekst}` |
| `/api/tid` | live | Seconds since 1970 UTC as text |
| `/api/ordbok/<file>` | release `ordbok` (`ordbok.yml`, run by hand) | Redirect to the StarDict files and `ordbok.json` |
| `/api/fastvare/<file>` | release `waddle-latest` in this repo | Streams `version.json` and `waddleos-x4pro.bin` |

- `nyheter.yml` runs hourly, but GitHub skips many scheduled runs on quiet
  repos (in practice every 3–6 hours). It keeps stories published or updated
  in the last 36 hours. Images are baseline grayscale JPEG, 480 px wide
  (the reader decodes progressive JPEG at 1/8 resolution only).
- Cloudflare blocks the `Python-urllib` user agent with 403. Tools must send
  their own (`waddle` sends "waddle").

## Behavior worth knowing

- **Sleep:** sleeping in a book keeps the page with a sleeping penguin, and
  waking opens the book again. Sleeping anywhere else shows the penguin
  screen, and waking goes to Hjem.
- **Settings** are saved as JSON by key, so a removed setting is just
  ignored. Enum values are saved as numbers: do not reorder enums.
- **Text:** UI strings go through `tr(STR_*)`. Only `norwegian.yaml` and
  `english.yaml` exist; `scripts/gen_i18n.py` generates the code and lists
  unused keys (`--verbose`). WaddleOS-only text may be hardcoded in
  Norwegian.
- **Fonts:** the UI fonts cover Latin-1 (æøå, °, ·) but not every symbol:
  avoid en dash, "›" and similar in UI text.
- **Memory:** allocations over 4 KB go to PSRAM on the S3. Internal RAM is
  tight while a book is open. See `.agents/rules/hardware-resources.md`.
- **Name clashes:** `SETTINGS` and `GUI` are macros. Do not use them as enum
  or variable names.

## Removed from CrossPoint

So you do not go looking for them: plugins, the OPDS catalog, KOReader sync,
OTA from CrossPoint's GitHub, LCP DRM, mDNS, font download and web font
upload, Calibre wireless, NTP sync, the theme setting and other themes, the
old home screen, 33 UI languages, keyboard layouts, other boards (Picco,
Metalio, Paper Mono, X3/X4), quick resume after timeout, and the host unit
tests. Still present but unused: right-to-left text (woven into layout) and
protected-book support inside the EPUB loader.

## Making a change

1. Branch from `waddle` (`git fetch origin waddle`), make the change, push,
   open a PR into `waddle` in `ArneeMe/waddleos`. Never touch
   `crosspoint-reader/crosspoint-reader`.
2. For anything visual, show Arne an image mockup first.
3. Wait for the `build` check (or let Arne merge and fix forward if he asks).
4. Write a short PR description with a "test on the reader" list. Only Arne
   can flash and test.
5. Changes on arnee.me go to `ArneeMe/arneeme` (Norwegian comments there).

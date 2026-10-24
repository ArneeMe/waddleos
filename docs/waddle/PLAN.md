# WaddleOS plan

WaddleOS is a personal fork of [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader)
for the Xteink X4 Pro. Repo: `ArneeMe/waddleos`. Mascot: a pixel-art penguin.

Goals: my own look, easy updates over Wi-Fi, and nothing phones home.

How to work on WaddleOS: see [AGENTS.md](../../AGENTS.md).

## Status

- [x] X4 Pro confirmed: ESP32-S3, 16 MB flash, 8 MB PSRAM. Unlocked (bought from xteink.com).
- [x] Stock firmware backed up (`xteink-x4pro-stock.bin`, 16 MB,
      sha256 `16eea9ef7ab702e39990b4e96d0c0fb62162b8bbad2bd3a0d2147447a681cf92`).
- [x] Official CrossPoint flashed with the web flasher. Boots, Wi-Fi works.
- [x] Name and mascot chosen: WaddleOS, penguin (`assets/waddle/`).
- [x] File upload over Wi-Fi from Arch works (`curl` to `/upload`).
- [x] Penguin sleep screen as `sleep.bmp` (Sleep Screen → Custom).
- [x] `addwaddle` upload script (`tools/addwaddle`).
- [x] Waddle home theme: penguin greeting "Hei Arne!" with a random line,
      a penguin on the book tile, a penguin family at the bottom (`src/waddle/`).
- [x] GitHub Actions builds only the X4 Pro (`.github/workflows/waddle-build.yml`).
      Upstream workflows removed.
- [ ] Our own build running on the device (download `waddle-latest`, then install).

## How we work

- Work on a branch, open a PR into `waddle` in `ArneeMe/waddleos`, merge it.
- The PR build checks that the firmware compiles. A merge to `waddle` publishes
  `waddleos-x4pro.bin` to the `waddle-latest` pre-release.
- Never open anything in the upstream repo.

## Step 1: update over Wi-Fi with one command (built, not tested on the device)

Goal: run one script on Arch, and the reader installs the new firmware.

- [x] Device: `POST /api/firmware/apply` in the web server. Takes a `.bin` that
      `/upload` already put on the SD card, checks it with
      `firmware_flash::validateImageFile` (checksum, SHA, chip, board tag),
      flashes it with `firmware_flash::flashFromSdPath`, then reboots.
      See `src/network/FirmwareFlasher.h`.
- [x] The reader asks "Update firmware?" before flashing (the existing SD update screen).
- [x] Arch: `tools/waddle-update`: downloads the latest build (or takes a local
      `.bin`), checks it, uploads it and calls apply.
- [ ] First install of this build: once over USB with `esptool`, or upload the
      `.bin` and use **Settings → firmware update** on the reader.

## Step 2: design (boot and sleep)

- [x] Build target `x4pro-waddle` with version string `…-waddle`.
- [x] Home screen theme (`src/waddle/WaddleTheme.*`).
- [x] Boot and default sleep screens show the penguin and "WaddleOS"
      (`src/activities/boot_sleep/BootActivity.cpp`, `SleepActivity.cpp:705`).
      The default sleep screen is no longer inverted in our build.
- [ ] Later: grayscale penguin built into the firmware (the boot logo is 1-bit today).
- [x] README for WaddleOS, with credit to CrossPoint (`.github/README.md`).

## Step 3: privacy

Detailed draft: [phase2-privacy.md](phase2-privacy.md).

Done: features that talked to outside servers are deleted. The reader only
talks to arnee.me (news, weather, started by Arne) and the local network.

## Step 4: fun stuff

### Bluesound Node remote

- [x] "Node" on the home screen: presets as sources (Spotify, TV, Platespiller),
      volume, play/pause, next/previous (`src/waddle/NodeActivity.*`, `BluOsClient.*`).
      BluOS API on port 11000, local network only, no account.
- Node IP: `192.168.50.239` (build flag `WADDLE_NODE_IP`), or put another IP in
  `/.waddle/node-ip.txt` on the SD card.

### Hjem (home screen)

- [x] `src/waddle/WaddleHomeActivity.*` replaces CrossPoint's home screen and
      the old Waddle theme: big pixel clock and date, today's weather from yr.no,
      the book being read, the top 3 unread NRK headlines, the penguins, and
      buttons for Bibliotek, Filer, Overføring, Bluesound and Innstillinger.
- Shows the saved data at once. When it is older than 30 minutes, it joins
  Wi-Fi after 2 s on the screen, fetches `arnee.me/api/hjem.json` (weather,
  `/.waddle/hjem.json`) and the NRK paper, redraws once and turns Wi-Fi off.
  A failed fetch waits 10 minutes before the next try. Never while asleep.
- Weather place: `nyheter/vaer.mjs` in `ArneeMe/arneeme` (Oslo).

### Nyheter (NRK)

- [x] arnee.me builds an EPUB of all NRK top stories every hour
      (`ArneeMe/arneeme`: `nyheter/build.mjs`, `.github/workflows/nyheter.yml`)
      and serves it at `arnee.me/api/nrk.epub`, with the headlines in `/api/nrk.json`.
- [x] "Nyheter" from Hjem (`src/waddle/NewsActivity.*`, `NewsPaper.*`): joins Wi-Fi,
      fetches the paper to `/Nyheter/`, turns Wi-Fi off and lists the headlines.
      A headline opens the reader at that article; Back returns to the list.
      Read stories are kept in `/Nyheter/lest.txt`. The paper is not added to
      recent books.
- Skip an article: **Kontroller → Atferd ved langt trykk → Kapittelhopp**.
- Later: long reads and blogs, the same way.

## Notes for Arch Linux

- `sudo pacman -S esptool`. For building: `sudo pacman -S platformio-core`.
- Serial access: add yourself to the `uucp` group. If the browser still can't
  open the port: `sudo chmod a+rw /dev/ttyACM0` (resets on replug).
- The web flasher needs a Chromium-based browser (Vivaldi, Chromium).
  Brave has Web Serial off by default.
- Backup: `esptool --chip esp32s3 -p /dev/ttyACM0 -b 921600 read-flash 0 ALL xteink-x4pro-stock.bin`
- Wi-Fi transfer: on the reader open **File Transfer → Join a Network** and keep
  that screen open. Use the IP shown on screen, because Arch does not resolve
  `crosspoint.local` without `avahi` and `nss-mdns`.
  `curl -F file=@book.epub "http://<ip>/upload?path=/"`
- If the reader is "unreachable": the screen is closed or asleep, or the router
  blocks Wi-Fi to wired traffic ("AP Isolated" on ASUS).

## Removed from CrossPoint

Plugins, the OPDS book catalog, KOReader sync, OTA updates, LCP DRM, mDNS,
font download and web font upload, Calibre wireless, the theme setting and
other themes, and code for other devices. Lyra is the only theme.

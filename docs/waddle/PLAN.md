# WaddleOS plan

WaddleOS is a personal fork of [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader)
for the Xteink X4 Pro. Repo: `ArneeMe/waddleos`. Mascot: a pixel-art penguin.

Goals: my own look, easy updates over Wi-Fi, and nothing phones home.

How to work in this fork: see [FORK.md](FORK.md).

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
- Never open anything in the upstream repo. See [FORK.md](FORK.md).

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

Short version: hide features that talk to outside servers behind `WADDLE_PRIVATE`,
and add a host allowlist where all outbound HTTP goes through.

## Step 4: fun stuff

### arnee.me app (SD card plugin)

Plugins are folders on the SD card, no firmware build needed (`docs/sd-plugins.md`).
Enable them in **Settings → Connectivity → Plugins**.

- [ ] Catalog plugin (`device.json`) that reads a static `books.json` on arnee.me.
      Pick a book on the reader, it downloads to the SD card over Wi-Fi.
- [ ] The same catalog can list firmware `.bin` files.
- Simpler alternative: an OPDS feed on arnee.me. The reader supports OPDS already.

### Other ideas

- RSS and articles, with a host allowlist.
- Ideas from `docs/sd-plugins.md`: dictionary store, font or theme store,
  free-books catalog (Standard Ebooks, Gutenberg), read-later services.
- Updates from GitHub releases: point `src/network/OtaUpdater.cpp:22` at
  `ArneeMe/waddleos` and trim `.github/workflows/release.yml` to the X4 Pro.

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

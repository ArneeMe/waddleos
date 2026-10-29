# WaddleOS agent guide

WaddleOS is Arne's personal firmware for the **Xteink X4 Pro** e-reader
(ESP32-S3, 16 MB flash, 8 MB PSRAM, 800x480 e-ink, GT911 touch) and for the
**LilyGO T5 E-Paper S3 Pro** mesh units (960x540 e-ink, 868 MHz LoRa, GPS),
plus their gateway and Pi server. It started as
a fork of [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader)
but no longer follows upstream: change, move or delete any code freely.

Read [docs/waddle/ARCHITECTURE.md](docs/waddle/ARCHITECTURE.md) for how the system fits
together (firmware, arnee.me, the `waddle` command) and
[docs/waddle/PLAN.md](docs/waddle/PLAN.md) for what is done and what is next.

## Goals, in order

1. **Privacy.** Nothing about reading or use leaves the device by itself. No
   cloud services, telemetry, background network or device identifiers.
   Network use is local (Wi-Fi upload, Bluesound) or started by Arne.
2. **Simple.** Fewer features and less code beat options. Delete what is unused.
3. **Arne's device and taste.** Norwegian UI, the penguin, his defaults.

## Working here

- Branch from `waddle`, open a pull request into `waddle` in `ArneeMe/waddleos`.
  Never open anything in `crosspoint-reader/crosspoint-reader`.
- Commit and push feature branches without asking. Never force-push `waddle`.
- Write short, factual PR descriptions, in Norwegian.
- Compile before pushing: in a cloud session run `bash scripts/cloud-setup.sh`
  once (a few minutes), then `pio run -e x4pro-waddle -e t5s3-waddle -e heltec-gateway`
  and, for mesh changes, `bash test/mesh/run.sh`, `bash test/sim/run.sh`,
  `bash test/phone/build.sh` and the server tests. The PR build in GitHub Actions (`.github/workflows/waddle-build.yml`)
  checks again, one job per device; compiler errors show up as annotations on
  the `build (<device>)` check runs. Each PR's firmware is published as a
  `pr-<n>` pre-release that Arne installs with `waddle update --pr <n>`
  (the phone app as `waddle-mobil.apk` in the same release).
- Only Arne can flash and look at the devices, and he is not testing on them
  for now: no "test on the device" lists. `docs/waddle/TESTING.md` stays as the
  list for when he does.
- Show a preview (image mockup) before changing anything visual: T5 screens
  from `bash test/screens/run.sh`, phone screens drawn from the Kotlin layout.
- Explain things plainly; Arne is not a C++ or firmware specialist. Write to
  him in Norwegian, short and direct, without filler or AI-sounding phrasing.

## Code

- `src/` is split into `core/` (shared), `apps/` (one folder per app) and
  `products/` (one folder per device; `x4pro/` is the reader, `t5s3/` the mesh
  unit). WaddleOS base code is in `src/core/waddle/`; the mesh node (shared by
  the T5, the phone app and the simulator) is in `src/services/mesh/`. Upstream
  code may be edited directly.
- `phone/` is the Android app (the member's second device, docs/waddle/PHONE.md):
  the same mesh C++ through the NDK, Kotlin screens. It cannot be built in a
  cloud session (Android SDK downloads are blocked); CI builds it.
- `WADDLE` and `WADDLE_PRIVATE` are always on. Old `#if WADDLE` blocks may be
  simplified to plain code. A change in `core/` must build for both devices.
- User-facing text goes through `tr(STR_*)` with Norwegian in
  `lib/I18n/translations/norwegian.yaml`; WaddleOS-only text may be hardcoded
  in Norwegian.
- Memory is tight. Read `.agents/rules/hardware-resources.md` and the
  `heap-discipline` skill before adding buffers, caches or tasks. The other
  files in `.agents/` describe the display, input and activity model.

## Tools for Arne's computer (`tools/`)

- `waddle`: the one command Arne runs. It dispatches to the files below and
  does `waddle dict get` itself.
- `waddle-update` (`waddle update`): download the latest build and install it over Wi-Fi.
- `waddlebook` (`waddle book`): get an EPUB from a link or file, clean it, send it to the reader.
- `addwaddle` (`waddle add`): upload any file as it is.
- `waddle-mesh` (`waddle mesh new-unit`, `setup`, `check`): flash a new mesh unit or
  the gateway over USB, set up the units and the Pi server as one group, and
  check that every unit is well.

All use only the Python standard library or plain bash and work on Linux and macOS.

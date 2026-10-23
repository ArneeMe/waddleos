<p align="center">
  <img src="/assets/waddle/waddle-gray-big.png" width="160" alt="WaddleOS penguin">
</p>

<h1 align="center">WaddleOS</h1>

<p align="center">Personal e-reader firmware for the Xteink X4 Pro. 🐧</p>

WaddleOS is my fork of [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader),
open-source firmware for Xteink e-readers. All the reading features come from
CrossPoint. WaddleOS adds:

- A penguin home screen that says hello in Norwegian ("Hei Arne!").
- Penguin sleep screen and assets (`assets/waddle/`).
- Wi-Fi upload from the command line (`tools/addwaddle`).
- A build for the X4 Pro only.

Planned: one-command firmware updates over Wi-Fi, and a privacy build where
nothing phones home. See [the plan](/docs/waddle/PLAN.md).

## Install

1. Download [`waddleos-x4pro.bin`](https://github.com/ArneeMe/ebook-os/releases/download/waddle-latest/waddleos-x4pro.bin)
   from the `waddle-latest` pre-release. It is built from the `waddle` branch.
2. Flash it with the web flasher at <https://crosspointreader.com> (choose
   "Custom .bin"), or upload it over Wi-Fi and use **Settings → firmware update**
   on the reader.

Back up the stock firmware first. Only for units that are not USB-locked
(bought from xteink.com).

## Build

```sh
pio run -e x4pro-waddle
```

## Working in this fork

- `develop` mirrors CrossPoint. `waddle` is the WaddleOS branch.
- Read [FORK.md](/docs/waddle/FORK.md) before changing code. It explains how
  to keep merges from CrossPoint easy.

## Credits

All credit for the reader goes to the [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader)
team and the [FreeInk SDK](https://github.com/Free-Ink/freeink-sdk). MIT license,
see [LICENSE](/LICENSE).

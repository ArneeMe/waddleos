<p align="center">
  <img src="assets/waddle/waddle-gray-big.png" width="160" alt="WaddleOS penguin">
</p>

<h1 align="center">WaddleOS</h1>

<p align="center">Personal e-reader firmware for the Xteink X4 Pro. 🐧</p>

WaddleOS is my fork of [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader),
open-source firmware for Xteink e-readers. All the reading features come from
CrossPoint. WaddleOS adds:

- Hjem: clock, weather, the book you are reading and NRK headlines, with penguins.
- Nyheter: NRK's top stories as a book, fetched from arnee.me.
- A Bluesound remote.
- Penguin boot and sleep screens (`src/core/waddle/`, `assets/waddle/`).
- One command on the computer, `waddle`, for updates, books and the dictionary.
- A build for the X4 Pro only.

Privacy: nothing about what or how much you read leaves the reader by itself.
No cloud sync, telemetry, update checks, plugin uploads or network announcements.
See [the plan](docs/waddle/PLAN.md) and [privacy notes](docs/waddle/phase2-privacy.md).

## Install

1. Download [`waddleos-x4pro.bin`](https://github.com/ArneeMe/waddleos/releases/download/waddle-latest/waddleos-x4pro.bin)
   from the `waddle-latest` pre-release. It is built from the `waddle` branch.
2. Flash it with the web flasher at <https://crosspointreader.com> (choose
   "Custom .bin"), or upload it over Wi-Fi and use **Settings → firmware update**
   on the reader.

Back up the stock firmware first. Only for units that are not USB-locked
(bought from xteink.com).

## The `waddle` command

Add it to your shell once (zsh; use `~/.bashrc` for bash):

```sh
echo 'waddle() { curl -sL https://raw.githubusercontent.com/ArneeMe/waddleos/waddle/tools/waddle | python3 - "$@"; }' >> ~/.zshrc
```

Open **File Transfer → Join a Network** on the reader, then:

```sh
waddle update              # install the latest build from GitHub (or: waddle update firmware.bin)
waddle book <link or file> # get an EPUB, remove scripts and trackers, send it to /Books
waddle add [-d /folder] f  # upload any file as it is
waddle dict get            # put the English dictionary from arnee.me on the reader
```

Set `WADDLE_IP` if the reader's IP is not `192.168.50.185`.

## Build

```sh
pio run -e x4pro-waddle
```

## Working on WaddleOS

- `waddle` is the main branch. Work on a branch and open a pull request into it.
- WaddleOS started from CrossPoint but no longer follows it; see
  [AGENTS.md](AGENTS.md) for the goals and rules.

## Credits

All credit for the reader goes to the [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader)
team and the [FreeInk SDK](https://github.com/Free-Ink/freeink-sdk). MIT license,
see [LICENSE](LICENSE).

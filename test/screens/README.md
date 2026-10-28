# Screen simulator

Draws the T5 mesh unit's screens to PNG files on a computer, with the real
firmware code, so a change can be looked at before it is flashed.

    bash test/screens/run.sh            # writes build/screens/*.png

Needs g++, git and python3, no PlatformIO. The first run fetches the
freeink-sdk submodule and ArduinoJson.

The PR build does the same and uploads the pictures as the `screens` artifact
(on the run's summary page in GitHub Actions).

## What it shows

Arne's unit (member 1 of Arne, Kari, Ola and Per) at tuesday 27 October 2026,
14:32, on a trip:

| File | Screen |
| --- | --- |
| `t5-hjem.png` | Hjem |
| `t5-sover.png` | Sleep screen with the "nye meldinger" line |
| `t5-meldinger.png` | Meldinger |
| `t5-ny-gruppe.png` | Ny gruppe, Kari ticked |
| `t5-chat-alle.png`, `t5-chat-kari.png` | A conversation |
| `t5-kart.png` | Kart |
| `t5-sos-varsel.png` | An SOS from Per |
| `t5-sos.png` | My own SOS, received and seen |

## How it works

`screens.cpp` runs the real `mesh::Node`, message store and screens. Kari, Ola
and Per are played by the program: their messages, positions and answers reach
the node as real encrypted radio packets. Then each screen's `render()` is
called and the frame buffer is written as a PNG.

`stubs/` stands in for the hardware (display buffer, buttons, clock, GPS, radio,
FreeRTOS) and for SD-card fonts; `test/sim/stubs` gives the SD card (a folder,
`build/screens/sd`) and flash.

To add a screen: build its activity in `main()` in `screens.cpp`, add its
`.cpp` (and what it needs) to `build.sh`, and give it data in `story()`.

## Differences from the device

- Black and white, as these screens are on the device; grayscale passes (book
  images) are not drawn.
- Only the built-in UI fonts; no fonts from the SD card.
- Nothing is tapped or pressed, except the one tap that ticks Kari.
- Battery is always 76 %, nothing is charging.

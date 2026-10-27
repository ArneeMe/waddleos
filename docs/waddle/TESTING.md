# Things to test on the devices

Only Arne can flash and look at the devices, so every change that needs a
device check lands here. Tick a box when it works; write what went wrong under
it when it does not. Agents add to this list in the same PR as the change and
remove items once Arne has ticked them.

## Reader (X4 Pro)

From PR #29 (Oppdater fastvare) and #30 (core/apps/products, settings per app):

- [ ] Innstillinger → Oppdater fastvare: "Se etter oppdatering" finds the newest
      build, "Oppdater" downloads and installs it, the version number goes up.
- [ ] After updating: font, font size, margins, night mode and sleep time are
      the same as before.
- [ ] Change a text setting (Tekstinnstillinger) and a display setting (Skjerm),
      restart, both stayed.
- [ ] Hjem, a book, Nyheter, Bluesound and Oppdater fastvare all open.
- [ ] Sleep and wake from a book: the page comes back.
- [ ] Web settings page (Filoverføring → open the reader's address in a browser):
      settings show and can be changed.

## Mesh unit (T5 S3 Pro)

Not arrived yet. Items are added as the code for them is written.

## Gateway (Heltec V4) and server (Pi)

Items are added as the code for them is written.

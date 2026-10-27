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

Flash the gateway: plug it into the computer, `pio run -e heltec-gateway -t upload`
(hold BOOT while plugging in if it is not found). Install the server:
`sudo bash server/install.sh` on the Pi (see server/README.md).

- [ ] Gateway: `pio device monitor -e heltec-gateway` prints
      `HELLO waddle-gateway 1 gc1109` (or `kct8103l`). Note which one.
- [ ] Gateway on the Pi: `journalctl -u waddlemesh -f` says "gateway connected"
      and the status page `https://<pi>:8443/` says "tilkoblet".
- [ ] Status page opens from a phone on the home Wi-Fi, and not from mobile data.
- [ ] After a reboot of the Pi the server starts by itself.
- [ ] Next morning: a backup file in `/var/backups/waddlemesh/`.

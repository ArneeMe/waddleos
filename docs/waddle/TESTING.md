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

From PR #32 (keyboard, one version per device):

- [ ] Any text field (Wi-Fi password): the keyboard has æ, ø and å; the "Lang"
      key switches to the English layout and back.
- [ ] Oppdater fastvare shows "WaddleOS 2.<n>". After installing it says it is
      the newest; a change to only the mesh unit does not offer the reader an update.
- [ ] `waddle update --pr <n>` installs a pull request's test build.

## Mesh unit (T5 S3 Pro)

Flash the first time over USB: `waddle mesh new-unit` (needs esptool; hold
BOOT while plugging in if it is not found). It downloads the full image, flashes
it and checks the unit. Watch the log with `pio device monitor` if needed.
After that, `waddle update` and Innstillinger → Oppdater fastvare pick the T5
file by themselves.

Start-up, screen and buttons:
- [ ] It boots to the penguin, then Hjem as in `docs/waddle/mockups/t5-screens.png`.
- [ ] Text is sharp and the right way up; touch hits what you tap.
- [ ] The BOOT button sleeps and wakes; the user button moves down in lists.
- [ ] Battery % on Hjem looks right; it goes up while charging.
- [ ] The clock keeps time after a restart (RTC).
- [ ] Bøker opens the library; a book reads like on the reader.
- [ ] Innstillinger shows Modus, Sjekk meldinger hjemme / på tur, Bruk Wi-Fi
      hjemme and Del posisjon på tur.

Radio and GPS (look in the log):
- [ ] `RADIO Listening on 869.525 MHz` (not `SX1262 start failed`).
- [ ] `GPS GPS talks at ... baud` with Modus "På tur". **Write down the baud and
      the GPS lines with TXT in them**: that answers which GPS this unit has.
- [ ] Outside on "På tur": Kart shows "Du: 59,9… N …" within a few minutes.

Setup and messages (two units, or one unit and the gateway):
- [ ] Meldinger on a new unit says "ikke satt opp"; tapping it opens File Transfer.
- [ ] `waddle mesh setup` makes the network, asks for the Pi, then for each of your
      units on File Transfer its address, name and the setup code; Hjem then greets
      each by name. `~/.config/waddle/admin.json` exists and only you can read it.
- [ ] A group message arrives on the other unit within seconds; it shows
      "levert til alle", and "lest" after the other opens the chat.
- [ ] A DM reaches only that person.
- [ ] Out of range of each other but both near the gateway: messages still arrive
      (through the server) and show "lagret på server".
- [ ] One unit off, a message sent, then switched on: the message arrives soon after.
- [ ] Synk in Meldinger with the Pi reachable: "Server: tilkoblet".
- [ ] Away from home on a phone hotspot: Synk reaches the Pi through the router.

- [ ] Asleep (BOOT button), then a message from the other unit: the sleep screen
      gets a line "1 ny melding · sist fra …" without waking up (its drawing moved
      to MeshUi so the screen simulator can show it; it should look as before).
- [ ] Keyboard: æ, ø, å and the "Lang" key, as on the reader.

Modus and checks (PR #33):
- [ ] Hjem shows "Hjemme · sjekker en gang i døgnet"; tapping it switches to
      "På tur · sjekker hver time" and back. The log says `GPS` wakes on "På tur".
- [ ] Hjemme, asleep: the log says "radio asleep between checks". A message sent to
      it does not arrive until you wake it; then it arrives within ~20 seconds
      (from the Pi, or from the other unit) and Hjem says "sist sjekket <time>".
- [ ] Set "Sjekk meldinger hjemme" to "Hver time", sleep, wait an hour: the sleep
      screen gets "1 ny melding" without waking it.
- [ ] "Lytt hele tiden": messages arrive at once while asleep, as before.
- [ ] Your own SOS goes out at once in every mode. Someone else's SOS reaches a
      sleeping unit at its next check (at once with "Lytt hele tiden").

Four people and groups (PR #33):
- [ ] Four units in the network: Hjem shows "Alle" and three contacts, each
      with "hørt … siden", battery % and (on a trip) distance.
- [ ] Ticking one person in "+ Ny gruppe" opens the DM.

Groups made on the unit (PR #35):
- [ ] Meldinger → "+ Ny gruppe", tick Kari and Per, "Skriv til dem", give it a
      name: the group shows up on Kari's and Per's units within a minute, with
      that name. The fourth unit gets nothing it can read.
- [ ] Per off while the group is made and written in; switch him on: the group
      and the messages arrive.
- [ ] Tap the group's title: the members. "Forlat gruppe" on Per (tap twice):
      the others keep writing and reading, Per still sees the old messages but
      no new ones.
- [ ] "Fjern" on a member (twice): the same, without their unit doing anything.
- [ ] After updating from an older build: the log says "Removed the old group
      setup", and the unit asks to be joined (`waddle mesh join`).
- [ ] File Transfer on a T5 shows "Oppsettkode: 123 456" under the address, and
      `waddle mesh setup` asks you to compare it before going on.
- [ ] Two "Fjern" at once from two units (different people): both are out, and
      neither of them can read or write in the group afterwards.
- [ ] Make a group while one member is off, restart your own unit, switch the
      member on: the group still reaches them (at your next check).
- [ ] `waddle mesh remove <name>` with that unit switched off ("lost"): the others
      get the new member list at their next check, make new keys and still talk,
      keeping their numbers. The removed unit, switched on, can open nothing new.
- [ ] Lose one message on purpose (switch a unit off while the other sends two,
      then on just before the third): the "(!) en melding kan mangle" warning
      shows briefly or not at all, and the missing message arrives.

Joining (protocol 3: up to 32 members):
- [ ] `waddle mesh invite Ola` prints a `waddle://join/…` link and how Ola runs it.
- [ ] On a second computer (Ola's), unit on File Transfer → Join a Network:
      `waddle mesh join '<link>' --name Ola` asks the address. Only then does
      "Oppsettkode: … …" appear on the unit's screen (within 2 s), the same as
      the tool asks about; answer y. It then prints six digits that the unit
      shows too, and "Waiting for the admin".
- [ ] Answer n to the setup code question: the tool stops and the unit has
      nothing. Run it again: a new, different setup code.
- [ ] `waddle mesh approve` on your computer shows "Ola" and the same six digits;
      answer y. Within 10 s Ola's tool says "You are in!" and his unit starts.
- [ ] The other units show Ola after their next check (the new member list
      reaches them over Wi-Fi or LoRa), and a DM to Ola arrives.
- [ ] `waddle mesh members` lists everyone with numbers; Ola has the lowest free one.
- [ ] The same link a second time: the server says it is used; approve forgets it.
- [ ] The status page shows Ola by name and "Medlemsliste versjon N".
- [ ] `waddle mesh check` on Ola's computer asks his unit how it is.
- [ ] Messages to "Alle" from Ola open on every unit (his sender key reached them;
      a message that came first opens when the key comes).
- [ ] With 6+ members: Hjem shows "Alle" and the three most recent contacts;
      Meldinger lists everyone, newest first; Ny gruppe scrolls ("Vis flere").
- [ ] Kart lists the nearest first and "og N til" when they do not fit.
- [ ] `waddle mesh join` on a unit that is already in the network: it says
      "enheten er allerede med i et nettverk".
- [ ] `waddle mesh remove Ola`: after his next check Ola's unit says it is not
      in a network, also after a restart, and
      `curl http://<its address>/api/mesh/identity` (File Transfer open) shows
      another publicKey than before (new identity). `waddle mesh invite Ola` and
      approve: he is member 3 again, with his old chats, and DMs work both ways.
- [ ] Make a group of three, then switch one member's unit off for the
      invitation: when it is back it gets the group within a check.
- [ ] Taking out a SD card and reading it on a computer: nothing in /.mesh/ is
      readable (keys, `chat/*.sealed`, `packets/*.sealed`, positions, read
      counts), and no group.json. Put the card back: the conversations are all
      there.

- [ ] Gateway alone (Pi off): a unit taken out of the network sends a message;
      the gateway does not keep or pass it on (it learned the members from the
      Pi's PING while the Pi was on).

Power cuts and long messages:
- [ ] Send messages to a unit and pull its battery (or USB) a few times while
      they arrive. After each start: no messages missing, groups still there.
- [ ] With 9+ members: a group with member 9 or higher shows its name and
      "levert til alle" / "lest" correctly.
- [ ] A long message (3+ pieces) to a unit that is half out of range: it either
      arrives whole or stays "sendt"; never "levert" without the other unit
      showing it.

SOS:
- [ ] Hold the user button 3 seconds: the black SOS screen; the other unit shows
      "SOS fra …" with distance. "Avbryt SOS" ends it on both.
- [ ] The SOS screen shows "Mottatt av <name>" once the other unit has it, "og
      serveren" when the Pi has it, and "Sett av <name>" after they tap "Sett".

Battery (the spec asks for 3 days):
- [ ] Note the battery % in the morning and the evening of a normal day with the
      unit asleep in a pocket, Modus "Hjemme" (checks once a day). Tell Claude the numbers.
- [ ] The same on "På tur" (GPS on, checks every hour), and with "Lytt hele tiden".

Deploy (PR #34):
- [ ] `waddle mesh new-unit` on a new unit: flashes, then after File Transfer
      says "radio OK · GPS …".
- [ ] `waddle mesh check` lists every unit with version, radio, GPS and battery.
- [ ] Update a unit with Oppdater fastvare: after a minute the log says
      `UPDATE New build confirmed`.
- [ ] `waddle mesh server` updates the Pi and prints "Running · gateway tilkoblet".

## Gateway (Heltec V4) and server (Pi)

Flash the gateway: plug it into the computer, `waddle mesh new-unit gateway`
(hold BOOT while plugging in if it is not found). It flashes and waits for the
gateway's HELLO. Install the server:
`sudo bash server/install.sh` on the Pi (see server/README.md).

- [ ] Gateway: `pio device monitor -e heltec-gateway` prints
      `HELLO waddle-gateway 1 gc1109` (or `kct8103l`). Note which one.
- [ ] Gateway on the Pi: `journalctl -u waddlemesh -f` says "gateway connected"
      and the status page `https://<pi>:8443/` says "tilkoblet".
- [ ] Status page opens from a phone on the home Wi-Fi, and not from mobile data.
- [ ] After a reboot of the Pi the server starts by itself.
- [ ] Two units out of range of each other but both near the gateway: a message
      goes straight through (the gateway passes it on), within seconds.
- [ ] Next morning: a backup file in `/var/backups/waddlemesh/`.
- [ ] Gateway: `pio device monitor -e heltec-gateway`, type `PING <network>`
      (the "network" in `/etc/waddlemesh.json`; the gateway saves it): it
      answers `PONG 0 2 gc1109` (or `kct8103l`).
- [ ] Status page on a phone (home Wi-Fi): every unit by name, "Sist hørt",
      battery, "Hjemme" or "På tur" with how often it checks; signal for a unit
      last heard over LoRa. It reloads by itself every 30 s.
- [ ] Switch a unit to "På tur": after its next sync the page says "På tur".
- [ ] Start an SOS on a unit: a red "SOS fra <name>" at the top within a
      minute; cancel it: gone within 5 minutes.
- [ ] Standalone: stop the server (`sudo systemctl stop waddlemesh`). After
      30 s, send a message from one unit while the other is off; turn the other
      on: it gets the message from the gateway (the server is still off).
- [ ] Standalone on a power bank: move the gateway to a USB power bank, send a
      few messages and an SOS between units; the sender sees "lagret".
- [ ] Unplug and replug the power bank: the gateway still has them (back on
      the Pi, step below shows the count).
- [ ] Handover: back on the Pi, start the server. Within 10 s the log says
      "gateway handed over N packets" and the page says "leverte N pakker".
      A unit that syncs over Wi-Fi gets the messages sent while the Pi was off.
- [ ] The page says "Svarer ikke" for the gateway when it is plugged in but
      silent (e.g. hold its RST button for a while).

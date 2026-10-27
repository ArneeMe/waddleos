# Waddle mesh server

The always-on mailbox for the mesh units, on the Raspberry Pi. It hears the
units through the Heltec gateway (USB) and over HTTPS when they are on Wi-Fi,
stores every packet as it arrived, and hands units what they missed. It has no
keys and cannot read messages or positions.

Only the Python standard library; Python 3.9 or newer.

## Install on the Pi

```
git clone https://github.com/ArneeMe/waddleos && cd waddleos
sudo bash server/install.sh
```

Then `waddle mesh setup` (on Arne's computer) writes the group id and unit
keys into `/etc/waddlemesh.json`. Plug the gateway into a USB port.

- Status page (home network only): `https://<pi>:8443/`
- Logs: `journalctl -u waddlemesh -f`
- Backups: `/var/backups/waddlemesh/`, one per weekday, at 03:17.

From outside: forward TCP port 8443 on the router to the Pi, and give the
units the home address (a fixed IP or a dynamic DNS name).

## Tests

```
bash test/mesh/run.sh                                  # C++ side, writes protocol/vectors.json
cd server && python3 -m unittest discover -s tests     # Python side
```
`pip install cryptography` makes the tests also decrypt the C++ packets.

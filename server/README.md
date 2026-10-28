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

Then `waddle mesh setup` (on Arne's computer) writes the network id and the
admin's public signing key into `/etc/waddlemesh.json`, and puts the signed
member list (the roster) on the Pi. Plug the gateway into a USB port.

Who may use the server comes from the roster: a unit's token must match its
token hash there. The roster is signed by the admin's key, which stays on
Arne's computer, so the Pi cannot add anyone itself.

Commands the `waddle mesh` tool runs over SSH (`cd /opt/waddlemesh && sudo -u
waddlemesh python3 -m waddlemesh --config /etc/waddlemesh.json ...`):

- `set-network ID KEY`: the network id and the admin's Ed25519 public key (hex). Needs `sudo` (writes /etc).
- `set-roster ROSTER PACKET...`: a newer signed roster and its ROSTER packets. The
  server sends it to units whose sync shows an older one, and once over LoRa.
- `joins`: join requests waiting for `waddle mesh approve` (JSON; at most 20, kept 7 days).
- `welcome INVITE BOX`: the sealed welcome for an approved request; given out once.
- `forget INVITE`: drop a request that was turned down.

Joining needs no token: `POST /api/join` and `GET /api/join/<invitation>` are
open, and what they carry is sealed so that only the invited unit and the
admin can read it. Units get the roster at `GET /api/roster` (with their token).

- Status page (home network only): `https://<pi>:8443/`
- Logs: `journalctl -u waddlemesh -f`
- Backups: `/var/backups/waddlemesh/`, one per weekday, at 03:17.

From outside: forward TCP port 8443 on the router to the Pi, and give the
units the home address (a fixed IP or a dynamic DNS name).

## Tests

```
bash test/mesh/run.sh                                  # C++ side, writes protocol/vectors.json
cd server && python3 -m unittest discover -s tests     # Python side
python3 -m unittest discover -s tools/tests            # the waddle-mesh tool (from the repo root)
```
`pip install cryptography` makes the tests also decrypt the C++ packets.

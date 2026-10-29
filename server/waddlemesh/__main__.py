"""Waddle mesh server.

  python3 -m waddlemesh serve                     run the server
  python3 -m waddlemesh set-network ID ADMINKEY   the network id and the admin's Ed25519 public key
                                                  (hex), from `waddle mesh setup`
  python3 -m waddlemesh set-roster ROSTER PACKET...
                                                  a newer signed roster and its ROSTER packets (hex)
  python3 -m waddlemesh joins                     waiting join requests, as JSON
  python3 -m waddlemesh welcome INVITE BOX        the sealed welcome for an approved request (hex)
  python3 -m waddlemesh forget INVITE             drop a join request that was turned down
  python3 -m waddlemesh backup DEST               copy the database to DEST
  python3 -m waddlemesh make-cert                 make the TLS certificate (needs openssl)

Every command takes --config FILE (default /etc/waddlemesh.json); the file is
made on first use. The commands other than serve are run by `waddle mesh` over SSH.
"""

import json
import logging
from contextlib import closing
import os
import subprocess
import sys

from . import packet as P
from . import roster as R
from .core import Server
from .gateway import Gateway
from .store import Store
from .web import WebServer

DEFAULTS = {
    "network": None,    # set by `waddle mesh setup`, same as on the units
    "adminSign": None,  # the admin's Ed25519 public key (hex): only rosters it signed are taken
    "db": "/var/lib/waddlemesh/mesh.db",
    "gateway": "/dev/ttyACM0",
    "listen": "0.0.0.0:8443",
    "cert": "/var/lib/waddlemesh/cert.pem",
    "key": "/var/lib/waddlemesh/key.pem",
}


def load_config(path):
    cfg = dict(DEFAULTS)
    if os.path.exists(path):
        with open(path) as f:
            cfg.update(json.load(f))
    return cfg


def save_config(path, cfg):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(cfg, f, indent=2)
    os.chmod(tmp, 0o600)
    if os.path.exists(path) and os.geteuid() == 0:
        st = os.stat(path)  # run with sudo: the server's user must still read it
        os.chown(tmp, st.st_uid, st.st_gid)
    os.replace(tmp, path)


def serve(cfg):
    if cfg["network"] is None or not cfg["adminSign"]:
        sys.exit("No network in the config yet: run `waddle mesh setup` first.")
    os.makedirs(os.path.dirname(cfg["db"]), exist_ok=True)
    mesh = Server(Store(cfg["db"]), cfg["network"], bytes.fromhex(cfg["adminSign"]))
    gateway = Gateway(cfg["gateway"], mesh)
    gateway.start()
    host, port = cfg["listen"].rsplit(":", 1)
    cert = cfg["cert"] if os.path.exists(cfg["cert"]) else None
    if not cert:
        logging.warning("No certificate at %s: serving plain HTTP. Run make-cert.", cfg["cert"])
    web = WebServer((host, int(port)), mesh, gateway.status, cert, cfg["key"])
    logging.info("listening on %s", cfg["listen"])
    web.serve_forever()


def set_roster(cfg, roster_hex, packets_hex):
    """Checks the admin's signature, that the roster is newer, and that the packets
    are its ROSTER packets; then keeps both. The running server picks it up."""
    if not cfg["adminSign"]:
        print("no admin key yet: run set-network first")
        return 1
    try:
        signed = bytes.fromhex(roster_hex)
        roster = R.parse(signed, bytes.fromhex(cfg["adminSign"]))
        packets = [bytes.fromhex(p) for p in packets_hex]
        heads = [P.parse_header(p) for p in packets]
    except (ValueError, R.BadRoster, P.BadPacket) as e:
        print("not taken: %s" % e)
        return 1
    if roster.network != cfg["network"]:
        print("not taken: roster for network %d, this is %d" % (roster.network, cfg["network"]))
        return 1
    counter = 0x40000000 + roster.version
    if any(h.type != P.ROSTER or h.sender != P.SERVER or h.counter != counter or h.network != cfg["network"]
           or h.piece_count != len(packets) for h in heads) or sorted(h.piece_index for h in heads) != list(
            range(len(packets))):
        print("not taken: these are not the roster's packets")
        return 1
    with closing(Store(cfg["db"])) as store:
        return _keep_roster(store, cfg, roster, signed, packets)


def _keep_roster(store, cfg, roster, signed, packets):
    # A roster of an earlier network (another admin key or id) is simply replaced.
    old = store.roster()
    try:
        replace = old is not None and R.parse(old[1], bytes.fromhex(cfg["adminSign"])).network != roster.network
    except R.BadRoster:
        replace = True
    if not store.set_roster(roster.version, signed, packets, replace):
        print("not taken: version %d is not newer than the one the server has" % roster.version)
        return 1
    print("Roster version %d with %d members kept" % (roster.version, len(roster.members)))
    return 0


def main(argv):
    path = "/etc/waddlemesh.json"
    if "--config" in argv:
        i = argv.index("--config")
        path = argv[i + 1]
        del argv[i:i + 2]
    if not argv or argv[0] in ("-h", "--help", "help"):
        print(__doc__)
        return 0
    cfg = load_config(path)
    cmd = argv[0]
    if cmd == "serve":
        logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(message)s")
        serve(cfg)
    elif cmd == "set-network" and len(argv) == 3:
        network, key = int(argv[1]), bytes.fromhex(argv[2])
        if not 0 <= network <= 255 or len(key) != 32:
            print("expected a network id (0-255) and a 32-byte key in hex")
            return 1
        cfg["network"], cfg["adminSign"] = network, key.hex()
        save_config(path, cfg)
        print("Network %d saved. Restart: sudo systemctl restart waddlemesh" % network)
    elif cmd == "set-roster" and len(argv) >= 3:
        return set_roster(cfg, argv[1], argv[2:])
    elif cmd == "joins" and len(argv) == 1:
        with closing(Store(cfg["db"])) as store:
            print(json.dumps(store.joins(), indent=1))
    elif cmd == "welcome" and len(argv) == 3:
        invite, box = bytes.fromhex(argv[1]), bytes.fromhex(argv[2])
        if len(invite) != 8 or not box:
            print("expected an invitation id (8 bytes) and the sealed welcome, in hex")
            return 1
        with closing(Store(cfg["db"])) as store:
            store.add_welcome(invite.hex(), box.hex())
        print("Welcome kept for invitation %s" % invite.hex())
    elif cmd == "forget" and len(argv) == 2:
        with closing(Store(cfg["db"])) as store:
            store.forget_join(bytes.fromhex(argv[1]).hex())
    elif cmd == "backup" and len(argv) == 2:
        with closing(Store(cfg["db"])) as store:
            store.backup(argv[1])
    elif cmd == "make-cert":
        os.makedirs(os.path.dirname(cfg["cert"]), exist_ok=True)
        subprocess.run(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1",
                        "-nodes", "-days", "3650", "-subj", "/CN=waddlemesh", "-keyout", cfg["key"],
                        "-out", cfg["cert"]], check=True)
        os.chmod(cfg["key"], 0o600)
        out = subprocess.run(["openssl", "x509", "-in", cfg["cert"], "-noout", "-fingerprint", "-sha256"],
                             check=True, capture_output=True, text=True).stdout
        print("Certificate made. The units check this fingerprint:")
        print(out.split("=", 1)[1].strip().replace(":", "").lower())
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

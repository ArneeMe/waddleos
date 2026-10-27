"""Waddle mesh server.

  python3 -m waddlemesh serve [--config FILE]     run the server
  python3 -m waddlemesh add-unit N [--config F]   make a key for member N (1-3)
  python3 -m waddlemesh backup DEST [--config F]  copy the database to DEST
  python3 -m waddlemesh make-cert [--config F]    make the TLS certificate (needs openssl)

The config file (default /etc/waddlemesh.json) is made on first use.
"""

import json
import logging
import os
import secrets
import subprocess
import sys

from .core import Server
from .gateway import Gateway
from .store import Store
from .web import WebServer, token_hash

DEFAULTS = {
    "group": None,  # set by `waddle mesh setup`, same as on the units
    "db": "/var/lib/waddlemesh/mesh.db",
    "gateway": "/dev/ttyACM0",
    "listen": "0.0.0.0:8443",
    "cert": "/var/lib/waddlemesh/cert.pem",
    "key": "/var/lib/waddlemesh/key.pem",
    "tokens": {},
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
    os.replace(tmp, path)


def serve(cfg):
    if cfg["group"] is None:
        sys.exit("No group id in the config yet: run `waddle mesh setup` first.")
    os.makedirs(os.path.dirname(cfg["db"]), exist_ok=True)
    mesh = Server(Store(cfg["db"]), cfg["group"])
    gateway = Gateway(cfg["gateway"], mesh)
    gateway.start()
    host, port = cfg["listen"].rsplit(":", 1)
    cert = cfg["cert"] if os.path.exists(cfg["cert"]) else None
    if not cert:
        logging.warning("No certificate at %s: serving plain HTTP. Run make-cert.", cfg["cert"])

    def gateway_status():
        return ("tilkoblet (%s)" % gateway.version) if gateway.connected else "ikke tilkoblet"

    tokens = {int(k): v for k, v in cfg["tokens"].items()}
    web = WebServer((host, int(port)), mesh, tokens, gateway_status, cert, cfg["key"])
    logging.info("listening on %s", cfg["listen"])
    web.serve_forever()


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
    elif cmd == "add-unit" and len(argv) == 2 and argv[1] in ("1", "2", "3"):
        token = secrets.token_hex(16)
        cfg["tokens"][argv[1]] = token_hash(token)
        save_config(path, cfg)
        print(token)
    elif cmd == "backup" and len(argv) == 2:
        Store(cfg["db"]).backup(argv[1])
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

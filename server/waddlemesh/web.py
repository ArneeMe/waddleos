"""HTTPS for units on Wi-Fi, and a small status page for Arne's home network.

POST /api/packets       units: length-prefixed packets in, length-prefixed packets out
GET  /api/roster        units: the newest signed roster
POST /api/join          a newcomer's sealed request (no token: the invitation's password proves it)
GET  /api/join/<id>     the sealed welcome, once the admin let them in (404 before)
GET  /                  status page (only from private addresses: the home network)
GET  /api/status        the same as JSON
"""

import html
import ipaddress
import json
import logging
import ssl
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from . import packet as P

log = logging.getLogger("waddlemesh.web")

MAX_BODY = 64 * 1024
MAX_JOIN_BODY = 4096
JOINS_PER_ADDRESS = 3  # join requests per address per hour
MAX_JOIN_BOX = 1024  # bytes, sealed


def _hex(value, size=None, max_size=None):
    """value as bytes when it is hex of the right size, else None."""
    try:
        raw = bytes.fromhex(value)
    except (TypeError, ValueError):
        return None
    if (size is not None and len(raw) != size) or (max_size is not None and not 0 < len(raw) <= max_size):
        return None
    return raw


class Handler(BaseHTTPRequestHandler):
    server_version = "waddlemesh"
    sys_version = ""

    def log_message(self, fmt, *args):
        log.debug("%s %s", self.client_address[0], fmt % args)

    def _send(self, code, body, ctype="application/octet-stream"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _member(self):
        """The unit the bearer token belongs to, or None."""
        auth = self.headers.get("Authorization", "")
        if not auth.startswith("Bearer "):
            return None
        return self.server.mesh.member_for_token(auth[7:].strip())

    def _local(self):
        try:
            return ipaddress.ip_address(self.client_address[0]).is_private
        except ValueError:
            return False

    def _json(self, code, doc):
        self._send(code, json.dumps(doc).encode(), "application/json")

    def _length(self, limit):
        """The body's length, or None when missing, negative or over limit."""
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            return None
        return length if 0 <= length <= limit else None

    def _join(self):
        """POST /api/join {"invite", "from", "box"}: kept for the admin's `waddle mesh approve`."""
        length = self._length(MAX_JOIN_BODY)
        if length is None:
            return self._send(413, b"")
        if not self._local() and not self.server.allow_join(self.client_address[0]):
            return self._json(429, {"error": "too many requests"})
        try:
            doc = json.loads(self.rfile.read(length))
        except ValueError:
            return self._send(400, b"")
        if not isinstance(doc, dict):
            return self._send(400, b"")
        invite, unit, box = _hex(doc.get("invite"), 8), _hex(doc.get("from"), 32), _hex(doc.get("box"), None,
                                                                                         MAX_JOIN_BOX)
        if invite is None or unit is None or box is None:
            return self._send(400, b"")
        result = self.server.mesh.store.add_join(invite.hex(), unit.hex(), box.hex())
        if result == "taken":
            return self._json(409, {"error": "invitation already used"})
        if result == "full":
            return self._json(429, {"error": "too many waiting"})
        log.info("join request for invitation %s", invite.hex())
        self._json(200, {})

    def do_POST(self):
        if self.path == "/api/join":
            return self._join()
        if self.path != "/api/packets":
            return self._send(404, b"")
        member = self._member()
        if member is None:
            time.sleep(1)  # slow down guessing
            return self._send(401, b"")
        length = self._length(MAX_BODY)
        if length is None:
            return self._send(413, b"")
        try:
            packets = P.unframe(self.rfile.read(length))
        except P.BadPacket:
            return self._send(400, b"")
        answers = []
        for raw in packets:
            try:
                h = P.parse_header(raw)
            except P.BadPacket:
                continue
            if P.person(h.sender) != member:
                continue  # a unit only speaks for itself
            answers += self.server.mesh.handle(raw, "wifi")
        self._send(200, P.frame(answers))

    def do_GET(self):
        if self.path.startswith("/api/join/"):
            invite = _hex(self.path[len("/api/join/"):], 8)
            box = invite and self.server.mesh.store.take_welcome(invite.hex())
            return self._json(200, {"box": box}) if box else self._send(404, b"")
        if self.path == "/api/roster":
            if self._member() is None:
                time.sleep(1)
                return self._send(401, b"")
            stored = self.server.mesh.store.roster()
            return self._send(200, stored[1]) if stored else self._send(404, b"")
        if not self._local():
            return self._send(404, b"")
        status = self.server.mesh.status()
        status["gateway"] = self.server.gateway_status()
        if self.path == "/api/status":
            return self._send(200, json.dumps(status).encode(), "application/json")
        if self.path != "/":
            return self._send(404, b"")
        self._send(200, status_page(status).encode(), "text/html; charset=utf-8")


# --- Status page ---------------------------------------------------------------

VIA = {"lora": "LoRa", "wifi": "Wi-Fi"}
COUNT_NAMES = {"MSG": "meldinger", "POS": "posisjoner", "SOS": "SOS", "ACK": "kvitteringer"}

STYLE = """
:root{--bg:#f4f1ea;--card:#fff;--ink:#1d1d1b;--dim:#6b675e;--line:#e3ded2;--ok:#2f7d4f;--warn:#b26b00;--sos:#c4161c}
@media (prefers-color-scheme:dark){:root{--bg:#161614;--card:#22221f;--ink:#eeebe3;--dim:#a29d91;--line:#35342f;
--ok:#5cbf83;--warn:#e0a03a}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);font:16px/1.45 system-ui,-apple-system,"Segoe UI",sans-serif}
main{max-width:40rem;margin:0 auto;padding:16px}
header{display:flex;justify-content:space-between;align-items:baseline;gap:1rem;margin:.5rem 0 1rem}
h1{font-size:1.5rem;margin:0}
h2{font-size:.8rem;text-transform:uppercase;letter-spacing:.06em;color:var(--dim);margin:1.5rem 0 .5rem}
.dim{color:var(--dim)}.small{font-size:.875rem}
.banner{background:var(--sos);color:#fff;border-radius:14px;padding:1rem 1.2rem;margin:0 0 .75rem}
.banner strong{display:block;font-size:1.6rem;line-height:1.2}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:1rem 1.2rem;margin:0 0 .75rem}
.card.alert{border:2px solid var(--sos)}
.top{display:flex;justify-content:space-between;align-items:baseline;gap:1rem}
.name{font-size:1.2rem;font-weight:600}
.state{margin:.3rem 0 0}
.dot{display:inline-block;width:.65em;height:.65em;border-radius:50%;margin-right:.45em;background:var(--line)}
.dot.ok{background:var(--ok)}.dot.warn{background:var(--warn)}.dot.sos{background:var(--sos)}
dl{display:grid;grid-template-columns:auto 1fr;gap:.25rem 1rem;margin:.75rem 0 0}
dt{color:var(--dim)}dd{margin:0}
.tag{display:inline-block;border-radius:999px;padding:0 .6em;font-size:.875rem;background:var(--line)}
.tag.trip{background:var(--warn);color:#fff}
.low{color:var(--sos);font-weight:600}
footer{margin:2rem 0 1rem;font-size:.875rem;color:var(--dim)}
"""


def ago(seconds):
    """How long ago, in Norwegian."""
    if seconds is None:
        return "aldri"
    if seconds < 60:
        return "nå nettopp"
    if seconds < 3600:
        return "for %d min siden" % (seconds // 60)
    if seconds < 86400:
        return "for %d t siden" % (seconds // 3600)
    return "for %d d siden" % (seconds // 86400)


def duration(seconds):
    d, h, m = seconds // 86400, seconds // 3600 % 24, seconds // 60 % 60
    return "%d d %d t" % (d, h) if d else ("%d t %d min" % (h, m) if h else "%d min" % m)


def number(x, decimals=0):
    """Norwegian style: a decimal comma and a real minus sign."""
    return ("%.*f" % (decimals, x)).replace(".", ",").replace("-", "−")


def _rows(rows):
    return "".join("<dt>%s</dt><dd>%s</dd>" % r for r in rows)


def _unit_card(u):
    heard = u["ago_s"] is not None
    dot = "sos" if u["sos"] else ("ok" if heard and u["ago_s"] < 3 * 3600 else ("warn" if heard else ""))
    seen = ago(u["ago_s"])
    if heard and u["via"]:
        seen += " via " + VIA.get(u["via"], u["via"])
    rows = [("Sist hørt", html.escape(seen))]
    # The signal is from the last packet heard over LoRa; over Wi-Fi there is none.
    if heard and u["via"] == "lora" and u["rssi"] is not None:
        signal = "%s dBm" % number(u["rssi"])
        if u["snr"] is not None:
            signal += " · SNR %s dB" % number(u["snr"], 1)
        rows.append(("Signal", signal))
    if u["battery"] is not None:
        rows.append(("Batteri", '<span class="%s">%d %%</span>' % ("low" if u["battery"] <= 20 else "", u["battery"])))
    if u["on_trip"] is not None:
        mode = '<span class="tag trip">På tur</span>' if u["on_trip"] else '<span class="tag">Hjemme</span>'
        if u["check"]:
            check = u["check"] if u["check"].startswith("lytter") else "sjekker " + u["check"]
            mode += ' <span class="dim small">%s</span>' % html.escape(check)
        rows.append(("Modus", mode))
    if u["build"]:
        rows.append(("Fastvare", html.escape(u["build"])))
    return ('<section class="card%s"><div class="top"><span class="name"><span class="dot %s"></span>%s</span>'
            '<span class="dim small">nr. %d</span></div><dl>%s</dl></section>\n' % (
                " alert" if u["sos"] else "", dot, html.escape(u["name"]), u["member"], _rows(rows)))


def _gateway_card(gw, now):
    if gw["answering"]:
        dot, state = "ok", "Tilkoblet og svarer"
    elif gw["connected"]:
        dot, state = "warn", "Svarer ikke. Uten Pi-en klarer den seg alene og tar vare på det den hører."
    else:
        dot, state = "", "Ikke tilkoblet"
    rows = []
    if gw["version"]:
        rows.append(("Versjon", html.escape(gw["version"])))
    if gw["kept"]:
        rows.append(("Lagret alene", "%d pakker, hentes nå" % gw["kept"]))
    last = gw["last_handover"]
    if last:
        rows.append(("Sist alene", "leverte %d pakker %s (%d i alt)" % (
            last["packets"], ago(now - last["at"]), gw["handed_over"])))
    else:
        rows.append(("Sist alene", "ikke siden serveren startet"))
    return ('<section class="card"><div class="top"><span class="name"><span class="dot %s"></span>Gateway</span>'
            '</div><p class="state small">%s</p><dl>%s</dl></section>\n' % (dot, state, _rows(rows)))


def status_page(status, now=None):
    """Phone-sized, Norwegian, no scripts or outside files; reloads every 30 s."""
    now = int(now or time.time())
    banners = "".join(
        '<div class="banner"><strong>SOS fra %s</strong>Sist hørt %s. Se posisjonen på en enhet.</div>\n' % (
            html.escape(u["name"]), html.escape(ago(u["ago_s"]))) for u in status["units"] if u["sos"])
    units = "".join(_unit_card(u) for u in status["units"]) or '<p class="dim">Ingen enheter ennå.</p>'
    counts = ", ".join("%d %s" % (n, COUNT_NAMES.get(t, t)) for t, n in status["packets"].items())
    rows = [("Lagret", counts or "ingenting ennå"), ("Oppe i", duration(status["uptime_s"]))]
    if status.get("roster"):
        rows.insert(0, ("Medlemsliste", "versjon %d" % status["roster"]))
    server = _rows(rows)
    return """<!doctype html><html lang="no"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="refresh" content="30">
<title>%sWaddle mesh</title><style>%s</style></head><body><main>
<header><h1>Waddle mesh</h1><span class="dim small">kl. %s</span></header>
%s<h2>Enheter</h2>
%s<h2>Gateway og server</h2>
%s<section class="card"><div class="top"><span class="name">Server</span></div><dl>%s</dl></section>
<footer>Serveren lagrer bare krypterte pakker og kan ikke lese meldinger eller posisjoner.
Siden oppdateres hvert 30. sekund.</footer>
</main></body></html>""" % ("SOS · " if banners else "", STYLE, time.strftime("%H:%M", time.localtime(now)),
                             banners, units, _gateway_card(status["gateway"], now), server)


class WebServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address, mesh, gateway_status, cert=None, key=None):
        super().__init__(address, Handler)
        self.mesh = mesh
        self.gateway_status = gateway_status
        self.join_times = {}  # address -> times of its join requests in the last hour
        self.join_lock = threading.Lock()
        if cert:
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            ctx.minimum_version = ssl.TLSVersion.TLSv1_2
            ctx.load_cert_chain(cert, key)
            # Handshake in the request's own thread, so a slow unit blocks nobody.
            self.socket = ctx.wrap_socket(self.socket, server_side=True, do_handshake_on_connect=False)

    def allow_join(self, address, now=None):
        """At most JOINS_PER_ADDRESS join requests per internet address per hour,
        so nobody fills the waiting list from outside."""
        now = now or time.time()
        with self.join_lock:
            times = [t for t in self.join_times.get(address, []) if now - t < 3600]
            if len(times) >= JOINS_PER_ADDRESS:
                self.join_times[address] = times
                return False
            self.join_times[address] = times + [now]
            if len(self.join_times) > 1000:  # forget quiet addresses
                self.join_times = {a: t for a, t in self.join_times.items() if t and now - t[-1] < 3600}
            return True

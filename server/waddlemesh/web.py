"""HTTPS for units on Wi-Fi, and a small status page for Arne's home network.

POST /api/packets   units: length-prefixed packets in, length-prefixed packets out
GET  /              status page (only from private addresses: the home network)
GET  /api/status    the same as JSON
"""

import hashlib
import hmac
import html
import ipaddress
import json
import logging
import ssl
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from . import packet as P

log = logging.getLogger("waddlemesh.web")

MAX_BODY = 64 * 1024


def token_hash(token):
    return hashlib.sha256(token.encode()).hexdigest()


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
        digest = token_hash(auth[7:].strip())
        for member, expected in self.server.tokens.items():
            if hmac.compare_digest(digest, expected):
                return member
        return None

    def _local(self):
        try:
            return ipaddress.ip_address(self.client_address[0]).is_private
        except ValueError:
            return False

    def do_POST(self):
        if self.path != "/api/packets":
            return self._send(404, b"")
        member = self._member()
        if member is None:
            time.sleep(1)  # slow down guessing
            return self._send(401, b"")
        length = int(self.headers.get("Content-Length", "0"))
        if length > MAX_BODY:
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
            if h.sender != member:
                continue  # a unit only speaks for itself
            answers += self.server.mesh.handle(raw, "wifi")
        self._send(200, P.frame(answers))

    def do_GET(self):
        if not self._local():
            return self._send(404, b"")
        status = self.server.mesh.status()
        status["gateway"] = self.server.gateway_status()
        if self.path == "/api/status":
            return self._send(200, json.dumps(status).encode(), "application/json")
        if self.path != "/":
            return self._send(404, b"")
        self._send(200, status_page(status).encode(), "text/html; charset=utf-8")


def status_page(status):
    rows = []
    for u in status["units"]:
        ago = u["ago_s"]
        seen = "aldri" if ago is None else ("%d min siden" % (ago // 60) if ago < 86400 else "%d d siden" % (ago // 86400))
        rows.append("<tr><td>%d</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td></tr>" % (
            u["member"], seen, html.escape(u["via"] or ""), "" if u["battery"] is None else "%d %%" % u["battery"],
            html.escape(u["build"] or ""), "" if u["rssi"] is None else "%d dBm" % u["rssi"]))
    names = {P.MSG: "meldinger", P.POS: "posisjoner", P.SOS: "SOS"}
    counts = ", ".join("%d %s" % (n, names.get(t, "andre")) for t, n in sorted(status["packets"].items()))
    gw = status["gateway"]
    return """<!doctype html><html lang="no"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Waddle mesh</title>
<style>body{font:16px system-ui;margin:2em;max-width:50em}td,th{padding:.3em .8em;text-align:left}</style>
<h1>Waddle mesh</h1>
<p>Gateway: %s. Lagret: %s.</p>
<table><tr><th>Enhet</th><th>Sist sett</th><th>Via</th><th>Batteri</th><th>Versjon</th><th>Signal</th></tr>%s</table>
<p>Serveren lagrer bare krypterte pakker og kan ikke lese meldinger.</p>
</html>""" % (html.escape(gw), counts or "ingenting ennå", "".join(rows))


class WebServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address, mesh, tokens, gateway_status, cert=None, key=None):
        super().__init__(address, Handler)
        self.mesh = mesh
        self.tokens = tokens  # member -> sha256 hex of its token
        self.gateway_status = gateway_status
        if cert:
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            ctx.minimum_version = ssl.TLSVersion.TLSv1_2
            ctx.load_cert_chain(cert, key)
            # Handshake in the request's own thread, so a slow unit blocks nobody.
            self.socket = ctx.wrap_socket(self.socket, server_side=True, do_handshake_on_connect=False)

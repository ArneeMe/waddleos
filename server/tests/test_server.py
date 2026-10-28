"""Server tests. Run from the repo root: python3 -m unittest discover -s server/tests -t server

Reads protocol/vectors.json, written by the C++ tests (test/mesh/run.sh), so the
two sides are checked against each other.
"""

import hashlib
import json
import os
import shutil
import ssl
import subprocess
import tempfile
import threading
import unittest
import urllib.error
import urllib.request

from waddlemesh import packet as P
from waddlemesh.core import Server
from waddlemesh.store import Store
from waddlemesh.web import WebServer, token_hash

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
with open(os.path.join(ROOT, "protocol", "vectors.json")) as f:
    V = json.load(f)

GROUP = 0x42


def packets(name):
    case = next(m for m in V["messages"] if m["name"] == name)
    return [bytes.fromhex(p) for p in case["packets"]], bytes.fromhex(case["plain"])


class PacketTests(unittest.TestCase):
    def test_headers_of_cpp_packets(self):
        pkts, _ = packets("group message")
        h = P.parse_header(pkts[0])
        self.assertEqual((h.type, h.group, h.sender, h.recipient, h.counter), (P.MSG, GROUP, 1, P.GROUP, 10))
        self.assertEqual(h.pack(), pkts[0][:P.HEADER_SIZE])
        dm, _ = packets("direct message in 3 pieces")
        heads = [P.parse_header(p) for p in dm]
        self.assertEqual([x.piece_index for x in heads], [0, 1, 2])
        self.assertTrue(all(x.fragmented and x.piece_count == 3 and x.recipient == 2 for x in heads))

    def test_ack_and_sync(self):
        ack = bytes.fromhex(V["ack"])
        h = P.parse_header(ack)
        self.assertEqual((h.type, h.sender, h.recipient), (P.ACK, P.SERVER, 1))
        a = P.parse_ack(ack[P.HEADER_SIZE:])
        self.assertEqual((a.sender, a.counter, a.state), (1, 10, P.STORED))
        sync = bytes.fromhex(V["syncReq"])
        req = P.parse_sync_req(sync[P.HEADER_SIZE:])
        self.assertEqual(req.last_counter, {1: 10, 2: 0, 3: 7})
        self.assertEqual((req.battery, req.build), (64, 0x2A))
        self.assertEqual(req.pack(), sync[P.HEADER_SIZE:])

    def test_bad_packets(self):
        good = packets("group message")[0][0]
        for bad in (b"", good[:5], b"\x00" + good[1:], good[:4] + b"\x09" + good[5:], good + bytes(200)):
            with self.assertRaises(P.BadPacket):
                P.parse_header(bad)

    def test_framing(self):
        pkts, _ = packets("direct message in 3 pieces")
        self.assertEqual(P.unframe(P.frame(pkts)), pkts)
        with self.assertRaises(P.BadPacket):
            P.unframe(b"\x05abc")

    def test_airtime_matches_cpp(self):
        self.assertEqual(P.airtime_ms(30), 260)
        self.assertEqual(P.airtime_ms(200), 956)


try:
    from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
    from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
    from cryptography.hazmat.primitives import serialization
    HAVE_CRYPTO = True
except ImportError:
    HAVE_CRYPTO = False


@unittest.skipUnless(HAVE_CRYPTO, "pip install cryptography to cross-check the encryption")
class CryptoCrossCheck(unittest.TestCase):
    """Decrypts the C++ packets with an independent implementation."""

    def open(self, pkts, key):
        h = P.parse_header(pkts[0])
        joined = b"".join(p[P.HEADER_SIZE:] for p in pkts)
        return ChaCha20Poly1305(key).decrypt(h.nonce(), joined, h.aad())

    def test_group_message(self):
        pkts, plain = packets("group message")
        self.assertEqual(self.open(pkts, bytes.fromhex(V["senderKey"])), plain)
        self.assertEqual(plain[8:], b"Hei fra skogen!")

    def test_position(self):
        pkts, plain = packets("position")
        self.assertEqual(self.open(pkts, bytes.fromhex(V["senderKey"])), plain)

    def test_direct_message_and_pair_key(self):
        a, b = V["memberA"], V["memberB"]
        for m in (a, b):
            sk = X25519PrivateKey.from_private_bytes(bytes.fromhex(m["secret"]))
            pk = sk.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
            self.assertEqual(pk.hex(), m["public"])
        shared = X25519PrivateKey.from_private_bytes(bytes.fromhex(a["secret"])).exchange(
            X25519PublicKey.from_public_bytes(bytes.fromhex(b["public"])))
        lo, hi = sorted([bytes.fromhex(a["public"]), bytes.fromhex(b["public"])])
        key = hashlib.blake2b(shared + lo + hi, digest_size=32).digest()
        self.assertEqual(key.hex(), V["pairKey"])
        pkts, plain = packets("direct message in 3 pieces")
        self.assertEqual(self.open(pkts, key), plain)
        group_plain = packets("group message")[1]
        self.assertEqual(plain[4:8], hashlib.blake2b(group_plain, digest_size=32).digest()[:4])

    def test_tampering_is_caught(self):
        pkts, _ = packets("group message")
        bad = bytearray(pkts[0])
        bad[-1] ^= 1
        with self.assertRaises(Exception):
            self.open([bytes(bad)], bytes.fromhex(V["senderKey"]))


def sync_req(member, last, counter=1):
    h = P.Header(type=P.SYNC_REQ, group=GROUP, sender=member, recipient=P.SERVER, counter=counter)
    return h.pack() + P.SyncReq(last, battery=50, build=0x2A).pack()


class ServerTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.store = Store(os.path.join(self.dir, "mesh.db"))
        self.server = Server(self.store, GROUP)

    def tearDown(self):
        shutil.rmtree(self.dir)

    def test_message_is_stored_and_acked(self):
        pkts, _ = packets("group message")
        answers = self.server.handle(pkts[0], "lora", -80, 7.5)
        self.assertEqual(len(answers), 1)
        a = P.parse_ack(answers[0][P.HEADER_SIZE:])
        self.assertEqual((a.sender, a.counter, a.state), (1, 10, P.STORED))
        self.assertEqual(P.parse_header(answers[0]).recipient, 1)

    def test_pieces_ack_once_complete(self):
        dm, _ = packets("direct message in 3 pieces")
        self.assertEqual(self.server.handle(dm[2], "lora"), [])
        self.assertEqual(self.server.handle(dm[0], "lora"), [])
        self.assertEqual(len(self.server.handle(dm[1], "lora")), 1)

    def test_sync_gives_what_each_member_may_read(self):
        group, _ = packets("group message")
        dm, _ = packets("direct message in 3 pieces")
        pos, _ = packets("position")
        for p in group + dm + pos:
            self.server.handle(p, "lora")
        to_two = self.server.handle(sync_req(2, {1: 0, 2: 0, 3: 0}), "wifi")
        self.assertEqual(to_two[:4], group + dm)
        self.assertIn(pos[0], to_two)
        to_three = self.server.handle(sync_req(3, {1: 0, 2: 0, 3: 0}), "wifi")
        self.assertIn(group[0], to_three)
        self.assertFalse(any(p in to_three for p in dm))  # a DM between 1 and 2
        caught_up = self.server.handle(sync_req(2, {1: 11, 2: 0, 3: 0}, counter=2), "wifi")
        self.assertFalse(any(P.parse_header(p).type == P.MSG for p in caught_up))
        units = {u["member"]: u for u in self.store.units()}
        self.assertEqual((units[2]["battery"], units[2]["build"], units[2]["via"]), (50, "2a", "wifi"))

    def test_lora_sync_is_limited(self):
        for counter in range(1, 30):
            h = P.Header(type=P.MSG, group=GROUP, sender=1, recipient=P.GROUP, counter=counter)
            self.server.handle(h.pack() + bytes(20), "lora")
        self.assertEqual(len(self.server.handle(sync_req(2, {1: 0, 2: 0, 3: 0}), "lora")), 12)
        self.assertEqual(len(self.server.handle(sync_req(2, {1: 0, 2: 0, 3: 0}, 2), "wifi")), 29)

    def test_sos_is_answered(self):
        h = P.Header(type=P.SOS, group=GROUP, sender=2, recipient=P.GROUP, counter=40)
        answers = self.server.handle(h.pack() + bytes(32), "lora")
        self.assertEqual(len(answers), 1)
        a = P.parse_ack(answers[0][P.HEADER_SIZE:])
        self.assertEqual((a.sender, a.counter, a.state), (2, 40, P.STORED))

    def test_other_groups_and_server_packets_are_ignored(self):
        pkts, _ = packets("group message")
        other = bytearray(pkts[0])
        other[2] = 0x43
        self.assertEqual(self.server.handle(bytes(other), "lora"), [])
        self.assertEqual(self.server.handle(bytes.fromhex(V["ack"]), "lora"), [])
        self.assertEqual(self.store.counts(), {})

    def test_lora_packets_are_relayed_once(self):
        sent = []

        class Radio:
            def send(self, raw):
                sent.append(raw)

        self.server.radio = Radio()
        pkts, _ = packets("group message")
        self.server.handle(pkts[0], "lora")
        self.server.handle(pkts[0], "lora")
        self.assertEqual(len(sent), 1)
        self.assertEqual(P.parse_header(sent[0]).hops, P.DEFAULT_HOPS - 1)
        self.assertEqual(sent[0][4:], pkts[0][4:])  # only the hops changed

    def test_wifi_packets_go_out_over_lora(self):
        sent = []

        class Radio:
            def send(self, raw):
                sent.append(raw)

        self.server.radio = Radio()
        pkts, _ = packets("group message")
        self.server.handle(pkts[0], "wifi")
        self.server.handle(pkts[0], "wifi")  # a repeat is not sent again
        self.assertEqual(sent, [pkts[0]])

    def test_backup(self):
        pkts, _ = packets("group message")
        self.server.handle(pkts[0], "lora")
        copy = os.path.join(self.dir, "copy.db")
        self.store.backup(copy)
        self.assertEqual(Store(copy).counts(), {P.MSG: 1})


class WebTests(unittest.TestCase):
    def start(self, cert=None, key=None):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir)
        self.mesh = Server(Store(os.path.join(self.dir, "mesh.db")), GROUP)
        self.web = WebServer(("127.0.0.1", 0), self.mesh, {1: token_hash("secret-1")}, lambda: "test", cert, key)
        threading.Thread(target=self.web.serve_forever, daemon=True).start()
        self.addCleanup(self.web.server_close)
        self.addCleanup(self.web.shutdown)
        return "%s://127.0.0.1:%d" % ("https" if cert else "http", self.web.server_address[1])

    def post(self, base, body, token, context=None):
        req = urllib.request.Request(base + "/api/packets", data=body, method="POST",
                                     headers={"Authorization": "Bearer " + token})
        with urllib.request.urlopen(req, timeout=5, context=context) as r:
            return r.read()

    def test_packets_over_http(self):
        base = self.start()
        pkts, _ = packets("group message")
        answers = P.unframe(self.post(base, P.frame([pkts[0]]), "secret-1"))
        self.assertEqual(P.parse_ack(answers[0][P.HEADER_SIZE:]).state, P.STORED)
        with self.assertRaises(urllib.error.HTTPError) as e:
            self.post(base, P.frame([pkts[0]]), "wrong")
        self.assertEqual(e.exception.code, 401)

    def test_unit_cannot_send_as_another(self):
        base = self.start()
        dm, _ = packets("direct message in 3 pieces")
        h = P.parse_header(dm[0])
        forged = P.Header(type=P.MSG, group=GROUP, sender=2, recipient=1, counter=h.counter).pack() + dm[0][11:]
        self.assertEqual(self.post(base, P.frame([forged]), "secret-1"), b"")
        self.assertEqual(self.mesh.store.counts(), {})

    def test_status_page(self):
        base = self.start()
        with urllib.request.urlopen(base + "/", timeout=5) as r:
            self.assertIn("Waddle mesh", r.read().decode())
        with urllib.request.urlopen(base + "/api/status", timeout=5) as r:
            self.assertEqual(json.load(r)["gateway"], "test")

    @unittest.skipUnless(shutil.which("openssl"), "needs openssl")
    def test_https(self):
        d = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, d)
        cert, key = os.path.join(d, "cert.pem"), os.path.join(d, "key.pem")
        subprocess.run(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1",
                        "-nodes", "-days", "1", "-subj", "/CN=waddlemesh", "-keyout", key, "-out", cert],
                       check=True, capture_output=True)
        base = self.start(cert, key)
        ctx = ssl.create_default_context(cafile=cert)
        ctx.check_hostname = False
        pkts, _ = packets("group message")
        answers = P.unframe(self.post(base, P.frame([pkts[0]]), "secret-1", ctx))
        self.assertEqual(len(answers), 1)


if __name__ == "__main__":
    unittest.main()

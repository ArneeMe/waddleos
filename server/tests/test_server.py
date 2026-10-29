"""Server tests. Run from the repo root: python3 -m unittest discover -s server/tests -t server

Reads protocol/vectors.json, written by the C++ tests (test/mesh/run.sh), so the
two sides are checked against each other.
"""

import contextlib
import hashlib
import io
import json
import os
import pty
import select
import shutil
import sqlite3
import ssl
import struct
import subprocess
import tempfile
import threading
import time
import unittest
from unittest import mock
import urllib.error
import urllib.request

from waddlemesh import ed25519
from waddlemesh import packet as P
from waddlemesh import roster as R
from waddlemesh.__main__ import load_config, main
from waddlemesh.core import Server, no_hops
from waddlemesh.gateway import Gateway
from waddlemesh.store import Store
from waddlemesh.web import WebServer, status_page

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
with open(os.path.join(ROOT, "protocol", "vectors.json")) as f:
    V = json.load(f)

NETWORK = 0x42
ADMIN_SEED = bytes.fromhex(V["roster"]["adminSeed"])
ADMIN_SIGN = bytes.fromhex(V["roster"]["adminSign"])
TOKEN_1 = "11" * 16


def token_hash(token):
    return hashlib.sha256(token.encode()).digest()[:16]


def make_roster(version, members, network=NETWORK, seed=ADMIN_SEED):
    """A signed roster like the admin's tool makes. members: (number, name, token)."""
    out = struct.pack("<BBHB", 1, network, version, len(members))
    for number, name, token in members:
        raw = name.encode()
        out += bytes([number, 1 if number == 1 else 0, len(raw)]) + raw + bytes([number]) * 32 + token_hash(token)
    return out + ed25519.sign(seed, out)


def roster_packets(version, pieces, network=NETWORK):
    """Stand-ins for the ROSTER packets (the server cannot open them anyway)."""
    return [P.Header(type=P.ROSTER, network=network, sender=P.SERVER, recipient=P.EVERYONE, counter=0x40000000 + version,
                     fragmented=pieces > 1, piece_index=i, piece_count=pieces).pack() + bytes([i]) * 186
            for i in range(pieces)]


def set_roster(store, version, members, pieces=2):
    store.set_roster(version, make_roster(version, members), roster_packets(version, pieces))


def packets(name):
    case = next(m for m in V["messages"] if m["name"] == name)
    return [bytes.fromhex(p) for p in case["packets"]], bytes.fromhex(case["plain"])


class PacketTests(unittest.TestCase):
    def test_headers_of_cpp_packets(self):
        pkts, _ = packets("group message")
        h = P.parse_header(pkts[0])
        self.assertEqual((h.type, h.network, h.sender, h.recipient, h.counter), (P.MSG, NETWORK, 1, P.EVERYONE, 10))
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
        self.assertEqual({m: c for m, c in req.last_counter.items() if c}, {1: 10, 3: 7, 4: 2})
        self.assertEqual(len(req.last_counter), 64)  # units and phones
        self.assertEqual(req.roster_version, 3)
        self.assertEqual(req.flags, P.SYNC_ON_TRIP | (4 << P.SYNC_CHECK_SHIFT))
        fetch = bytes.fromhex(V["fetch"])
        self.assertEqual(P.parse_header(fetch).type, P.FETCH)
        f = P.parse_fetch(fetch[P.HEADER_SIZE:])
        self.assertEqual((f.sender, f.after, f.before), (1, 10, 13))
        self.assertEqual(f.pack(), fetch[P.HEADER_SIZE:])
        self.assertEqual((req.battery, req.build), (64, 0x2A))
        self.assertEqual(req.pack(), sync[P.HEADER_SIZE:])

    def test_bad_packets(self):
        good = packets("group message")[0][0]
        for bad in (b"", good[:5], b"\x00" + good[1:], good[:4] + b"\x21" + good[5:], good + bytes(200)):
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
        self.assertEqual(key.hex(), V["dmKey"])
        pkts, plain = packets("direct message in 3 pieces")
        self.assertEqual(self.open(pkts, key), plain)
        group_plain = packets("group message")[1]
        self.assertEqual(plain[4:8], hashlib.blake2b(group_plain, digest_size=32).digest()[:4])

    def test_group_message_with_key_number(self):
        pkts, plain = packets("group message with key 2")
        h = P.parse_header(pkts[0])
        self.assertEqual((h.recipient, h.group_id, h.key), (P.TO_GROUP, 0x100, 2))
        self.assertEqual(h.recipient, P.TO_GROUP)
        self.assertTrue(all(P.can_read(m, 1, h.recipient) for m in P.MEMBERS))
        self.assertEqual(self.open(pkts, bytes.fromhex(V["groupKey"])), plain)
        # The key number is sealed: a changed one does not open.
        bad = bytearray(pkts[0])
        bad[13] = 3
        with self.assertRaises(Exception):
            self.open([bytes(bad)], bytes.fromhex(V["groupKey"]))

    def test_tampering_is_caught(self):
        pkts, _ = packets("group message")
        bad = bytearray(pkts[0])
        bad[-1] ^= 1
        with self.assertRaises(Exception):
            self.open([bytes(bad)], bytes.fromhex(V["senderKey"]))


def sync_req(member, last, counter=1):
    h = P.Header(type=P.SYNC_REQ, network=NETWORK, sender=member, recipient=P.SERVER, counter=counter)
    return h.pack() + P.SyncReq(last, battery=50, build=0x2A).pack()


class ServerTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.store = Store(os.path.join(self.dir, "mesh.db"))
        self.server = Server(self.store, NETWORK)

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
        # Answers go with hops 0: the other units do not pass them on.
        self.assertEqual(to_two[:4], [no_hops(p) for p in group + dm])
        self.assertTrue(all(P.parse_header(p).hops == 0 for p in to_two))
        self.assertIn(no_hops(pos[0]), to_two)
        to_three = self.server.handle(sync_req(3, {1: 0, 2: 0, 3: 0}), "wifi")
        self.assertIn(no_hops(group[0]), to_three)
        self.assertFalse(any(no_hops(p) in to_three for p in dm))  # a DM between 1 and 2
        caught_up = self.server.handle(sync_req(2, {1: 11, 2: 0, 3: 0}, counter=2), "wifi")
        self.assertFalse(any(P.parse_header(p).type == P.MSG for p in caught_up))
        units = {u["member"]: u for u in self.store.units()}
        self.assertEqual((units[2]["battery"], units[2]["build"], units[2]["via"]), (50, "2a", "wifi"))

    def test_lora_sync_sends_receipts_and_positions_once(self):
        """Over LoRa a second sync gets only what is new since the first: the
        "stored" receipts and positions are not sent again and again."""
        pos, _ = packets("position")
        self.server.handle(pos[0], "lora")
        mine = P.Header(type=P.MSG, network=NETWORK, sender=2, recipient=P.EVERYONE, counter=7).pack() + bytes(20)
        self.server.handle(mine, "lora")
        types = lambda answer: sorted(P.parse_header(p).type for p in answer)
        first = self.server.handle(sync_req(2, {1: 0, 2: 7}, counter=8), "lora")
        self.assertEqual(types(first), [P.ACK, P.POS])
        self.store.db.execute("UPDATE units SET last_sync = last_sync - 5")  # the next sync comes later
        self.store.db.execute("UPDATE packets SET received_at = received_at - 10")
        self.assertEqual(self.server.handle(sync_req(2, {1: 0, 2: 7}, counter=9), "lora"), [])

    def test_wifi_sync_is_cut_at_200(self):
        for counter in range(1, 260):
            h = P.Header(type=P.MSG, network=NETWORK, sender=1, recipient=P.EVERYONE, counter=counter)
            self.server.handle(h.pack() + bytes(20), "lora")
        answer = self.server.handle(sync_req(2, {1: 0, 2: 0}), "wifi")
        self.assertEqual(len(answer), 200)

    def test_fetch_gives_the_missing_message(self):
        for counter in (10, 11, 12, 13):
            h = P.Header(type=P.MSG, network=NETWORK, sender=1, recipient=P.EVERYONE, counter=counter)
            self.server.handle(h.pack() + bytes(20), "lora")
        h = P.Header(type=P.FETCH, network=NETWORK, sender=2, recipient=P.SERVER, counter=5)
        answer = self.server.handle(h.pack() + P.Fetch(1, 10, 13).pack(), "lora")
        self.assertEqual([P.parse_header(p).counter for p in answer], [11, 12])

    def test_sync_flags_and_sos_for_the_status_page(self):
        flags = P.SYNC_ON_TRIP | P.SYNC_SOS | (2 << P.SYNC_CHECK_SHIFT)
        h = P.Header(type=P.SYNC_REQ, network=NETWORK, sender=3, recipient=P.SERVER, counter=1)
        self.server.handle(h.pack() + P.SyncReq({}, battery=61, build=1, flags=flags).pack(), "lora", -70, 4.0)
        sos = P.Header(type=P.SOS, network=NETWORK, sender=3, recipient=P.EVERYONE, counter=2)
        self.server.handle(sos.pack() + bytes(32), "lora")
        unit = {u["member"]: u for u in self.store.units()}[3]
        self.assertEqual((unit["flags"], unit["battery"]), (flags, 61))
        self.assertTrue(unit["sos_at"])

    def test_lora_sync_is_limited(self):
        for counter in range(1, 30):
            h = P.Header(type=P.MSG, network=NETWORK, sender=1, recipient=P.EVERYONE, counter=counter)
            self.server.handle(h.pack() + bytes(20), "lora")
        self.assertEqual(len(self.server.handle(sync_req(2, {1: 0, 2: 0, 3: 0}), "lora")), 12)
        self.assertEqual(len(self.server.handle(sync_req(2, {1: 0, 2: 0, 3: 0}, 2), "wifi")), 29)

    def test_sos_is_answered(self):
        h = P.Header(type=P.SOS, network=NETWORK, sender=2, recipient=P.EVERYONE, counter=40)
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


GATEWAY = {"connected": True, "answering": True, "version": "2 gc1109", "kept": 0, "handed_over": 0,
           "last_handover": None}


class FakeGateway:
    """The gateway's end of the serial link: a pseudo-terminal the Gateway class opens."""

    def __init__(self, test):
        self.master, slave = pty.openpty()
        self.path = os.ttyname(slave)
        test.addCleanup(os.close, self.master)
        test.addCleanup(os.close, slave)
        self.buf = b""

    def say(self, line):
        os.write(self.master, line.encode() + b"\n")

    def expect(self, prefix, timeout=5):
        """The next line from the Pi that starts with prefix (PINGs in between are skipped)."""
        end = time.time() + timeout
        while time.time() < end:
            while b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                text = line.decode().strip()
                if text.startswith(prefix):
                    return text
            if select.select([self.master], [], [], 0.1)[0]:
                self.buf += os.read(self.master, 4096)
        raise AssertionError("no %s from the Pi" % prefix)


class GatewayLinkTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir)
        self.store = Store(os.path.join(self.dir, "mesh.db"))
        self.server = Server(self.store, NETWORK)

    def start(self):
        fake = FakeGateway(self)
        link = Gateway(fake.path, self.server)
        threading.Thread(target=link.run, daemon=True).start()
        self.addCleanup(setattr, link, "running", False)
        return fake, link

    def test_ping_pong_and_handover(self):
        fake, link = self.start()
        self.assertEqual(fake.expect("PING"), "PING %d 00000000" % NETWORK)  # no roster yet
        group, _ = packets("group message")
        sos = P.Header(type=P.SOS, network=NETWORK, sender=2, recipient=P.EVERYONE, counter=40).pack() + bytes(32)
        ack = P.Header(type=P.ACK, network=NETWORK, sender=3, recipient=1, counter=9).pack() + P.Ack(1, 10, P.READ).pack()
        fake.say("PONG 3 2 gc1109")
        self.assertEqual(fake.expect("DUMP"), "DUMP")
        for raw in (group[0], sos, ack):
            fake.say("STORED " + raw.hex())
        fake.say("DUMPED 3")
        self.assertEqual(fake.expect("CLEAR"), "CLEAR")
        self.assertEqual(self.store.counts(), {P.MSG: 1, P.SOS: 1, P.ACK: 1})
        self.assertEqual(self.store.acks_for(1, 0), [ack])
        # Kept packets are only filed: not sent out again, not answered, and they
        # say nothing about when a unit was last heard.
        self.assertTrue(link.outbox.empty())
        self.assertEqual(self.store.units(), [])
        status = link.status()
        self.assertEqual((status["version"], status["kept"], status["handed_over"]), ("2 gc1109", 0, 3))
        self.assertTrue(status["answering"])

    def test_handover_waits_for_every_packet(self):
        link = Gateway("/nonexistent", self.server)
        written = []
        link._write = written.append
        group, _ = packets("group message")
        link._handle_line("PONG 2 2 gc1109")
        link._handle_line("STORED " + group[0].hex())
        link._handle_line("STORED zz")  # garbled on the way
        link._handle_line("DUMPED 2")
        self.assertEqual(written, ["DUMP"])  # no CLEAR: the gateway keeps them
        self.assertEqual(self.store.counts(), {})
        link._handle_line("PONG 2 2 gc1109")  # the next ping asks again
        self.assertEqual(written, ["DUMP", "DUMP"])
        link._handle_line("PONG 2 2 gc1109")  # a DUMP still under way is not asked again at once
        self.assertEqual(written, ["DUMP", "DUMP"])
        link.dump_at -= 61
        link._handle_line("PONG 2 2 gc1109")
        self.assertEqual(written, ["DUMP", "DUMP", "DUMP"])

    def test_gateway_that_stops_answering(self):
        link = Gateway("/nonexistent", self.server)
        link.connected = True
        link._handle_line("HELLO waddle-gateway 2 kct8103l")
        self.assertEqual(link.version, "2 kct8103l")
        self.assertTrue(link.status()["answering"])
        link.last_heard -= 31
        self.assertFalse(link.status()["answering"])


class StatusPageTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir)
        self.store = Store(os.path.join(self.dir, "mesh.db"))
        self.server = Server(self.store, NETWORK, ADMIN_SIGN)
        set_roster(self.store, 1, [(1, "Arne", TOKEN_1), (2, "Kari <3", "22" * 16)])

    def page(self):
        status = self.server.status()
        status["gateway"] = dict(GATEWAY)
        return status, status_page(status)

    def test_units_and_modes(self):
        flags = P.SYNC_ON_TRIP | (1 << P.SYNC_CHECK_SHIFT)
        h = P.Header(type=P.SYNC_REQ, network=NETWORK, sender=1, recipient=P.SERVER, counter=1)
        self.server.handle(h.pack() + P.SyncReq({}, battery=64, build=0x2A, flags=flags).pack(), "lora", -87, 7.5)
        h = P.Header(type=P.SYNC_REQ, network=NETWORK, sender=3, recipient=P.SERVER, counter=1)
        self.server.handle(h.pack() + P.SyncReq({}, battery=12, build=0x2A).pack(), "wifi")
        status, page = self.page()
        self.assertEqual([u["name"] for u in status["units"]], ["Arne", "Kari <3", "Enhet 3"])
        arne = status["units"][0]
        self.assertEqual((arne["on_trip"], arne["check"], arne["sos"]), (True, "hvert kvarter", False))
        self.assertIn("På tur", page)
        self.assertIn("sjekker hvert kvarter", page)
        self.assertIn("Hjemme", page)  # unit 3
        self.assertIn("−87 dBm · SNR 7,5 dB", page)
        self.assertIn("64 %", page)
        self.assertIn("Kari &lt;3", page)  # names are escaped
        self.assertIn("aldri", page)  # Kari was never heard
        self.assertIn("via Wi-Fi", page)
        self.assertNotIn("SOS fra", page)
        self.assertIn("ingenting ennå", page)  # sync requests are not stored
        self.assertIn("versjon 1", page)  # the roster

    def test_sos_banner(self):
        sos = P.Header(type=P.SOS, network=NETWORK, sender=2, recipient=P.EVERYONE, counter=5)
        self.server.handle(sos.pack() + bytes(32), "lora", -100, -3.0)
        status, page = self.page()
        self.assertTrue(status["units"][1]["sos"])
        self.assertIn("SOS fra Kari &lt;3", page)
        self.assertIn("<title>SOS · Waddle mesh", page)
        # Five minutes later the packet alone no longer shows; the unit's SOS flag does.
        self.store.sos_heard(2, now=time.time() - 301)
        self.assertNotIn("SOS fra", self.page()[1])
        h = P.Header(type=P.SYNC_REQ, network=NETWORK, sender=2, recipient=P.SERVER, counter=6)
        self.server.handle(h.pack() + P.SyncReq({}, flags=P.SYNC_SOS).pack(), "lora")
        self.assertIn("SOS fra Kari", self.page()[1])

    def test_gateway_states(self):
        status, _ = self.page()
        status["gateway"].update(answering=False, kept=4, handed_over=7,
                                 last_handover={"at": int(time.time()) - 7200, "packets": 7})
        page = status_page(status)
        self.assertIn("Svarer ikke", page)
        self.assertIn("4 pakker, hentes nå", page)
        self.assertIn("leverte 7 pakker for 2 t siden", page)
        status["gateway"].update(connected=False)
        self.assertIn("Ikke tilkoblet", status_page(status))


class WebTests(unittest.TestCase):
    def start(self, cert=None, key=None):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir)
        self.mesh = Server(Store(os.path.join(self.dir, "mesh.db")), NETWORK, ADMIN_SIGN)
        set_roster(self.mesh.store, 1, [(1, "Arne", TOKEN_1)])
        self.web = WebServer(("127.0.0.1", 0), self.mesh, lambda: dict(GATEWAY), cert, key)
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
        answers = P.unframe(self.post(base, P.frame([pkts[0]]), TOKEN_1))
        self.assertEqual(P.parse_ack(answers[0][P.HEADER_SIZE:]).state, P.STORED)
        with self.assertRaises(urllib.error.HTTPError) as e:
            self.post(base, P.frame([pkts[0]]), "wrong")
        self.assertEqual(e.exception.code, 401)

    def test_unit_cannot_send_as_another(self):
        """Another member's packet in a unit's upload is what it heard over LoRa:
        kept (as heard, not as from that member), never answered or passed on,
        and it cannot replace a copy the member uploads itself."""
        base = self.start()
        dm, _ = packets("direct message in 3 pieces")
        h = P.parse_header(dm[0])
        forged = P.Header(type=P.MSG, network=NETWORK, sender=2, recipient=1, counter=h.counter).pack() + dm[0][P.HEADER_SIZE:]
        self.assertEqual(self.post(base, P.frame([forged]), TOKEN_1), b"")
        self.assertEqual(self.mesh.store.counts(), {P.MSG: 1})
        (via,) = self.mesh.store.db.execute("SELECT via FROM packets").fetchone()
        self.assertEqual(via, "relay")

    def test_status_page(self):
        base = self.start()
        with urllib.request.urlopen(base + "/", timeout=5) as r:
            page = r.read().decode()
        self.assertIn("Waddle mesh", page)
        self.assertIn('http-equiv="refresh" content="30"', page)
        self.assertNotIn("<script", page)
        with urllib.request.urlopen(base + "/api/status", timeout=5) as r:
            self.assertEqual(json.load(r)["gateway"]["version"], "2 gc1109")

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
        answers = P.unframe(self.post(base, P.frame([pkts[0]]), TOKEN_1, ctx))
        self.assertEqual(len(answers), 1)


# RFC 8032 section 7.1, tests 1 to 3: (secret seed, public key, message, signature).
RFC8032 = [
    ("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
     "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
     "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"),
    ("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
     "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
     "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"),
    ("c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
     "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
     "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"),
]


class RosterTests(unittest.TestCase):
    def test_ed25519_rfc8032(self):
        for seed, public, msg, sig in RFC8032:
            seed, public, msg, sig = map(bytes.fromhex, (seed, public, msg, sig))
            self.assertEqual(ed25519.public_key(seed), public)
            self.assertEqual(ed25519.sign(seed, msg), sig)
            self.assertTrue(ed25519.verify(public, msg, sig))
            self.assertFalse(ed25519.verify(public, msg + b"x", sig))
            self.assertFalse(ed25519.verify(public, msg, sig[:63] + bytes([sig[63] ^ 1])))

    def test_cpp_roster(self):
        signed = bytes.fromhex(V["roster"]["signed"])
        self.assertEqual(ed25519.public_key(ADMIN_SEED), ADMIN_SIGN)
        self.assertEqual(ed25519.sign(ADMIN_SEED, signed[:-64]), signed[-64:])  # the same signature as the C++
        r = R.parse(signed, ADMIN_SIGN)
        self.assertEqual((r.network, r.version), (NETWORK, 3))
        self.assertEqual([(m.number, m.admin, m.name) for m in r.members], [(1, True, "Arne"), (7, False, "Kåre")])
        self.assertEqual(r.member(7).public.hex(), V["memberB"]["public"])
        self.assertEqual(r.member(1).token_hash, bytes(range(0xA0, 0xB0)))
        with self.assertRaises(R.BadRoster):
            R.parse(signed, ed25519.public_key(bytes(32)))  # another admin
        bad = bytearray(signed)
        bad[8] ^= 1
        with self.assertRaises(R.BadRoster):
            R.parse(bytes(bad), ADMIN_SIGN)


class RosterServerTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir)
        self.store = Store(os.path.join(self.dir, "mesh.db"))
        self.server = Server(self.store, NETWORK, ADMIN_SIGN)

    def sync(self, version, via="wifi", counter=1):
        h = P.Header(type=P.SYNC_REQ, network=NETWORK, sender=2, recipient=P.SERVER, counter=counter)
        return self.server.handle(h.pack() + P.SyncReq({}, roster_version=version).pack(), via)

    def test_older_roster_gets_the_new_one_first(self):
        set_roster(self.store, 4, [(1, "Arne", TOKEN_1), (2, "Kari", "22" * 16)], pieces=3)
        msg = P.Header(type=P.MSG, network=NETWORK, sender=1, recipient=P.EVERYONE, counter=5).pack() + bytes(20)
        self.server.handle(msg, "lora")
        answer = self.sync(3)
        self.assertEqual([P.parse_header(p).type for p in answer[:3]], [P.ROSTER] * 3)
        self.assertIn(no_hops(msg), answer)
        self.assertFalse(any(P.parse_header(p).type == P.ROSTER for p in self.sync(4, counter=2)))

    def test_whole_roster_over_lora(self):
        for counter in range(1, 30):
            h = P.Header(type=P.MSG, network=NETWORK, sender=1, recipient=P.EVERYONE, counter=counter)
            self.server.handle(h.pack() + bytes(20), "lora")
        set_roster(self.store, 1, [(1, "Arne", TOKEN_1)], pieces=3)
        answer = self.sync(0, "lora")
        self.assertEqual([P.parse_header(p).type for p in answer], [P.ROSTER] * 3 + [P.MSG] * 9)
        # 32 members make 13 pieces, more than the usual 12: still sent whole.
        set_roster(self.store, 2, [(1, "Arne", TOKEN_1)], pieces=13)
        self.assertEqual([P.parse_header(p).type for p in self.sync(1, "lora", 2)], [P.ROSTER] * 13)

    def test_a_database_error_loses_one_line_not_the_link(self):
        link = Gateway("/nonexistent", self.server)
        calls = []

        def broken(*args, **kwargs):
            calls.append(1)
            raise sqlite3.OperationalError("database is locked")
        self.server.handle = broken
        lines = iter([b"RX -90 5.0 " + packets("group message")[0][0].hex().encode() + b"\nHELLO x y\n"])
        link.fd = 1
        link._ping = lambda: None
        link._write_pending = lambda: setattr(link, "running", False)
        with mock.patch("os.read", lambda fd, n: next(lines)):
            link.run()
        self.assertEqual(calls, [1])
        self.assertEqual(link.version, "y")  # the next line was still read

    def test_airtime_refusal_sends_the_packet_again_later(self):
        link = Gateway("/nonexistent", self.server)
        written = []
        link._write = written.append
        link.send(b"\x01\x02")
        link._write_pending()
        link._handle_line("ERR airtime")
        self.assertEqual(list(link.outbox.queue), [b"\x01\x02"])
        link._write_pending()
        self.assertEqual(written, ["TX 0102"])  # held for a minute

    def test_roster_goes_out_over_lora_once(self):
        set_roster(self.store, 1, [(1, "Arne", TOKEN_1)], pieces=2)
        link = Gateway("/nonexistent", self.server)
        written = []
        link._write = written.append
        link._ping()
        self.assertEqual(written, ["PING %d 00000001" % NETWORK])  # Arne is member 1
        self.assertEqual(list(link.outbox.queue), self.store.roster()[2])
        link.last_ping = 0
        link._ping()
        self.assertEqual(link.outbox.qsize(), 2)
        set_roster(self.store, 2, [(1, "Arne", TOKEN_1)], pieces=1)
        link.last_ping = 0
        link._ping()
        self.assertEqual(link.outbox.qsize(), 3)

    def test_tokens_and_names_come_from_the_roster(self):
        self.assertIsNone(self.server.member_for_token(TOKEN_1))
        set_roster(self.store, 1, [(1, "Arne", TOKEN_1), (5, "Ola", "55" * 16)])
        self.assertEqual(self.server.member_for_token("55" * 16), 5)
        self.assertIsNone(self.server.member_for_token("56" * 16))
        self.assertEqual([(u["member"], u["name"]) for u in self.server.status()["units"]], [(1, "Arne"), (5, "Ola")])
        set_roster(self.store, 2, [(1, "Arne", TOKEN_1)])  # Ola is out
        self.assertIsNone(self.server.member_for_token("55" * 16))

    def test_roster_not_signed_by_the_admin_is_not_used(self):
        self.store.set_roster(1, make_roster(1, [(1, "Arne", TOKEN_1)], seed=bytes(32)), roster_packets(1, 1))
        with self.assertLogs("waddlemesh", "ERROR"):
            self.assertIsNone(self.server.member_for_token(TOKEN_1))

    def test_joins_are_kept_a_week(self):
        old = time.time() - 8 * 86400
        self.assertEqual(self.store.add_join("01" * 8, "aa" * 32, "bb", now=old), "ok")
        self.assertEqual(self.store.joins(), [])
        self.store.add_welcome("02" * 8, "cc", now=old)
        self.assertIsNone(self.store.take_welcome("02" * 8))


def cli(*args):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        code = main(list(args))
    return code, out.getvalue()


class CommandTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir)
        self.path = os.path.join(self.dir, "cfg.json")
        with open(self.path, "w") as f:
            json.dump({"db": os.path.join(self.dir, "mesh.db")}, f)

    def run_cli(self, *args):
        return cli("--config", self.path, *args)

    def test_set_network_and_roster(self):
        self.assertEqual(self.run_cli("set-network", str(NETWORK), ADMIN_SIGN.hex())[0], 0)
        cfg = load_config(self.path)
        self.assertEqual((cfg["network"], cfg["adminSign"]), (NETWORK, ADMIN_SIGN.hex()))
        signed = V["roster"]["signed"]
        pkts = [p.hex() for p in roster_packets(3, 2)]
        code, out = self.run_cli("set-roster", signed, *pkts)
        self.assertEqual(code, 0, out)
        self.assertIn("version 3 with 2 members", out)
        self.assertEqual(Store(cfg["db"]).roster()[1].hex(), signed)
        # The same version again, or older: not taken.
        self.assertEqual(self.run_cli("set-roster", signed, *pkts)[0], 1)
        # Not the admin's signature.
        forged = make_roster(4, [(1, "Arne", TOKEN_1)], seed=bytes(32)).hex()
        self.assertEqual(self.run_cli("set-roster", forged, *[p.hex() for p in roster_packets(4, 1)])[0], 1)
        # Packets of another version.
        newer = make_roster(4, [(1, "Arne", TOKEN_1)]).hex()
        self.assertEqual(self.run_cli("set-roster", newer, *pkts)[0], 1)
        self.assertEqual(self.run_cli("set-roster", newer, *[p.hex() for p in roster_packets(4, 1)])[0], 0)
        # A new network (other admin key): its roster replaces the old one even at version 1.
        other = bytes(range(32))
        self.assertEqual(self.run_cli("set-network", "9", ed25519.public_key(other).hex())[0], 0)
        first = make_roster(1, [(1, "Arne", TOKEN_1)], network=9, seed=other).hex()
        self.assertEqual(self.run_cli("set-roster", first, *[p.hex() for p in roster_packets(1, 1, 9)])[0], 0)

    def test_joins_and_welcome(self):
        store = Store(os.path.join(self.dir, "mesh.db"))
        store.add_join("01" * 8, "aa" * 32, "bbbb")
        code, out = self.run_cli("joins")
        self.assertEqual(json.loads(out)[0]["from"], "aa" * 32)
        self.assertEqual(self.run_cli("welcome", "01" * 8, "cafe")[0], 0)
        self.assertEqual(json.loads(self.run_cli("joins")[1]), [])
        self.assertEqual(store.take_welcome("01" * 8), "cafe")
        store.add_join("02" * 8, "aa" * 32, "bbbb")
        self.assertEqual(self.run_cli("forget", "02" * 8)[0], 0)
        self.assertEqual(store.joins(), [])


class JoinWebTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir)
        self.mesh = Server(Store(os.path.join(self.dir, "mesh.db")), NETWORK, ADMIN_SIGN)
        self.web = WebServer(("127.0.0.1", 0), self.mesh, lambda: dict(GATEWAY))
        threading.Thread(target=self.web.serve_forever, daemon=True).start()
        self.addCleanup(self.web.server_close)
        self.addCleanup(self.web.shutdown)
        self.base = "http://127.0.0.1:%d" % self.web.server_address[1]

    def request(self, path, doc=None, token=None):
        headers = {"Authorization": "Bearer " + token} if token else {}
        data = json.dumps(doc).encode() if doc is not None else None
        req = urllib.request.Request(self.base + path, data=data, headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=5) as r:
                return r.status, r.read()
        except urllib.error.HTTPError as e:
            return e.code, e.read()

    def test_join_and_welcome(self):
        req = {"invite": "01" * 8, "from": "aa" * 32, "box": "bb" * 40}
        self.assertEqual(self.request("/api/join", req)[0], 200)
        self.assertEqual(self.request("/api/join", req)[0], 409)  # an invitation is used once
        self.assertEqual(self.request("/api/join/" + "01" * 8)[0], 404)  # not approved yet
        self.assertEqual(self.mesh.store.joins()[0]["box"], "bb" * 40)
        self.mesh.store.add_welcome("01" * 8, "cc" * 50)
        self.assertEqual(self.request("/api/join/" + "01" * 8), (200, json.dumps({"box": "cc" * 50}).encode()))
        self.assertEqual(self.request("/api/join/" + "01" * 8)[0], 404)  # given once
        self.assertEqual(self.request("/api/join/nothex")[0], 404)

    def test_bad_requests(self):
        good = {"invite": "01" * 8, "from": "aa" * 32, "box": "bb" * 40}
        for bad in ({**good, "invite": "01" * 9}, {**good, "from": "aa"}, {**good, "box": "bb" * 1025},
                    {**good, "box": ""}, {**good, "box": "zz"}, ["x"]):
            self.assertEqual(self.request("/api/join", bad)[0], 400, bad)
        self.assertEqual(self.mesh.store.joins(), [])

    def test_at_most_20_waiting(self):
        for i in range(20):
            self.assertEqual(self.request("/api/join", {"invite": "%016x" % i, "from": "aa" * 32, "box": "bb"})[0],
                             200)
        self.assertEqual(self.request("/api/join", {"invite": "ff" * 8, "from": "aa" * 32, "box": "bb"})[0], 429)

    def test_join_requests_limited_per_internet_address(self):
        for _ in range(3):
            self.assertTrue(self.web.allow_join("203.0.113.5", now=1000))
        self.assertFalse(self.web.allow_join("203.0.113.5", now=1000))
        self.assertTrue(self.web.allow_join("203.0.113.6", now=1000))
        self.assertTrue(self.web.allow_join("203.0.113.5", now=1000 + 3601))

    def test_negative_length_is_refused(self):
        import http.client
        port = self.web.server_address[1]
        for path in ("/api/join", "/api/packets"):
            c = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
            c.putrequest("POST", path)
            c.putheader("Content-Length", "-1")
            c.putheader("Authorization", "Bearer " + TOKEN_1)
            c.endheaders()
            self.assertIn(c.getresponse().status, (401, 413), path)
            c.close()

    def test_wifi_copy_replaces_a_forged_lora_copy(self):
        raw = packets("group message")[0][0]
        h = P.parse_header(raw)
        forged = raw[:-1] + bytes([raw[-1] ^ 1])
        store = self.mesh.store
        self.assertTrue(store.add_packet(h, forged, "lora"))
        self.assertFalse(store.add_packet(h, forged, "lora"))
        self.assertTrue(store.add_packet(h, raw, "wifi"))
        self.assertEqual(store.messages_after(h.sender, 0, 2), [raw])
        self.assertFalse(store.add_packet(h, raw, "wifi"))

    def test_roster_for_members_only(self):
        self.assertEqual(self.request("/api/roster", token=TOKEN_1)[0], 401)
        set_roster(self.mesh.store, 1, [(1, "Arne", TOKEN_1)])
        code, body = self.request("/api/roster", token=TOKEN_1)
        self.assertEqual(code, 200)
        self.assertEqual(R.parse(body, ADMIN_SIGN).members[0].name, "Arne")
        self.assertEqual(self.request("/api/roster", token="22" * 16)[0], 401)


if __name__ == "__main__":
    unittest.main()

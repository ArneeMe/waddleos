"""The network commands of tools/waddle-mesh: Ed25519, the roster, joining and
ROSTER packets, against the RFCs, the firmware (protocol/vectors.json) and the
Pi server (server/waddlemesh, started here)."""

import contextlib
import hashlib
import importlib.machinery
import importlib.util
import io
import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
loader = importlib.machinery.SourceFileLoader("waddle_mesh", str(ROOT / "tools" / "waddle-mesh"))
spec = importlib.util.spec_from_loader("waddle_mesh", loader)
wm = importlib.util.module_from_spec(spec)
loader.exec_module(wm)
V = json.loads((ROOT / "protocol" / "vectors.json").read_text())

sys.path.insert(0, str(ROOT / "server"))
from waddlemesh import packet as P  # noqa: E402
from waddlemesh import roster as SR  # noqa: E402
from waddlemesh.__main__ import main as server_main  # noqa: E402
from waddlemesh.core import Server  # noqa: E402
from waddlemesh.store import Store  # noqa: E402
from waddlemesh.web import WebServer  # noqa: E402

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


class Ed25519(unittest.TestCase):
    def test_rfc8032(self):
        for seed, public, msg, sig in RFC8032:
            seed, public, msg, sig = map(bytes.fromhex, (seed, public, msg, sig))
            self.assertEqual(wm.ed25519_public(seed), public)
            self.assertEqual(wm.ed25519_sign(seed, msg), sig)
            self.assertTrue(wm.ed25519_verify(public, msg, sig))
            self.assertFalse(wm.ed25519_verify(public, msg + b"!", sig))


class Roster(unittest.TestCase):
    def test_same_bytes_as_the_firmware(self):
        v = V["roster"]
        seed, signed = bytes.fromhex(v["adminSeed"]), bytes.fromhex(v["signed"])
        self.assertEqual(wm.ed25519_public(seed).hex(), v["adminSign"])
        roster = wm.read_roster(signed, bytes.fromhex(v["adminSign"]))
        self.assertEqual([(m["number"], m["name"], m["admin"]) for m in roster["members"]],
                         [(1, "Arne", True), (7, "Kåre", False)])
        self.assertEqual(wm.write_roster(roster, seed), signed)
        with self.assertRaises(RuntimeError):
            wm.read_roster(signed, wm.ed25519_public(bytes(32)))

    def test_names_are_cut_at_20_bytes(self):
        self.assertEqual(wm.fit_name("x" * 19 + "ø"), "x" * 19)  # not half an ø
        self.assertEqual(wm.fit_name(" Kåre "), "Kåre")
        self.assertLessEqual(len(wm.fit_name("ø" * 30).encode()), 20)


class Joining(unittest.TestCase):
    def test_join_key_and_code_as_the_firmware(self):
        j = V["join"]
        admin_secret = bytes.fromhex(j["adminBoxSecret"])
        admin_public = wm.x25519_public(admin_secret)
        unit_secret, unit_public = bytes.fromhex(V["memberB"]["secret"]), bytes.fromhex(V["memberB"]["public"])
        password = bytes.fromhex(j["password"])
        # The admin's side and the unit's side get the same K.
        admin_side = wm.join_key(wm.x25519(admin_secret, unit_public), password, unit_public, admin_public)
        unit_side = wm.join_key(wm.x25519(unit_secret, admin_public), password, unit_public, admin_public)
        self.assertEqual(admin_side.hex(), j["key"])
        self.assertEqual(unit_side.hex(), j["key"])
        self.assertEqual(wm.join_code(admin_public, unit_public), j["code"])

    def test_link(self):
        args = (0x42, bytes(range(8)), bytes(range(16)), bytes(range(32)), bytes(range(32, 64)),
                "https://home.example:8443")
        link = wm.make_link(*args)
        self.assertTrue(link.startswith("waddle://join/"))
        self.assertNotIn("=", link)
        got = wm.read_link(link)
        self.assertEqual((got["network"], got["invite"], got["password"], got["adminBox"], got["certHash"], got["url"]),
                         args)
        with self.assertRaises(RuntimeError):
            wm.read_link(link[:60])


def open_pieces(key, packets):
    """Joins the pieces and opens them, building nonce and associated data from
    the header bytes as protocol/PROTOCOL.md says (not with the tool's helpers)."""
    first = packets[0]
    nonce = first[4:10] + bytes([first[1] & 0x0F]) + bytes(5)
    aad = first[0:3] + first[4:10] + first[11:14]
    return wm.aead_open(key, nonce, b"".join(p[14:] for p in packets), aad)


class RosterPackets(unittest.TestCase):

    def test_round_trip(self):
        key = bytes(range(32))
        signed = bytes.fromhex(V["roster"]["signed"])
        packets = wm.roster_packets(key, 0x42, 3, signed)
        self.assertEqual(len(packets), 2)  # 197 + 16 bytes
        heads = [P.parse_header(p) for p in packets]
        for i, h in enumerate(heads):
            self.assertEqual((h.type, h.network, h.sender, h.recipient, h.counter, h.hops),
                             (P.ROSTER, 0x42, P.SERVER, P.EVERYONE, 0x40000003, 3))
            self.assertEqual((h.piece_index, h.piece_count, h.fragmented, h.group_id, h.key), (i, 2, True, 0, 0))
        self.assertEqual(len(packets[0]), 200)
        self.assertEqual(open_pieces(key, packets), signed)
        with self.assertRaises(RuntimeError):
            open_pieces(bytes(32), packets)

    def test_32_members_fit(self):
        seed = bytes(32)
        roster = {"network": 1, "version": 1, "members": [
            {"number": n, "admin": n == 1, "name": "x" * 20, "public": bytes(32), "tokenHash": bytes(16)}
            for n in range(1, 33)]}
        signed = wm.write_roster(roster, seed)
        packets = wm.roster_packets(bytes(32), 1, 1, signed)
        self.assertEqual(len(packets), 13)
        self.assertEqual(open_pieces(bytes(32), packets), signed)
        self.assertTrue(all(len(p) <= 200 for p in packets))

    def test_cpp_seal_layout(self):
        """seal() in MeshCrypto.cpp with the same header gives the same bytes:
        checked with the vector's group message (one piece, sender key)."""
        case = next(m for m in V["messages"] if m["name"] == "group message")
        raw = bytes.fromhex(case["packets"][0])
        h = raw[:14]
        sealed = wm.aead_seal(bytes.fromhex(V["senderKey"]), wm.packet_nonce(h), bytes.fromhex(case["plain"]),
                              wm.packet_aad(h))
        self.assertEqual(h + sealed, raw)


class FakeUnit:
    """A mesh unit's File Transfer API for joining, as protocol/PROTOCOL.md says."""

    def __init__(self, seed):
        self.secret = hashlib.sha256(seed).digest()
        self.public = wm.x25519_public(self.secret)
        self.welcome = None

    def get(self, path):
        assert path.startswith("/api/mesh/identity?commit=")
        self.commit, self.nonce, self.code, self.tool = bytes.fromhex(path.split("=")[1]), os.urandom(16), None, None
        return {"publicKey": self.public.hex(), "nonce": self.nonce.hex()}

    def post(self, path, doc):
        if path == "/api/mesh/pair":
            tool, tool_nonce = bytes.fromhex(doc["tool"]), bytes.fromhex(doc["nonce"])
            assert wm.pair_commit(tool, tool_nonce) == self.commit
            self.tool, self.code = tool, wm.pair_code(tool, self.public, tool_nonce, self.nonce)
            return "OK"
        if path == "/api/mesh/join":
            tool = bytes.fromhex(doc["tool"])
            assert tool == self.tool
            key = hashlib.blake2b(wm.x25519(self.secret, tool) + tool + self.public, digest_size=32).digest()
            inner = json.loads(wm.aead_open(key, wm.setup_nonce(2), bytes.fromhex(doc["box"])))
            self.invite = wm.read_link(inner["invite"])
            admin = self.invite["adminBox"]
            self.k = wm.join_key(wm.x25519(self.secret, admin), self.invite["password"], self.public, admin)
            box = wm.aead_seal(self.k, wm.setup_nonce(3), json.dumps({"name": inner["name"]}).encode())
            return json.dumps({"from": self.public.hex(), "box": box.hex(), "code": wm.join_code(admin, self.public)})
        if path == "/api/mesh/welcome":
            doc = json.loads(wm.aead_open(self.k, wm.setup_nonce(4), bytes.fromhex(doc["box"])))
            wm.read_roster(bytes.fromhex(doc["roster"]), bytes.fromhex(doc["adminSign"]))
            self.welcome = doc
            return "OK"
        raise AssertionError(path)


@unittest.skipUnless(shutil.which("openssl"), "needs openssl")
class EndToEnd(unittest.TestCase):
    """setup, invite, join, approve and remove against a real server (HTTPS, its
    CLI as over SSH) and fake units."""

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir)
        self.cert, self.key = os.path.join(self.dir, "cert.pem"), os.path.join(self.dir, "key.pem")
        subprocess.run(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1",
                        "-nodes", "-days", "1", "-subj", "/CN=waddlemesh", "-keyout", self.key, "-out", self.cert],
                       check=True, capture_output=True)
        self.server_cfg = os.path.join(self.dir, "waddlemesh.json")
        self.db = os.path.join(self.dir, "mesh.db")
        with open(self.server_cfg, "w") as f:
            json.dump({"db": self.db}, f)
        for name, value in (("ADMIN", os.path.join(self.dir, "home", "admin.json")),
                            ("MESH", os.path.join(self.dir, "home", "mesh.json")), ("POLL_S", 0.05),
                            ("ssh", self.ssh), ("ask", self.ask), ("unit_get", self.unit_get),
                            ("unit_post", self.unit_post)):
            old = getattr(wm, name)
            setattr(wm, name, value)
            self.addCleanup(setattr, wm, name, old)
        self.units = {"10.0.0.%d" % i: FakeUnit(bytes([i])) for i in range(1, 5)}
        self.answers = {}
        self.port = None

    # --- What the tool talks to -------------------------------------------------------

    def ssh(self, host, command):
        self.assertEqual(host, "pi@test")
        if command == "sudo cat /var/lib/waddlemesh/cert.pem":
            return Path(self.cert).read_text()
        command = command.split(" && sudo systemctl restart")[0]
        args = shlex.split(command.split(" -m waddlemesh ", 1)[1])
        args[args.index("--config") + 1] = self.server_cfg
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = server_main(args)
        if code:
            raise RuntimeError(out.getvalue())
        return out.getvalue()

    def ask(self, prompt, default=""):
        for key, answer in self.answers.items():
            if key in prompt:
                return answer.pop(0) if isinstance(answer, list) else answer
        if "setup code" in prompt:
            # The person compares with the unit's screen.
            return "y" if any(u.code and u.code in prompt for u in self.units.values()) else "n"
        if "(y/n)" in prompt:
            return "y"
        raise AssertionError("unexpected question: " + prompt)

    def unit_get(self, ip, path):
        return self.units[ip].get(path)

    def unit_post(self, ip, path, doc):
        return self.units[ip].post(path, doc)

    def start_server(self):
        cfg = json.loads(Path(self.server_cfg).read_text())
        self.mesh = Server(Store(self.db), cfg["network"], bytes.fromhex(cfg["adminSign"]))
        self.web = WebServer(("127.0.0.1", self.port), self.mesh, lambda: {}, self.cert, self.key)
        threading.Thread(target=self.web.serve_forever, daemon=True).start()
        self.addCleanup(self.web.server_close)
        self.addCleanup(self.web.shutdown)

    def wait_for_request(self):
        end = time.time() + 10
        while not self.mesh.store.joins() and time.time() < end:
            time.sleep(0.05)
        self.assertEqual(len(self.mesh.store.joins()), 1)

    def free_port(self):
        import socket
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            self.port = s.getsockname()[1]
        return "https://127.0.0.1:%d" % self.port

    def tool(self, *argv):
        with contextlib.redirect_stdout(io.StringIO()) as out:
            code = wm.main(list(argv))
        self.assertEqual(code, 0, out.getvalue())
        return out.getvalue()

    # --- The story ----------------------------------------------------------------------

    def test_setup_invite_join_approve_remove(self):
        url = self.free_port()

        # Arne makes the network and adds his two units.
        self.answers = {"Pi over SSH": "pi@test", "Address units use": url,
                        "address on its screen": ["10.0.0.1", "10.0.0.2", ""], "name": ["Arne", "Kari"]}
        self.tool("setup")
        admin = json.loads(Path(wm.ADMIN).read_text())
        self.assertEqual(os.stat(wm.ADMIN).st_mode & 0o777, 0o600)
        arne, kari = self.units["10.0.0.1"].welcome, self.units["10.0.0.2"].welcome
        self.assertEqual((arne["me"], kari["me"], arne["network"]), (1, 2, admin["network"]))
        self.assertEqual(arne["server"]["url"], url)
        self.assertEqual(arne["counterFloor"] // 100, int(time.time()) // 100)
        roster = SR.parse(bytes.fromhex(kari["roster"]), bytes.fromhex(kari["adminSign"]))
        self.assertEqual([(m.number, m.name, m.admin) for m in roster.members], [(1, "Arne", True), (2, "Kari", False)])
        self.assertEqual(len(json.loads(Path(wm.MESH).read_text())["units"]), 2)
        self.start_server()
        self.assertEqual(self.mesh.member_for_token(kari["server"]["token"]), 2)
        stored = self.mesh.store.roster()
        self.assertEqual(stored[0], 1)
        self.assertEqual(open_pieces(bytes.fromhex(admin["networkKey"]), stored[2]), stored[1])

        # Ola gets an invitation and joins from his computer.
        out = self.tool("invite", "Ola")
        link = next(w.strip("'") for w in out.split() if w.startswith("'waddle://"))
        self.answers = {"Address on the unit's screen": "10.0.0.3"}
        joined = {}

        def ola():
            with contextlib.redirect_stdout(io.StringIO()) as o:
                joined["code"] = wm.main(["join", link, "--name", "Ola Nordmann"])
            joined["out"] = o.getvalue()

        t = threading.Thread(target=ola, daemon=True)
        t.start()
        self.wait_for_request()
        self.assertIn("Ola Nordmann", self.tool("approve"))
        t.join(10)
        self.assertEqual(joined.get("code"), 0, joined.get("out"))
        ola_doc = self.units["10.0.0.3"].welcome
        self.assertEqual((ola_doc["me"], ola_doc["name"]), (3, "Ola Nordmann"))
        self.assertEqual(self.mesh.member_for_token(ola_doc["server"]["token"]), 3)
        self.assertEqual(self.mesh.store.roster()[0], 2)
        # His token gets the roster over HTTPS (the certificate checked as `join` does).
        status, _ = wm.server_request(url, wm.cert_hash(Path(self.cert).read_text()), "GET", "/api/join/" + "00" * 8)
        self.assertEqual(status, 404)
        self.assertNotIn("Ola", json.dumps(admin["invites"]))
        self.assertIn("Ola Nordmann", self.tool("members"))
        # The invitation is used up: a second try is turned down by the admin's tool.
        self.assertEqual(self.mesh.store.add_join(wm.read_link(link)["invite"].hex(), "aa" * 32, "bb"), "ok")
        self.assertIn("forgotten", self.tool("approve"))
        self.assertEqual(self.mesh.store.joins(), [])

        # Kari's unit is lost.
        self.answers = {}
        self.tool("remove", "kari")
        self.assertEqual(self.mesh.store.roster()[0], 3)
        self.assertIsNone(self.mesh.member_for_token(kari["server"]["token"]))
        self.assertEqual([m.number for m in self.mesh.roster().members], [1, 3])
        # The next newcomer gets a number nobody had; Kari's is kept for her.
        def join(name, ip):
            self.answers = {"Address on the unit's screen": ip}
            link = next(w.strip("'") for w in self.tool("invite", name).split() if w.startswith("'waddle://"))
            t = threading.Thread(target=lambda: wm.main(["join", link, "--name", name]), daemon=True)
            with contextlib.redirect_stdout(io.StringIO()):
                t.start()
                self.wait_for_request()
                self.tool("approve")
                t.join(10)

        join("Per", "10.0.0.4")
        self.assertEqual(self.units["10.0.0.4"].welcome["me"], 4)
        # Kari, with new keys on her unit, is invited again: her old number.
        self.units["10.0.0.2"] = FakeUnit(b"kari again")
        join("Kari", "10.0.0.2")
        self.assertEqual(self.units["10.0.0.2"].welcome["me"], 2)

    def test_wrong_certificate_is_refused(self):
        url = self.free_port()
        Path(self.server_cfg).write_text(json.dumps({"db": self.db, "network": 5, "adminSign": "00" * 32}))
        self.start_server()
        with self.assertRaises(RuntimeError) as e:
            wm.server_request(url, bytes(32), "GET", "/api/join/" + "00" * 8)
        self.assertIn("another certificate", str(e.exception))


if __name__ == "__main__":
    unittest.main()

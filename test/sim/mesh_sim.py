"""Three mesh units and the Pi server, on this computer.

Each unit is test/sim/sim_node (the real firmware mesh code with the hardware
replaced); the server is the real server/waddlemesh code. This file plays the
air between them: who hears whom is a table the tests change as they go.

Run: bash test/sim/run.sh   (builds sim_node, then runs these tests)
"""

import email.utils
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "server"))

from waddlemesh import packet as P  # noqa: E402
from waddlemesh.core import Server  # noqa: E402
from waddlemesh.store import Store  # noqa: E402

SIM_NODE = os.environ.get("SIM_NODE", os.path.join(ROOT, "build", "sim", "sim_node"))
GROUP = 0x42
GATEWAY = "G"
STEP_MS = 100


class Unit:
    def __init__(self, world, number, name):
        self.world, self.number, self.name = world, number, name
        self.dir = os.path.join(world.dir, "unit%d" % number)
        self.proc = None
        self.alive = False
        self.start()

    def start(self):
        self.proc = subprocess.Popen([SIM_NODE, self.dir], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, text=True, bufsize=1)
        self.alive = True

    def stop(self):
        """Switched off: the process ends, the SD card and flash stay."""
        self.cmd("quit", expect_ok=False)
        self.proc.wait(timeout=10)
        self.proc.stdin.close()
        self.proc.stdout.close()
        self.alive = False

    def cmd(self, line, expect_ok=True):
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()
        out = []
        while expect_ok:
            reply = self.proc.stdout.readline()
            if not reply:
                raise RuntimeError("unit %d stopped" % self.number)
            reply = reply.rstrip("\n")
            if reply == "ok":
                break
            if reply.startswith("http "):
                status, date, answer = self.world.http(self.number, bytes.fromhex(reply[5:]))
                self.proc.stdin.write("httpresp %d %s|%s\n" % (status, date, answer.hex()))
                self.proc.stdin.flush()
                continue
            out.append(reply)
        return out

    def state(self):
        return json.loads(self.cmd("dump")[0])

    def chat(self, conv):
        return self.state()["chats"][str(conv)]

    def texts(self, conv):
        return [m["text"] for m in self.chat(conv)]


class World:
    """The units, the server, and who can hear whom."""

    def __init__(self, names=("Arne", "Kari", "Ola"), server_wifi=True):
        self.dir = tempfile.mkdtemp(prefix="meshsim-")
        self.now = 1000
        self.links = set()
        self.drop = None  # drop(raw, sender, receiver) -> True to lose a packet
        self.air = []  # (who sent, raw) waiting to be heard next step
        self.store = Store(os.path.join(self.dir, "server.db"))
        self.server = Server(self.store, GROUP)

        world = self

        class Radio:
            def send(self, raw):
                world.air.append((GATEWAY, raw))

        self.server.radio = Radio()
        self.units = {}
        for n, name in enumerate(names, 1):
            self.units[n] = Unit(self, n, name)
        members = []
        for n, u in self.units.items():
            pub, sender = u.cmd("identity")[0].split()[1:]
            members.append({"number": n, "name": u.name, "publicKey": pub, "senderKey": sender})
        for n, u in self.units.items():
            doc = {"group": GROUP, "me": n, "members": members}
            if server_wifi:
                doc["server"] = {"url": "https://pi.example:8443", "token": "t%d" % n,
                                 "cert": "-----BEGIN CERTIFICATE-----"}
            os.makedirs(os.path.join(u.dir, ".mesh"), exist_ok=True)
            with open(os.path.join(u.dir, ".mesh", "group.json"), "w") as f:
                json.dump(doc, f)

    def close(self):
        for u in self.units.values():
            if u.alive:
                u.stop()
        self.store.db.close()
        shutil.rmtree(self.dir)

    def link(self, a, b, on=True):
        pair = frozenset((a, b))
        (self.links.add if on else self.links.discard)(pair)

    def all_in_range(self, gateway=False):
        names = list(self.units) + ([GATEWAY] if gateway else [])
        for a in names:
            for b in names:
                if a != b:
                    self.link(a, b)

    def hears(self, a, b):
        return frozenset((a, b)) in self.links

    def begin(self, *numbers):
        for n in numbers or self.units:
            self.units[n].cmd("begin")

    def http(self, member, body):
        """A unit's HTTPS request, answered by the real server code."""
        answers = []
        for raw in P.unframe(body):
            if P.parse_header(raw).sender == member:
                answers += self.server.handle(raw, "wifi")
        return 200, email.utils.formatdate(usegmt=True), P.frame(answers)

    def step(self):
        self.now += STEP_MS
        # Deliver what was sent last step.
        air, self.air = self.air, []
        for sender, raw in air:
            for n, u in self.units.items():
                if n != sender and u.alive and self.hears(sender, n):
                    if not (self.drop and self.drop(raw, sender, n)):
                        u.cmd("rx " + raw.hex())
            if sender != GATEWAY and self.hears(sender, GATEWAY):
                for answer in self.server.handle(raw, "lora", -90, 5.0):
                    self.air.append((GATEWAY, answer))
        for n, u in self.units.items():
            if not u.alive:
                continue
            u.cmd("time %d" % self.now)
            for line in u.cmd("loop"):
                if line.startswith("tx "):
                    self.air.append((n, bytes.fromhex(line[3:])))

    def run(self, seconds):
        for _ in range(int(seconds * 1000 / STEP_MS)):
            self.step()


def packet_type(raw):
    return P.parse_header(raw).type


class MeshTests(unittest.TestCase):
    def setUp(self):
        self.w = World()

    expect_warning = False

    def tearDown(self):
        # No message anywhere may carry a false "a message may be missing".
        for u in self.w.units.values() if not self.expect_warning else []:
            if u.alive:
                for msgs in u.state()["chats"].values():
                    self.assertFalse(any(m["warning"] for m in msgs), "chain warning on unit %d" % u.number)
        self.w.close()

    def test_group_message_delivered_and_read(self):
        w = self.w
        w.all_in_range()
        w.begin()
        w.run(5)
        self.assertEqual(w.units[1].cmd("send 0 Hei fra skogen!"), ["sent"])
        w.run(15)
        for n in (2, 3):
            self.assertEqual(w.units[n].texts(0), ["Hei fra skogen!"])
            self.assertEqual(w.units[n].state()["unread"], 1)
        mine = w.units[1].chat(0)[0]
        self.assertTrue(mine["sent"])
        self.assertEqual(mine["delivered"], 0b110)  # Kari and Ola
        w.units[2].cmd("read 0")
        w.run(10)
        self.assertEqual(w.units[1].chat(0)[0]["read"], 0b010)
        self.assertEqual(w.units[2].state()["unread"], 0)

    def test_relay_reaches_unit_out_of_range(self):
        w = self.w
        w.link(1, 2)
        w.link(2, 3)  # Arne and Ola cannot hear each other
        w.begin()
        w.run(5)
        w.units[1].cmd("send 0 Via Kari")
        w.run(20)
        self.assertEqual(w.units[3].texts(0), ["Via Kari"])
        self.assertEqual(w.units[1].chat(0)[0]["delivered"], 0b110)  # Ola's answer came back via Kari

    def test_direct_message_only_for_its_reader(self):
        w = self.w
        w.all_in_range()
        w.begin()
        w.run(5)
        w.units[1].cmd("send 2 Bare til deg")
        w.run(15)
        self.assertEqual(w.units[2].texts(1), ["Bare til deg"])
        ola = w.units[3].state()
        self.assertTrue(all(not msgs for msgs in ola["chats"].values()))
        # Ola keeps the encrypted packet (to help others catch up) but not the text.
        for path, _, files in os.walk(w.units[3].dir):
            for f in files:
                with open(os.path.join(path, f), "rb") as fh:
                    self.assertNotIn(b"Bare til deg", fh.read())

    def test_switched_off_unit_catches_up_from_the_others(self):
        w = self.w
        w.all_in_range()
        w.units[3].stop()
        w.begin(1, 2)
        w.run(5)
        w.units[1].cmd("send 0 Første")
        w.run(10)
        w.units[1].cmd("send 0 Andre")
        w.run(10)
        w.units[3].start()
        w.units[3].cmd("time %d" % w.now)
        w.begin(3)  # power-on: asks the others what it missed
        w.run(20)
        self.assertEqual(w.units[3].texts(0), ["Første", "Andre"])

    def test_lost_answer_is_retried(self):
        w = self.w
        w.link(1, 2)  # only Arne and Kari: with Ola near, his relay of the answer would get through
        w.begin()
        w.run(5)
        lost = []

        def drop(raw, sender, receiver):
            # Lose Kari's first answer to Arne.
            if sender == 2 and receiver == 1 and packet_type(raw) == P.ACK and not lost:
                lost.append(raw)
                return True
            return False

        w.drop = drop
        w.units[1].cmd("send 2 Hører du meg?")
        w.run(10)
        self.assertTrue(lost)
        self.assertEqual(w.units[1].chat(2)[0]["delivered"], 0)
        w.run(40)  # the first retry is after 30 s; Kari answers it again
        self.assertEqual(w.units[1].chat(2)[0]["delivered"], 0b010)
        self.assertEqual(w.units[2].texts(1), ["Hører du meg?"])  # once, not twice

    def test_gateway_relays_between_units_that_cannot_hear_each_other(self):
        w = self.w
        # Arne and Ola are both near the gateway but not each other.
        w.link(1, GATEWAY)
        w.link(3, GATEWAY)
        w.begin()
        w.run(5)
        w.units[1].cmd("send 0 Via hjemme")
        w.run(10)
        self.assertEqual(w.units[3].texts(0), ["Via hjemme"])
        mine = w.units[1].chat(0)[0]
        self.assertTrue(mine["stored"])
        self.assertEqual(mine["delivered"], 0b100)  # Ola's answer, relayed back by the gateway

    def test_server_hands_out_what_a_unit_missed(self):
        w = self.w
        w.link(1, GATEWAY)
        w.units[3].stop()
        w.begin(1, 2)
        w.run(5)
        w.units[1].cmd("send 0 Til serveren")
        w.run(10)
        self.assertTrue(w.units[1].chat(0)[0]["stored"])
        # Ola comes on near home and asks what it missed: the server has it.
        w.units[3].start()
        w.units[3].cmd("time %d" % w.now)
        w.link(3, GATEWAY)
        w.begin(3)
        w.run(15)
        self.assertEqual(w.units[3].texts(0), ["Til serveren"])

    def test_wifi_sync_when_lora_reaches_nobody(self):
        w = self.w
        w.begin()
        w.run(2)
        for n in (1, 3):
            w.units[n].cmd("wifi 1")
        w.units[1].cmd("send 0 Over nettet")
        w.run(5)
        w.units[1].cmd("wifisync")
        w.run(2)
        self.assertTrue(w.units[1].chat(0)[0]["stored"])
        w.units[3].cmd("wifisync")
        w.run(2)
        self.assertEqual(w.units[3].texts(0), ["Over nettet"])

    def test_sos_is_answered_seen_and_cancelled(self):
        w = self.w
        w.all_in_range(gateway=True)
        w.begin()
        w.run(5)
        w.units[1].cmd("gps 59.9139 10.7522")
        w.units[1].cmd("sos")
        w.run(10)
        for n in (2, 3):
            self.assertTrue(w.units[n].state()["members"]["1"]["sos"])
            self.assertEqual(w.units[n].cmd("takesos"), ["newsos 1"])
        arne = w.units[1].state()
        self.assertEqual(arne["sosReceivedBy"] & 0b110, 0b110)
        self.assertTrue(arne["sosReceivedBy"] & 0x80)  # the server
        w.units[2].cmd("seen 1")
        w.run(5)
        self.assertEqual(w.units[1].state()["sosSeenBy"], 0b010)
        w.units[1].cmd("cancel")
        w.run(10)
        for n in (2, 3):
            self.assertFalse(w.units[n].state()["members"]["1"]["sos"])

    def test_unsent_message_survives_a_restart(self):
        w = self.w
        w.begin()  # nobody in range
        w.run(5)
        w.units[1].cmd("send 3 Når du kommer tilbake")
        w.run(5)
        w.units[1].stop()
        w.units[1].start()
        w.units[1].cmd("time %d" % w.now)
        w.units[1].cmd("begin")
        w.link(1, 3)  # now Ola is in range
        w.run(40)
        self.assertEqual(w.units[3].texts(1), ["Når du kommer tilbake"])
        self.assertEqual(w.units[1].chat(3)[0]["delivered"], 0b100)

    def test_positions_shared_off_grid(self):
        w = self.w
        w.all_in_range()
        w.begin()
        w.units[2].cmd("gps 60.0 10.8")
        w.units[2].cmd("offgrid 1")
        w.run(10)
        kari = w.units[1].state()["members"]["2"]
        self.assertTrue(kari["hasPosition"])
        self.assertEqual(kari["lat"], 600000000)

    def test_missing_message_shows_a_warning(self):
        w = self.w
        w.all_in_range()
        w.begin()
        w.run(5)
        w.units[1].cmd("send 0 En")
        w.run(10)
        # Ola misses the second message completely (and every copy of it).
        w.drop = lambda raw, sender, receiver: receiver == 3 and packet_type(raw) == P.MSG
        w.units[1].cmd("send 0 To")
        w.run(10)
        w.drop = None
        w.units[1].cmd("send 0 Tre")
        w.run(10)
        ola = w.units[3].chat(0)
        self.assertEqual([m["text"] for m in ola], ["En", "Tre"])
        self.assertEqual([m["warning"] for m in ola], [False, True])
        self.expect_warning = True

    def test_long_message_in_pieces(self):
        w = self.w
        w.all_in_range()
        w.begin()
        w.run(5)
        text = "Lang melding. " * 50  # 700 bytes: four pieces
        w.units[1].cmd("send 0 " + text.strip())
        w.run(20)
        self.assertEqual(w.units[2].texts(0), [text.strip()])


if __name__ == "__main__":
    unittest.main()

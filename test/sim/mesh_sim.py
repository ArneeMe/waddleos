"""Mesh units and the Pi server, on this computer.

Each unit is test/sim/sim_node (the real firmware mesh code with the hardware
replaced); the server is the real server/waddlemesh code. This file plays the
air between them: who hears whom is a table the tests change as they go.

Run: bash test/sim/run.sh   (builds sim_node, then runs these tests)
"""

import base64
import email.utils
import importlib.machinery
import importlib.util
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

# The setup tool's own X25519, for the admin's side of joining.
_loader = importlib.machinery.SourceFileLoader("waddle_mesh_tool", os.path.join(ROOT, "tools", "waddle-mesh"))
tool = importlib.util.module_from_spec(importlib.util.spec_from_loader("waddle_mesh_tool", _loader))
_loader.exec_module(tool)
NETWORK = 0x42
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
        self.roster_only = None
        self.air = []  # (who sent, raw) waiting to be heard next step
        self.store = Store(os.path.join(self.dir, "server.db"))
        self.server = Server(self.store, NETWORK)

        world = self

        class Radio:
            def send(self, raw):
                world.air.append((GATEWAY, raw))

        self.server.radio = Radio()
        self.server_wifi = server_wifi
        # The admin (protocol/PROTOCOL.md, "Joining"): keys kept here, as on Arne's computer.
        self.admin_seed = os.urandom(32)
        self.admin_box_secret = os.urandom(32)
        self.admin_box = tool.x25519_public(self.admin_box_secret)
        self.network_key = os.urandom(32)
        self.roster_version = 0
        self.publics = {}  # member -> X25519 public hex
        self.units = {}
        for n, name in enumerate(names, 1):
            self.units[n] = Unit(self, n, name)
        self.admin_sign = self.units[1].cmd("admin-sign " + self.admin_seed.hex())[0].split()[1]
        for n in self.units:
            self.join(n)

    # --- The admin's side --------------------------------------------------------

    def invite(self):
        password = os.urandom(16)
        raw = bytes([NETWORK]) + os.urandom(8) + password + self.admin_box + bytes(32) + b"https://pi.example:8443"
        return "waddle://join/" + base64.urlsafe_b64encode(raw).decode().rstrip("="), password

    def signed_roster(self):
        """A roster of every member that has joined (the admin signs a new version)."""
        self.roster_version += 1
        items = " ".join("%d:%s:%s" % (n, self.units[n].name, pub) for n, pub in sorted(self.publics.items()))
        out = self.units[min(self.units)].cmd("admin-roster %s %d %d %s" % (self.admin_seed.hex(), NETWORK,
                                                                              self.roster_version, items))
        return out[0].split()[1]

    def join(self, n):
        """Unit n joins as member n: invitation, request, roster, welcome."""
        u = self.units[n]
        link, password = self.invite()
        reply = u.cmd("join %s %s" % (link, u.name))[0].split()
        assert reply[0] == "join", reply
        self.publics[n] = reply[1]
        roster = self.signed_roster()
        welcome = {"network": NETWORK, "me": n, "name": u.name, "networkKey": self.network_key.hex(),
                   "adminSign": self.admin_sign, "adminBox": self.admin_box.hex(), "counterFloor": 0,
                   "roster": roster}
        if self.server_wifi:
            welcome["server"] = {"url": "https://pi.example:8443", "token": "t%d" % n,
                                 "cert": "-----BEGIN CERTIFICATE-----"}
        box = u.cmd("admin-welcome %s %s %s %s" % (self.admin_box_secret.hex(), password.hex(), reply[1],
                                                    json.dumps(welcome).encode().hex()))[0].split()[1]
        assert u.cmd("welcome " + box) == ["joined"]
        self.broadcast_roster(roster)

    def broadcast_roster(self, roster, only=None):
        """The server sends a new roster out over LoRa (here: straight into the air,
        to every unit, or only to the units in `only`)."""
        self.roster_only = only
        for line in self.units[min(self.units)].cmd("admin-packets %d %s %d %s" % (
                NETWORK, self.network_key.hex(), self.roster_version, roster)):
            self.air.append((GATEWAY, bytes.fromhex(line.split()[1])))
        self.all_hear_gateway = True

    def add_phone(self, n):
        """Member n pairs their phone (PHONE.md): the code on the unit's screen
        matches the phone's, then the unit hands over a sealed copy of its keys.
        The phone has no radio here, only Wi-Fi to the server."""
        u = self.units[n]
        phone = n | P.PHONE
        p = Unit(self, phone, u.name)
        commit = p.cmd("pairc-begin")[0].split()[1]
        answer = u.cmd("pair-start " + commit)[0].split()
        tool = p.cmd("pairc-answer %s %s" % (answer[3], answer[1]))[0].split()
        unit_code = u.cmd("pair-reveal %s %s" % (tool[1], tool[3]))
        assert unit_code == ["code " + " ".join(tool[5:7])], (unit_code, tool)
        request = p.cmd("pairc-request")[0].split()[1]
        box = u.cmd("pair-phone " + request)[0].split()
        assert box[0] == "box", box
        assert p.cmd("pairc-finish " + box[1]) == ["paired"]
        p.cmd("time %d" % self.now)
        p.cmd("wifi 1")
        p.cmd("begin")
        self.units[phone] = p
        return p

    def remove(self, n):
        """The admin takes member n out of the roster."""
        del self.publics[n]
        self.broadcast_roster(self.signed_roster())

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
                # The roster from the admin reaches every unit (in the field: the
                # server sends it at each unit's next sync).
                roster = sender == GATEWAY and packet_type(raw) == P.ROSTER and (
                    self.roster_only is None or n in self.roster_only)
                if n != sender and u.alive and (self.hears(sender, n) or roster):
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


class FourUnitTests(unittest.TestCase):
    def setUp(self):
        self.w = World(names=("Arne", "Kari", "Ola", "Per"))

    def tearDown(self):
        self.w.close()

    def test_fourth_member(self):
        w = self.w
        w.all_in_range()
        w.begin()
        w.run(5)
        w.units[4].cmd("send 0 Per her")
        w.units[1].cmd("send 4 Hei Per")
        w.run(15)
        for n in (1, 2, 3):
            self.assertEqual(w.units[n].texts(0), ["Per her"])
        self.assertEqual(w.units[4].texts(1), ["Hei Per"])
        self.assertEqual(w.units[4].chat(0)[0]["delivered"], 0b0111)

    def make_group(self, unit, bits, name):
        return int(self.w.units[unit].cmd("group %d %s" % (bits, name))[0].split()[1])

    def test_group_made_on_a_unit(self):
        w = self.w
        w.link(1, 2)
        w.link(1, 3)
        w.link(3, 4)  # Per hears only Ola, who is not in the group
        w.begin()
        w.run(5)
        conv = self.make_group(1, 0b1010, "Hyttetur")  # Arne makes it with Kari and Per
        w.run(20)
        for n in (2, 4):
            g = w.units[n].state()["groups"][str(conv)]
            self.assertEqual((g["name"], g["members"], g["key"]), ("Hyttetur", 0b1011, 1))
        self.assertNotIn("groups", w.units[3].state())
        self.assertEqual(w.units[1].cmd("send %d Uten Ola" % conv), ["sent"])
        w.run(20)
        self.assertEqual(w.units[2].texts(conv), ["Uten Ola"])
        self.assertEqual(w.units[4].texts(conv), ["Uten Ola"])  # relayed by Ola, who cannot read it
        self.assertTrue(all(not msgs for msgs in w.units[3].state()["chats"].values()))
        self.assertEqual(w.units[1].chat(conv)[0]["delivered"], 0b1010)
        self.assertEqual(w.units[4].cmd("send %d Fint" % conv), ["sent"])
        w.run(20)
        self.assertEqual(w.units[1].texts(conv), ["Uten Ola", "Fint"])
        self.assertEqual(w.units[3].cmd("send %d Hallo?" % conv), ["refused"])

    def test_leaving_gives_the_rest_a_new_key(self):
        w = self.w
        w.all_in_range()
        w.begin()
        w.run(5)
        conv = self.make_group(2, 0b1101, "Alle fire")  # Kari makes it with everyone
        w.run(20)
        w.units[1].cmd("send %d Før Per gikk" % conv)
        w.run(15)
        w.units[4].cmd("leave %d" % conv)
        w.run(30)
        # Arne (lowest number left) made key 2 and gave it to Kari and Ola.
        for n in (1, 2, 3):
            g = w.units[n].state()["groups"][str(conv)]
            self.assertEqual((g["members"], g["key"]), (0b0111, 2), "unit %d" % n)
        self.assertTrue(w.units[4].state()["groups"][str(conv)]["left"])
        w.units[3].cmd("send %d Etter Per gikk" % conv)
        w.run(20)
        self.assertEqual(w.units[2].texts(conv), ["Før Per gikk", "Etter Per gikk"])
        self.assertEqual(w.units[4].texts(conv), ["Før Per gikk"])  # keeps the old, not the new

    def test_removing_a_lost_unit(self):
        w = self.w
        w.all_in_range()
        w.begin()
        w.run(5)
        conv = self.make_group(1, 0b1110, "Tur")
        w.run(20)
        w.units[4].stop()  # Per's unit is lost
        w.units[2].cmd("remove %d 4" % conv)
        w.run(30)
        for n in (1, 2, 3):
            g = w.units[n].state()["groups"][str(conv)]
            self.assertEqual((g["members"], g["key"]), (0b0111, 2), "unit %d" % n)

    def test_invitation_and_messages_wait_for_a_unit_that_was_off(self):
        w = self.w
        w.all_in_range(gateway=True)
        w.units[4].stop()
        w.begin(1, 2, 3)
        w.run(5)
        conv = self.make_group(2, 0b1001, "Middag")  # Kari, with Arne and Per
        w.run(15)
        w.units[1].cmd("send %d Kl 18?" % conv)
        w.run(15)
        w.units[4].start()
        w.units[4].cmd("time %d" % w.now)
        w.begin(4)  # catches up from the server: Arne's message, then Kari's invitation
        w.run(30)
        per = w.units[4].state()
        self.assertEqual(per["groups"][str(conv)]["name"], "Middag")
        self.assertEqual(w.units[4].texts(conv), ["Kl 18?"])


class GroupSafetyTests(unittest.TestCase):
    """What the review found: clashing key changes, removed members, restarts."""

    def setUp(self):
        self.w = World(names=("Arne", "Kari", "Ola", "Per"))
        self.w.all_in_range()
        self.w.begin()
        self.w.run(5)

    def tearDown(self):
        self.w.close()

    def group(self, unit, bits, name="G"):
        conv = int(self.w.units[unit].cmd("group %d %s" % (bits, name))[0].split()[1])
        self.w.run(20)
        return conv

    def state(self, unit, conv):
        return self.w.units[unit].state()["groups"][str(conv)]

    def test_two_removals_at_once_both_happen(self):
        w = self.w
        conv = self.group(1, 0b1110)
        w.units[2].cmd("remove %d 4" % conv)  # Kari takes Per out ...
        w.units[1].cmd("remove %d 3" % conv)  # ... while Arne takes Ola out
        w.run(40)
        for n in (1, 2):
            self.assertEqual(self.state(n, conv)["members"], 0b0011, "unit %d" % n)
        w.units[1].cmd("send %d Bare oss to" % conv)
        w.run(20)
        self.assertEqual(w.units[2].texts(conv), ["Bare oss to"])
        self.assertEqual(w.units[3].texts(conv), [])
        self.assertEqual(w.units[4].texts(conv), [])

    def test_removed_member_cannot_write_or_take_over(self):
        w = self.w
        conv = self.group(1, 0b1110)
        w.units[1].cmd("remove %d 4" % conv)
        w.run(30)
        # Per still has key 1 and can send under it, but nobody shows it as new.
        w.units[4].cmd("send %d Hei, jeg er her fortsatt" % conv)
        w.run(20)
        for n in (1, 2, 3):
            self.assertEqual(w.units[n].texts(conv), [], "unit %d" % n)
            self.assertEqual(self.state(n, conv)["key"], 2)

    def test_invitation_survives_a_restart(self):
        w = self.w
        w.units[3].stop()
        conv = self.group(1, 0b0110, "Restart")  # Ola is off and misses it
        w.run(200)  # Arne's retries run out
        w.units[1].stop()
        w.units[1].start()
        w.units[1].cmd("time %d" % w.now)
        w.begin(1)  # Arne's unit restarts, then checks: the invitation goes again
        w.units[3].start()
        w.units[3].cmd("time %d" % w.now)
        w.begin(3)
        w.run(20)
        w.units[1].cmd("check")
        w.run(20)
        self.assertEqual(self.state(3, conv)["name"], "Restart")

    def test_leaving_twice_and_stale_invitation_do_not_rejoin(self):
        w = self.w
        conv = self.group(1, 0b1110)
        w.units[4].cmd("leave %d" % conv)
        w.run(30)
        self.assertTrue(self.state(4, conv)["left"])
        w.units[1].cmd("check")  # resends nothing to Per: he is out
        w.run(20)
        self.assertTrue(self.state(4, conv)["left"])
        self.assertEqual(self.state(1, conv)["members"], 0b0111)


class CheckTests(unittest.TestCase):
    """The unit as a backup: the radio sleeps and wakes to check now and then."""

    def setUp(self):
        self.w = World(names=("Arne", "Kari"), server_wifi=False)

    def tearDown(self):
        self.w.close()

    def test_default_checks_once_a_day_at_home(self):
        w = self.w
        w.begin()
        kari = w.units[2].state()
        self.assertFalse(kari["listensAlways"])
        self.assertGreater(kari["msUntilCheck"], 23 * 3600 * 1000)
        w.units[2].cmd("checks 0 2")  # "Lytt hele tiden" at home
        self.assertTrue(w.units[2].state()["listensAlways"])
        w.units[2].cmd("trip 1")  # on a trip: every hour
        self.assertFalse(w.units[2].state()["listensAlways"])

    def test_message_waits_on_the_server_until_the_check(self):
        w = self.w
        w.all_in_range(gateway=True)
        w.begin()
        w.run(25)
        w.units[2].cmd("pause")  # Kari's unit sleeps between checks
        w.units[1].cmd("send 0 Ring meg")
        w.run(10)
        self.assertEqual(w.units[2].texts(0), [])
        self.assertTrue(w.units[1].chat(0)[0]["stored"])  # the server has it
        w.units[2].cmd("resume")
        w.units[2].cmd("check")  # the check (or a press of the button)
        w.run(10)
        self.assertEqual(w.units[2].texts(0), ["Ring meg"])
        self.assertTrue(w.units[2].state()["lastCheck"])
        w.run(10)
        self.assertEqual(w.units[1].chat(0)[0]["delivered"], 0b10)


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
        w.run(20)  # sender keys swapped (their answers are ACKs too)
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
        self.assertTrue(arne["sosOnServer"])
        w.units[2].cmd("seen 1")
        w.run(5)
        self.assertEqual(w.units[1].state()["sosSeenBy"], 0b010)
        w.units[1].cmd("cancel")
        w.run(10)
        for n in (2, 3):
            self.assertFalse(w.units[n].state()["members"]["1"]["sos"])

    def test_card_shows_no_messages(self):
        """Everything on the SD card is sealed: no text, no names, no positions."""
        w = self.w
        w.all_in_range()
        w.begin()
        w.run(5)
        w.units[1].cmd("send 0 Hemmelig til alle")
        w.units[1].cmd("send 2 Hemmelig til Kari")
        w.run(15)
        w.units[2].cmd("read 1")
        w.run(5)
        self.assertEqual(w.units[2].texts(1), ["Hemmelig til Kari"])
        checked = 0
        for n in (1, 2):
            for root, _, files in os.walk(w.units[n].dir):
                for f in files:
                    if "/.mesh" not in os.path.join(root, f):
                        continue
                    with open(os.path.join(root, f), "rb") as fh:
                        data = fh.read()
                    self.assertNotIn(b"Hemmelig", data, f)
                    self.assertNotIn(b"Kari", data, f)
                    checked += 1
        self.assertGreater(checked, 10)

    def test_a_member_cannot_take_over_or_make_up_a_group(self):
        w = self.w
        w.all_in_range()
        w.begin()
        w.run(10)
        w.units[1].cmd("group %d Hytta" % 0b10)  # Arne and Kari
        w.run(15)
        gid = list(w.units[2].state()["groups"].values())[0]["id"]

        def invitation(group_id, members, name):
            return (b"\0G" + group_id.to_bytes(2, "little") + bytes([1, 1]) + members.to_bytes(4, "little") +
                    os.urandom(32) + name).hex()

        # Ola, not in it, says the group starts afresh with him in it, and makes
        # up a group under Arne's id.
        w.units[3].cmd("control 2 " + invitation(gid, 0b111, b"Kapret"))
        w.units[3].cmd("control 2 " + invitation(1 * 256 + 77, 0b110, b"Falsk"))
        w.run(15)
        groups = list(w.units[2].state()["groups"].values())
        self.assertEqual([(g["id"], g["name"], g["members"]) for g in groups], [(gid, "Hytta", 0b011)])
        self.assertEqual(w.units[3].state().get("groups", {}), {})

    def test_lost_key_is_asked_for(self):
        """Arne's sender key never reaches Kari and someone forges her
        confirmation: she asks for it when his message comes."""
        w = self.w
        w.all_in_range()
        lost = set()

        def drop(raw, sender, receiver):
            h = P.parse_header(raw)
            if h.type == P.MSG and h.sender == 1 and h.recipient == 2 and (collecting or h.counter in lost):
                lost.add(h.counter)
                return True
            return False

        collecting = True
        w.drop = drop
        w.begin()
        w.run(5)
        collecting = False
        self.assertTrue(lost)
        for i, c in enumerate(sorted(lost)):
            h = P.Header(type=P.ACK, network=NETWORK, sender=2, recipient=1, counter=4000000000 + i)
            w.air.append((2, h.pack() + P.Ack(1, c, 1).pack()))
        w.run(20)
        w.units[1].cmd("send 0 Til alle")
        w.run(30)
        self.assertEqual(w.units[2].texts(0), ["Til alle"])

    def test_units_pass_a_new_roster_on(self):
        """Out of the gateway's reach, a unit that missed a new roster gets it
        from another unit's sync answer."""
        w = self.w
        w.all_in_range()
        w.begin()
        w.run(10)
        before = w.units[2].state()["rosterVersion"]
        w.link(1, 2, False)  # Kari away while it comes (units relay it too)
        w.link(2, 3, False)
        w.broadcast_roster(w.signed_roster(), only={1})
        w.run(5)
        w.all_in_range()
        self.assertEqual(w.units[1].state()["rosterVersion"], before + 1)
        self.assertEqual(w.units[2].state()["rosterVersion"], before)
        w.units[2].cmd("check")
        w.run(20)
        self.assertEqual(w.units[2].state()["rosterVersion"], before + 1)

    def test_removed_unit_stays_out_after_a_restart(self):
        w = self.w
        w.all_in_range()
        w.begin()
        w.run(10)
        w.remove(3)
        w.run(10)
        self.assertFalse(w.units[3].state()["configured"])
        w.units[3].stop()
        w.units[3].start()
        w.units[3].cmd("begin")
        self.assertFalse(w.units[3].state()["configured"])

    def test_taken_out_and_back_as_the_same_number(self):
        """A lost phone or unit: Arne takes Ola out, Ola's unit makes new keys,
        and he comes back as member 3 with his chats."""
        w = self.w
        w.all_in_range()
        w.begin()
        w.run(10)
        w.units[1].cmd("send 3 Før")
        w.run(15)
        old = w.publics[3]
        w.remove(3)
        w.run(10)
        self.assertFalse(w.units[3].state()["configured"])
        w.join(3)
        self.assertNotEqual(w.publics[3], old)
        w.units[3].cmd("begin")
        w.run(20)
        w.units[1].cmd("send 3 Etter")
        w.units[3].cmd("send 0 Tilbake")
        w.run(30)
        self.assertEqual(w.units[3].texts(1), ["Før", "Etter"])
        self.assertEqual(w.units[2].texts(0), ["Tilbake"])

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
        w.run(70)  # sender keys go out spread over a minute
        w.units[2].cmd("trip 1")
        w.run(10)
        kari = w.units[1].state()["members"]["2"]
        self.assertTrue(kari["hasPosition"])
        self.assertEqual(kari["lat"], 600000000)

    def test_missing_message_is_fetched(self):
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
        w.run(15)
        # "Tre" said a message was missing; Ola asked for it and got it.
        ola = w.units[3].chat(0)
        self.assertEqual([m["text"] for m in ola], ["En", "To", "Tre"])
        self.assertEqual([m["warning"] for m in ola], [False, False, False])

    def test_missing_message_nobody_has_shows_a_warning(self):
        w = self.w
        w.all_in_range()
        w.begin()
        w.run(5)
        w.units[1].cmd("send 0 En")
        w.run(10)
        w.units[1].cmd("send 0 To")
        lost = w.units[1].chat(0)[-1]["counter"]
        # Every copy of "To" to Ola is lost, the fetched ones too.
        w.drop = lambda raw, sender, receiver: (receiver == 3 and packet_type(raw) == P.MSG
                                                and P.parse_header(raw).counter == lost)
        w.run(10)
        w.units[1].cmd("send 0 Tre")
        w.run(15)
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


    def test_piece_lost_for_minutes_no_false_receipt(self):
        """One piece of a long DM is lost for longer than the piece assembler
        waits: the message still completes from the pieces on the card, and the
        sender is not told "delivered" before it has."""
        w = self.w
        w.all_in_range()
        w.begin()
        w.run(70)  # sender keys spread over a minute
        blocking = True

        def drop(raw, sender, receiver):
            h = P.parse_header(raw)
            return blocking and h.type == P.MSG and h.sender == 1 and h.recipient == 2 and h.piece_index == 1
        w.drop = drop
        text = ("Lang melding til Kari. " * 30).strip()  # three pieces
        w.units[1].cmd("send 2 " + text)
        w.run(100)  # the first retry (after 30 s) is lost too
        self.assertEqual(w.units[2].texts(1), [])
        self.assertEqual(w.units[1].chat(2)[0]["delivered"], 0)
        blocking = False
        w.run(120)  # the next retry
        self.assertEqual(w.units[2].texts(1), [text])
        self.assertEqual(w.units[1].chat(2)[0]["delivered"], 0b10)

    def test_power_cut_while_adding_a_message(self):
        """Half a record at the end of a chat log does not hide later messages."""
        w = self.w
        w.all_in_range()
        w.begin()
        w.run(70)
        w.units[1].cmd("send 0 Første")
        w.run(15)
        u = w.units[3]
        u.stop()
        with open(os.path.join(u.dir, ".mesh", "chat", "0.sealed"), "ab") as f:
            f.write(bytes([60, 0]) + os.urandom(20))  # cut off mid-write
        u.start()
        u.cmd("time %d" % w.now)
        u.cmd("begin")
        w.units[1].cmd("send 0 Andre")
        w.run(15)
        self.assertEqual(u.texts(0), ["Første", "Andre"])

    def test_power_cut_while_replacing_a_file(self):
        """Only the new copy (path.tmp) is left when the power went between
        removing the old file and renaming the new one: it is used."""
        w = self.w
        w.all_in_range()
        w.begin()
        w.run(70)
        w.units[1].cmd("group %d Hytta" % 0b110)
        w.run(20)
        u = w.units[3]
        u.stop()
        mesh = os.path.join(u.dir, ".mesh")
        for name in ("roster.bin", "groups.bin"):
            os.rename(os.path.join(mesh, name), os.path.join(mesh, name + ".tmp"))
        u.start()
        u.cmd("time %d" % w.now)
        u.cmd("begin")
        state = u.state()
        self.assertTrue(state["configured"])
        self.assertEqual([g["name"] for g in state["groups"].values()], ["Hytta"])

    def test_damaged_groups_file_keeps_the_chats(self):
        w = self.w
        w.all_in_range()
        w.begin()
        w.run(70)
        conv = int(w.units[1].cmd("group %d Hytta" % 0b110)[0].split()[1])
        w.run(20)
        w.units[1].cmd("send %d Hei gruppa" % conv)
        w.run(20)
        u = w.units[3]
        theirs = list(u.state()["groups"])[0]
        u.stop()
        with open(os.path.join(u.dir, ".mesh", "groups.bin"), "r+b") as f:
            f.seek(20)
            f.write(b"\0" * 8)
        u.start()
        u.cmd("time %d" % w.now)
        u.cmd("begin")
        self.assertEqual(u.texts(theirs), ["Hei gruppa"])

    def test_group_with_member_nine_and_up(self):
        w = World(names=tuple("Unit%d" % n for n in range(1, 11)))
        try:
            w.all_in_range()
            w.begin()
            w.run(70)
            conv = int(w.units[1].cmd("group %d Store" % ((1 << 9) | (1 << 1)))[0].split()[1])  # 2 and 10
            w.run(30)
            self.assertEqual(w.units[1].state()["groups"][str(conv)]["members"], (1 << 9) | (1 << 1) | 1)
        finally:
            w.close()


    def test_not_busy_25_days_after_a_check(self):
        """"Når jeg vekker den": weeks after its last check the unit must not
        think it is still listening for answers (millis() wraps at 2^31)."""
        w = self.w
        w.units[1].cmd("checks 5 5")
        w.begin()
        w.run(90)
        self.assertFalse(w.units[1].state()["busy"])
        w.now += 2**31 + 10000
        w.run(1)
        self.assertFalse(w.units[1].state()["busy"])

    def test_many_small_packets_fit_the_airtime(self):
        """The airtime limit counts time, not packets: a busy hour with many
        ACKs and relays still lets a unit send (it used to stop at 96)."""
        w = World(names=tuple("Unit%d" % n for n in range(1, 11)))
        try:
            w.all_in_range()
            w.begin()
            w.run(70)
            conv = int(w.units[1].cmd("group %d Store" % ((1 << 9) | (1 << 1)))[0].split()[1])
            w.run(60)
            self.assertEqual(len(w.units[10].state()["groups"]), 1)
            w.units[1].cmd("send %d Hei" % conv)
            w.run(30)
            self.assertEqual(w.units[10].texts(list(w.units[10].state()["groups"])[0]), ["Hei"])
        finally:
            w.close()

    def test_server_answers_are_not_passed_on(self):
        """A check answered by the Pi over LoRa: the other units do not send the
        answer on again, and a second check gets nothing it already had."""
        w = self.w
        w.all_in_range(gateway=True)
        w.begin()
        w.run(90)
        w.units[1].cmd("send 0 Hei")
        w.run(30)
        sent = []
        real = w.step

        def counting():
            for sender, raw in w.air:
                sent.append(sender)
            real()
        w.step = counting
        for n in (1, 2, 3):
            w.units[n].cmd("check")
        w.run(30)
        relays = [s for s in sent if s != GATEWAY]
        self.assertLessEqual(len(relays), 12, sent)



class PhoneTests(unittest.TestCase):
    """Kari has a T5 and a phone (PHONE.md): one person, two devices."""

    def setUp(self):
        self.w = World()
        w = self.w
        w.all_in_range(gateway=True)
        w.begin()
        w.run(90)  # sender keys go out over a minute
        self.phone = w.add_phone(2)
        w.run(5)

    def tearDown(self):
        self.w.close()

    def sync(self, unit):
        unit.cmd("wifi 1")
        unit.cmd("wifisync")
        self.w.run(3)

    def test_phone_reads_and_writes_as_kari(self):
        w, phone = self.w, self.phone
        w.units[1].cmd("send 2 Hei Kari")
        w.run(20)
        self.sync(phone)
        self.assertEqual(phone.texts(1), ["Hei Kari"])
        # From the phone to Arne: through the Pi, out over LoRa by the gateway.
        phone.cmd("send 1 Fra mobilen")
        self.sync(phone)
        w.run(20)
        arne = w.units[1].chat(2)
        self.assertEqual([m["text"] for m in arne], ["Hei Kari", "Fra mobilen"])
        self.assertEqual(arne[1]["sender"], 2 | P.PHONE)
        # Kari's T5 shows the phone's message as her own (from the server).
        self.sync(w.units[2])
        self.assertEqual(w.units[2].texts(1), ["Hei Kari", "Fra mobilen"])
        # To everyone from the phone: the others open it with Kari's sender key.
        phone.cmd("send 0 Alle fra mobilen")
        self.sync(phone)
        w.run(20)
        self.assertEqual(w.units[3].texts(0), ["Alle fra mobilen"])

    def test_groups_and_new_keys_reach_the_phone(self):
        w, phone = self.w, self.phone
        conv = int(w.units[2].cmd("group %d Tur" % 0b101)[0].split()[1])  # Kari, Arne, Ola
        w.run(30)
        self.sync(phone)
        groups = list(phone.state()["groups"].values())
        self.assertEqual([(g["name"], g["members"]) for g in groups], [("Tur", 0b111)])
        # Ola is taken out: the phone stops writing to everyone until Kari's
        # unit sends it the new sender key (a note to self).
        w.remove(3)
        w.run(5)
        self.assertNotEqual(phone.cmd("send 0 For tidlig"), ["sent"])
        w.run(70)
        self.sync(phone)
        self.assertEqual(phone.cmd("send 0 Nå går det"), ["sent"])
        self.sync(phone)
        w.run(20)
        self.assertEqual(w.units[1].texts(0)[-1:], ["Nå går det"])
        del conv


if __name__ == "__main__":
    unittest.main()

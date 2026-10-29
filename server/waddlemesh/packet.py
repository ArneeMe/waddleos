"""The mesh packet format (protocol/PROTOCOL.md), for the parts the server reads.

The server never decrypts anything: it reads headers, ACK and SYNC_REQ bodies,
and stores every packet exactly as received.
"""

import math
import struct
from dataclasses import dataclass, field

MAGIC = 0x57
VERSION = 3  # 3: up to 32 members, a signed roster, 2-byte group ids
MAX_PACKET = 200
HEADER_SIZE = 14
DEFAULT_HOPS = 3
SERVER = 0
GROUP = 0xFF     # everyone
GROUP_TO = 0xFE  # a group made on a unit; its id is in the header
MEMBERS = tuple(range(1, 33))

MSG, ACK, SYNC_REQ, POS, SOS, FETCH, ROSTER = 1, 2, 3, 4, 5, 6, 7
TYPE_NAMES = {MSG: "MSG", ACK: "ACK", SYNC_REQ: "SYNC_REQ", POS: "POS", SOS: "SOS", FETCH: "FETCH", ROSTER: "ROSTER"}

# SYNC_REQ flags: how the unit is set up (for the status page).
SYNC_ON_TRIP, SYNC_SOS, SYNC_CHECK_SHIFT = 1, 2, 2
CHECK_NAMES = ["lytter hele tiden", "hvert kvarter", "hver time", "hver 6. time", "en gang i døgnet",
               "når den vekkes"]

DELIVERED, STORED, READ = 1, 2, 3


class BadPacket(ValueError):
    pass


@dataclass
class Header:
    type: int
    group: int
    sender: int
    recipient: int
    counter: int
    hops: int = DEFAULT_HOPS
    fragmented: bool = False
    piece_index: int = 0
    piece_count: int = 1
    group_id: int = 0  # recipient GROUP_TO: creator * 256 + their count
    key: int = 0       # which of the sender's or the group's keys sealed it

    def pack(self):
        return bytes([
            MAGIC,
            (VERSION << 4) | self.type,
            self.group,
            (self.hops & 0x0F) | (0x10 if self.fragmented else 0),
            self.sender,
            self.recipient,
        ]) + struct.pack("<IBHB", self.counter, (self.piece_index << 4) | self.piece_count, self.group_id, self.key)

    def aad(self):
        """Header bytes 0-2, 4-9 and 11-13: authenticated, not encrypted."""
        raw = self.pack()
        return raw[0:3] + raw[4:10] + raw[11:14]

    def nonce(self):
        return bytes([self.sender, self.recipient]) + struct.pack("<I", self.counter) + bytes([self.type]) + bytes(5)


def is_member(n):
    return n in MEMBERS


def is_group(recipient):
    """A group made on a unit (who is in it, only its members know)."""
    return recipient == GROUP_TO


def can_read(member, sender, recipient):
    """Whether member may be given a packet sender sent to recipient. Groups go
    to everyone; only those with the key can open them."""
    return recipient in (GROUP, GROUP_TO) or member in (sender, recipient)


def parse_header(raw):
    if len(raw) < HEADER_SIZE or len(raw) > MAX_PACKET:
        raise BadPacket("length %d" % len(raw))
    if raw[0] != MAGIC or raw[1] >> 4 != VERSION:
        raise BadPacket("not a WaddleOS packet")
    ptype = raw[1] & 0x0F
    if ptype not in TYPE_NAMES:
        raise BadPacket("type %d" % ptype)
    h = Header(
        type=ptype,
        group=raw[2],
        hops=raw[3] & 0x0F,
        fragmented=bool(raw[3] & 0x10),
        sender=raw[4],
        recipient=raw[5],
        counter=struct.unpack_from("<I", raw, 6)[0],
        piece_index=raw[10] >> 4,
        piece_count=raw[10] & 0x0F,
        group_id=struct.unpack_from("<H", raw, 11)[0],
        key=raw[13],
    )
    if h.piece_count == 0 or h.piece_index >= h.piece_count or (not h.fragmented and h.piece_count != 1):
        raise BadPacket("pieces")
    if h.sender != SERVER and not is_member(h.sender):
        raise BadPacket("sender %d" % h.sender)
    if h.recipient not in (GROUP, GROUP_TO, SERVER) and not is_member(h.recipient):
        raise BadPacket("recipient %d" % h.recipient)
    if (h.recipient == GROUP_TO) != (h.group_id != 0) or (h.group_id and not is_member(h.group_id >> 8)):
        raise BadPacket("group id %d" % h.group_id)
    return h


@dataclass
class Ack:
    sender: int
    counter: int
    state: int

    def pack(self):
        return struct.pack("<BIB", self.sender, self.counter, self.state)


def parse_ack(body):
    if len(body) != 6 or body[5] not in (DELIVERED, STORED, READ):
        raise BadPacket("ack body")
    sender, counter, state = struct.unpack("<BIB", body)
    return Ack(sender, counter, state)


@dataclass
class SyncReq:
    last_counter: dict = field(default_factory=dict)  # member -> last MSG counter it has (0 when missing)
    battery: int = 0
    build: int = 0
    flags: int = 0
    roster_version: int = 0

    def pack(self):
        out = struct.pack("<BBBH", self.battery, self.build, self.flags, self.roster_version)
        for m in MEMBERS:
            if self.last_counter.get(m):
                out += struct.pack("<BI", m, self.last_counter[m])
        return out


def parse_sync_req(body):
    if len(body) < 5 or (len(body) - 5) % 5 or len(body) > 5 + 5 * len(MEMBERS):
        raise BadPacket("sync body")
    battery, build, flags, roster = struct.unpack_from("<BBBH", body)
    last = {m: 0 for m in MEMBERS}
    for i in range(5, len(body), 5):
        member, counter = struct.unpack_from("<BI", body, i)
        if not is_member(member):
            raise BadPacket("sync member %d" % member)
        last[member] = counter
    return SyncReq(last, battery, build, flags, roster)


@dataclass
class Fetch:
    """Send me sender's messages between these two counters (one the chain says is missing)."""
    sender: int
    after: int
    before: int

    def pack(self):
        return struct.pack("<BII", self.sender, self.after, self.before)


def parse_fetch(body):
    if len(body) != 9:
        raise BadPacket("fetch body")
    f = Fetch(*struct.unpack("<BII", body))
    if not is_member(f.sender) or f.before <= f.after:
        raise BadPacket("fetch range")
    return f


def frame(packets):
    """Packets for HTTP: each prefixed by its length (one byte)."""
    out = bytearray()
    for p in packets:
        out.append(len(p))
        out += p
    return bytes(out)


def unframe(data):
    packets, i = [], 0
    while i < len(data):
        n = data[i]
        if n == 0 or i + 1 + n > len(data):
            raise BadPacket("framing")
        packets.append(bytes(data[i + 1:i + 1 + n]))
        i += 1 + n
    return packets


def airtime_ms(payload_bytes, sf=10, bw_khz=250.0, cr=5, preamble=16):
    """Time on air, same formula as lib/WaddleMesh/src/MeshRadioRules.cpp."""
    symbol = (2 ** sf) / bw_khz
    low_rate = 1 if symbol > 16.0 else 0
    symbols = math.ceil((8 * payload_bytes - 4 * sf + 28 + 16) / (4 * (sf - 2 * low_rate))) * cr
    return math.ceil((preamble + 4.25) * symbol + (8 + max(symbols, 0)) * symbol)

"""The mesh packet format (protocol/PROTOCOL.md), for the parts the server reads.

The server never decrypts anything: it reads headers, ACK and SYNC_REQ bodies,
and stores every packet exactly as received.
"""

import math
import struct
from dataclasses import dataclass, field

MAGIC = 0x57
VERSION = 1
MAX_PACKET = 200
HEADER_SIZE = 11
DEFAULT_HOPS = 3
SERVER = 0
GROUP = 0xFF
MEMBERS = (1, 2, 3)

MSG, ACK, SYNC_REQ, POS, SOS = 1, 2, 3, 4, 5
TYPE_NAMES = {MSG: "MSG", ACK: "ACK", SYNC_REQ: "SYNC_REQ", POS: "POS", SOS: "SOS"}

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

    def pack(self):
        return bytes([
            MAGIC,
            (VERSION << 4) | self.type,
            self.group,
            (self.hops & 0x0F) | (0x10 if self.fragmented else 0),
            self.sender,
            self.recipient,
        ]) + struct.pack("<I", self.counter) + bytes([(self.piece_index << 4) | self.piece_count])

    def aad(self):
        """Header bytes 0-2 and 4-9: authenticated, not encrypted."""
        raw = self.pack()
        return raw[0:3] + raw[4:10]

    def nonce(self):
        return bytes([self.sender, self.recipient]) + struct.pack("<I", self.counter) + bytes([self.type]) + bytes(5)


def is_member(n):
    return n in MEMBERS


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
    )
    if h.piece_count == 0 or h.piece_index >= h.piece_count or (not h.fragmented and h.piece_count != 1):
        raise BadPacket("pieces")
    if h.sender != SERVER and not is_member(h.sender):
        raise BadPacket("sender %d" % h.sender)
    if h.recipient not in (GROUP, SERVER) and not is_member(h.recipient):
        raise BadPacket("recipient %d" % h.recipient)
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
    last_counter: dict = field(default_factory=lambda: {1: 0, 2: 0, 3: 0})
    battery: int = 0
    build: int = 0

    def pack(self):
        return struct.pack("<IIIBB", self.last_counter[1], self.last_counter[2], self.last_counter[3],
                           self.battery, self.build)


def parse_sync_req(body):
    if len(body) != 14:
        raise BadPacket("sync body")
    a, b, c, battery, build = struct.unpack("<IIIBB", body)
    return SyncReq({1: a, 2: b, 3: c}, battery, build)


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

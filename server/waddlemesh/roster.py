"""The roster: who is in the network, signed by the admin (protocol/PROTOCOL.md,
"The roster"). The server only reads it: names for the status page, token
hashes to know a unit on Wi-Fi."""

import struct
from dataclasses import dataclass, field

from . import ed25519
from .packet import is_member

FORMAT = 1
HEAD = 5  # format, network, version (2), count
SIGNATURE = 64
NAME_MAX = 20


class BadRoster(ValueError):
    pass


@dataclass
class Member:
    number: int
    admin: bool
    name: str
    public: bytes      # X25519, 32 bytes
    token_hash: bytes  # first 16 bytes of SHA-256 of the server token's hex text


@dataclass
class Roster:
    network: int
    version: int
    members: list = field(default_factory=list)

    def member(self, number):
        return next((m for m in self.members if m.number == number), None)


def parse(data, admin_key=None):
    """Reads a signed roster, checking the admin's signature when admin_key is given."""
    data = bytes(data)
    if len(data) < HEAD + SIGNATURE or data[0] != FORMAT:
        raise BadRoster("not a roster")
    signed = len(data) - SIGNATURE
    if admin_key is not None and not ed25519.verify(admin_key, data[:signed], data[signed:]):
        raise BadRoster("not signed by the admin")
    network, version, count = data[1], struct.unpack_from("<H", data, 2)[0], data[4]
    r, i = Roster(network, version), HEAD
    for _ in range(count):
        if i + 3 > signed:
            raise BadRoster("short")
        number, flags, name_len = data[i:i + 3]
        i += 3
        if not is_member(number) or r.member(number) or name_len > NAME_MAX or i + name_len + 48 > signed:
            raise BadRoster("member %d" % number)
        name = data[i:i + name_len].decode("utf-8", "replace")
        i += name_len
        r.members.append(Member(number, bool(flags & 1), name, data[i:i + 32], data[i + 32:i + 48]))
        i += 48
    if i != signed:
        raise BadRoster("length")
    return r

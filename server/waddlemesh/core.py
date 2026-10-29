"""What the server does with a packet, whichever way it came (LoRa or Wi-Fi)."""

import collections
import hashlib
import hmac
import logging
import time

from . import packet as P
from . import roster as R

log = logging.getLogger("waddlemesh")

# Over LoRa an answer must stay short: at most this many packets per SYNC_REQ.
LORA_SYNC_LIMIT = 12
WIFI_SYNC_LIMIT = 200  # a unit that was away long asks again while answers are full
# The status page shows an SOS packet this long.
SOS_SHOWN_S = 300


def no_hops(raw):
    """A copy with hops 0: answers to one unit are not passed on by the others."""
    return raw[:3] + bytes([raw[3] & 0xF0]) + raw[4:]


class Server:
    def __init__(self, store, network, admin_key=None):
        self.store = store
        self.network = network
        self.admin_key = admin_key  # Ed25519 public key that signs the roster
        self._roster = None  # (version, parsed roster), read again when a newer one is set
        self.started = time.time()
        self.radio = None  # set by the gateway link: radio.send(raw)
        self.relayed = collections.deque(maxlen=256)  # packets already passed on

    def roster(self):
        """The newest roster (set by the admin's tool with set-roster), or None."""
        version = self.store.roster_version()
        if not version:
            return None
        if not self._roster or self._roster[0] != version:
            _, signed, _ = self.store.roster()
            try:
                self._roster = (version, R.parse(signed, self.admin_key))
            except R.BadRoster as e:
                log.error("stored roster %d is no good: %s", version, e)
                return None
        return self._roster[1]

    def member_for_token(self, token):
        """The member whose roster token hash matches token (its hex text), or None."""
        r = self.roster()
        if not r or not token:
            return None
        digest = hashlib.sha256(token.encode()).digest()[:16]
        found = None
        for m in r.members:
            if hmac.compare_digest(digest, m.token_hash):
                found = m.number
        return found

    def _ack(self, member, counter, state, to):
        h = P.Header(type=P.ACK, network=self.network, sender=P.SERVER, recipient=to,
                     counter=self.store.next_counter())
        return h.pack() + P.Ack(member, counter, state).pack()

    def handle(self, raw, via, rssi=None, snr=None, handed_over=False):
        """Handles one packet. Returns the packets to send back to that unit.

        via is "lora" or "wifi". Packets that arrived over Wi-Fi and are meant
        for other units are also sent out over LoRa, so units in range of the
        gateway get them at once. handed_over: kept by the gateway while the Pi
        was away; it relayed and answered then, so only store it.
        """
        try:
            h = P.parse_header(raw)
        except P.BadPacket as e:
            log.debug("dropped packet: %s", e)
            return []
        if h.network != self.network or h.sender == P.SERVER:
            return []
        if handed_over:
            if h.type in (P.MSG, P.POS, P.SOS):
                self.store.add_packet(h, raw, via)
            elif h.type == P.ACK:
                try:
                    self.store.add_ack(P.parse_ack(raw[P.HEADER_SIZE:]), h.sender, raw)
                except P.BadPacket:
                    pass
            return []
        self.store.seen_unit(h.sender, via, rssi=rssi, snr=snr)
        if via == "lora":
            self._relay(h, raw)

        if h.type in (P.MSG, P.POS, P.SOS):
            new = self.store.add_packet(h, raw, via)
            if new and via == "wifi" and self.radio:
                self.radio.send(raw)
            if h.type == P.SOS:
                self.store.sos_heard(h.sender)
                if new:
                    log.warning("SOS from member %d", h.sender)
                # The sender's SOS screen shows that the server has it.
                return [self._ack(h.sender, h.counter, P.STORED, h.sender)]
            if h.type == P.MSG and self.store.has_all_pieces(h):
                return [self._ack(h.sender, h.counter, P.STORED, h.sender)]
            return []

        body = raw[P.HEADER_SIZE:]
        if h.type == P.ACK:
            try:
                ack = P.parse_ack(body)
            except P.BadPacket:
                return []
            self.store.add_ack(ack, h.sender, raw)
            if via == "wifi" and self.radio:
                self.radio.send(raw)
            return []

        if h.type == P.SYNC_REQ:
            try:
                req = P.parse_sync_req(body)
            except P.BadPacket:
                return []
            self.store.seen_unit(h.sender, via, battery=req.battery, build="%02x" % req.build, flags=req.flags)
            return self.sync_answer(h.sender, req, via)

        if h.type == P.FETCH:
            try:
                f = P.parse_fetch(body)
            except P.BadPacket:
                return []
            limit = LORA_SYNC_LIMIT if via == "lora" else WIFI_SYNC_LIMIT
            return [no_hops(raw) for raw in self.store.messages_between(f.sender, f.after, f.before, h.sender, limit)]
        return []

    def _relay(self, h, raw):
        """The gateway passes packets on like a unit does (protocol/PROTOCOL.md,
        "Relaying"), at once, so the units hear it and skip their own relay."""
        key = (h.type, h.sender, h.counter, h.piece_index)
        if not self.radio or h.hops == 0 or h.type == P.SYNC_REQ or key in self.relayed:
            return
        self.relayed.append(key)
        out = bytearray(raw)
        out[3] = (out[3] & 0xF0) | (h.hops - 1)
        self.radio.send(bytes(out))

    def sync_answer(self, member, req, via):
        """Everything member is missing: the newest roster when its own is older,
        then messages, positions and ACKs. Over LoRa the roster always goes whole
        (up to 13 pieces with 32 members), and first, so it is never cut off;
        positions and ACKs only when new since its last LoRa sync. Everything
        goes with hops 0: the unit asked, the others need not pass it on."""
        lora = via == "lora"
        limit = LORA_SYNC_LIMIT if lora else WIFI_SYNC_LIMIT
        since = self.store.last_sync(member) if lora else 0
        if lora:
            self.store.set_last_sync(member)
        out = []
        stored = self.store.roster()
        if stored and req.roster_version < stored[0]:
            out += stored[2]
            limit = max(limit, len(out))
        for sender in P.MEMBERS:
            if sender == member:
                continue
            out += self.store.messages_after(sender, req.last_counter[sender], member, limit)
        out += self.store.latest_positions(member, since)
        for counter in self.store.newest_whole_messages(member, since=since):
            out.append(self._ack(member, counter, P.STORED, member))
        out += self.store.acks_for(member, max(0, req.last_counter.get(member, 0) - 50), since)
        out = [no_hops(raw) for raw in out[:limit]]
        log.info("sync for member %d via %s: %d packets", member, via, len(out))
        return out

    def status(self):
        """Every unit set up or heard, for the status page (no keys, no content)."""
        now = int(time.time())
        heard = {u["member"]: u for u in self.store.units()}
        r = self.roster()
        names = {m.number: m.name for m in r.members} if r else {}
        units = []
        for member in sorted(set(heard) | set(names)):
            u = heard.get(member) or {"member": member, "last_seen": None, "via": None, "battery": None,
                                      "build": None, "rssi": None, "snr": None, "flags": None, "sos_at": None}
            u["name"] = names.get(member) or "Enhet %d" % member
            u["ago_s"] = now - u["last_seen"] if u["last_seen"] else None
            flags = u["flags"]
            u["on_trip"] = None if flags is None else bool(flags & P.SYNC_ON_TRIP)
            check = None if flags is None else (flags >> P.SYNC_CHECK_SHIFT) & 7
            u["check"] = P.CHECK_NAMES[check] if check is not None and check < len(P.CHECK_NAMES) else None
            # SOS: the unit says its SOS is on, or an SOS packet came in the last 5 minutes.
            u["sos"] = bool(flags and flags & P.SYNC_SOS) or bool(u["sos_at"] and now - u["sos_at"] < SOS_SHOWN_S)
            units.append(u)
        counts = self.store.counts()
        return {"units": units, "packets": {P.TYPE_NAMES[t]: n for t, n in sorted(counts.items())},
                "roster": r.version if r else None, "uptime_s": int(time.time() - self.started)}

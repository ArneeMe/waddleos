"""What the server does with a packet, whichever way it came (LoRa or Wi-Fi)."""

import collections
import logging
import time

from . import packet as P

log = logging.getLogger("waddlemesh")

# Over LoRa an answer must stay short: at most this many packets per SYNC_REQ.
LORA_SYNC_LIMIT = 12


class Server:
    def __init__(self, store, group):
        self.store = store
        self.group = group
        self.radio = None  # set by the gateway link: radio.send(raw)
        self.relayed = collections.deque(maxlen=256)  # packets already passed on

    def _ack(self, member, counter, state, to):
        h = P.Header(type=P.ACK, group=self.group, sender=P.SERVER, recipient=to,
                     counter=self.store.next_counter())
        return h.pack() + P.Ack(member, counter, state).pack()

    def handle(self, raw, via, rssi=None, snr=None):
        """Handles one packet. Returns the packets to send back to that unit.

        via is "lora" or "wifi". Packets that arrived over Wi-Fi and are meant
        for other units are also sent out over LoRa, so units in range of the
        gateway get them at once.
        """
        try:
            h = P.parse_header(raw)
        except P.BadPacket as e:
            log.debug("dropped packet: %s", e)
            return []
        if h.group != self.group or h.sender == P.SERVER:
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
            limit = LORA_SYNC_LIMIT if via == "lora" else None
            return self.store.messages_between(f.sender, f.after, f.before, h.sender, limit)
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
        """Everything member is missing: messages, then positions and ACKs."""
        limit = LORA_SYNC_LIMIT if via == "lora" else None
        out = []
        for sender in P.MEMBERS:
            if sender == member:
                continue
            out += self.store.messages_after(sender, req.last_counter[sender], member, limit)
        out += self.store.latest_positions(member)
        for counter in self.store.newest_whole_messages(member):
            out.append(self._ack(member, counter, P.STORED, member))
        out += self.store.acks_for(member, max(0, req.last_counter.get(member, 0) - 50))
        if limit:
            out = out[:limit]
        log.info("sync for member %d via %s: %d packets", member, via, len(out))
        return out

    def status(self):
        now = int(time.time())
        units = self.store.units()
        for u in units:
            u["ago_s"] = now - u["last_seen"] if u["last_seen"] else None
        return {"units": units, "packets": self.store.counts()}

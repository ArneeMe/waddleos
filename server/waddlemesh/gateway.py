"""The USB serial link to the Heltec gateway (protocol/PROTOCOL.md, "Gateway serial link").

Text lines at 115200 baud: "RX <rssi> <snr> <hex>" in, "TX <hex>" out.
Every 10 s a "PING <network id> <member bits>". Without one for 30 s the gateway carries on alone
and keeps what it hears; its "PONG <kept> ..." says how many it kept. The
server then asks for them with DUMP and, once all came, sends CLEAR.
Uses only the standard library (termios), so no pyserial is needed.
"""

import logging
import os
import queue
import termios
import threading
import time

from . import packet as P

log = logging.getLogger("waddlemesh.gateway")

# Keep the server's own LoRa airtime under 6 % of each hour, like the units.
BUDGET_MS_PER_HOUR = 216000
PING_EVERY_S = 10
# The gateway goes standalone after 30 s without a line from the Pi. One that
# has said nothing for as long is shown as "not answering".
ANSWER_TIMEOUT_S = 30


class Gateway:
    def __init__(self, device, server):
        self.device = device
        self.server = server
        self.outbox = queue.Queue(maxsize=200)
        self.sent = []  # (time, airtime ms) in the last hour
        self.fd = None
        self.connected = False
        self.running = True
        self.version = None
        self.last_ping = 0
        self.last_heard = 0  # the gateway's last line
        self.kept = 0  # packets the gateway holds from being alone
        self.dump = None  # packets coming in after a DUMP, until DUMPED
        self.dump_at = 0
        self.handed_over = 0  # since the server started
        self.last_handover = None  # {"at": time, "packets": n}
        server.radio = self

    def send(self, raw):
        try:
            self.outbox.put_nowait(raw)
        except queue.Full:
            log.warning("LoRa outbox full, dropping a packet")

    def status(self):
        """What the status page shows about the gateway."""
        answering = self.connected and time.time() - self.last_heard < ANSWER_TIMEOUT_S
        return {"connected": self.connected, "answering": answering, "version": self.version, "kept": self.kept,
                "handed_over": self.handed_over, "last_handover": self.last_handover}

    def _open(self):
        fd = os.open(self.device, os.O_RDWR | os.O_NOCTTY)
        attrs = termios.tcgetattr(fd)
        attrs[0] = 0  # iflag
        attrs[1] = 0  # oflag
        attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        attrs[3] = 0  # lflag: raw
        attrs[4] = attrs[5] = termios.B115200
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 1  # reads return after 0.1 s
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        return fd

    def _write(self, line):
        os.write(self.fd, line.encode() + b"\n")

    def _airtime_ok(self, raw):
        now = time.time()
        self.sent = [(t, ms) for t, ms in self.sent if now - t < 3600]
        ms = P.airtime_ms(len(raw))
        if sum(m for _, m in self.sent) + ms > BUDGET_MS_PER_HOUR:
            return False
        self.sent.append((now, ms))
        return True

    def _write_pending(self):
        while not self.outbox.empty():
            raw = self.outbox.queue[0]
            if not self._airtime_ok(raw):
                return  # try again later; the queue keeps its order
            self.outbox.get_nowait()
            self._write("TX " + raw.hex())
            # The gateway sends one packet at a time; give it the airtime.
            time.sleep(P.airtime_ms(len(raw)) / 1000 + 0.05)

    def _ping(self):
        """Tells the gateway the Pi is here (and the network id, for when it is
        alone). A roster the admin just set goes out over LoRa once, too."""
        now = time.time()
        if now - self.last_ping >= PING_EVERY_S:
            self.last_ping = now
            roster = self.server.roster()
            members = sum(1 << (m.number - 1) for m in roster.members) if roster else 0
            self._write("PING %d %08x" % (self.server.network, members))
            for raw in self.server.store.take_unsent_roster():
                self.send(raw)

    def _handle_line(self, line):
        parts = line.split()
        if not parts:
            return
        self.last_heard = time.time()
        if parts[0] == "RX" and len(parts) == 4:
            try:
                rssi, snr, raw = int(parts[1]), float(parts[2]), bytes.fromhex(parts[3])
            except ValueError:
                return
            for answer in self.server.handle(raw, "lora", rssi, snr):
                self.send(answer)
        elif parts[0] == "PONG" and len(parts) >= 2 and parts[1].isdigit():
            self.kept = int(parts[1])
            self.version = " ".join(parts[2:]) or self.version
            # A DUMP that never finished is asked again after a minute.
            if self.kept and (self.dump is None or time.time() - self.dump_at > 60):
                log.info("gateway kept %d packets while alone: asking for them", self.kept)
                self.dump = []
                self.dump_at = time.time()
                self._write("DUMP")
        elif parts[0] == "STORED" and len(parts) == 2 and self.dump is not None:
            try:
                self.dump.append(bytes.fromhex(parts[1]))
            except ValueError:
                pass  # counted as missing below
        elif parts[0] == "DUMPED" and len(parts) == 2 and self.dump is not None:
            self._handover(self.dump, int(parts[1]) if parts[1].isdigit() else -1)
            self.dump = None
        elif parts[0] == "HELLO":
            self.version = " ".join(parts[2:])
            log.info("gateway: %s", line)
        elif parts[0] == "ERR":
            log.warning("gateway: %s", line)

    def _handover(self, packets, count):
        """Files what the gateway kept while alone, then lets it forget them. When
        some did not come through it keeps them, and the next PONG asks again."""
        if count != len(packets):
            log.warning("gateway handover: %d of %d packets came, asking again", len(packets), count)
            return
        for raw in packets:
            self.server.handle(raw, "lora", handed_over=True)
        self._write("CLEAR")
        self.kept = 0
        self.handed_over += count
        self.last_handover = {"at": int(time.time()), "packets": count}
        log.info("gateway handed over %d packets", count)

    def run(self):
        """Reads lines for ever, reopening the port when the gateway is unplugged."""
        buf = b""
        while self.running:
            try:
                if self.fd is None:
                    self.fd = self._open()
                    self.connected = True
                    self.last_ping = 0
                    self.dump = None
                    log.info("gateway connected on %s", self.device)
                self._ping()
                chunk = os.read(self.fd, 512)
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    self._handle_line(line.decode("ascii", "replace").strip())
                self._write_pending()
            except OSError as e:
                if self.connected:
                    log.warning("gateway lost: %s", e)
                self.connected = False
                if self.fd is not None:
                    os.close(self.fd)
                self.fd = None
                buf = b""
                time.sleep(5)

    def start(self):
        threading.Thread(target=self.run, name="gateway", daemon=True).start()

"""The USB serial link to the Heltec gateway (protocol/PROTOCOL.md, "Gateway serial link").

Text lines at 115200 baud: "RX <rssi> <snr> <hex>" in, "TX <hex>" out.
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


class Gateway:
    def __init__(self, device, server):
        self.device = device
        self.server = server
        self.outbox = queue.Queue(maxsize=200)
        self.sent = []  # (time, airtime ms) in the last hour
        self.fd = None
        self.connected = False
        self.version = None
        server.radio = self

    def send(self, raw):
        try:
            self.outbox.put_nowait(raw)
        except queue.Full:
            log.warning("LoRa outbox full, dropping a packet")

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
            os.write(self.fd, b"TX " + raw.hex().encode() + b"\n")
            # The gateway sends one packet at a time; give it the airtime.
            time.sleep(P.airtime_ms(len(raw)) / 1000 + 0.05)

    def _handle_line(self, line):
        parts = line.split()
        if not parts:
            return
        if parts[0] == "RX" and len(parts) == 4:
            try:
                rssi, snr, raw = int(parts[1]), float(parts[2]), bytes.fromhex(parts[3])
            except ValueError:
                return
            for answer in self.server.handle(raw, "lora", rssi, snr):
                self.send(answer)
        elif parts[0] == "HELLO":
            self.version = " ".join(parts[1:])
            log.info("gateway: %s", self.version)
        elif parts[0] == "ERR":
            log.warning("gateway: %s", line)

    def run(self):
        """Reads lines for ever, reopening the port when the gateway is unplugged."""
        buf = b""
        while True:
            try:
                if self.fd is None:
                    self.fd = self._open()
                    self.connected = True
                    log.info("gateway connected on %s", self.device)
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

"""`waddle mesh check`: what a unit's own pages (status, identity, diag) turn into."""

import importlib.machinery
import importlib.util
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
loader = importlib.machinery.SourceFileLoader("waddle_mesh_check", str(ROOT / "tools" / "waddle-mesh"))
spec = importlib.util.spec_from_loader("waddle_mesh_check", loader)
wm = importlib.util.module_from_spec(spec)
loader.exec_module(wm)

STATUS = {"board": "t5s3", "version": "WaddleOS 1.50"}
IDENT = {"radio": True, "gps": "MIA-M10Q", "battery": 80, "configured": True, "me": 2, "name": "Kari",
         "version": "WaddleOS 1.50"}
DIAG = {"uptimeS": 7200, "resetReason": "POWERON", "crashReport": False, "clockSet": True, "sd": True,
        "heapLargest": 90000, "airtimeMs": 21600, "outbox": 1, "pending": 0, "server": "online",
        "logs": "[MESH] Member 2 of network 66\n[MESH] Server sync: 3 packets"}


class Check(unittest.TestCase):
    def health(self, diag, logs=False):
        pages = {"/api/status": STATUS, "/api/mesh/identity": IDENT}
        if diag is not None:
            pages["/api/mesh/diag"] = diag

        def get(ip, path):
            if path not in pages:
                raise OSError("404")
            return pages[path]
        old = wm.unit_get
        wm.unit_get = get
        try:
            return wm.unit_health("10.0.0.2", logs)
        finally:
            wm.unit_get = old

    def test_a_well_unit(self):
        ok, text = self.health(DIAG)
        self.assertTrue(ok)
        self.assertIn("up 2 h", text)
        self.assertIn("airtime 10 %", text)
        self.assertNotIn("Server sync", text)
        self.assertIn("Server sync: 3 packets", self.health(DIAG, logs=True)[1])

    def test_problems_are_named(self):
        ok, text = self.health({**DIAG, "sd": False, "clockSet": False, "resetReason": "BROWNOUT"})
        self.assertFalse(ok)
        for words in ("no SD card", "clock is not set", "BROWNOUT"):
            self.assertIn(words, text)

    def test_older_firmware_without_the_page(self):
        ok, text = self.health(None)
        self.assertTrue(ok)
        self.assertIn("member 2 (Kari)", text)


if __name__ == "__main__":
    unittest.main()

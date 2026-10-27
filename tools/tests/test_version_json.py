"""scripts/version_json.py: one version.json entry per device."""

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "version_json.py"


def run(old, *entries):
    with tempfile.TemporaryDirectory() as d:
        d = Path(d)
        (d / "old.json").write_text(json.dumps(old)) if old is not None else None
        paths = []
        for i, e in enumerate(entries):
            paths.append(d / f"e{i}.json")
            paths[-1].write_text(json.dumps(e))
        subprocess.check_call([sys.executable, SCRIPT, d / "old.json", d / "new.json", *paths])
        return json.loads((d / "new.json").read_text())


class VersionJson(unittest.TestCase):
    def test_first_release(self):
        out = run(None, {"board": "t5s3", "build": "b", "versjon": "WaddleOS 2.1"})
        self.assertEqual(out, {"t5s3": {"build": "b", "versjon": "WaddleOS 2.1"}})

    def test_old_single_entry_becomes_the_reader(self):
        out = run({"build": "a", "versjon": "WaddleOS 1.40"}, {"board": "t5s3", "build": "b"})
        self.assertEqual(out["x4pro"]["build"], "a")
        self.assertEqual(out["build"], "a")
        self.assertEqual(out["t5s3"]["build"], "b")

    def test_device_that_did_not_build_keeps_its_entry(self):
        old = {"x4pro": {"build": "a"}, "t5s3": {"build": "b"}, "build": "a"}
        out = run(old, {"board": "x4pro", "build": "c"})
        self.assertEqual(out["t5s3"]["build"], "b")
        self.assertEqual(out["x4pro"]["build"], "c")
        self.assertEqual(out["build"], "c")


if __name__ == "__main__":
    unittest.main()

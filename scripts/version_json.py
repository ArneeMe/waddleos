#!/usr/bin/env python3
"""Builds version.json for a release: one entry per device.

  version_json.py OLD.json NEW.json ENTRY.json...

OLD.json is the release's current version.json (may be missing). Each
ENTRY.json is {"board": "t5s3", "versjon": ..., "build": ..., "dato": ...,
"melding": ...}, written by the build job of a device that built. Devices that
did not build keep their old entry. The reader's entry is also copied to the
top level, which is where readers from before this file had entries look.
"""

import json
import sys
from pathlib import Path

FIELDS = ("versjon", "build", "dato", "melding")


def main():
    old_path, new_path, *entries = sys.argv[1:]
    try:
        old = json.loads(Path(old_path).read_text())
    except (OSError, ValueError):
        old = {}
    out = {board: old[board] for board in ("x4pro", "t5s3", "gateway") if isinstance(old.get(board), dict)}
    if "x4pro" not in out and "build" in old:
        out["x4pro"] = {k: old[k] for k in FIELDS if k in old}
    for path in entries:
        entry = json.loads(Path(path).read_text())
        out[entry.pop("board")] = entry
    if "x4pro" in out:
        out.update(out["x4pro"])
    Path(new_path).write_text(json.dumps(out, ensure_ascii=False, indent=1))


if __name__ == "__main__":
    main()

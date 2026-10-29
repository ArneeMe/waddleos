"""SQLite store: every packet as received, plus what the server knows about each unit."""

import logging
import sqlite3
import threading
import time

log = logging.getLogger("waddlemesh")

from . import packet as P

SCHEMA = """
CREATE TABLE IF NOT EXISTS packets (
    id INTEGER PRIMARY KEY,
    type INTEGER NOT NULL,
    sender INTEGER NOT NULL,
    recipient INTEGER NOT NULL,
    counter INTEGER NOT NULL,
    piece_index INTEGER NOT NULL,
    piece_count INTEGER NOT NULL,
    raw BLOB NOT NULL,
    received_at INTEGER NOT NULL,
    via TEXT NOT NULL,
    UNIQUE (type, sender, counter, piece_index)
);
CREATE INDEX IF NOT EXISTS packets_by_sender ON packets (type, sender, counter);
CREATE TABLE IF NOT EXISTS acks (
    acked_sender INTEGER NOT NULL,
    acked_counter INTEGER NOT NULL,
    by_member INTEGER NOT NULL,
    state INTEGER NOT NULL,
    raw BLOB NOT NULL,
    PRIMARY KEY (acked_sender, acked_counter, by_member)
);
CREATE TABLE IF NOT EXISTS meta (
    key TEXT PRIMARY KEY,
    value INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS units (
    member INTEGER PRIMARY KEY,
    last_seen INTEGER,
    via TEXT,
    battery INTEGER,
    build TEXT,
    rssi INTEGER,
    snr REAL,
    flags INTEGER,
    sos_at INTEGER
);
CREATE TABLE IF NOT EXISTS roster (
    version INTEGER PRIMARY KEY,
    signed BLOB NOT NULL,
    packets BLOB NOT NULL,
    sent INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS joins (
    invite TEXT PRIMARY KEY,
    from_key TEXT NOT NULL,
    box TEXT NOT NULL,
    at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS welcomes (
    invite TEXT PRIMARY KEY,
    box TEXT NOT NULL,
    at INTEGER NOT NULL
);
"""
# Join requests and welcomes are forgotten after a week.
JOIN_KEPT_S = 7 * 86400
MAX_JOINS = 20
# Columns added after the first release, for databases made before them.
NEW_COLUMNS = {"units": ("flags INTEGER", "sos_at INTEGER")}


class Store:
    def __init__(self, path):
        # The CLI (set-roster, welcome) writes while the server runs: wait for each other.
        self.db = sqlite3.connect(path, check_same_thread=False, timeout=10)
        self.lock = threading.Lock()
        with self.lock:
            self.db.executescript(SCHEMA)
            for table, columns in NEW_COLUMNS.items():
                have = {r[1] for r in self.db.execute("PRAGMA table_info(%s)" % table)}
                for column in columns:
                    if column.split()[0] not in have:
                        self.db.execute("ALTER TABLE %s ADD COLUMN %s" % (table, column))
            self.db.commit()

    def add_packet(self, h, raw, via, now=None):
        """Stores a packet. Returns False when it was already stored.

        Anyone near the gateway can send a packet with a member's number and
        counter, and the server cannot tell (it has no keys). Over Wi-Fi the
        token proves the sender, so a Wi-Fi copy replaces a different one heard
        over LoRa."""
        with self.lock:
            if via == "wifi":
                row = self.db.execute("SELECT raw, via FROM packets WHERE type = ? AND sender = ? AND counter = ?"
                                      " AND piece_index = ?", (h.type, h.sender, h.counter, h.piece_index)).fetchone()
                if row and bytes(row[0]) != raw:
                    self.db.execute("UPDATE packets SET raw = ?, recipient = ?, piece_count = ?, via = ?"
                                    " WHERE type = ? AND sender = ? AND counter = ? AND piece_index = ?",
                                    (raw, h.recipient, h.piece_count, via, h.type, h.sender, h.counter,
                                     h.piece_index))
                    self.db.commit()
                    log.warning("member %d's packet %d replaced a different copy heard over LoRa", h.sender,
                                h.counter)
                    return True
            cur = self.db.execute(
                "INSERT OR IGNORE INTO packets (type, sender, recipient, counter, piece_index, piece_count, raw,"
                " received_at, via) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                (h.type, h.sender, h.recipient, h.counter, h.piece_index, h.piece_count, raw,
                 int(now or time.time()), via))
            self.db.commit()
            return cur.rowcount == 1

    def has_all_pieces(self, h):
        with self.lock:
            (n,) = self.db.execute("SELECT COUNT(*) FROM packets WHERE type = ? AND sender = ? AND counter = ?",
                                   (h.type, h.sender, h.counter)).fetchone()
        return n == h.piece_count

    def messages_after(self, sender, counter, for_member, limit=None):
        """Stored MSG pieces from sender with a higher counter, oldest first.

        DMs between two others are left out. Group messages go to everyone: only
        their members have the key.
        """
        sql = ("SELECT raw FROM packets WHERE type = 1 AND sender = ? AND counter > ?"
               " AND (recipient = 255 OR recipient = ? OR sender = ? OR recipient & 128)"
               " ORDER BY counter, piece_index")
        args = [sender, counter, for_member, for_member]
        if limit:
            sql += " LIMIT ?"
            args.append(limit)
        with self.lock:
            return [bytes(r[0]) for r in self.db.execute(sql, args)]

    def messages_between(self, sender, after, before, for_member, limit=None):
        """Stored MSG pieces from sender with after < counter < before (a fetch)."""
        return [raw for raw in self.messages_after(sender, after, for_member)
                if int.from_bytes(raw[6:10], "little") < before][:limit]

    def latest_positions(self, except_member):
        """The newest POS (and any newer SOS) of every other member."""
        out = []
        with self.lock:
            for ptype in (4, 5):
                rows = self.db.execute(
                    "SELECT raw FROM packets p WHERE type = ? AND sender != ? AND counter ="
                    " (SELECT MAX(counter) FROM packets q WHERE q.type = p.type AND q.sender = p.sender)",
                    (ptype, except_member)).fetchall()
                out += [bytes(r[0]) for r in rows]
        return out

    def add_ack(self, ack, by_member, raw):
        with self.lock:
            self.db.execute(
                "INSERT INTO acks (acked_sender, acked_counter, by_member, state, raw) VALUES (?, ?, ?, ?, ?)"
                " ON CONFLICT (acked_sender, acked_counter, by_member) DO UPDATE SET state = MAX(state, excluded.state),"
                " raw = CASE WHEN excluded.state > state THEN excluded.raw ELSE raw END",
                (ack.sender, ack.counter, by_member, ack.state, raw))
            self.db.commit()

    def acks_for(self, member, from_counter):
        """ACKs from the other members for member's messages from from_counter on."""
        with self.lock:
            return [bytes(r[0]) for r in self.db.execute(
                "SELECT raw FROM acks WHERE acked_sender = ? AND acked_counter >= ? ORDER BY acked_counter",
                (member, from_counter))]

    def last_counter(self, sender):
        with self.lock:
            (n,) = self.db.execute("SELECT COALESCE(MAX(counter), 0) FROM packets WHERE sender = ?",
                                   (sender,)).fetchone()
        return n

    def next_counter(self):
        """The server's own packet counter (sender 0), never reused."""
        with self.lock:
            self.db.execute("INSERT INTO meta (key, value) VALUES ('counter', 1)"
                            " ON CONFLICT (key) DO UPDATE SET value = value + 1")
            (n,) = self.db.execute("SELECT value FROM meta WHERE key = 'counter'").fetchone()
            self.db.commit()
        return n

    def newest_whole_messages(self, sender, limit=10):
        """Counters of sender's newest MSGs the server has every piece of."""
        with self.lock:
            rows = self.db.execute(
                "SELECT counter FROM packets WHERE type = 1 AND sender = ? GROUP BY counter"
                " HAVING COUNT(*) = MAX(piece_count) ORDER BY counter DESC LIMIT ?", (sender, limit)).fetchall()
        return [r[0] for r in reversed(rows)]

    def seen_unit(self, member, via, battery=None, build=None, rssi=None, snr=None, flags=None, now=None):
        with self.lock:
            self.db.execute(
                "INSERT INTO units (member, last_seen, via, battery, build, rssi, snr, flags)"
                " VALUES (?, ?, ?, ?, ?, ?, ?, ?)"
                " ON CONFLICT (member) DO UPDATE SET last_seen = excluded.last_seen, via = excluded.via,"
                " battery = COALESCE(excluded.battery, battery), build = COALESCE(excluded.build, build),"
                " rssi = COALESCE(excluded.rssi, rssi), snr = COALESCE(excluded.snr, snr),"
                " flags = COALESCE(excluded.flags, flags)",
                (member, int(now or time.time()), via, battery, build, rssi, snr, flags))
            self.db.commit()

    def sos_heard(self, member, now=None):
        """An SOS packet from member: the status page shows it for a while."""
        with self.lock:
            self.db.execute("UPDATE units SET sos_at = ? WHERE member = ?", (int(now or time.time()), member))
            self.db.commit()

    def units(self):
        with self.lock:
            rows = self.db.execute("SELECT member, last_seen, via, battery, build, rssi, snr, flags, sos_at FROM units"
                                   " ORDER BY member").fetchall()
        keys = ("member", "last_seen", "via", "battery", "build", "rssi", "snr", "flags", "sos_at")
        return [dict(zip(keys, r)) for r in rows]

    def counts(self):
        """Stored packets by type (ACKs too, when there are any)."""
        with self.lock:
            counts = dict(self.db.execute("SELECT type, COUNT(*) FROM packets GROUP BY type").fetchall())
            (acks,) = self.db.execute("SELECT COUNT(*) FROM acks").fetchone()
        if acks:
            counts[2] = acks
        return counts

    # --- Roster ---------------------------------------------------------------

    def set_roster(self, version, signed, packets, replace=False):
        """Keeps the newest roster and its ROSTER packets. False when not newer
        (unless replace: the stored one belongs to an earlier network)."""
        with self.lock:
            (have,) = self.db.execute("SELECT COALESCE(MAX(version), 0) FROM roster").fetchone()
            if version <= have and not replace:
                return False
            self.db.execute("DELETE FROM roster")
            self.db.execute("INSERT INTO roster (version, signed, packets) VALUES (?, ?, ?)",
                            (version, signed, P.frame(packets)))
            self.db.commit()
        return True

    def roster(self):
        """(version, signed bytes, ROSTER packets) of the newest roster, or None."""
        with self.lock:
            row = self.db.execute("SELECT version, signed, packets FROM roster").fetchone()
        return None if row is None else (row[0], bytes(row[1]), P.unframe(bytes(row[2])))

    def roster_version(self):
        with self.lock:
            (v,) = self.db.execute("SELECT COALESCE(MAX(version), 0) FROM roster").fetchone()
        return v

    def take_unsent_roster(self):
        """The ROSTER packets of a roster not yet sent out over LoRa (then marked sent), or []."""
        with self.lock:
            row = self.db.execute("SELECT version, packets FROM roster WHERE sent = 0").fetchone()
            if row is None:
                return []
            self.db.execute("UPDATE roster SET sent = 1 WHERE version = ?", (row[0],))
            self.db.commit()
        return P.unframe(bytes(row[1]))

    # --- Joining ----------------------------------------------------------------

    def _forget_old_joins(self, now):
        for table in ("joins", "welcomes"):
            self.db.execute("DELETE FROM %s WHERE at < ?" % table, (now - JOIN_KEPT_S,))

    def add_join(self, invite, from_key, box, now=None):
        """A newcomer's request. Returns "ok", "taken" (that invitation already
        has one) or "full"."""
        now = int(now or time.time())
        with self.lock:
            self._forget_old_joins(now)
            taken = self.db.execute("SELECT 1 FROM joins WHERE invite = ? UNION SELECT 1 FROM welcomes"
                                    " WHERE invite = ?", (invite, invite)).fetchone()
            (waiting,) = self.db.execute("SELECT COUNT(*) FROM joins").fetchone()
            if taken:
                result = "taken"
            elif waiting >= MAX_JOINS:
                result = "full"
            else:
                self.db.execute("INSERT INTO joins (invite, from_key, box, at) VALUES (?, ?, ?, ?)",
                                (invite, from_key, box, now))
                result = "ok"
            self.db.commit()
        return result

    def joins(self, now=None):
        with self.lock:
            self._forget_old_joins(int(now or time.time()))
            self.db.commit()
            rows = self.db.execute("SELECT invite, from_key, box, at FROM joins ORDER BY at").fetchall()
        return [{"invite": r[0], "from": r[1], "box": r[2], "at": r[3]} for r in rows]

    def forget_join(self, invite):
        with self.lock:
            self.db.execute("DELETE FROM joins WHERE invite = ?", (invite,))
            self.db.commit()

    def add_welcome(self, invite, box, now=None):
        """The admin let them in: the request is done, the welcome waits to be fetched."""
        with self.lock:
            self.db.execute("DELETE FROM joins WHERE invite = ?", (invite,))
            self.db.execute("INSERT OR REPLACE INTO welcomes (invite, box, at) VALUES (?, ?, ?)",
                            (invite, box, int(now or time.time())))
            self.db.commit()

    def take_welcome(self, invite, now=None):
        """The sealed welcome for an invitation, once (then forgotten), or None."""
        with self.lock:
            self._forget_old_joins(int(now or time.time()))
            row = self.db.execute("SELECT box FROM welcomes WHERE invite = ?", (invite,)).fetchone()
            self.db.execute("DELETE FROM welcomes WHERE invite = ?", (invite,))
            self.db.commit()
        return row[0] if row else None

    def close(self):
        self.db.close()

    def backup(self, path):
        """A consistent copy of the database, safe while the server runs."""
        dest = sqlite3.connect(path)
        with self.lock:
            self.db.backup(dest)
        dest.close()

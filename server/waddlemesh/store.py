"""SQLite store: every packet as received, plus what the server knows about each unit."""

import sqlite3
import threading
import time

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
    snr REAL
);
"""


class Store:
    def __init__(self, path):
        self.db = sqlite3.connect(path, check_same_thread=False)
        self.lock = threading.Lock()
        with self.lock:
            self.db.executescript(SCHEMA)
            self.db.commit()

    def add_packet(self, h, raw, via, now=None):
        """Stores a packet. Returns False when it was already stored."""
        with self.lock:
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

        DMs between two other members are left out: for_member could not read them.
        """
        sql = ("SELECT raw FROM packets WHERE type = 1 AND sender = ? AND counter > ?"
               " AND (recipient = 255 OR recipient = ? OR sender = ?) ORDER BY counter, piece_index")
        args = [sender, counter, for_member, for_member]
        if limit:
            sql += " LIMIT ?"
            args.append(limit)
        with self.lock:
            return [bytes(r[0]) for r in self.db.execute(sql, args)]

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

    def seen_unit(self, member, via, battery=None, build=None, rssi=None, snr=None, now=None):
        with self.lock:
            self.db.execute(
                "INSERT INTO units (member, last_seen, via, battery, build, rssi, snr) VALUES (?, ?, ?, ?, ?, ?, ?)"
                " ON CONFLICT (member) DO UPDATE SET last_seen = excluded.last_seen, via = excluded.via,"
                " battery = COALESCE(excluded.battery, battery), build = COALESCE(excluded.build, build),"
                " rssi = COALESCE(excluded.rssi, rssi), snr = COALESCE(excluded.snr, snr)",
                (member, int(now or time.time()), via, battery, build, rssi, snr))
            self.db.commit()

    def units(self):
        with self.lock:
            rows = self.db.execute("SELECT member, last_seen, via, battery, build, rssi, snr FROM units"
                                   " ORDER BY member").fetchall()
        keys = ("member", "last_seen", "via", "battery", "build", "rssi", "snr")
        return [dict(zip(keys, r)) for r in rows]

    def counts(self):
        with self.lock:
            return dict(self.db.execute("SELECT type, COUNT(*) FROM packets GROUP BY type").fetchall())

    def backup(self, path):
        """A consistent copy of the database, safe while the server runs."""
        dest = sqlite3.connect(path)
        with self.lock:
            self.db.backup(dest)
        dest.close()

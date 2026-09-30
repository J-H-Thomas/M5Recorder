"""SQLite record of every memo: makes uploads idempotent and lets unfinished
transcriptions resume or retry."""

import sqlite3
import threading
from dataclasses import dataclass
from pathlib import Path

PENDING, DONE, FAILED, GAVE_UP = "pending", "done", "failed", "gave_up"

# Columns added after the first release; created on startup if missing.
_ADDED_COLUMNS = {
    "size": "INTEGER",
    "sha256": "TEXT",
    "time_source": "TEXT",
    "attempts": "INTEGER NOT NULL DEFAULT 0",
    "next_attempt": "REAL",
}


@dataclass(frozen=True)
class Memo:
    memo_id: str
    device: str
    created: float      # Unix time the memo was recorded (or received)
    audio_path: str     # relative to the vault
    duration: float
    status: str
    note_path: str | None
    size: int | None = None
    sha256: str | None = None
    time_source: str | None = None  # device / corrected / received
    attempts: int = 0


_COLUMNS = "memo_id, device, created, audio_path, duration, status, note_path, size, sha256, time_source, attempts"


class Store:
    def __init__(self, path: Path):
        path.parent.mkdir(parents=True, exist_ok=True)
        self._db = sqlite3.connect(path, check_same_thread=False, isolation_level=None)
        self._lock = threading.Lock()
        self._db.execute("PRAGMA journal_mode=WAL")
        self._db.execute(
            """CREATE TABLE IF NOT EXISTS memos (
                 memo_id    TEXT PRIMARY KEY,
                 device     TEXT NOT NULL,
                 created    REAL NOT NULL,
                 audio_path TEXT NOT NULL,
                 duration   REAL NOT NULL,
                 status     TEXT NOT NULL,
                 note_path  TEXT,
                 error      TEXT
               )"""
        )
        have = {row[1] for row in self._db.execute("PRAGMA table_info(memos)")}
        for name, kind in _ADDED_COLUMNS.items():
            if name not in have:
                self._db.execute(f"ALTER TABLE memos ADD COLUMN {name} {kind}")

    def get(self, memo_id: str) -> Memo | None:
        with self._lock:
            row = self._db.execute(f"SELECT {_COLUMNS} FROM memos WHERE memo_id = ?", (memo_id,)).fetchone()
        return Memo(*row) if row else None

    def add(self, memo: Memo) -> bool:
        """Insert a new memo; False if the id already exists."""
        with self._lock:
            try:
                self._db.execute(
                    "INSERT INTO memos (memo_id, device, created, audio_path, duration, status,"
                    " note_path, size, sha256, time_source) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                    (memo.memo_id, memo.device, memo.created, memo.audio_path, memo.duration,
                     PENDING, memo.note_path, memo.size, memo.sha256, memo.time_source),
                )
            except sqlite3.IntegrityError:
                return False
        return True

    def unfinished(self) -> list[str]:
        """Pending or failed (still to be retried), oldest first."""
        with self._lock:
            rows = self._db.execute(
                "SELECT memo_id FROM memos WHERE status IN (?, ?) ORDER BY created", (PENDING, FAILED)
            ).fetchall()
        return [r[0] for r in rows]

    def due_retries(self, now: float) -> list[str]:
        with self._lock:
            rows = self._db.execute(
                "SELECT memo_id FROM memos WHERE status = ? AND (next_attempt IS NULL OR next_attempt <= ?)"
                " ORDER BY created", (FAILED, now)
            ).fetchall()
        return [r[0] for r in rows]

    def set_note_path(self, memo_id: str, note_path: str) -> None:
        with self._lock:
            self._db.execute("UPDATE memos SET note_path = ? WHERE memo_id = ?", (note_path, memo_id))

    def mark_done(self, memo_id: str, note_path: str) -> None:
        with self._lock:
            self._db.execute(
                "UPDATE memos SET status = ?, note_path = ?, error = NULL, next_attempt = NULL WHERE memo_id = ?",
                (DONE, note_path, memo_id),
            )

    def mark_failed(self, memo_id: str, error: str, next_attempt: float) -> int:
        """Records a failed attempt; returns the number of attempts so far."""
        with self._lock:
            self._db.execute(
                "UPDATE memos SET status = ?, error = ?, attempts = attempts + 1, next_attempt = ?"
                " WHERE memo_id = ?", (FAILED, error, next_attempt, memo_id)
            )
            return self._db.execute("SELECT attempts FROM memos WHERE memo_id = ?", (memo_id,)).fetchone()[0]

    def mark_gave_up(self, memo_id: str) -> None:
        with self._lock:
            self._db.execute(
                "UPDATE memos SET status = ?, next_attempt = NULL WHERE memo_id = ?", (GAVE_UP, memo_id)
            )

    def counts(self) -> dict[str, int]:
        with self._lock:
            rows = self._db.execute("SELECT status, COUNT(*) FROM memos GROUP BY status").fetchall()
        return {status: n for status, n in rows}

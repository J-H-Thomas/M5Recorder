"""SQLite record of every memo: makes uploads idempotent and lets unfinished
transcriptions resume after a restart."""

import sqlite3
import threading
from dataclasses import dataclass
from pathlib import Path

PENDING, DONE, FAILED = "pending", "done", "failed"


@dataclass(frozen=True)
class Memo:
    memo_id: str
    device: str
    created: float      # Unix time the memo was recorded (or received)
    audio_path: str     # relative to the vault
    duration: float
    status: str
    note_path: str | None


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

    def get(self, memo_id: str) -> Memo | None:
        with self._lock:
            row = self._db.execute(
                "SELECT memo_id, device, created, audio_path, duration, status, note_path"
                " FROM memos WHERE memo_id = ?",
                (memo_id,),
            ).fetchone()
        return Memo(*row) if row else None

    def add(self, memo: Memo) -> bool:
        """Insert a new memo; False if the id already exists."""
        with self._lock:
            try:
                self._db.execute(
                    "INSERT INTO memos (memo_id, device, created, audio_path, duration, status)"
                    " VALUES (?, ?, ?, ?, ?, ?)",
                    (memo.memo_id, memo.device, memo.created, memo.audio_path, memo.duration, PENDING),
                )
            except sqlite3.IntegrityError:
                return False
        return True

    def unfinished(self) -> list[str]:
        with self._lock:
            rows = self._db.execute(
                "SELECT memo_id FROM memos WHERE status != ? ORDER BY created", (DONE,)
            ).fetchall()
        return [r[0] for r in rows]

    def mark_done(self, memo_id: str, note_path: str) -> None:
        with self._lock:
            self._db.execute(
                "UPDATE memos SET status = ?, note_path = ?, error = NULL WHERE memo_id = ?",
                (DONE, note_path, memo_id),
            )

    def mark_failed(self, memo_id: str, error: str) -> None:
        with self._lock:
            self._db.execute(
                "UPDATE memos SET status = ?, error = ? WHERE memo_id = ?", (FAILED, error, memo_id)
            )

    def count_unfinished(self) -> int:
        with self._lock:
            return self._db.execute(
                "SELECT COUNT(*) FROM memos WHERE status != ?", (DONE,)
            ).fetchone()[0]

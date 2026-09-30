"""Background transcription: one memo at a time, in upload order. Failures are
retried with backoff; after MAX_ATTEMPTS the note says so, with the audio."""

import logging
import queue
import threading
import time
from datetime import datetime
from pathlib import Path
from typing import Protocol
from zoneinfo import ZoneInfo

from . import notes
from .config import Settings
from .store import DONE, GAVE_UP, Memo, Store

log = logging.getLogger("memos.worker")

MAX_ATTEMPTS = 5
RETRY_BASE_S = 60        # 1, 4, 16, 64 min between attempts
POLL_S = 30              # how often to look for retries that are due


class Transcriber(Protocol):
    def __call__(self, audio: Path) -> str: ...


class WhisperTranscriber:
    """faster-whisper, loaded on first use so the API starts straight away."""

    def __init__(self, settings: Settings):
        self._settings = settings
        self._model = None

    def __call__(self, audio: Path) -> str:
        if self._model is None:
            from faster_whisper import WhisperModel

            s = self._settings
            log.info("loading whisper model %s (%s, %s)", s.whisper_model, s.whisper_device, s.whisper_compute)
            self._model = WhisperModel(
                s.whisper_model,
                device=s.whisper_device,
                compute_type=s.whisper_compute,
                download_root=str(s.whisper_model_dir) if s.whisper_model_dir else None,
            )
        segments, _ = self._model.transcribe(
            load_audio(audio), language=self._settings.language, vad_filter=True, beam_size=5
        )
        return " ".join(seg.text.strip() for seg in segments)


def load_audio(path: Path):
    """The stick sends 16 kHz mono 16-bit WAV, which is what Whisper wants:
    read it directly rather than through PyAV (whose API changes have broken
    faster-whisper's decoder). Anything else goes to faster-whisper's decoder."""
    import wave

    import numpy as np

    with wave.open(str(path), "rb") as w:
        if (w.getframerate(), w.getnchannels(), w.getsampwidth()) != (16000, 1, 2):
            return str(path)
        frames = w.readframes(w.getnframes())
    return np.frombuffer(frames, dtype="<i2").astype(np.float32) / 32768.0


class Worker:
    def __init__(self, settings: Settings, store: Store, transcribe: Transcriber,
                 on_change=None):
        self._settings = settings
        self._store = store
        self._transcribe = transcribe
        self._on_change = on_change  # called with the device after a memo is done or given up
        self._queue: queue.Queue[str] = queue.Queue()
        self._thread: threading.Thread | None = None

    # --- notes ---------------------------------------------------------------

    def new_note_path(self, audio_path: str) -> str:
        """Picks the note path for a new memo (same stem as its audio)."""
        s = self._settings
        s.notes_dir.mkdir(parents=True, exist_ok=True)
        note = notes.unique_path(s.notes_dir, Path(audio_path).stem, ".md")
        return note.relative_to(s.vault_dir).as_posix()

    def write_note(self, memo: Memo, note_path: str, body: str, status: str) -> None:
        s = self._settings
        created = datetime.fromtimestamp(memo.created, ZoneInfo(s.timezone))
        notes.atomic_write_bytes(s.vault_dir / note_path, notes.render_note(
            created=created, duration=memo.duration, device=memo.device, memo_id=memo.memo_id,
            audio_rel=memo.audio_path, body=body, status=status, time_source=memo.time_source,
        ).encode("utf-8"))

    # --- queue -----------------------------------------------------------------

    def enqueue(self, memo_id: str) -> None:
        self._queue.put(memo_id)

    def requeue_unfinished(self) -> int:
        ids = self._store.unfinished()
        for memo_id in ids:
            self.enqueue(memo_id)
        return len(ids)

    def start(self) -> None:
        self._thread = threading.Thread(target=self._run, name="transcriber", daemon=True)
        self._thread.start()

    def alive(self) -> bool:
        return self._thread is not None and self._thread.is_alive()

    def _run(self) -> None:
        while True:
            try:
                memo_id = self._queue.get(timeout=POLL_S)
            except queue.Empty:
                memo_id = None
            try:  # nothing may kill this thread
                if memo_id:
                    self.process(memo_id)
                else:
                    for due in self._store.due_retries(time.time()):
                        self.process(due)
            except Exception:
                log.exception("worker loop error")

    def run_pending(self) -> None:
        """Process everything queued, on the calling thread (tests)."""
        while not self._queue.empty():
            self.process(self._queue.get())

    def process(self, memo_id: str) -> None:
        memo = self._store.get(memo_id)
        if memo is None or memo.status in (DONE, GAVE_UP):
            return
        note_path = memo.note_path
        if note_path is None:  # uploaded before placeholder notes existed
            note_path = self.new_note_path(memo.audio_path)
            self._store.set_note_path(memo_id, note_path)
        try:
            text = self._transcribe(self._settings.vault_dir / memo.audio_path)
            self.write_note(memo, note_path, text.strip() or notes.NO_SPEECH, "done")
            self._store.mark_done(memo_id, note_path)
            log.info("memo %s -> %s", memo_id, Path(note_path).name)
        except Exception as e:
            attempts = self._store.mark_failed(
                memo_id, repr(e), time.time() + RETRY_BASE_S * 4 ** memo.attempts)
            log.exception("transcription failed for %s (attempt %d of %d)", memo_id, attempts, MAX_ATTEMPTS)
            if attempts >= MAX_ATTEMPTS:
                self.write_note(memo, note_path, notes.FAILED, "failed")
                self._store.mark_gave_up(memo_id)
                log.error("gave up on %s; note written without a transcript", memo_id)
        if self._on_change:
            self._on_change(memo.device)

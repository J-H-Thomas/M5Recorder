"""Background transcription: one memo at a time, in upload order."""

import logging
import queue
import threading
from datetime import datetime
from pathlib import Path
from typing import Protocol
from zoneinfo import ZoneInfo

from .config import Settings
from .notes import atomic_write_bytes, render_note, unique_path
from .store import Store

log = logging.getLogger("memos.worker")


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
    def __init__(self, settings: Settings, store: Store, transcribe: Transcriber):
        self._settings = settings
        self._store = store
        self._transcribe = transcribe
        self._queue: queue.Queue[str] = queue.Queue()
        self._thread: threading.Thread | None = None

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

    def _run(self) -> None:
        while True:
            self.process(self._queue.get())

    def run_pending(self) -> None:
        """Process everything queued, on the calling thread (tests)."""
        while not self._queue.empty():
            self.process(self._queue.get())

    def process(self, memo_id: str) -> None:
        memo = self._store.get(memo_id)
        if memo is None or memo.status == "done":
            return
        s = self._settings
        try:
            text = self._transcribe(s.vault_dir / memo.audio_path)
            created = datetime.fromtimestamp(memo.created, ZoneInfo(s.timezone))
            s.notes_dir.mkdir(parents=True, exist_ok=True)
            note = unique_path(s.notes_dir, Path(memo.audio_path).stem, ".md")
            atomic_write_bytes(note, render_note(
                created=created, duration=memo.duration, device=memo.device,
                memo_id=memo.memo_id, audio_rel=memo.audio_path, transcript=text,
            ).encode("utf-8"))
            self._store.mark_done(memo_id, note.relative_to(s.vault_dir).as_posix())
            log.info("memo %s -> %s", memo_id, note.name)
        except Exception as e:  # keep the worker alive; retried on next start
            log.exception("transcription failed for %s", memo_id)
            self._store.mark_failed(memo_id, repr(e))

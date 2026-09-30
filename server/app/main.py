"""Memo receiver: the M5StickS3 uploads WAV memos here; each becomes a
transcribed Markdown note in the Obsidian vault.

Run: uvicorn --factory app.main:build --host 0.0.0.0 --port 8080
"""

import hmac
import logging
import os
import re
import time
import wave
from contextlib import asynccontextmanager
from datetime import datetime
from zoneinfo import ZoneInfo

from fastapi import FastAPI, Header, HTTPException, Request

from .config import Settings
from .notes import unique_path
from .store import Memo, Store
from .worker import Transcriber, WhisperTranscriber, Worker

log = logging.getLogger("memos")

MEMO_ID_RE = re.compile(r"^[A-Za-z0-9_-]{1,64}$")
EARLIEST_VALID_TIME = 1_700_000_000  # Nov 2023: anything earlier means the stick's clock was never set


def create_app(settings: Settings, transcribe: Transcriber | None = None,
               start_worker: bool = True) -> FastAPI:
    store = Store(settings.data_dir / "memos.db")
    worker = Worker(settings, store, transcribe or WhisperTranscriber(settings))

    @asynccontextmanager
    async def lifespan(_: FastAPI):
        settings.audio_dir.mkdir(parents=True, exist_ok=True)
        settings.notes_dir.mkdir(parents=True, exist_ok=True)
        log.info("vault %s, notes in %s, time zone %s, model %s on %s",
                 settings.vault_dir, settings.notes_subdir, settings.timezone,
                 settings.whisper_model, settings.whisper_device)
        if start_worker:
            n = worker.requeue_unfinished()
            if n:
                log.info("re-queued %d unfinished memo(s)", n)
            worker.start()
        yield

    app = FastAPI(title="M5Recorder receiver", lifespan=lifespan, docs_url=None, redoc_url=None)
    app.state.store = store
    app.state.worker = worker

    @app.get("/health")
    def health():
        return {"ok": True, "unfinished": store.count_unfinished()}

    @app.post("/upload")
    async def upload(
        request: Request,
        authorization: str | None = Header(None),
        x_memo_id: str | None = Header(None),
        x_memo_time: int = Header(0),
        x_memo_device: str | None = Header(None),
    ):
        expected = f"Bearer {settings.token}"
        if not authorization or not hmac.compare_digest(authorization.encode(), expected.encode()):
            raise HTTPException(401, "bad token")
        if not x_memo_id or not MEMO_ID_RE.match(x_memo_id):
            raise HTTPException(400, "X-Memo-Id missing or invalid")
        if store.get(x_memo_id):
            return {"status": "duplicate"}  # the stick re-sent it; it can delete its copy

        length = request.headers.get("content-length")
        if length and int(length) > settings.max_upload_bytes:
            raise HTTPException(413, "too large")

        settings.audio_dir.mkdir(parents=True, exist_ok=True)
        tmp = settings.audio_dir / f".upload-{x_memo_id}.part"
        try:
            size = 0
            with tmp.open("wb") as f:
                async for chunk in request.stream():
                    size += len(chunk)
                    if size > settings.max_upload_bytes:
                        raise HTTPException(413, "too large")
                    f.write(chunk)
            try:
                with wave.open(str(tmp), "rb") as w:
                    duration = w.getnframes() / w.getframerate()
            except (wave.Error, EOFError, ZeroDivisionError) as e:
                raise HTTPException(400, f"not a WAV file: {e}")

            now = time.time()
            created = x_memo_time if EARLIEST_VALID_TIME < x_memo_time < now + 86400 else now
            stem = datetime.fromtimestamp(created, ZoneInfo(settings.timezone)).strftime("%Y-%m-%d %H%M%S")
            final = unique_path(settings.audio_dir, stem, ".wav")
            os.replace(tmp, final)
        finally:
            tmp.unlink(missing_ok=True)

        memo = Memo(
            memo_id=x_memo_id,
            device=x_memo_device or x_memo_id.rsplit("-", 1)[0],
            created=created,
            audio_path=final.relative_to(settings.vault_dir).as_posix(),
            duration=duration,
            status="pending",
            note_path=None,
        )
        if not store.add(memo):  # lost a race with a parallel re-send
            final.unlink(missing_ok=True)
            return {"status": "duplicate"}
        worker.enqueue(x_memo_id)
        log.info("received %s (%.1f s) -> %s", x_memo_id, duration, final.name)
        return {"status": "queued"}

    return app


def build() -> FastAPI:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    return create_app(Settings.from_env())

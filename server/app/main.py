"""Memo receiver: the M5StickS3 uploads WAV memos here; each becomes a
transcribed Markdown note in the Obsidian vault.

Run: uvicorn --factory app.main:build --host 0.0.0.0 --port 8080
"""

import hashlib
import hmac
import logging
import os
import re
import time
import uuid
import wave
from contextlib import asynccontextmanager
from datetime import datetime
from zoneinfo import ZoneInfo

from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import JSONResponse

from . import notes
from .config import Settings
from .notes import unique_path
from .store import FAILED, GAVE_UP, PENDING, Memo, Store
from .worker import Transcriber, WhisperTranscriber, Worker

log = logging.getLogger("memos")

MEMO_ID_RE = re.compile(r"^[A-Za-z0-9_-]{1,64}$")
DEVICE_RE = re.compile(r"^[A-Za-z0-9_-]{1,32}$")
EARLIEST_VALID_TIME = 1_700_000_000  # Nov 2023: anything earlier means the stick's clock was never set
CLOCK_TOLERANCE_S = 60               # device clocks further off than this get corrected
WAV_FORMAT = (1, 2, 16000)           # mono, 16-bit, 16 kHz: what the stick records


def _int_header(request: Request, name: str) -> int:
    value = request.headers.get(name)
    if value is None:
        return 0
    try:
        return int(value)
    except ValueError:
        raise HTTPException(400, f"{name} must be an integer")


def memo_time(memo_time: int, device_now: int, now: float) -> tuple[float, str]:
    """When the memo was recorded, and where that came from:
    - device: the stick's clock (right to within CLOCK_TOLERANCE_S, or it
      didn't send X-Device-Now);
    - corrected: the stick's clock was off by more than that; shifted by the
      difference between its clock and ours at upload;
    - received: the stick's clock wasn't set; the time the upload arrived."""
    if not EARLIEST_VALID_TIME < memo_time < now + 86400:
        return now, "received"
    if EARLIEST_VALID_TIME < device_now:
        offset = now - device_now
        if abs(offset) > CLOCK_TOLERANCE_S:
            return min(memo_time + offset, now), "corrected"
    return min(memo_time, now), "device"


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

    app = FastAPI(title="M5Recorder receiver", lifespan=lifespan,
                  docs_url=None, redoc_url=None, openapi_url=None)
    app.state.store = store
    app.state.worker = worker

    @app.get("/health")
    def health():
        counts = store.counts()
        alive = worker.alive() or not start_worker
        body = {
            "ok": alive,
            "worker": "running" if alive else "stopped",
            "unfinished": counts.get(PENDING, 0) + counts.get(FAILED, 0),
            "failed": counts.get(FAILED, 0),
            "gave_up": counts.get(GAVE_UP, 0),
        }
        return JSONResponse(body, status_code=200 if alive else 503)

    @app.post("/upload")
    async def upload(request: Request):
        # The token is checked before anything else, so an unauthenticated
        # request learns nothing from other errors.
        authorization = request.headers.get("authorization", "")
        if not hmac.compare_digest(authorization.encode(), f"Bearer {settings.token}".encode()):
            client = request.headers.get("x-forwarded-for") or (request.client.host if request.client else "?")
            log.warning("rejected upload: bad token (from %s)", client)
            raise HTTPException(401, "bad token")

        memo_id = request.headers.get("x-memo-id", "")
        if not MEMO_ID_RE.match(memo_id):
            raise HTTPException(400, "X-Memo-Id missing or invalid")
        device = request.headers.get("x-memo-device") or memo_id.split("-", 1)[0]
        if not DEVICE_RE.match(device):
            raise HTTPException(400, "X-Memo-Device invalid")
        x_memo_time = _int_header(request, "x-memo-time")
        device_now = _int_header(request, "x-device-now")

        length = request.headers.get("content-length")
        if length and length.isdigit() and int(length) > settings.max_upload_bytes:
            raise HTTPException(413, "too large")

        settings.audio_dir.mkdir(parents=True, exist_ok=True)
        tmp = settings.audio_dir / f".upload-{uuid.uuid4().hex}.part"
        try:
            size = 0
            sha = hashlib.sha256()
            with tmp.open("wb") as f:
                async for chunk in request.stream():
                    size += len(chunk)
                    if size > settings.max_upload_bytes:
                        raise HTTPException(413, "too large")
                    sha.update(chunk)
                    f.write(chunk)
            digest = sha.hexdigest()

            existing = store.get(memo_id)
            if existing:
                # The same memo re-sent (e.g. the reply was lost): fine. The same
                # id with different audio means the stick's ids restarted; say
                # so, or the stick would delete a memo we never stored.
                if existing.sha256 and existing.sha256 != digest:
                    log.warning("memo id %s reused with different audio: 409", memo_id)
                    raise HTTPException(409, "memo id already used for different audio")
                return {"status": "duplicate"}

            try:
                with wave.open(str(tmp), "rb") as w:
                    fmt = (w.getnchannels(), w.getsampwidth(), w.getframerate())
                    if fmt != WAV_FORMAT:
                        raise HTTPException(400, f"expected 16 kHz mono 16-bit WAV, got {fmt}")
                    # Count the bytes actually there, not what the header claims.
                    frames = len(w.readframes(w.getnframes())) // 2
            except (wave.Error, EOFError) as e:
                raise HTTPException(400, f"not a WAV file: {e}")
            if frames == 0:
                raise HTTPException(400, "no audio")
            duration = frames / WAV_FORMAT[2]

            now = time.time()
            created, time_source = memo_time(x_memo_time, device_now, now)
            stem = datetime.fromtimestamp(created, ZoneInfo(settings.timezone)).strftime("%Y-%m-%d %H%M%S")
            final = unique_path(settings.audio_dir, stem, ".wav")
            os.replace(tmp, final)
        finally:
            tmp.unlink(missing_ok=True)

        audio_rel = final.relative_to(settings.vault_dir).as_posix()
        memo = Memo(memo_id=memo_id, device=device, created=created, audio_path=audio_rel,
                    duration=duration, status=PENDING, note_path=worker.new_note_path(audio_rel),
                    size=size, sha256=digest, time_source=time_source)
        if not store.add(memo):  # lost a race with a parallel re-send
            final.unlink(missing_ok=True)
            return {"status": "duplicate"}
        # A note right away, so the memo shows up on the phone even before
        # (or if never) it's transcribed.
        try:
            worker.write_note(memo, memo.note_path, notes.TRANSCRIBING, "transcribing")
        except OSError:
            log.exception("couldn't write the placeholder note for %s", memo_id)
        worker.enqueue(memo_id)
        log.info("received %s (%.1f s, time %s) -> %s", memo_id, duration, time_source, final.name)
        return {"status": "queued"}

    return app


def build() -> FastAPI:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    return create_app(Settings.from_env())

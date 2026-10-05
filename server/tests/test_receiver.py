import io
import sqlite3
import struct
import wave
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

from app import worker as worker_mod
from app.config import Settings
from app.main import create_app, memo_time
from app.store import Store
from app.worker import load_audio

TOKEN = "t" * 32
AUTH = {"Authorization": f"Bearer {TOKEN}"}
MEMO_TIME = 1_790_000_000  # 2026-09-21 14:13:20 UTC = 15:13:20 BST
STEM = "2026-09-21 151320"


def make_wav(seconds: float = 1.0, rate: int = 16000, channels: int = 1, sample: bytes = b"\x01\x00") -> bytes:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(sample * channels * int(seconds * rate))
    return buf.getvalue()


class FakeTranscriber:
    def __init__(self, text="hello world", fail=False):
        self.text, self.fail, self.calls = text, fail, []

    def __call__(self, audio: Path) -> str:
        self.calls.append(audio)
        if self.fail:
            raise RuntimeError("model broke")
        return self.text


@pytest.fixture
def settings(tmp_path):
    return Settings(token=TOKEN, vault_dir=tmp_path / "vault", data_dir=tmp_path / "data",
                    timezone="Europe/London", max_upload_bytes=200_000)


def client_for(settings, transcriber):
    app = create_app(settings, transcriber, start_worker=False)
    return app, TestClient(app)


def upload(client, memo_id="aabbccddeeff-1", body=None, headers=None, t=MEMO_TIME):
    h = {**AUTH, "X-Memo-Id": memo_id, "X-Memo-Time": str(t), **(headers or {})}
    return client.post("/upload", content=make_wav() if body is None else body, headers=h)


def note_text(settings, stem=STEM):
    return (settings.notes_dir / f"{stem}.md").read_text(encoding="utf-8")


# --- auth and input checks ---------------------------------------------------

def test_rejects_missing_and_wrong_token_before_anything_else(settings):
    _, client = client_for(settings, FakeTranscriber())
    with client:
        assert client.post("/upload", content=make_wav(), headers={"X-Memo-Id": "a-1"}).status_code == 401
        assert upload(client, headers={"Authorization": "Bearer nope"}).status_code == 401
        # Bad headers without a token still get 401, not a validation error.
        bad = client.post("/upload", content=b"x", headers={"X-Memo-Time": "abc", "X-Memo-Id": "../x"})
        assert bad.status_code == 401
    assert not list(settings.audio_dir.glob("*"))


def test_rejects_bad_input(settings):
    _, client = client_for(settings, FakeTranscriber())
    with client:
        assert upload(client, memo_id="../etc").status_code == 400
        assert upload(client, headers={"X-Memo-Device": 'x" : [ #'}).status_code == 400
        assert upload(client, headers={"X-Memo-Time": "soon"}).status_code == 400
        assert upload(client, body=b"not audio at all").status_code == 400
        assert upload(client, body=make_wav(rate=8000)).status_code == 400
        assert upload(client, body=make_wav(channels=2)).status_code == 400
        assert upload(client, body=make_wav(seconds=0)).status_code == 400
        assert upload(client, body=make_wav(seconds=10)).status_code == 413  # 320 KB > 200 KB cap
    assert not list(settings.audio_dir.glob("*"))  # no leftovers, including .part files
    assert not list(settings.notes_dir.glob("*.md"))


def test_duration_comes_from_the_bytes_not_the_header(settings):
    app, client = client_for(settings, FakeTranscriber())
    wav = bytearray(make_wav(seconds=0.5))
    struct.pack_into("<I", wav, 40, 0x7FFFFFF0)  # data chunk claims ~2 GB
    with client:
        assert upload(client, body=bytes(wav)).json()["status"] == "queued"
    assert app.state.store.get("aabbccddeeff-1").duration == pytest.approx(0.5)


def test_openapi_is_off(settings):
    _, client = client_for(settings, FakeTranscriber())
    with client:
        assert client.get("/openapi.json").status_code == 404
        assert client.get("/docs").status_code == 404


# --- the normal path -----------------------------------------------------------

def test_upload_writes_placeholder_then_transcript(settings):
    fake = FakeTranscriber("Buy milk.")
    app, client = client_for(settings, fake)
    with client:
        r = upload(client)
        assert r.status_code == 200 and r.json() == {"status": "queued"}
        placeholder = note_text(settings)
        assert "transcript: transcribing" in placeholder and "status: inbox" in placeholder and "transcribing" in placeholder.split("---")[-1]
        app.state.worker.run_pending()

    assert (settings.audio_dir / f"{STEM}.wav").exists()
    note = note_text(settings)
    assert "created: 2026-09-21T15:13:20+01:00" in note
    assert "duration: 1.0" in note
    assert "device: aabbccddeeff" in note
    assert "time_source: device" in note
    assert "transcript: done" in note and "status: inbox" in note
    assert 'day: "[[2026-09-21]]"' in note
    assert f"![[Memos/audio/{STEM}.wav]]" in note
    assert note.rstrip().endswith("Buy milk.")
    assert app.state.store.get("aabbccddeeff-1").status == "done"
    assert len(list(settings.notes_dir.glob("*.md"))) == 1  # the placeholder was replaced, not duplicated


def test_transcription_timing_is_recorded(settings, tmp_path):
    fake = FakeTranscriber("Buy milk.")
    fake.last = {"transcribe_ms": 500, "cold": True, "duration_after_vad": 0.8}
    app, client = client_for(settings, fake)
    with client:
        upload(client)
        app.state.worker.run_pending()
    db = sqlite3.connect(settings.data_dir / "memos.db")
    row = db.execute("SELECT transcribe_ms, audio_s, rtf, cold, duration_after_vad FROM memos").fetchone()
    assert row == (500, 1.0, 0.5, 1, 0.8)


def test_timing_is_measured_without_transcriber_stats(settings):
    app, client = client_for(settings, FakeTranscriber())
    with client:
        upload(client)
        app.state.worker.run_pending()
    db = sqlite3.connect(settings.data_dir / "memos.db")
    ms, cold = db.execute("SELECT transcribe_ms, cold FROM memos").fetchone()
    assert ms is not None and ms >= 0 and cold is None


def test_same_second_gets_distinct_files(settings):
    app, client = client_for(settings, FakeTranscriber())
    with client:
        upload(client, memo_id="dev-1")
        upload(client, memo_id="dev-2", body=make_wav(sample=b"\x02\x00"))
        app.state.worker.run_pending()
    assert sorted(p.name for p in settings.notes_dir.glob("*.md")) == [f"{STEM}-2.md", f"{STEM}.md"]


def test_empty_transcript_is_marked(settings):
    app, client = client_for(settings, FakeTranscriber(""))
    with client:
        upload(client)
        app.state.worker.run_pending()
    assert "_(no speech detected)_" in note_text(settings)


# --- duplicates ----------------------------------------------------------------

def test_resent_memo_is_a_duplicate(settings):
    app, client = client_for(settings, FakeTranscriber())
    with client:
        assert upload(client).json()["status"] == "queued"
        assert upload(client).json()["status"] == "duplicate"
        app.state.worker.run_pending()
    assert len(list(settings.audio_dir.glob("*.wav"))) == 1
    assert len(list(settings.notes_dir.glob("*.md"))) == 1


def test_reused_id_with_different_audio_is_409(settings):
    _, client = client_for(settings, FakeTranscriber())
    with client:
        assert upload(client).json()["status"] == "queued"
        r = upload(client, body=make_wav(sample=b"\x05\x00"))
        assert r.status_code == 409
    assert len(list(settings.audio_dir.glob("*.wav"))) == 1


def test_rows_from_before_hashes_count_as_duplicates(settings):
    app, client = client_for(settings, FakeTranscriber())
    with client:
        upload(client)
        db = sqlite3.connect(settings.data_dir / "memos.db")
        db.execute("UPDATE memos SET sha256 = NULL")
        db.commit()
        db.close()
        assert upload(client, body=make_wav(sample=b"\x05\x00")).json()["status"] == "duplicate"


# --- time ------------------------------------------------------------------------

def test_memo_time_sources():
    now = 1_800_000_000.0
    assert memo_time(0, 0, now) == (now, "received")
    assert memo_time(1_799_999_000, 0, now) == (1_799_999_000, "device")               # old firmware
    assert memo_time(1_799_999_000, int(now) - 30, now) == (1_799_999_000, "device")   # within tolerance
    # Stick's clock 10 min slow: shift the memo forward by 10 min.
    assert memo_time(1_799_999_000, int(now) - 600, now) == (1_799_999_600, "corrected")
    # Never in the future.
    assert memo_time(1_799_999_900, int(now) - 600, now) == (now, "corrected")


def test_unset_clock_uses_receive_time(settings):
    app, client = client_for(settings, FakeTranscriber())
    with client:
        upload(client, t=0)
    memo = app.state.store.get("aabbccddeeff-1")
    assert memo.created > MEMO_TIME and memo.time_source == "received"


def test_drifted_clock_is_corrected(settings):
    app, client = client_for(settings, FakeTranscriber())
    import time as _time
    now = int(_time.time())
    with client:
        upload(client, t=now - 3600, headers={"X-Device-Now": str(now - 600)})  # stick 10 min slow
    memo = app.state.store.get("aabbccddeeff-1")
    assert memo.time_source == "corrected"
    assert memo.created == pytest.approx(now - 3000, abs=5)


# --- failures and retries ----------------------------------------------------------

def test_failed_transcription_is_retried_then_gives_up_with_a_note(settings, monkeypatch):
    fake = FakeTranscriber(fail=True)
    app, client = client_for(settings, fake)
    store, worker = app.state.store, app.state.worker
    with client:
        upload(client)
        worker.run_pending()
    assert store.get("aabbccddeeff-1").status == "failed"
    assert "transcribing" in note_text(settings)  # placeholder still there
    assert store.due_retries(0) == []              # not due yet

    clock = [1e12]
    monkeypatch.setattr(worker_mod.time, "time", lambda: clock[0])
    for _ in range(worker_mod.MAX_ATTEMPTS - 1):
        due = store.due_retries(clock[0])
        assert due == ["aabbccddeeff-1"]
        worker.process(due[0])
        clock[0] += 1e9
    memo = store.get("aabbccddeeff-1")
    assert memo.status == "gave_up" and memo.attempts == worker_mod.MAX_ATTEMPTS
    note = note_text(settings)
    assert "transcript: failed" in note and "transcription failed" in note
    assert f"![[Memos/audio/{STEM}.wav]]" in note
    assert len(list(settings.notes_dir.glob("*.md"))) == 1


def test_failed_transcription_succeeds_on_retry(settings):
    app, client = client_for(settings, FakeTranscriber(fail=True))
    with client:
        upload(client)
        app.state.worker.run_pending()
    # New process (e.g. after a fix): unfinished memos are queued again and succeed.
    app2 = create_app(settings, FakeTranscriber("second try"), start_worker=False)
    assert app2.state.worker.requeue_unfinished() == 1
    app2.state.worker.run_pending()
    assert app2.state.store.get("aabbccddeeff-1").status == "done"
    assert note_text(settings).rstrip().endswith("second try")


def test_memo_from_before_placeholder_notes_gets_a_note(settings):
    app, client = client_for(settings, FakeTranscriber())
    with client:
        upload(client)
        db = sqlite3.connect(settings.data_dir / "memos.db")
        db.execute("UPDATE memos SET note_path = NULL")
        db.commit()
        db.close()
        for p in settings.notes_dir.glob("*.md"):
            p.unlink()
        app.state.worker.run_pending()
    assert note_text(settings).rstrip().endswith("hello world")


def test_old_database_is_migrated(tmp_path):
    path = tmp_path / "memos.db"
    db = sqlite3.connect(path)
    db.execute("""CREATE TABLE memos (memo_id TEXT PRIMARY KEY, device TEXT NOT NULL, created REAL NOT NULL,
                  audio_path TEXT NOT NULL, duration REAL NOT NULL, status TEXT NOT NULL, note_path TEXT, error TEXT)""")
    db.execute("INSERT INTO memos VALUES ('dev-1', 'dev', 1.0, 'a.wav', 2.0, 'done', 'a.md', NULL)")
    db.commit()
    db.close()
    memo = Store(path).get("dev-1")
    assert memo.status == "done" and memo.sha256 is None and memo.attempts == 0


# --- health ----------------------------------------------------------------------

def test_health(settings):
    _, client = client_for(settings, FakeTranscriber())
    with client:
        assert client.get("/health").json() == {
            "ok": True, "worker": "running", "unfinished": 0, "failed": 0, "gave_up": 0}


def test_health_is_503_when_the_worker_isnt_running(settings):
    app = create_app(settings, FakeTranscriber(), start_worker=True)
    client = TestClient(app)  # no `with`: the lifespan (and so the worker) never starts
    r = client.get("/health")
    assert r.status_code == 503 and r.json()["ok"] is False


# --- audio loading -------------------------------------------------------------------

def test_load_audio_reads_stick_format_directly(tmp_path):
    stick = tmp_path / "stick.wav"
    stick.write_bytes(make_wav(seconds=0.5))
    samples = load_audio(stick)
    assert samples.dtype.name == "float32" and len(samples) == 8000
    assert abs(samples[0] - 1 / 32768) < 1e-9

    other = tmp_path / "other.wav"
    other.write_bytes(make_wav(seconds=0.5, rate=8000))
    assert load_audio(other) == str(other)  # left to faster-whisper's decoder

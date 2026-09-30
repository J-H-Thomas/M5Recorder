import io
import wave
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

from app.config import Settings
from app.main import create_app
from app.worker import load_audio

TOKEN = "t" * 32
AUTH = {"Authorization": f"Bearer {TOKEN}"}
MEMO_TIME = 1_790_000_000  # 2026-09-21 14:13:20 UTC


def make_wav(seconds: float = 1.0, rate: int = 16000) -> bytes:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"\x01\x00" * int(seconds * rate))
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


def test_rejects_missing_and_wrong_token(settings):
    _, client = client_for(settings, FakeTranscriber())
    with client:
        assert client.post("/upload", content=make_wav(), headers={"X-Memo-Id": "a-1"}).status_code == 401
        assert upload(client, headers={"Authorization": "Bearer nope"}).status_code == 401
    assert not list(settings.audio_dir.glob("*"))


def test_upload_writes_audio_and_note(settings):
    fake = FakeTranscriber("Buy milk.")
    app, client = client_for(settings, fake)
    with client:
        r = upload(client)
        assert r.status_code == 200 and r.json() == {"status": "queued"}
        app.state.worker.run_pending()

    audio = settings.audio_dir / "2026-09-21 151320.wav"  # 14:13:20 UTC = 15:13:20 BST
    assert audio.exists()
    note = (settings.notes_dir / "2026-09-21 151320.md").read_text(encoding="utf-8")
    assert "created: 2026-09-21T15:13:20+01:00" in note
    assert "duration: 1.0" in note
    assert "device: aabbccddeeff" in note
    assert "![[Memos/audio/2026-09-21 151320.wav]]" in note
    assert note.rstrip().endswith("Buy milk.")
    assert app.state.store.get("aabbccddeeff-1").status == "done"


def test_duplicate_memo_id_is_not_stored_twice(settings):
    app, client = client_for(settings, FakeTranscriber())
    with client:
        assert upload(client).json()["status"] == "queued"
        assert upload(client).json()["status"] == "duplicate"
        app.state.worker.run_pending()
    assert len(list(settings.audio_dir.glob("*.wav"))) == 1
    assert len(list(settings.notes_dir.glob("*.md"))) == 1


def test_same_second_gets_distinct_files(settings):
    app, client = client_for(settings, FakeTranscriber())
    with client:
        upload(client, memo_id="dev-1")
        upload(client, memo_id="dev-2")
        app.state.worker.run_pending()
    assert sorted(p.name for p in settings.notes_dir.glob("*.md")) == [
        "2026-09-21 151320-2.md", "2026-09-21 151320.md"]


def test_unset_clock_uses_receive_time(settings):
    app, client = client_for(settings, FakeTranscriber())
    with client:
        upload(client, t=0)
    memo = app.state.store.get("aabbccddeeff-1")
    assert memo.created > MEMO_TIME


def test_rejects_bad_input(settings):
    _, client = client_for(settings, FakeTranscriber())
    with client:
        assert upload(client, memo_id="../etc").status_code == 400
        assert upload(client, body=b"not audio at all").status_code == 400
        assert upload(client, body=make_wav(seconds=10)).status_code == 413  # 320 KB > 200 KB cap
    assert not list(settings.audio_dir.glob("*"))  # no leftovers, including .part files


def test_failed_transcription_is_retried_on_restart(settings):
    app, client = client_for(settings, FakeTranscriber(fail=True))
    with client:
        upload(client)
        app.state.worker.run_pending()
    assert app.state.store.get("aabbccddeeff-1").status == "failed"
    assert not list(settings.notes_dir.glob("*.md"))

    # New process: unfinished memos are queued again and succeed.
    app2 = create_app(settings, FakeTranscriber("second try"), start_worker=False)
    assert app2.state.worker.requeue_unfinished() == 1
    app2.state.worker.run_pending()
    assert app2.state.store.get("aabbccddeeff-1").status == "done"
    assert "second try" in next(settings.notes_dir.glob("*.md")).read_text(encoding="utf-8")


def test_empty_transcript_is_marked(settings):
    app, client = client_for(settings, FakeTranscriber(""))
    with client:
        upload(client)
        app.state.worker.run_pending()
    assert "_(no speech detected)_" in next(settings.notes_dir.glob("*.md")).read_text(encoding="utf-8")


def test_health(settings):
    _, client = client_for(settings, FakeTranscriber())
    with client:
        assert client.get("/health").json() == {"ok": True, "unfinished": 0}


def test_load_audio_reads_stick_format_directly(tmp_path):
    stick = tmp_path / "stick.wav"
    stick.write_bytes(make_wav(seconds=0.5))
    samples = load_audio(stick)
    assert samples.dtype.name == "float32" and len(samples) == 8000
    assert abs(samples[0] - 1 / 32768) < 1e-9

    other = tmp_path / "other.wav"
    other.write_bytes(make_wav(seconds=0.5, rate=8000))
    assert load_audio(other) == str(other)  # left to faster-whisper's decoder

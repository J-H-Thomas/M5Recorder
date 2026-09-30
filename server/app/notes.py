"""File naming and Markdown note output."""

import os
from datetime import datetime
from pathlib import Path

TRANSCRIBING = "_(transcribing…)_"
NO_SPEECH = "_(no speech detected)_"
FAILED = "_(transcription failed; the audio is above)_"


def unique_path(directory: Path, stem: str, suffix: str) -> Path:
    """directory/stem.suffix, or stem-2, stem-3... if taken."""
    path = directory / f"{stem}{suffix}"
    n = 2
    while path.exists():
        path = directory / f"{stem}-{n}{suffix}"
        n += 1
    return path


def atomic_write_bytes(path: Path, data: bytes) -> None:
    tmp = path.with_name(f".{path.name}.tmp")
    tmp.write_bytes(data)
    os.replace(tmp, path)


def render_note(*, created: datetime, duration: float, device: str, memo_id: str,
                audio_rel: str, body: str, status: str, time_source: str | None) -> str:
    """device and memo_id are validated to [A-Za-z0-9_-] by the receiver, so
    they're safe to write into the YAML unquoted."""
    return (
        "---\n"
        f"created: {created.isoformat(timespec='seconds')}\n"
        f"duration: {duration:.1f}\n"
        f"device: {device}\n"
        f"memo_id: {memo_id}\n"
        f"time_source: {time_source or 'device'}\n"
        f"status: {status}\n"
        "tags: [memo]\n"
        "---\n"
        f"![[{audio_rel}]]\n"
        "\n"
        f"{body.strip()}\n"
    )

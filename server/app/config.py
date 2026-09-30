"""Settings, read from environment variables."""

import os
from dataclasses import dataclass
from pathlib import Path

MIN_TOKEN_LENGTH = 24


@dataclass(frozen=True)
class Settings:
    token: str
    vault_dir: Path
    data_dir: Path
    notes_subdir: str = "Memos"
    audio_subdir: str = "Memos/audio"
    timezone: str = "UTC"
    max_upload_bytes: int = 20 * 1024 * 1024
    whisper_model: str = "large-v3-turbo"
    whisper_device: str = "auto"
    whisper_compute: str = "int8"
    whisper_model_dir: Path | None = None
    language: str | None = "en"
    # Home Assistant via MQTT discovery (off unless mqtt_host is set).
    mqtt_host: str | None = None
    mqtt_port: int = 1883
    mqtt_user: str | None = None
    mqtt_password: str | None = None
    mqtt_discovery_prefix: str = "homeassistant"

    @property
    def notes_dir(self) -> Path:
        return self.vault_dir / self.notes_subdir

    @property
    def audio_dir(self) -> Path:
        return self.vault_dir / self.audio_subdir

    @classmethod
    def from_env(cls) -> "Settings":
        token = os.environ.get("MEMO_TOKEN", "")
        if len(token) < MIN_TOKEN_LENGTH:
            raise SystemExit(
                f"MEMO_TOKEN must be set to a random string of at least {MIN_TOKEN_LENGTH} characters"
            )
        model_dir = os.environ.get("WHISPER_MODEL_DIR")
        return cls(
            token=token,
            vault_dir=Path(os.environ.get("VAULT_DIR", "/vault")),
            data_dir=Path(os.environ.get("DATA_DIR", "/data")),
            notes_subdir=os.environ.get("NOTES_SUBDIR", "Memos"),
            audio_subdir=os.environ.get("AUDIO_SUBDIR", "Memos/audio"),
            timezone=os.environ.get("TZ", "UTC"),
            max_upload_bytes=int(float(os.environ.get("MAX_UPLOAD_MB", "20")) * 1024 * 1024),
            whisper_model=os.environ.get("WHISPER_MODEL", "large-v3-turbo"),
            whisper_device=os.environ.get("WHISPER_DEVICE", "auto"),
            whisper_compute=os.environ.get("WHISPER_COMPUTE", "int8"),
            whisper_model_dir=Path(model_dir) if model_dir else None,
            language=os.environ.get("LANGUAGE", "en") or None,
            mqtt_host=os.environ.get("MQTT_HOST") or None,
            mqtt_port=int(os.environ.get("MQTT_PORT", "1883")),
            mqtt_user=os.environ.get("MQTT_USER") or None,
            mqtt_password=os.environ.get("MQTT_PASSWORD") or None,
            mqtt_discovery_prefix=os.environ.get("MQTT_DISCOVERY_PREFIX", "homeassistant"),
        )

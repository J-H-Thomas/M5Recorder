# M5Recorder receiver

Receives voice memos from the M5StickS3, transcribes them with
[faster-whisper](https://github.com/SYSTRAN/faster-whisper), and writes one
Markdown note per memo into an Obsidian vault, with the audio embedded.

```
stick ── HTTPS ──► Pangolin ──► Newt tunnel ──► this container (MS-01)
                                                 ├─ <vault>/Memos/audio/2026-09-30 141205.wav
                                                 └─ <vault>/Memos/2026-09-30 141205.md
```

## API

- `POST /upload`: body is a WAV file, **16 kHz mono 16-bit** (anything else
  gets 400). Headers:
  - `Authorization: Bearer <MEMO_TOKEN>` (checked first; 401 otherwise).
  - `X-Memo-Id`: unique per memo, `[A-Za-z0-9_-]{1,64}`. The stick sends
    `<MAC>-<epoch>-<seq>`, where the random epoch makes ids unique even if its
    counter restarts.
    - Re-sending the same id with the **same** audio returns
      `{"status": "duplicate"}`, so the stick can retry safely.
    - The same id with **different** audio returns **409**, so a memo is never
      mistaken for one already stored.
  - `X-Memo-Time`: Unix time at recording (`0`: the stick's clock wasn't set).
  - `X-Device-Now`: the stick's clock at upload. If it's more than 60 s off, the
    memo time is corrected by the difference.
  - `X-Memo-Device` (optional): defaults to the id's first part.

  Returns `{"status": "queued"}` as soon as the audio is saved, and writes a
  placeholder note ("transcribing…") straight away. Transcription runs in the
  background, one memo at a time, and replaces the placeholder. Errors are JSON
  with a `detail` field.
- `GET /health`:
  `{"ok", "worker", "unfinished", "failed", "gave_up"}`. It returns **503** if
  the transcription worker isn't running.

A failed transcription is retried after 1, 4, 16 and 64 minutes (and on
restart). After 5 attempts the note says "transcription failed" and keeps the
audio, so the memo is never lost from view.

## Note format

```markdown
---
created: 2026-09-30T14:12:05+01:00
duration: 12.4
device: aabbccddeeff
memo_id: aabbccddeeff-1a2b3c4d-17
time_source: device
status: done
tags: [memo]
---
![[Memos/audio/2026-09-30 141205.wav]]

Remember to book the car in for its MOT next week.
```

- `status`: `transcribing`, then `done`, or `failed` after 5 attempts.
- `time_source`:
  - `device`: the stick's clock;
  - `corrected`: the stick's clock was off, and the server shifted it;
  - `received`: the stick's clock wasn't set, so the upload time was used.

## Settings (environment variables)

| Variable | Default | Notes |
|---|---|---|
| `MEMO_TOKEN` | (required) | At least 24 characters. Generate one with `python -c "import secrets; print(secrets.token_urlsafe(32))"`; the same value goes in the firmware's `secrets.h`. |
| `TZ` | `UTC` | Time zone for note names and `created`, e.g. `Europe/London`. |
| `NOTES_SUBDIR` | `Memos` | Folder inside the vault for notes. |
| `AUDIO_SUBDIR` | `Memos/audio` | Folder inside the vault for the WAV files. |
| `WHISPER_MODEL` | `large-v3-turbo` | Any faster-whisper model name. `small` or `base` are faster and less accurate. |
| `WHISPER_DEVICE` | `auto` | `cpu`, `cuda` or `auto`. |
| `WHISPER_COMPUTE` | `int8` | `int8` on CPU; `float16` or `int8_float16` on an NVIDIA GPU. |
| `LANGUAGE` | `en` | Empty for auto-detect. |
| `MAX_UPLOAD_MB` | `20` | About 10 minutes of 16 kHz audio. |
| `MQTT_HOST` | (off) | MQTT broker for Home Assistant (e.g. the Mosquitto add-on). Unset = no HA. |
| `MQTT_PORT` | `1883` | |
| `MQTT_USER` / `MQTT_PASSWORD` | | A broker login (for the Mosquitto add-on, an HA user made for this). |
| `MQTT_DISCOVERY_PREFIX` | `homeassistant` | HA's MQTT discovery prefix. |

## Home Assistant

With `MQTT_HOST` set, each stick appears in HA (via MQTT discovery) as an
**M5Recorder** device with these sensors:
- battery %, battery voltage and charging;
- last seen;
- memos waiting and memos set aside;
- memos today;
- estimated days left (from the battery trend since the last charge);
- memos transcribing and transcription failures;
- ignored presses and firmware.

The receiver's own online/offline state is the entities' availability (an MQTT
last will), so HA also sees when the receiver is down. Values update on every
upload and heartbeat (the stick checks in every 6 h), after each
transcription, and every 10 minutes.

The stick reports through headers on `/upload` and on `POST /heartbeat`
(same bearer token): `X-Battery-mV`, `X-Battery-Pct`, `X-Charging` (0/1),
`X-Queue`, `X-Set-Aside`, `X-Ignored` and `X-Firmware`.

Volumes: `/vault` (the Obsidian vault), `/data` (the memo database), and
`/models` (downloaded Whisper models, about 1.6 GB for large-v3-turbo; the
model downloads on the first memo).

## Deploy on Unraid

1. Copy this `server/` folder to the MS-01 (e.g. `/mnt/user/appdata/m5recorder/src`)
   and build the image in the Unraid terminal:
   `docker build -t m5recorder-receiver /mnt/user/appdata/m5recorder/src`.
2. Add a container (Docker → Add Container) with repository
   `m5recorder-receiver`, and the port, variables and paths from
   `docker-compose.yml`. Run it as `99:100` (Extra Parameters: `--user 99:100`)
   so the files it writes into the vault are owned like the rest of the share.
   Alternatively, use `docker compose up -d --build` in that folder with the
   Compose Manager plugin.
3. Check `http://<ms01>:8080/health` on the LAN.

## Pangolin

Add a resource for the receiver, e.g. `memos.<your domain>` → `http://<ms01 LAN IP>:8080`
through the site whose Newt runs on the home network. The stick can't sign in
through Pangolin's login page, so either turn authentication off for this
resource, or keep it on and add rules that **bypass auth for the paths
`/upload` and `/health`**. The receiver's token check is what protects it.
Then, from outside the home network (a phone on mobile data):

```sh
curl https://memos.<domain>/health
curl -i -X POST https://memos.<domain>/upload                 # expect 401
```

## Run locally (development)

```sh
cd server
python -m venv .venv
.venv/Scripts/pip install -r requirements-dev.txt   # Windows; .venv/bin/pip elsewhere
.venv/Scripts/python -m pytest
MEMO_TOKEN=... VAULT_DIR=./vault DATA_DIR=./data WHISPER_MODEL=base \
  .venv/Scripts/uvicorn --factory app.main:build --port 8080
curl -H "Authorization: Bearer $MEMO_TOKEN" -H "X-Memo-Id: test-1" \
  --data-binary @memo.wav http://127.0.0.1:8080/upload
```

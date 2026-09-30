# M5Recorder roadmap: tidy-up after the first working version (2026-09-30)

## Context

The memo chain works end to end: the stick records, joins the phone hotspot in
about 1 s, and reaches the server about 5 s after release. The MS-01 transcribes
(large-v3-turbo), the note lands in the Memex vault, and vault sync puts it on
Jay's phone almost instantly. Jay wants a tidy-up roadmap and picked:
**OTA updates, a prebuilt server image, quieter logs, a battery life check,
a repo and docs tidy, and the home Wi-Fi test.**

Current pain points these address:
- Every firmware change needs USB and download mode (hold the side button 2 s).
- The server is rebuilt from source on Unraid by hand.
- Battery life is unknown: there's only a voltage-based % on the KEY2 screen.
- The receiver log is mostly once-a-minute health checks.
- The repo has CRLF warnings, a long CLAUDE.md, and no CI.

Order: quick, low-risk server and repo work first. Then battery telemetry, which
starts collecting data early. Then OTA last, since it needs one final USB flash
and a partition change.

## Phase 0: field fixes (done 2026-09-30)

After carrying the stick for an evening:
- **Pocket presses**: recording now needs press, release, press and hold; other
  presses sleep again at once without lighting the screen. Memos max 60 s. A held
  button no longer keeps the stick awake (it sleeps until the release).
- **Hotspot with no internet stuck on "Sending..."**: TLS/HTTP timeouts cut
  from the core's 120 s handshake / 30 s reads to 10–15 s, plus a per-request
  watchdog that forces sleep. It shows **No internet**, and retries back off
  15 → 30 → 60 → 120 min.

## Phase 1: server and repo housekeeping (no stick changes)

1. **Quieter logs**: `server/app/main.py`, in `build()`: add a `logging.Filter`
   on the `uvicorn.access` logger that drops `GET /health` lines (Docker
   HEALTHCHECK and Pangolin probes). Keep uploads and errors. Test: the filter
   drops health lines and keeps upload lines.
2. **Prebuilt image**: `.github/workflows/server.yml`. On pushes to `main` that
   touch `server/`: run `pytest`, then build and push
   `ghcr.io/j-h-thomas/m5recorder-receiver:latest` and `:sha-<short>` (public
   package; the image holds no secrets). Change `server/docker-compose.yml` and
   the README to `image: ghcr.io/...` (drop `build:`). Give Jay a prompt for the
   Unraid Claude session to switch the Compose Manager project to the pulled
   image. Updating is then "pull + recreate". Keep the `.env` (token) and
   volumes as they are.
3. **Repo and docs tidy**
   - `.gitattributes`: `* text=auto eol=lf`, with `*.bat`/`*.cmd` as `crlf` (ends
     the CRLF warnings; renormalise in one commit).
   - `.github/workflows/firmware.yml`: `pio run` for `firmware/recorder` with
     `secrets.example.h` copied to `secrets.h` (compile check only; artifacts are
     never published, since real builds contain Wi-Fi passwords).
   - Trim `CLAUDE.md` to layout, status, decisions and gotchas. Move the
     wake-test history to `firmware/wake_test/RESULTS.md` and mark `wake_test`
     as an archived experiment in its README.
   - Update the root README (architecture, links, how to update the stick and
     the server).

## Phase 2: battery life check (telemetry)

Goal: know when the stick will run out and how many days a charge lasts,
without a meter.

- **Stick** (`firmware/recorder/src/uploader.cpp`, `main.cpp`): add headers to
  each upload: `X-Battery-mV`, `X-Battery-Pct`, `X-Charging`, `X-Queue`,
  `X-Firmware` (version string from a build flag, e.g. git short hash + date).
  Add a **heartbeat**: a timer wake every 6 h (alongside the 15 min retry when
  memos are queued) that joins Wi-Fi and `POST /heartbeat` with the same headers.
  Screen stays off. This gives a discharge curve even on days without memos.
- **Receiver**: a `battery` table in the existing SQLite `Store`
  (`server/app/store.py`), filled from uploads and heartbeats; `POST /heartbeat`
  (same bearer token); `GET /battery.csv`. It also writes
  **`<vault>/Memos/_M5Recorder status.md`**: last seen, battery % and voltage,
  charging, queue, firmware version, memos today, and an estimated days left
  (from the slope over the last few days). It syncs to the phone like the memos.
  Tests for the table, endpoint and status note.
- **After 3–5 days of data**: report the real drain (mAh/day while idle, and per
  memo) and decide whether L3B-on-in-sleep or the heartbeat interval needs
  changing. Analysis only, no code.

## Phase 3: firmware updates over Wi-Fi (OTA)

- **Partitions** (`firmware/recorder/partitions.csv`): two 1.75 MB app slots
  (`ota_0`, `ota_1`; the app is 1.19 MB now) plus LittleFS 0x460000 (~4.4 MB).
  The **queue drops from ~2.8 to ~2.2 min of audio.** This is the last USB flash.
  It reformats the queue, so send any queued memos first.
- **Receiver**: `/data/firmware/` (volume) holds `firmware.bin` and
  `manifest.json` (`version`, `size`, `sha256`).
  - `GET /firmware/manifest` and `GET /firmware/bin` use the stick's token.
  - `POST /firmware` uploads a build and needs a **separate `ADMIN_TOKEN`**. It
    lives only on the PC and server, so a token pulled from a stick can't push
    firmware.
- **Stick**: new `firmware/recorder/src/ota.cpp`.
  - It checks the manifest after a successful upload round or a heartbeat,
    reusing the Wi-Fi connection, at most every 6 h.
  - It also checks on demand with a long press of KEY2 on the status screen,
    which shows "Checking for update…".
  - If the version differs and the battery is 30% or more (or it's charging):
    show "Updating…", stream `/firmware/bin` into the Arduino `Update` class
    while computing SHA-256, check the size and hash against the manifest,
    `Update.end(true)`, then reboot.
  - On any failure it keeps the current firmware and says so on screen.
  - After reboot the status screen shows the new version.
- **Publishing**: `tools/publish_firmware.py` builds with the local `secrets.h`,
  reads the version, and POSTs the bin and manifest with `ADMIN_TOKEN`
  (from an env var or a gitignored file).
- **Safety**: ESP-IDF rollback isn't enabled in the Arduino bootloader, so a bad
  build that can't reach Wi-Fi would need USB recovery. Mitigation: publish
  only builds that passed a USB-flashed smoke test. Every build keeps the OTA
  code path, and the KEY2 long press is the manual trigger.

## Home Wi-Fi test (any time Jay is home)

- Record at home: the log/status shows the home network (the hotspot is first
  in the list, but the "last good network" logic should settle on home).
- Walk out of range, then record: it falls back to the hotspot, then switches
  back at home. Record the connect times in CLAUDE.md.

## Later (not picked now)

Screen off while recording; catching "one" every time (boot-time work);
longer offline queue (IMA-ADPCM, about 4×); smarter notes (title, summary,
tags); links into daily notes; faster uploads (TLS session reuse).

## Verification

- Phase 1: `pytest` (with the new log-filter test) passes locally and in
  Actions. The GHCR image appears. Unraid runs the pulled image
  (`/health` ok through Pangolin, a test upload works). The firmware CI build is
  green. `git status` shows no CRLF warnings after renormalising.
- Phase 2: receiver tests for the heartbeat, battery table and status note. On
  the stick, a memo's upload carries the battery headers, and the status note
  appears in the vault on the phone. The heartbeat is visible in `battery.csv`
  6 h later.
- Phase 3: flash the new partitions over USB. Publish a build with a new
  version and trigger with a KEY2 long press; the stick reboots showing the new
  version. Publishing a corrupted bin fails the hash check and keeps the old
  firmware. A normal memo still uploads after the update.
- Each phase is committed and pushed to `main` when done, with docs updated
  in the same commit.

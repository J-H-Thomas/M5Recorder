# M5Recorder: project notes

Pocket voice-memo recorder on an **M5StickS3**, built for Jay. To record:
**press, release, then press and hold** the front button; let go to stop. The
memo is queued in flash, uploaded over Wi-Fi (Jay's phone hotspot, or home
Wi-Fi), transcribed on the home server, and lands as a note in the Obsidian
vault, which syncs to Jay's phone within seconds.

```
stick ─► WAV queue in LittleFS ─► Wi-Fi (last network that worked first,
  then hotspot, then home) ─► HTTPS POST https://memos.asdf.ac/upload (Bearer token)
  ─► Pangolin (Newt tunnel, no router ports) ─► MS-01 (Unraid) receiver container
  ─► faster-whisper ─► <vault>/Memos/<date time>.md with the audio embedded
```

Plans are in `ROADMAP.md` (data safety, then alerts and battery, then
compression and security, then OTA). The repo is **public**: no secrets,
Wi-Fi names, LAN addresses or personal data in files or commit messages.

## Layout

- `firmware/recorder/`: the stick's firmware (PlatformIO). `include/secrets.h`
  (Wi-Fi networks, upload URL, token) is gitignored; **never print or commit
  it**. Copy `secrets.example.h`. See its README for controls and screens.
- `server/`: the receiver (FastAPI + faster-whisper, Docker). Tests:
  `server/.venv/Scripts/python -m pytest` from `server/`. See its README.
- `firmware/wake_test/`: the wake-latency experiment the recorder's fast start
  came from (archived).

## Working on it

- **Branches:** multi-part work goes on a branch (e.g. `phase1-data-safety`)
  and is merged to `main` once the stick and the server are both tested. `main`
  should match what is deployed. Commit and push as you go.
- **Flashing:** `pio run -t upload --upload-port COMx` in `firmware/recorder`.
  The stick is usually asleep (no USB port), so Jay puts it in download mode:
  hold the **power button** (the small PMIC button, not KEY2) about 2 s until the
  green LED blinks. Find the port by USB VID 303A (it moves between COM8/COM9).
  After flashing, a single click of the power button restarts it.
- **Logs:** the stick prints to USB serial while awake. A reconnecting logger
  (a pyserial script that reopens the VID 303A port whenever it appears) catches
  wakes. Stop it before flashing, or it holds the port. Lines printed before
  USB enumerates (rejected presses, early setup) are lost.
- **Server changes are deployed by a Claude session on the Unraid box.** Give
  Jay a self-contained prompt for it. Source is in
  `/mnt/user/appdata/m5recorder/src`; rebuild with
  `docker build -t m5recorder-receiver /mnt/user/appdata/m5recorder/src/server`,
  then recreate the container through Compose Manager project `m5recorder`.
- **Shell gotcha:** in this environment, `\n` inside Bash heredocs and inline
  scripts gets turned into real newlines (and `\v` into a control character).
  Write files containing escapes with the Write/Edit tools, or build the
  backslash with `chr(92)` in Python.

## Deployment (MS-01, Unraid)

- Container `m5recorder-receiver`, host port 8090, user 99:100, large-v3-turbo on
  CPU int8 (no GPU). Data and models are in `/mnt/user/appdata/m5recorder/{data,models}`;
  `MEMO_TOKEN` is in the Compose Manager project's `.env` (root only).
- Vault `/mnt/user/Obsidian/Memex`: notes in `Memos/`, audio in `Memos/audio/`.
  It syncs to Jay's phone.
- Pangolin resource "m5-memos" → the MS-01's LAN address, port 8090, on site
  "Unraid". Public name `memos.asdf.ac`. Pangolin login is on, with bypass rules
  for `/upload`, `/heartbeat` and `/health` only; the receiver's bearer token
  protects those. **Any new path the stick calls needs its own bypass rule**:
  otherwise Pangolin answers with a 302 to its login page. That's how the first
  heartbeats failed (2026-10-01).
- **TLS:** Let's Encrypt "Gen Y" chain: leaf ← YR1 ← ISRG Root YR (sent
  cross-signed by ISRG Root X1). The stick's mbedTLS (ESP-IDF 4.4) has **no
  TLS 1.3**; the server also offers TLS 1.2 with ECDHE-RSA-AES-GCM. `ca_certs.h`
  holds ISRG X1, X2, YR and YE. The server key is RSA 4096, so the handshake
  (about 1.5–2 s per wake) dominates each upload.
- **Never run `docker image prune -a`** on Unraid: it deletes the tagged rollback
  image.

## Known-good versions (to roll back to)

- **`v0.1.0-mvp`** (git tag, 2026-09-30): the first version in daily use,
  before the post-review roadmap.
  - **Stick:** the exact flashed image (built with the real secrets, so
    private) is in `C:/AI Working/M5Recorder-backups/v0.1.0-mvp/` (outside any
    repo), with `RESTORE.md` (esptool command) and SHA256SUMS. Or check out the
    tag, restore `secrets.h` and `pio run -t upload`.
  - **Server:** image `m5recorder-receiver:v0.1.0-mvp` (= `fdf9b50c50e1`). A
    rebuild from the tag may differ, because transitive Python dependencies
    aren't pinned yet. DB snapshot:
    `/mnt/user/appdata/m5recorder/data/memos-v0.1.0-mvp.db` (24 memos).
  - **Server rollback:**
    1. Set `image: m5recorder-receiver:v0.1.0-mvp` in
       `/boot/config/plugins/compose.manager/projects/m5recorder/docker-compose.yml`
       and recreate the container.
    2. To restore the DB too: stop the container, copy the snapshot over
       `memos.db`, **delete `memos.db-wal` and `memos.db-shm`**, then start it.
    3. Phase 1 changed the memo id format (`<MAC>-<epoch>-<seq>`). The old
       server accepts it (the id regex allows it), but roll the stick and the
       server back together.

## How it works now (and why)

**Stick** (`firmware/recorder/src`):
- **Fast start, with the codec rail (L3B) off in sleep:** on a KEY1 wake,
  `audio::start()` powers L3B up, waits 20 ms, **zeroes ES8311 REG0B/REG0C**
  (power-up timing, as Espressif's driver does), and writes the mic
  registers. It then starts I2S and a capture task, all before `M5.begin()`
  (which alone takes about 400 ms). The first audio arrives about 2 ms after
  capture starts.
- **How we got here (2026-10-02 to 04):**
  - Originally L3B stayed on in sleep because, with the default REG0B/0C, the
    codec sent exact zeros for 1026 ms after power-up. That clipped the first
    word, and fast VMID charge (REG0D 0x03) made no difference.
  - But HA showed L3B-on costing about 3 mA asleep (a quiet 6 h cost 3–9 %, about
    2.5 days per charge) against about 1 % per 6 h with it off (weeks).
  - Zeroing REG0B/0C fixed the warm-up. Jay reports clear audio and perfect
    transcripts, with a slightly louder click at the start and end. A short
    software fade would hide it if it ever matters.
  - `L3B_OFF_IN_SLEEP` (default 1, in `audio.h`) can switch back to the old
    behaviour.
- **Gesture** (`detectGesture()`): the first press is released within 600 ms of
  app start, the second press comes within 600 ms, with a 30 ms debounce. Jay's
  presses measure 120–143 ms and 100–150 ms. It's checked before `M5.begin()`,
  so a rejected press never lights the screen; rejections count as "Ignored
  presses" on the status screen. The memo keeps 0.25 s of pre-roll before the
  second press, which usually saves "one". The cap is 60 s. KEY2 needs press,
  release, press for the status screen.
- **Held buttons:** `deepSleep()` arms ext0/ext1 to wake on the *release* of a
  held button (RTC flags `key1/2_wait_release`); that wake sleeps again at once.
- **Memo ids:** `<MAC>-<epoch>-<seq>`. The epoch is random, kept in NVS with the
  seq, and regenerated if NVS is wiped, so ids never repeat and the server can't
  mistake a new memo for a duplicate. Queue files are
  `/q/<seq>_<time>_<epoch>.wav`; older 2-field names are still sent with old ids.
- **Upload:**
  - Join by name, the last-good network first. A scan-first version saw "0
    networks": the core ends an async scan after 20 × the per-channel time.
    A network that times out gets one 12 s retry.
  - Wi-Fi sleep is off, and each memo is sent from PSRAM in one write.
  - Timeouts: 10 s TLS handshake, 15 s reads, 8 s connect. The core defaults
    (120 s handshake) left it stuck on "Sending..." on a hotspot with no data.
  - A per-request esp_timer watchdog forces sleep if a request still blocks.
  - A memo is deleted only on a 2xx whose body says `queued` or `duplicate`.
  - A receiver 4xx carrying `"detail"` (bad format, 409) moves the memo to
    `/bad/` ("Set aside" on the status screen).
  - Transport errors show "No internet", or "Cert error" when mbedTLS reports
    X509 verification failed.
- **Retries:** the backoff is 15/30/60/120 min from an **absolute deadline** in
  RTC memory (`retry_deadline`, on the `time()` clock), so short wakes don't
  restart it. It resets on success, and a new memo always tries at once.
- **Clock:** the sleep clock is the internal RC oscillator
  (`CONFIG_ESP32S3_RTC_CLK_SRC_INT_RC`), which drifts. NTP runs on every upload
  round, and each request carries `X-Device-Now` so the server can correct the
  memo time.
- **Storage:** LittleFS is formatted only if it won't mount twice (the screen
  then says "Storage reset"). `.part` leftovers are cleaned at mount. A memo
  that can't be read is kept, not deleted. The queue holds about 2.8 min of
  audio (Phase 3 adds compression).
- **Green LED:** PM1 `PWR_CFG` (0x06) bit 4 `LED_EN` comes up on, and M5Unified
  never clears it for the StickS3. The firmware clears it after `M5.begin()`;
  the PM1 keeps it through deep sleep. It probably drew more than the rest of
  the sleeping stick.

**Status in Home Assistant** (Phase 2):
- The stick sends `X-Battery-mV/-Pct`, `X-Charging`, `X-Queue` (memos left
  after this one), `X-Set-Aside`, `X-Ignored` and `X-Firmware` (`git describe`,
  via `version_flag.py`) on every upload. When there has been no report for 6 h,
  a timer wake does `POST /heartbeat` (`heartbeat_deadline` in RTC memory,
  alongside `retry_deadline`).
- The battery is read after `M5.begin()`, before Wi-Fi. There's no Wi-Fi below
  3.45 V (not charging) or after a brownout/crash reset.
- The receiver stores reports in a `telemetry` table and publishes MQTT
  discovery (`app/ha.py`) to HA's Mosquitto broker on the HA box (10.10.1.2:1883,
  HA user `m5recorder`, password in the receiver's `.env`). Entities are
  `sensor.m5recorder_*` and `binary_sensor.m5recorder_charging`, and their
  availability is the receiver's MQTT last will.
- "Estimated days left" is a least-squares battery trend since the last charge
  (`telemetry.days_left`).
- Five automations (created through the HA MCP, notify
  `mobile_app_james_s25`): `automation.m5recorder_no_check_in_for_24_h`,
  `_battery_low`, `_transcription_failed`, `_receiver_offline`,
  `_memos_stuck_transcribing`. The battery one keeps a template condition on
  purpose: it rejects a jump from unavailable/unknown, and numeric_state has no
  `not_from`.

**Receiver** (`server/app`):
- The token is checked first (constant-time).
- Upload checks: the id and device headers must be `[A-Za-z0-9_-]`; the audio
  must be 16 kHz mono 16-bit, with its length taken from the bytes actually
  received.
- It stores size and sha256: a known id with the same audio is a `duplicate`
  (200); with different audio, a **409**.
- It writes a placeholder note ("transcribing…") at once, and the worker
  replaces it.
- The worker catches everything and retries failures (1, 4, 16, 64 min). After
  5 attempts it writes "transcription failed" with the audio embedded.
- `/health` reports worker liveness and failed/gave-up counts, and returns 503
  if the worker is down.
- Time: `memo_time()` returns device, corrected (the stick's clock was more than
  60 s off, going by `X-Device-Now`), or received (the clock wasn't set). It's
  shown as `time_source:` in the note.
- The DB schema migrates itself (new columns added on startup).
- faster-whisper 1.1.1 needs `requests` and `huggingface-hub<1`. PyAV 19 broke
  its decoder, so `load_audio()` reads WAVs directly.

## Decisions (Jay, 2026-09-30)

- **Wi-Fi, not BLE**: the S3 has BLE only, and BLE would need a custom Android
  app kept alive on Samsung.
- **Reach home through Pangolin**, not the phone's Tailscale: non-rooted
  Android doesn't route hotspot clients through the VPN.
- **Transcribe on the MS-01**, not the phone.
- **One note per memo in Obsidian**, with the audio embedded.
- The hotspot is 2.4 GHz only (the ESP32 can't do 5 GHz) and listed first in
  `secrets.h`; Jay mostly records away from home.
- **Press, release, press and hold** to record (not a side-button lock). The
  side button uses the same gesture.
- Max memo 60 s.
- Battery: 2+ days per charge was the bar. With L3B off in sleep it's weeks
  (about 1 % per quiet 6 h, from HA's battery history).

## Hardware facts

From the sticker, docs.m5stack.com/en/core/StickS3, the M5PM1 datasheet v1.9
and schematic V0.6.

- SoC: ESP32-S3-PICO-1-N8R8: 8 MB quad flash, 8 MB **octal** PSRAM
  (`qio_opi`). Native USB (G19/G20).
- Battery 250 mAh. M5Stack measured, at 4.2 V: power-off 14 µA, L1 52 µA,
  **L2 (ESP32 sleeping) 102 µA**, L3A (running) 36.7 mA.
- **KEY1 = G11** (front), **KEY2 = G12** (side): both active-low and
  RTC-capable. The separate **PMIC power button**: single click = power on or
  reset, double = off, long press (2 s) = download mode (green LED blinks).
- **M5PM1** at I2C 0x6E on SDA G47 / SCL G48 (100 kHz).
  - PM1 GPIO2 = L3B rail (LCD + ES8311); GPIO3 = speaker amp; GPIO0 = charge
    status.
  - Registers: 0x05 WAKE_SRC, 0x06 PWR_CFG (bit 4 LED_EN), 0x09 I2C idle sleep
    (keep 0), 0x0C SYS_CMD, 0x11 GPIO_OUT, 0x48/0x49 button status/config, and
    0xA0–0xBF 32 B RAM that survives ESP32 power-off.
- **ES8311** codec at 0x18 and a MEMS mic. I2S MCLK G18, BCLK G17, LRCK G15,
  DIN G16, DOUT G14. The mic register sequence is copied from M5Unified's
  `_microphone_enabled_cb_sticks3`. Speaker and mic share the codec.
- LCD ST7789P3 135×240: MOSI G39, SCK G40, DC G45, CS G41, RST G21, BL G38.
- Toolchain: `espressif32@6.9.0` (Arduino core 2.0.x on IDF 4.4), board
  `esp32-s3-devkitc-1` with 8 MB / `qio_opi`; M5Unified 0.2.24, M5GFX 0.2.31.
  The core's sdkconfig has app rollback enabled
  (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`), but `initArduino()` marks images
  valid unless `verifyRollbackLater()` is overridden. Deep-sleep wakes already
  skip image validation.

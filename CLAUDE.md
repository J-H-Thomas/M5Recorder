# M5Recorder — project handoff notes

Push-to-talk voice memo recorder on an **M5StickS3**. Hold the button → record →
release → upload → transcribe on the home server (Whisper) → Obsidian notes.
The user (Jay) works locally with the device on USB (COM port changes between
normal and download mode; find it by USB VID 303A).

```
stick (KEY1 hold) ─► WAV queue in LittleFS ─► Wi-Fi (home, else S25+ hotspot)
  ─► HTTPS POST https://memos.<domain>/upload (Bearer token)
  ─► Pangolin (Newt tunnel, no router ports) ─► MS-01 (Unraid) receiver container
  ─► faster-whisper ─► <vault>/Memos/<date time>.md with the audio embedded
```

## Layout

- `firmware/recorder/`: the real firmware (fast-wake recording, flash queue,
  Wi-Fi + HTTPS upload, 15 min retry timer). `include/secrets.h` is gitignored
  (copy `secrets.example.h`). See its README.
- `server/`: receiver (FastAPI + faster-whisper, Docker). `pytest` in `server/`
  (venv in `server/.venv`). See its README for Unraid and Pangolin setup.
- `firmware/wake_test/`: the wake-latency experiment the recorder is based on.

## Status

- **Receiver (2026-09-30):** 10 tests pass. Run locally end to end with the
  `base` model: a TTS test memo was transcribed word for word; the retry-on-restart
  and duplicate-id paths were checked live.
- **Deployed on the MS-01 (2026-09-30, by a Claude session on the server):**
  - Container `m5recorder-receiver` (Compose Manager project `m5recorder`), host
    port 8090, user 99:100, large-v3-turbo on CPU int8 (no GPU).
  - Source is in /mnt/user/appdata/m5recorder/src (rebuild with
    `docker build -t m5recorder-receiver .../src/server`, then recreate the
    container through the compose project). Data and models are in
    /mnt/user/appdata/m5recorder/{data,models}. The token is in the project's
    `.env` (root only).
  - Vault /mnt/user/Obsidian/Memex (new; notes in Memos/, audio in Memos/audio/).
    **Not synced to any device yet** (Jay's decision).
  - Pangolin resource "m5-memos" → http://10.10.1.201:8090. Public name
    **https://memos.asdf.ac**. Login is on, with bypass rules for `/upload` and
    `/health` only. From outside: health ok, no token → 401, and a test upload
    was queued. The ids deploy-test-1 and deploy-test-2 are used.
  - **TLS:** Let's Encrypt "Gen Y" chain: leaf ← YR1 ← ISRG Root YR (sent
    cross-signed by ISRG Root X1). It verifies with X1 alone, and with Root YR
    alone. The server accepts TLS 1.2 with ECDHE-RSA-AES-GCM, which ESP-IDF 4.4's
    mbedTLS needs (it has no TLS 1.3). `ca_certs.h` holds X1, X2, YR and YE.
- **Recorder firmware: flashed and working end to end (2026-09-30)**, with
  `secrets.h` filled in by Jay (gitignored):
  - Memo 1 over the **phone hotspot** (Jay was away from home): note written with
    a correct transcript. Transcribed about 5 s after upload for 4.8 s of audio.
  - **Offline queue:** memos 2–4 recorded without Wi-Fi, then sent as one batch
    over one keep-alive connection when the hotspot came back. The notes carry
    the recording time (the clock was set by NTP earlier and survives deep
    sleep). All transcripts correct.
  - Transcription is about 0.7× the audio length on the MS-01 CPU; the VAD trims
    about 1 s of silence per clip.
  - **Not yet tested: home Wi-Fi.**
- **Known slowness (next work):** each upload takes about 7 s for 160–200 KB
  (about 25–30 KB/s). The likely cause is Wi-Fi modem sleep (on by default) plus
  small writes. Away from home, the stick also spends about 8 s trying the home
  SSID before the hotspot. Planned fix: scan first and join a known network in
  range, turn Wi-Fi sleep off during uploads, write in bigger chunks, and log
  the upload rate.
- **KEY2 status screen (2026-09-30, Jay's request):** KEY2 (G12) wakes the
  stick via ext1 (alongside ext0 on KEY1) and shows battery % (M5Unified's
  voltage-based estimate via the M5PM1, plus volts and charging state),
  storage free % and minutes left, and the queue count. "BATTERY LOW" (≤20%)
  appears after saving.
- **Wi-Fi joining and faster uploads (2026-09-30):** a scan-first version saw
  "0 networks" in the field. The cause: the Arduino core ends an async scan after
  20x max_ms_per_chan (2.4 s at 120 ms) and `scanComplete()` then reports
  WIFI_SCAN_FAILED. Replaced with join-by-name, trying the last network that
  worked first (RTC memory), then `WIFI_NETWORKS` order (phone hotspot
  first, home Wi-Fi second, at Jay's request, since it's mostly used away
  from home). It moves on early on WL_NO_SSID_AVAIL / WL_CONNECT_FAILED. Wi-Fi
  sleep is off, and each memo is read into PSRAM and sent in one write (a File
  stream went out in 1460-byte TLS records). The Sent screen and serial log
  show KB/s. Jay's hotspot is now 2.4 GHz only (it was mixed 2.4/5).
  - One field failure: a memo about 7 s after the previous one timed out joining
    the hotspot (not refused, not "not found"). Since then, a network that
    timed out gets one 12 s retry after a Wi-Fi reset, and the driver's
    disconnect reasons are logged.
  - **Field test after that (2026-09-30): 4 memos, 13–18 s apart, all joined
    the hotspot in about 1 s at the first attempt** (no retry needed), and
    uploaded in 3.3–4.4 s each (97–194 KB). Most of that is the TLS handshake:
    one per wake, and the server has a 4096-bit RSA key. Within one connection,
    later memos reach about 60 KB/s. A memo reaches the server about 5 s after
    release (it was about 15 s). Reusing the TLS session across sleeps could cut
    this further; not done. Reason 8 (ASSOC_LEAVE) in the log is the stick's
    own disconnect before sleep.
  - The KEY2 status screen works on the device (Jay, 2026-09-30).
- Other ideas: hide the Docker health-check lines in the receiver's access log.
  Jay still has to set up sync for the Memex vault.

- **Field fixes (2026-09-30, after an evening in Jay's pocket):**
  - **Record gesture is now press, release, press and hold** (Jay's choice).
    Capture starts at the first press; `detectGesture()` in main.cpp checks it
    before `M5.begin()` (first press released within 600 ms of app start,
    second within 600 ms of that, 30 ms debounce). A rejected press sleeps
    without lighting the screen and counts toward "Ignored presses" on the
    status screen. The memo keeps 0.25 s of pre-roll before the second press.
    Max 60 s. Limit: a very fast double press whose second press is already
    down at app start is rejected. Tune from the `gesture:` serial log.
  - **The side button is guarded the same way** (Jay, 2026-09-30): press,
    release, press opens the status screen (no hold needed). A rejected KEY2
    press sleeps at once and counts as ignored. Before, a pocket press lit the
    screen for 5 s (about 0.08 mAh, roughly 8x a rejected KEY1 press).
    Tested on the device by Jay (2026-09-30): works.
  - **Held buttons**: `deepSleep()` arms ext0/ext1 to wake on *release* for a
    button that's held (an RTC flag says so), and that wake sleeps again at once.
    Before this, `sleepNow()` spun awake until release.
  - **Hotspot with no internet → stuck on "Sending..."**: the core defaults
    are a 120 s TLS handshake and 30 s reads. Now 10 s / 15 s, 8 s connect, 15 s
    HTTP, plus an esp_timer watchdog per request (20 s + size/20 KB/s) that calls
    `uploadStuck()` → deep sleep. HTTPClient codes < 0 show "No internet".
    Retry backoff 15/30/60/120 min in RTC memory, reset on success or on a new
    memo.
  - **Tested on the device (2026-09-30):** single taps, press-and-hold and a 30 s
    hold didn't record; the gesture recorded every time. Jay's timings: first
    release 120–143 ms after app start, second press 100–150 ms after that
    (the 600 ms windows have plenty of margin). Hotspot with no data: the
    connect failed at 8 s → "No internet", 15 min retry; with data back, the
    queue (including the old pocket memos) sent in one batch.

## Wake-latency experiment (history)

- `firmware/wake_test/` — PlatformIO test firmware that measures how much speech is
  lost between pressing KEY1 and the mic delivering real audio after deep sleep.
  See its README for how to run it and which numbers to collect.
- **First version, run on hardware 2026-09-30** (4 s holds, counting aloud):
  - L3B off in sleep: setup 51, M5.begin 459, Mic.begin 516 ms; then the ES8311 sent
    exact zeros for **994 ms**. Playback started at "five" ("one"–"four" lost).
  - L3B on in sleep: same timings, audio live at 0 ms after mic start. Only "one" lost.
  - So the mic works, keeping L3B on removes the codec warm-up, and the remaining
    loss is start-up time, mostly `M5.begin()` (~408 ms).
- **Current version (branch `fast-wake`)**: L3B on in sleep by default; on a KEY1
  wake it writes the ES8311 registers and starts I2S + a capture task *before*
  `M5.begin()`, so the display comes up while it's already recording. Adds a
  `gap` figure (longest zero run after audio starts) to catch codec resets or
  overruns. **Run on hardware 2026-09-30:** setup 52, mic **55** ms (was 516),
  M5.begin 463 ms (now after the mic), audio +0, speech +0, gap 0. "One" is caught
  most times. speech +0 means the user was already talking at the first sample,
  so the remaining misses come from ROM + bootloader + Arduino start-up before
  `setup()`, which the app can't see.
- "One" is now clipped only sometimes (Jay, 2026-09-30); good enough for now.
- Parked: the sleep-current cost of keeping L3B on (Jay: 2+ days of battery is
  fine; optimise once it works). Cutting boot time before `setup()` (bootloader
  image check on wake, PSRAM memtest, log level) needs a custom sdkconfig, e.g.
  pioarduino or Arduino as an ESP-IDF component.

## Decisions so far

- **Transport (2026-09-30): Wi-Fi to home or the phone hotspot, not BLE.** The
  ESP32-S3 has BLE only (no Classic BT). BLE would need a custom Android app kept
  alive in the background on Samsung, and it's slow.
- **Reaching home: Jay's existing Pangolin tunnel.** Not the phone's Tailscale:
  on non-rooted Android, hotspot clients' traffic doesn't go through the VPN
  (tailscale issues #14980, #15114). Pangolin login is bypassed for `/upload`
  and `/health`; the receiver checks a bearer token (at least 24 characters).
- **Transcription on the MS-01, not the phone** (faster-whisper; CPU int8 by
  default; it's unknown whether the MS-01 has a GPU). The phone is only the network.
- **Notes go into Jay's Obsidian vault**, one per memo, with the audio embedded
  (vault path on Unraid still to be given).
- Battery: 2+ days per charge is acceptable for now.

- **Wake approach: ESP32 deep sleep, ext0 wake on KEY1 (G11, front button).**
  KEY1 cannot power the device on from full power-off — only the separate PMIC
  power button can (see the schematic power-mode notes). Full-off standby saves only about
  2 mAh/day, so it isn't worth using the small side power button to record.
- Record to flash/PSRAM first, upload afterwards over Wi-Fi to a small server on
  the user's computer; queue memos when Wi-Fi is unavailable. No live streaming.
- Ignore presses under about 0.5 s. Keep the LCD backlight off while recording
  (deferred: the red REC screen stays on for now to help testing).

## Hardware facts (from the sticker, docs.m5stack.com/en/core/StickS3, M5PM1 datasheet v1.9, schematic V0.6)

- SoC: ESP32-S3-PICO-1-N8R8 — 8 MB quad flash, 8 MB **octal** PSRAM (`qio_opi`).
  Native USB (G19/G20).
- Battery 250 mAh. Measured by M5Stack at 4.2 V: power-off 14 µA, L1 52 µA,
  **L2 (ESP32 sleeping) 102 µA**, L3A (running) 36.7 mA.
- Buttons: **KEY1 = G11** (front), **KEY2 = G12** (side), both active-low, both
  RTC-capable. A separate **PMIC power button**: single click = power on / reset,
  double = off, long press (2 s default) = download mode (green LED blinks).
- PMIC **M5PM1** at I2C 0x6E on the internal bus SDA=G47 / SCL=G48 (100 kHz).
  PM1 GPIO2 = L3B rail enable (LCD + ES8311 LDO); GPIO3 = speaker amp enable;
  GPIO0 = charge status; GPIO4 = IMU interrupt. Key registers: 0x05 WAKE_SRC,
  0x06 PWR_CFG, 0x09 I2C idle sleep (keep 0), 0x0C SYS_CMD (0xA1 off, 0xA2 restart,
  0xA3 download), 0x11 GPIO_OUT, 0x38–0x3C wake/power-on timer, 0x48 BTN_Status,
  0x49 BTN_CFG (bit7 DL_LOCK, bit0 SINGLE_RESET_DIS), 0x4A double-click-off disable,
  0xA0–0xBF 32 B RAM that survives ESP32 power-off.
- Audio: ES8311 codec (I2C 0x18) + MEMS mic (65 dB SNR). I2S MCLK G18, BCLK G17,
  LRCK G15, DIN (mic→ESP) G16, DOUT G14. AW8737 speaker amp. Speaker and mic share
  the codec/I2S, so call `Speaker.end()` before `Mic.begin()` and the other way round.
  Keep speaker volume under ~75% on battery (brown-out resets).
- **Known risk:** M5Unified says the ES8311 may output all-zero samples for about 1 s
  after power-up. The wake test measures this (`audio +` value). One possible fix
  is to keep L3B powered in sleep (KEY2 toggle in the test).
- LCD ST7789P3 135×240: MOSI G39, SCK G40, DC G45, CS G41, RST G21, BL G38.
- IMU BMI270 (0x68). IR TX G46 / RX G42. Grove Port A G9/G10.

## Software notes

- PlatformIO, `espressif32@6.9.0` (Arduino core 2.0.x), board `esp32-s3-devkitc-1`
  with 8 MB / `qio_opi` overrides; M5Unified 0.2.24 + M5GFX 0.2.31 both support
  StickS3 (autodetected through the PM1). USB CDC on boot is enabled.
- M5GFX's StickS3 autodetect turns on L3B and has ~200 ms of fixed delays, so if
  `M5.begin()` is slow on wake, one fix is to bypass it on the wake path.
- M5Unified's `deepSleep()` doesn't set up a wake pin for StickS3; the test uses
  `esp_sleep_enable_ext0_wakeup(G11, 0)` plus an RTC pull-up directly.

## Battery estimate (30 memos/day, 10–20 s)

About 0.3 mAh per memo (≈45 mA recording + a Wi-Fi upload) ≈ 9 mAh/day, plus
~2.5 mAh/day sleep → **about 2½ weeks per charge** (≈200 mAh usable). Recording
and upload dominate; failed Wi-Fi retries are the main thing to avoid.

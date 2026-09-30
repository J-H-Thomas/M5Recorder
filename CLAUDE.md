# M5Recorder — project handoff notes

Push-to-talk voice memo recorder on an **M5StickS3**. Hold the button → record →
release → upload → transcribe on a computer (Whisper) → Markdown notes.
The user is now working locally with the device on USB.

## Status

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
  overruns. Builds cleanly; **not yet run on hardware**.
- Next: run the fast version (a few 4 s holds), check "one" survives, then measure
  the sleep-current cost of keeping L3B on.
- After that: build the real recorder firmware (step 1) and the computer-side
  receiver + Whisper (step 2). A phone link is deferred.

## Decisions so far

- **Wake approach: ESP32 deep sleep, ext0 wake on KEY1 (G11, front button).**
  KEY1 cannot power the device on from full power-off — only the separate PMIC
  power button can (see the schematic power-mode notes). Full-off standby saves only about
  2 mAh/day, so it isn't worth using the small side power button to record.
- Record to flash/PSRAM first, upload afterwards over Wi-Fi to a small server on
  the user's computer; queue memos when Wi-Fi is unavailable. No live streaming.
- Transcription: local Whisper is the default suggestion. The user hasn't
  confirmed this vs. a cloud service yet — ask.
- Ignore presses under about 0.5 s. Keep the LCD backlight off while recording.

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

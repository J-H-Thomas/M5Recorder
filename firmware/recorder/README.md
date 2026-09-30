# Recorder firmware (M5StickS3)

Push-to-talk memos: **hold the front button (KEY1), talk, let go.** The memo
is saved to flash, then uploaded over Wi-Fi to the receiver (`server/`), which
turns it into a transcribed note in Obsidian.

## Set up

1. Copy `include/secrets.example.h` to `include/secrets.h` (gitignored) and fill in:
   - `WIFI_NETWORKS`: home Wi-Fi first, then the phone hotspot. The ESP32 only
     does **2.4 GHz**. On the Samsung hotspot, set the band to 2.4 GHz (or a
     setting that includes it), security to WPA2, and "Turn off hotspot
     automatically" to **Never**, otherwise it switches off between memos.
   - `UPLOAD_URL`: e.g. `https://memos.<domain>/upload` through Pangolin.
   - `MEMO_TOKEN`: the same value as the receiver's `MEMO_TOKEN`.
2. Flash: `pio run -t upload`, and optionally `pio device monitor` for logs.
   If the port doesn't show up, hold the side power button about 2 s until the
   green LED blinks (download mode), flash, then single-click it to restart.

## Use

| Screen | Meaning |
|---|---|
| red **REC** | Recording; let go to stop. Up to 2 minutes. |
| **Saved** 12.3 s | Stored in flash, now uploading. |
| **Sending...** 1 of 3 | Uploading the queue, oldest first. |
| **Sent** | All memos delivered; the stick sleeps. |
| **No Wi-Fi** | Kept in the queue; it retries every 15 minutes and after the next memo. |
| **Upload failed** HTTP n | Reached the network but not the receiver (or it errored); kept in the queue. |
| **Bad token** | The receiver rejected `MEMO_TOKEN`; memos are kept. |
| **Queue full** | About 2.8 minutes of audio are waiting; new memos can't be saved until they're sent. |

- Taps shorter than 0.5 s are ignored.
- Pressing the button while it's uploading stops the upload and starts a new
  recording straight away.
- Powering on (side button) shows the queue size and the device id, then
  uploads anything waiting.

## How it works

- `audio.cpp`: the fast-wake path from `firmware/wake_test` (see the notes in
  `CLAUDE.md`). It keeps the codec rail (L3B) on through sleep, and on a KEY1
  wake sets up the ES8311 and I2S before `M5.begin()`. A capture task records
  into PSRAM and stops itself when KEY1 is released.
- `memo_queue.cpp`: WAV files in LittleFS, `/q/<seq>_<unix time>.wav`, written
  as `.part` and renamed, so a crash never leaves half a memo in the queue.
  The sequence number is kept in NVS.
- `uploader.cpp`: tries each network (8 s each), syncs the clock over NTP if
  it isn't set (the clock keeps running through deep sleep), then POSTs each
  memo over HTTPS with the Let's Encrypt roots in `ca_certs.h`. It deletes a
  memo only after a 2xx. The memo id is `<MAC>-<seq>`, so a re-send after a
  lost reply isn't duplicated.
- Partitions (`partitions.csv`): 2.5 MB app, about 5.4 MB LittleFS queue.

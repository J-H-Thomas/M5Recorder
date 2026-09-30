# Recorder firmware (M5StickS3)

Voice memos: **press the front button (KEY1), release, then press and hold it
while you talk; let go to stop.** A single press, or a single press held down
(e.g. in a pocket), does nothing. The memo
is saved to flash, then uploaded over Wi-Fi to the receiver (`server/`), which
turns it into a transcribed note in Obsidian.

## Set up

1. Copy `include/secrets.example.h` to `include/secrets.h` (gitignored) and fill in:
   - `WIFI_NETWORKS`: the phone hotspot first, then home Wi-Fi. The network that
     worked last time is always tried first, then the rest in this order. The ESP32 only
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
| red **REC** | Recording; let go to stop. Up to 60 seconds (it stops itself). |
| **Saved** 12.3 s | Stored in flash, now uploading. |
| **Sending...** 1 of 3 | Uploading the queue, oldest first. |
| **Sent** n memos, x KB/s | All memos delivered (with the upload speed); the stick sleeps. |
| **No Wi-Fi** | Kept in the queue; retried after the next memo, and on a timer (15, 30, 60, then every 120 min while it keeps failing). |
| **No internet** via (network) | Joined the network (e.g. the hotspot) but couldn't reach the server; kept, same retries. |
| **Upload failed** HTTP n | Reached the network but not the receiver (or it errored); kept in the queue. |
| **Bad token** | The receiver rejected `MEMO_TOKEN`; memos are kept. |
| **Queue full** | About 2.8 minutes of audio are waiting; new memos can't be saved until they're sent. |

- **Side button (KEY2): press, release, press** for the status screen (5 s;
  press again to close). A single press does nothing, so it can't light the
  screen in a pocket either. The status screen shows:
  - Battery % with its voltage and whether it's charging. The % is estimated
    from voltage (the StickS3 has no fuel gauge), so treat it as approximate;
    it reads high while charging.
  - Storage free % and how many minutes of audio still fit.
  - How many memos are waiting to send.

  Pressing the front button while it's showing starts a recording.
- "BATTERY LOW" appears under **Saved** when the battery is at 20% or less.
- The recording keeps about 0.25 s from before the second press (the mic starts
  on the first press), so the first word isn't clipped. Memos under 0.5 s are
  ignored.
- Presses that aren't the record gesture send the stick straight back to sleep
  with the screen off; the status screen shows how many were ignored. If a button
  is held down (in a pocket), the stick sleeps until it's released rather than
  staying awake.
- Pressing the front button while it's uploading stops the upload and starts the
  record gesture (release, then press and hold) for a new
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
- `uploader.cpp`: joins networks by name, last-good first. A network that isn't
  there is reported by the Wi-Fi driver after its own scan (a couple of seconds),
  so it moves on without waiting out the 8 s timeout. Wi-Fi power-save is off. It syncs the clock over NTP if
  it isn't set (the clock keeps running through deep sleep), then POSTs each
  memo over HTTPS (read into PSRAM and sent in one write) with the Let's Encrypt
  roots in `ca_certs.h`. It deletes a
  memo only after a 2xx. The memo id is `<MAC>-<seq>`, so a re-send after a
  lost reply isn't duplicated.
- Partitions (`partitions.csv`): 2.5 MB app, about 5.4 MB LittleFS queue.

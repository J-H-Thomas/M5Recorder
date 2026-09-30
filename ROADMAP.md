# M5Recorder roadmap

Status on 2026-09-30: the chain works end to end. The stick records (press,
release, press and hold), joins the phone hotspot in about 1 s, and reaches the
receiver about 5 s after release. The MS-01 transcribes (large-v3-turbo) into
the Memex vault, which syncs to the phone within seconds.

This roadmap was re-ordered after three independent reviews (firmware, server,
whole system) on 2026-09-30. **Data safety comes first**: the reviews found
several ways memos can be lost silently. OTA comes last and uses the app
rollback that the Arduino core's bootloader already supports.

## Phase 0: field fixes (done 2026-09-30)

After carrying the stick for an evening:
- **Pocket presses**: recording needs press, release, press and hold; the
  status screen needs press, release, press. Other presses sleep again at once
  without lighting the screen. Memos are capped at 60 s. A held button no
  longer keeps the stick awake (it sleeps until the release).
- **Hotspot with no internet stuck on "Sending..."**: TLS/HTTP timeouts are cut
  from the core's 120 s handshake / 30 s reads to 10–15 s, with a per-request
  watchdog that forces sleep. The screen shows **No internet**, and retries
  back off 15 → 30 → 60 → 120 min.
- **Green LED always on**: PM1 LED_EN is now cleared (it probably drew more
  than the whole sleeping stick).

## Phase 1: data safety (small, do first)

**Done 2026-09-30** (branch `phase1-data-safety`).
- The receiver is deployed on the MS-01 (the DB migrated itself; 20 tests).
- The stick is flashed and tested by Jay:
  - new ids (`<MAC>-<epoch>-<seq>`);
  - deletion only on a `queued` reply;
  - a placeholder note, then the transcript;
  - DNS failure → "No internet" with a 15 min retry that fired on its own
    despite pocket presses in between;
  - NTP setting an unset clock.
- **Left for when Jay is home:** deliberately trigger the upload watchdog (a
  test server that accepts the connection and then stalls).

1. **Unique memo ids** (all three reviews). The id `<MAC>-<seq>` restarts
   if NVS is wiped (full flash erase, or the Arduino core erasing NVS on
   NO_FREE_PAGES / NEW_VERSION_FOUND). The server then answers "duplicate" and
   the stick deletes new memos. Fix:
   - Stick: add a random 32-bit epoch, kept in NVS and regenerated whenever the
     seq is missing: `<MAC>-<epoch>-<seq>`.
   - Server: store the body's size and sha256; a known id with different
     content → **409**. The stick keeps the memo on 409 and deletes only on a
     2xx whose body says `queued` or `duplicate`.
2. **A bad memo mustn't block the queue.** A 400 or 413 is retried forever and
   blocks every later memo. Fix:
   - Stick: on a 4xx other than 401/408/429, rename to `.bad` (kept, out of
     the queue) and carry on.
   - Server: accept only 16 kHz mono 16-bit WAV; compute the duration from the
     bytes received, not the header.
3. **Transcription failures must be visible and retried.** Today a failed memo
   gets no note, is retried only at container restart, and the worker thread can
   die silently while `/health` says ok. Fix:
   - Write a placeholder note at upload time ("transcribing…", with the audio
     embedded) and fill in the transcript later.
   - Catch everything in the worker loop.
   - Retry failed memos on a timer with backoff.
   - After N attempts, write "transcription failed" with the audio embedded.
   - `/health` reports worker liveness and failed/unfinished counts.
4. **The stick mustn't destroy memos itself.**
   - Don't delete a memo when opening, allocating or reading it fails (only if
     it's shorter than a WAV header).
   - Don't format LittleFS on the first mount failure: show the error; format
     only as a last resort.
   - Clean `.part` strays in `memo_queue::begin()`.
5. **Retry timer from an absolute deadline.** Every short wake (ignored press,
   button release, status screen) currently re-arms the full interval, so at
   120 min a pocket press every hour means it never fires. Keep the deadline
   in RTC memory and arm the timer with the time remaining. Needed before the
   Phase 2 heartbeat.
6. **Clock accuracy.** The sleep clock is the internal RC oscillator
   (`CONFIG_ESP32S3_RTC_CLK_SRC_INT_RC`), and NTP runs only while the clock is
   unset, so memo times drift over weeks. Fix:
   - Sync NTP on every upload round.
   - Send `X-Device-Now` so the server can correct `X-Memo-Time` by the offset.
   - Add `time_source:` (device / corrected / received) to the note frontmatter.
7. **Small firmware fixes**:
   - A memo that hits the 60 s cap while KEY1 is still held must wait for the
     release before uploading (today it counts as "interrupted" and waits
     15+ min).
   - Show a certificate/TLS failure as its own error, not "No internet"
     (check `tls.lastError()`).
   - The upload watchdog must also power down the codec and the LCD. Drop
     `esp_wifi_stop()` from the timer callback; deep sleep powers the RF down.
   - Force the watchdog to fire once in a test (a server that accepts TCP and
     then stalls).
8. **Docs and repo cleanup**:
   - Bring `CLAUDE.md` up to date (stale lines: hold-to-record, "vault not
     synced", fixed 15 min retry, `fast-wake` branch, GPU unknown, "ignore
     presses under 0.5 s", resolved risks).
   - Remove the server's LAN address from `CLAUDE.md`.
   - Use a placeholder device id in the server README example.
   - Fix the "side button" vs power button wording.
   - `.gitignore`/`.dockerignore`: add `server/data/`, `server/vault/`, `.env`
     and `*.wav`.
   - Add `.gitattributes` (LF) to end the CRLF warnings.

## Phase 2: know that it's working

1. **Battery telemetry.** Stick:
   - Read the battery at wake, before Wi-Fi (TX sags the voltage).
   - Send `X-Battery-mV`, `X-Battery-Pct`, `X-Charging`, `X-Queue` and
     `X-Firmware` on every upload.
   - Add a heartbeat every 6 h (using the Phase 1 absolute deadlines).

   Server:
   - A `battery` table and `POST /heartbeat`.
   - A status note in the vault (`Memos/_M5Recorder status.md`): last seen,
     battery, charging, queue, firmware, memos today, estimated days left.
   - After 3–5 days of data, report the real drain and days per charge.
2. **Push alerts** (the status note stops updating exactly when things break).
   Via Home Assistant (already running) or ntfy, for:
   - no heartbeat for 24 h;
   - a transcription that failed after its retries;
   - `unfinished` > 0 for over 1 h;
   - the public `/health` failing (an external uptime check).
3. **Low-battery guard.** Skip Wi-Fi below ~3.4–3.5 V, or after a brownout,
   panic or watchdog reset, and sleep on a long timer (avoids a brownout loop
   on a flat cell). Show "Battery low, charge me" on a press.
4. **Queue nearly full warning** on the Saved screen (e.g. under 30 s left).
5. **Quieter logs**: drop the Docker health-check access lines (after Phase 1.3,
   so `/health` means something).
6. **Home Wi-Fi test** (the only path not yet tested): a memo at home, then
   walking out to the hotspot and back.

## Phase 3: storage and security (one USB flash)

1. **Compress audio on the stick (IMA-ADPCM, 4×)** so the queue holds about
   9–11 min instead of about 2.8 (OTA's partitions shrink it further). The
   server decodes to PCM for Whisper and stores Opus/M4A in the vault: WAV is
   about 5 GB/year synced to the phone, and phones don't play ADPCM WAV.
2. **OTA partition layout**, in the same flash:
   - Two app slots and a smaller LittleFS.
   - **NVS stays at 0x9000; no full erase** (safe anyway after Phase 1.1).
   - Send the queue before flashing.
3. **Secrets out of the firmware.**
   - Wi-Fi networks and the token go in NVS, set over USB serial (a small
     provisioning command). This keeps secrets out of OTA images and makes
     Wi-Fi or token changes cheap.
   - The server accepts several tokens (`MEMO_TOKENS`) so they can be rotated.
   - Consider putting the stick on an IoT/guest SSID (a lost stick still holds
     its credentials).
4. **Pin every Python dependency** (a lock or constraints file) before any
   automated image build: the PyAV 19 break came from an unpinned transitive
   dependency.
5. **Server hardening**:
   - Disable `openapi_url`.
   - Validate or quote the device header in the YAML.
   - Add `USER 99:100` to the image, plus CPU and memory limits for the
     container.
   - Log 401s (with X-Forwarded-For).
   - A per-token daily byte quota.
   - A random temp-file name per upload.
   - Insert the DB row before the final rename, and sweep orphans at startup.
6. **Backups**: a nightly `VACUUM INTO` of `memos.db`, and the vault included in
   the Unraid backup.

## Phase 4: firmware updates over Wi-Fi (OTA)

- **Rollback**: the core's bootloader has `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`,
  but `initArduino()` marks every new image valid at boot. Override
  `verifyRollbackLater()`, and call `esp_ota_mark_app_valid_cancel_rollback()`
  after the first successful upload or heartbeat, **before the first deep
  sleep** (every wake is a reboot, and a still-pending image is rolled back).
- **Receiver**:
  - Firmware manifest (`version`, `size`, `sha256`) and binary endpoints.
  - **Not** on the public Pangolin bypass for publishing: `POST /firmware` is
    LAN or SSO only, with a separate admin token.
  - Optional: sign the manifest (ECDSA, key on the PC) so the server alone can't
    push code.
- **Stick**:
  - Check after an upload round or heartbeat (no button trigger; a KEY2 long
    press would clash with "press to close" on the status screen).
  - Install only with battery ≥ 30% or charging.
  - Verify size and hash.
- **CA bundle**: OTA rides on the same TLS; plan how `ca_certs.h` gets
  updated before Let's Encrypt changes roots again.
- `tools/publish_firmware.py` builds and publishes (no secrets in the image
  after Phase 3.3).

## Demoted / later

- **Prebuilt GHCR image via GitHub Actions**: for one server, a
  `git pull && docker build && compose up -d` script is enough. If CI is added,
  pin dependencies first and don't auto-update Unraid to `:latest`.
- Screen off while recording; catching "one" every time (boot-time work; the
  deep-sleep wake already skips image validation); smarter notes (title,
  summary, tags); links into daily notes; TLS session reuse for faster uploads.

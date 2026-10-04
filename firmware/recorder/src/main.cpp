// M5Recorder: push-to-talk voice memos on an M5StickS3.
//
// To record: press the front button (KEY1), release, then press and hold it
// while talking; let go to stop. A single press, or a single press held down
// (e.g. in a pocket), does nothing; the side button (KEY2) needs press,
// release, press to show the status screen. The memo is saved to flash, then uploaded
// over Wi-Fi (phone hotspot or home network) to the receiver on the home
// server, which transcribes it into an Obsidian note. Memos that can't be sent
// stay queued; the stick wakes on a timer to retry (15, 30, 60, then every
// 120 min while it keeps failing).
//
// Wake paths:
//   KEY1 press (ext0)   gesture check -> record -> save -> upload -> sleep
//   KEY2 press (ext1)   gesture check -> status screen (battery, storage, queue) -> sleep
//   button release      (only if a button was held at sleep) straight back to sleep
//   timer               upload queued memos with the screen off -> sleep
//   power on            show status, upload anything queued -> sleep
//
// Recording starts before M5.begin() (see audio.cpp): the mic runs from the
// first press, so the memo keeps a little audio from before the second press.

#include <M5Unified.h>
#include <Preferences.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <esp_wifi.h>

#include "audio.h"
#include "memo_queue.h"
#include "uploader.h"

namespace {

constexpr gpio_num_t PIN_KEY1   = GPIO_NUM_11;  // front: record
constexpr gpio_num_t PIN_KEY2   = GPIO_NUM_12;  // side: status screen
constexpr gpio_num_t PIN_LCD_BL = GPIO_NUM_38;

constexpr uint8_t  PM1_ADDR    = 0x6E;
constexpr uint32_t PM1_FREQ    = 100000;
constexpr uint8_t  PM1_PWR_CFG = 0x06;
constexpr uint8_t  PM1_LED_EN  = 1 << 4;  // green LED

// Record gesture: press, release, press and hold.
constexpr uint32_t TAP_MAX_MS   = 600;  // first press must be released by then (ms since app start)
constexpr uint32_t GAP_MAX_MS   = 600;  // second press must start within this of the release
constexpr uint32_t DEBOUNCE_MS  = 30;   // a level must hold this long to count
constexpr size_t   PRE_ROLL     = audio::SAMPLE_RATE / 4;  // audio kept from before the second press

constexpr size_t   MIN_SAMPLES     = audio::SAMPLE_RATE / 2;  // memos under 0.5 s are ignored
constexpr uint32_t RETRY_MINUTES   = 15;   // first retry; doubles per failure up to MAX
constexpr uint32_t RETRY_MAX_MINUTES = 120;
constexpr uint32_t MESSAGE_MS      = 2000;
constexpr uint32_t STATUS_MS       = 5000;
constexpr int32_t  LOW_BATTERY_PCT = 20;
constexpr uint32_t BYTES_PER_SECOND = audio::SAMPLE_RATE * sizeof(int16_t);
constexpr uint8_t  BRIGHTNESS      = 80;
constexpr time_t   CLOCK_VALID_AFTER = 1700000000;  // Nov 2023
constexpr time_t   HEARTBEAT_S     = 6 * 3600;  // check in this often when there's nothing to upload
// Below this (and not charging), don't start Wi-Fi: its current peaks could
// brown the stick out, and a flat cell would loop resetting.
constexpr int16_t  LOW_BATTERY_MV  = 3450;
constexpr uint32_t FAULT_RETRY_MINUTES = 30;    // after a brownout or crash reset, wait before Wi-Fi
constexpr uint32_t QUEUE_WARN_SECONDS = 30;     // warn when less than this much audio still fits

// Kept through deep sleep.
RTC_DATA_ATTR bool     key1_wait_release = false;  // slept with KEY1 held: next ext0 wake is its release
RTC_DATA_ATTR bool     key2_wait_release = false;
RTC_DATA_ATTR uint32_t retry_minutes = RETRY_MINUTES;  // retry interval after the next failed round
// When the retry timer is due, on the time() clock (0 = no retry). Absolute, so
// short wakes (ignored presses, status screen) don't restart the wait.
RTC_DATA_ATTR time_t   retry_deadline = 0;
RTC_DATA_ATTR uint16_t ignored_presses = 0;        // rejected gestures since the status screen last showed them
RTC_DATA_ATTR uint32_t unreported_ignored = 0;     // rejected gestures not yet reported to the receiver
// When the next check-in is due (0 = set one on the next sleep).
RTC_DATA_ATTR time_t   heartbeat_deadline = 0;

// The stick's state, read after M5.begin() and before Wi-Fi (the radio's
// current draw pulls the battery voltage down).
uploader::Telemetry telemetry_now{0, -1, false, 0};

void readTelemetry() {
  telemetry_now.battery_mv = M5.Power.getBatteryVoltage();
  telemetry_now.battery_pct = M5.Power.getBatteryLevel();
  telemetry_now.charging = M5.Power.isCharging() == m5::Power_Class::is_charging;
  telemetry_now.ignored = unreported_ignored;
}

bool batteryTooLowForWifi() {
  return telemetry_now.battery_mv > 0 && telemetry_now.battery_mv < LOW_BATTERY_MV && !telemetry_now.charging;
}

// The receiver has the latest state: the next check-in is a full interval away.
void reported() {
  heartbeat_deadline = time(nullptr) + HEARTBEAT_S;
  unreported_ignored = 0;
}

void show(const char* title, const char* detail = nullptr, uint16_t bg = TFT_BLACK) {
  auto& d = M5.Display;
  d.fillScreen(bg);
  d.setTextColor(TFT_WHITE, bg);
  d.setTextSize(2);
  d.setCursor(4, 8);
  d.println(title);
  if (detail) {
    d.setTextSize(1);
    d.setCursor(4, d.getCursorY() + 6);
    d.println(detail);
  }
}

struct MemoNumber {
  uint32_t seq;
  uint32_t epoch;
};

// Memo ids are <MAC>-<epoch>-<seq>. The epoch is random and kept in NVS with
// the sequence number; if NVS is ever wiped (full flash erase, or the Arduino
// core resetting it) both start afresh, and the new epoch keeps the new ids
// from matching ones the receiver already has (it would call them duplicates
// and the stick would delete the memos).
MemoNumber nextMemoNumber() {
  Preferences prefs;
  prefs.begin("recorder", false);
  uint32_t epoch = prefs.getUInt("epoch", 0);
  if (epoch == 0) {
    epoch = esp_random() | 1;  // never 0 (0 means "queued before epochs")
    prefs.putUInt("epoch", epoch);
  }
  const uint32_t seq = prefs.getUInt("seq", 0) + 1;
  prefs.putUInt("seq", seq);
  prefs.end();
  return {seq, epoch};
}

void retryIn(uint32_t minutes) { retry_deadline = time(nullptr) + (time_t)minutes * 60; }

// Configures the wake sources and sleeps. Needs nothing initialised, so it's
// also used before M5.begin() and from the upload watchdog's timer task.
//
// A button that is held now would wake the stick straight back up, so it is
// armed to wake on its *release* instead (and that wake just sleeps again).
// key1_is_press: KEY1 is down because the user is starting the record gesture
// (pressed on the status screen or during an upload): wake at once on it.
// The retry timer, if any, is armed for whatever is left until retry_deadline.
[[noreturn]] void deepSleep(bool key1_is_press = false) {
  pinMode(PIN_LCD_BL, OUTPUT);  // keep the backlight off while the pads are unpowered
  digitalWrite(PIN_LCD_BL, LOW);
  gpio_hold_en(PIN_LCD_BL);
  gpio_deep_sleep_hold_en();

  const bool k1_held = digitalRead(PIN_KEY1) == LOW && !key1_is_press;
  const bool k2_held = digitalRead(PIN_KEY2) == LOW;
  key1_wait_release = k1_held;
  key2_wait_release = k2_held;

  rtc_gpio_pullup_en(PIN_KEY1);
  rtc_gpio_pulldown_dis(PIN_KEY1);
  esp_sleep_enable_ext0_wakeup(PIN_KEY1, k1_held ? 1 : 0);
  rtc_gpio_pullup_en(PIN_KEY2);
  rtc_gpio_pulldown_dis(PIN_KEY2);
  esp_sleep_enable_ext1_wakeup(1ULL << PIN_KEY2, k2_held ? ESP_EXT1_WAKEUP_ANY_HIGH : ESP_EXT1_WAKEUP_ANY_LOW);

  // Wake for whichever is due first: a retry of queued memos, or a check-in.
  const time_t now = time(nullptr);
  if (heartbeat_deadline == 0) { heartbeat_deadline = now + HEARTBEAT_S; }
  const time_t due = retry_deadline ? std::min(retry_deadline, heartbeat_deadline) : heartbeat_deadline;
  esp_sleep_enable_timer_wakeup((uint64_t)std::max<time_t>(due - now, 5) * 1000000ULL);
  esp_deep_sleep_start();
}

// Normal sleep after M5.begin(): Wi-Fi, codec and display off first.
[[noreturn]] void sleepNow(bool key1_is_press = false) {
  uploader::shutdown();
  audio::codecOff();
  M5.Display.sleep();
  M5.Display.waitDisplay();
  Serial.flush();
  deepSleep(key1_is_press);
}

struct Gesture {
  bool        ok;
  const char* why;           // reason when rejected
  bool        down_at_start; // KEY1 still down when the app started
  uint32_t    released_ms;   // first release (ms since app start)
  uint32_t    pressed_ms;    // second press
  size_t      press2_index;  // sample index at the second press
};

// Waits until `pin` has been at `level` for DEBOUNCE_MS. Returns when it first
// got there (ms since app start), or 0 if `deadline` passes first.
uint32_t waitKey(gpio_num_t pin, int level, uint32_t deadline) {
  uint32_t since = 0;
  while ((int32_t)(deadline - millis()) > 0) {
    if (digitalRead(pin) == level) {
      if (!since) { since = millis(); }
      if (millis() - since >= DEBOUNCE_MS) { return since; }
    } else {
      since = 0;
    }
    delay(2);
  }
  return 0;
}

// Press, release, press on `pin`; the first press is what woke the stick.
// (For KEY1 the second press is then held to record; see audio::arm().)
Gesture detectGesture(gpio_num_t pin) {
  Gesture g{false, "", digitalRead(pin) == LOW, 0, 0, 0};
  g.released_ms = millis();
  if (g.down_at_start) {
    g.released_ms = waitKey(pin, HIGH, TAP_MAX_MS);
    if (!g.released_ms) {
      g.why = "held (single press and hold)";
      return g;
    }
  }
  g.pressed_ms = waitKey(pin, LOW, g.released_ms + GAP_MAX_MS);
  if (!g.pressed_ms) {
    g.why = "no second press (single press)";
    return g;
  }
  if (pin == PIN_KEY1) {
    const size_t now = audio::currentIndex();
    const size_t back = (millis() - g.pressed_ms) * audio::SAMPLE_RATE / 1000;
    g.press2_index = now > back ? now - back : 0;
  }
  g.ok = true;
  return g;
}

void saveRecording(bool mic_ok, bool fs_ok) {
  show("REC", nullptr, TFT_RED);
  while (!audio::finished()) { delay(5); }
  audio::stop();

  const size_t n = audio::sampleCount();
  const float seconds = (float)n / audio::SAMPLE_RATE;
  Serial.printf("recorded %.1f s (codec %s)\n", seconds, audio::codecOk() ? "ok" : "FAILED");
  // How long the codec took to deliver audio, against where the memo starts
  // (second press minus pre-roll): anything past that start is speech lost.
  const int32_t first_audio_ms = audio::firstAudioMs();
  const int32_t memo_start_ms = (int32_t)(audio::startIndex() * 1000 / audio::SAMPLE_RATE);
  const int32_t lost_ms = first_audio_ms < 0 ? -1 : std::max<int32_t>(first_audio_ms - memo_start_ms, 0);
  Serial.printf("codec: %s start, first audio %ld ms after capture start, memo starts at %ld ms, lost %ld ms\n",
                audio::coldStart() ? "cold" : "warm", (long)first_audio_ms, (long)memo_start_ms, (long)lost_ms);
  char detail[160];

  if (!mic_ok) {
    show("Mic error", audio::codecOk() ? "I2S or memory" : "codec not responding");
    delay(MESSAGE_MS);
  } else if (n < MIN_SAMPLES + PRE_ROLL) {
    // Too short to be a memo.
  } else if (!fs_ok) {
    show("Storage error", "memo not saved");
    delay(MESSAGE_MS);
  } else {
    const time_t now = time(nullptr);
    const uint32_t started = now > CLOCK_VALID_AFTER ? (uint32_t)(now - (time_t)seconds) : 0;
    show("Saving...");
    const MemoNumber number = nextMemoNumber();
    if (memo_queue::save(audio::samples(), n, audio::SAMPLE_RATE, number.seq, started, number.epoch)) {
      retry_minutes = RETRY_MINUTES;  // a new memo starts the retry backoff afresh
      const int32_t battery = telemetry_now.battery_pct;
      int len = snprintf(detail, sizeof(detail), "%.1f s", seconds);
      if (battery >= 0 && battery <= LOW_BATTERY_PCT) {
        len += snprintf(detail + len, sizeof(detail) - len, "\n\nBATTERY LOW: %ld%%", (long)battery);
      }
      if (memo_queue::freeBytes() < QUEUE_WARN_SECONDS * BYTES_PER_SECOND) {
        len += snprintf(detail + len, sizeof(detail) - len, "\n\nQUEUE NEARLY FULL");
      }
#if L3B_OFF_IN_SLEEP
      // Experiment build: show the codec warm-up so it can be read without a cable.
      snprintf(detail + len, sizeof(detail) - len, "\n\n%s codec: audio at %ld ms\nmemo start %ld ms\nlost %ld ms",
               audio::coldStart() ? "cold" : "warm", (long)first_audio_ms, (long)memo_start_ms, (long)lost_ms);
#endif
      show("Saved", detail);
    } else {
      show("Queue full", "memo not saved");
      delay(MESSAGE_MS);
    }
  }
  audio::release();
}

// Battery (estimated from voltage: the StickS3 has no fuel gauge, and it
// reads high while charging), storage left for memos, the queue, and presses
// ignored since last time (pocket presses that didn't match the gesture).
void showStatus(bool fs_ok) {
  auto& d = M5.Display;
  d.fillScreen(TFT_BLACK);
  d.setCursor(0, 8);

  const int32_t level = M5.Power.getBatteryLevel();
  const int16_t mv = M5.Power.getBatteryVoltage();
  const auto charging = M5.Power.isCharging();
  d.setTextSize(1);
  d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  d.println(" Battery");
  d.setTextSize(3);
  d.setTextColor(level >= 0 && level <= LOW_BATTERY_PCT ? TFT_RED : TFT_GREEN, TFT_BLACK);
  if (level >= 0) { d.printf(" %ld%%\n", (long)level); } else { d.println(" ?"); }
  d.setTextSize(1);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.printf(" %.2f V%s\n\n", mv / 1000.0f,
           charging == m5::Power_Class::is_charging ? ", charging" : "");

  d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  d.println(" Storage free");
  if (fs_ok) {
    const size_t free_b = memo_queue::freeBytes(), cap = memo_queue::capacityBytes();
    const int pct = cap ? (int)(100.0f * free_b / cap + 0.5f) : 0;
    d.setTextSize(3);
    d.setTextColor(pct <= 10 ? TFT_RED : TFT_GREEN, TFT_BLACK);
    d.printf(" %d%%\n", pct);
    d.setTextSize(1);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.printf(" %.1f min of audio\n\n", free_b / (float)BYTES_PER_SECOND / 60.0f);
    d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    d.println(" Waiting to send");
    d.setTextSize(2);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.printf(" %u memo%s\n", (unsigned)memo_queue::count(), memo_queue::count() == 1 ? "" : "s");
  } else {
    d.setTextSize(2);
    d.setTextColor(TFT_RED, TFT_BLACK);
    d.println(" error");
  }
  d.setTextSize(1);
  d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  d.printf("\n Ignored presses: %u\n", (unsigned)ignored_presses);
  d.printf(" Firmware %s\n", FW_VERSION);
  const size_t bad = fs_ok ? memo_queue::badCount() : 0;
  if (bad) {
    d.setTextColor(TFT_ORANGE, TFT_BLACK);
    d.printf(" Set aside: %u (rejected)\n", (unsigned)bad);
  }
  Serial.printf("status: battery %ld%% %d mV charging %d, ignored presses %u\n", (long)level, mv,
                (int)charging, (unsigned)ignored_presses);
  ignored_presses = 0;
}

// Status screen for STATUS_MS, or until KEY2 is pressed again. A KEY1 press
// closes it and wakes straight into the record gesture.
[[noreturn]] void statusAndSleep(bool fs_ok) {
  M5.Display.setBrightness(BRIGHTNESS);
  showStatus(fs_ok);
  bool key2_armed = false;  // ignore the gesture's second press until it's released
  const uint32_t start = millis();
  while (millis() - start < STATUS_MS) {
    if (digitalRead(PIN_KEY1) == LOW) { sleepNow(true); }
    const bool key2_down = digitalRead(PIN_KEY2) == LOW;
    if (!key2_down) { key2_armed = true; }
    if (key2_armed && key2_down) { break; }
    delay(20);
  }
  sleepNow();
}

void showProgress(size_t sent, size_t remaining) {
  char detail[40];
  snprintf(detail, sizeof(detail), "%u of %u", (unsigned)(sent + 1), (unsigned)remaining + (unsigned)sent);
  show("Sending...", detail);
}

// Upload watchdog (runs in the esp_timer task): a request blocked past its
// budget. Count it as a failure and sleep; the queue is retried later. The
// main task is stuck in the network stack, so the codec (I2C) and display
// (SPI) are free to power down from here. Deep sleep powers the radio down.
[[noreturn]] void uploadStuck() {
  retryIn(retry_minutes);
  retry_minutes = std::min(retry_minutes * 2, RETRY_MAX_MINUTES);
  audio::codecOff();
  M5.Display.sleep();
  deepSleep();
}

// No Wi-Fi on a nearly flat battery: keep the memos and try again later.
[[noreturn]] void lowBatterySleep(bool visible) {
  Serial.printf("battery %d mV: skipping Wi-Fi\n", telemetry_now.battery_mv);
  if (visible) {
    char detail[64];
    snprintf(detail, sizeof(detail), "%.2f V: charge me\n%u memo%s kept", telemetry_now.battery_mv / 1000.0f,
             (unsigned)memo_queue::count(), memo_queue::count() == 1 ? "" : "s");
    show("Battery low", detail);
    delay(MESSAGE_MS);
  }
  if (memo_queue::count() && !retry_deadline) { retryIn(retry_minutes); }
  sleepNow();
}

// Timer wake with nothing to upload: check in with the receiver if it's due.
[[noreturn]] void heartbeatAndSleep() {
  if (time(nullptr) + 60 < heartbeat_deadline) { sleepNow(); }  // woke early (retry timer): nothing due
  if (batteryTooLowForWifi()) { lowBatterySleep(false); }
  const auto r = uploader::heartbeat(PIN_KEY1, telemetry_now);
  if (retry_deadline) { retry_deadline += r.clock_shift; }
  Serial.printf("heartbeat: outcome %d, http %d\n", (int)r.outcome, r.http_status);
  if (r.outcome == uploader::Outcome::Interrupted) { sleepNow(true); }
  // Next check-in a full interval away whether or not this one got through:
  // a stick that can't reach the receiver shouldn't keep waking to try.
  if (r.outcome == uploader::Outcome::AllSent) {
    reported();
  } else {
    heartbeat_deadline = time(nullptr) + HEARTBEAT_S;
  }
  sleepNow();
}

[[noreturn]] void uploadAndSleep(bool visible) {
  if (memo_queue::count() == 0) {
    retry_deadline = 0;
    if (!visible) { heartbeatAndSleep(); }
    sleepNow();
  }
  if (batteryTooLowForWifi()) { lowBatterySleep(visible); }

  const auto r = uploader::sendQueue(PIN_KEY1, visible ? showProgress : nullptr, uploadStuck, telemetry_now);
  const size_t left = memo_queue::count();
  // NTP may have moved the clock; keep the deadlines on the same footing.
  if (retry_deadline) { retry_deadline += r.clock_shift; }
  if (heartbeat_deadline) { heartbeat_deadline += r.clock_shift; }
  if (r.sent || r.set_aside || r.outcome == uploader::Outcome::AllSent) { reported(); }

  if (r.outcome == uploader::Outcome::Interrupted) {
    // Not a failure. KEY1 is down: wake straight back into the record gesture.
    if (left && !retry_deadline) { retryIn(retry_minutes); }
    sleepNow(true);
  }
  // On failure, retry after retry_in minutes, and back off for the round after.
  const uint32_t retry_in = retry_minutes;
  if (left == 0) {
    retry_minutes = RETRY_MINUTES;
    retry_deadline = 0;
  } else {
    retry_minutes = std::min(retry_minutes * 2, RETRY_MAX_MINUTES);
    retryIn(retry_in);
  }
  Serial.printf("upload: outcome %d, sent %u, set aside %u, left %u, http %d, retry in %lu min\n",
                (int)r.outcome, (unsigned)r.sent, (unsigned)r.set_aside, (unsigned)left, r.http_status,
                (unsigned long)(left ? retry_in : 0));

  if (visible) {
    char detail[80];
    switch (r.outcome) {
      case uploader::Outcome::AllSent:
        if (r.set_aside) {
          snprintf(detail, sizeof(detail), "%u memo%s\n%u rejected, set aside", (unsigned)r.sent,
                   r.sent == 1 ? "" : "s", (unsigned)r.set_aside);
        } else {
          snprintf(detail, sizeof(detail), "%u memo%s, %.0f KB/s", (unsigned)r.sent, r.sent == 1 ? "" : "s",
                   r.upload_ms ? r.bytes / 1.024f / r.upload_ms : 0.0f);
        }
        show("Sent", detail);
        break;
      case uploader::Outcome::CertError:
        snprintf(detail, sizeof(detail), "server certificate not trusted\n%u queued\nretry in %lu min",
                 (unsigned)left, (unsigned long)retry_in);
        show("Cert error", detail);
        break;
      case uploader::Outcome::NoWifi:
        snprintf(detail, sizeof(detail), "%u queued\nretry in %lu min", (unsigned)left, (unsigned long)retry_in);
        show("No Wi-Fi", detail);
        break;
      case uploader::Outcome::NoInternet:
        snprintf(detail, sizeof(detail), "via %s\n%u queued\nretry in %lu min", r.network.c_str(),
                 (unsigned)left, (unsigned long)retry_in);
        show("No internet", detail);
        break;
      case uploader::Outcome::BadToken:
        show("Bad token", "check secrets.h");
        break;
      default:
        snprintf(detail, sizeof(detail), "HTTP %d, %u queued\nretry in %lu min", r.http_status,
                 (unsigned)left, (unsigned long)retry_in);
        show("Upload failed", detail);
        break;
    }
    delay(MESSAGE_MS);
  }
  sleepNow();
}

}  // namespace

void setup() {
  gpio_hold_dis(PIN_LCD_BL);
  gpio_deep_sleep_hold_dis();
  pinMode(PIN_LCD_BL, OUTPUT);  // backlight stays off unless we get as far as M5.begin()
  digitalWrite(PIN_LCD_BL, LOW);
  rtc_gpio_deinit(PIN_KEY1);
  rtc_gpio_deinit(PIN_KEY2);
  pinMode(PIN_KEY1, INPUT_PULLUP);
  pinMode(PIN_KEY2, INPUT_PULLUP);
  const auto cause = esp_sleep_get_wakeup_cause();

  // A button that was held when we slept has been released: nothing to do.
  if (cause == ESP_SLEEP_WAKEUP_EXT0 && key1_wait_release) {
    key1_wait_release = false;
    deepSleep();
  }
  if (cause == ESP_SLEEP_WAKEUP_EXT1 && key2_wait_release) {
    key2_wait_release = false;
    deepSleep();
  }

  // KEY1: start the mic at once, then check for press, release, press and
  // hold before spending time on M5.begin() and the display.
  const bool recording = cause == ESP_SLEEP_WAKEUP_EXT0;
  bool mic_ok = false;
  Gesture g{};
  // KEY2: the status screen needs the same press, release, press, so pocket
  // presses on the side button don't light the screen either.
  if (cause == ESP_SLEEP_WAKEUP_EXT1) {
    g = detectGesture(PIN_KEY2);
    if (!g.ok) {
      ++ignored_presses;
      ++unreported_ignored;
      deepSleep();
    }
  }
  if (recording) {
    mic_ok = audio::start(PIN_KEY1);
    g = detectGesture(PIN_KEY1);
    if (!g.ok) {
      ++ignored_presses;
      ++unreported_ignored;
      audio::stop();
      audio::release();
      audio::codecOff();
      deepSleep();
    }
    audio::arm(g.press2_index > PRE_ROLL ? g.press2_index - PRE_ROLL : 0);
  }

  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  cfg.internal_imu = false;
  cfg.internal_rtc = false;
  cfg.internal_mic = false;  // audio.cpp owns the mic's I2S port
  M5.begin(cfg);
  M5.Display.setBrightness(cause == ESP_SLEEP_WAKEUP_TIMER ? 0 : BRIGHTNESS);
  M5.Display.setFont(&fonts::Font0);
  // The green LED is driven by the PM1's LED_EN output, which comes up on and
  // nothing else turns off. It would draw more than the whole stick asleep.
  // The PM1 stays powered, so this lasts through deep sleep.
  const uint8_t pwr_cfg = M5.In_I2C.readRegister8(PM1_ADDR, PM1_PWR_CFG, PM1_FREQ);
  if (pwr_cfg & PM1_LED_EN) {
    M5.In_I2C.bitOff(PM1_ADDR, PM1_PWR_CFG, PM1_LED_EN, PM1_FREQ);
    Serial.printf("led: PWR_CFG was 0x%02X, LED_EN turned off\n", pwr_cfg);
  }
  readTelemetry();
  if (recording) { audio::finishPowerUp(); }  // the internal I2C bus is ours again
  if (recording) {
    Serial.printf("gesture: down at start %d, released %lu ms, pressed %lu ms, index %u\n",
                  (int)g.down_at_start, (unsigned long)g.released_ms, (unsigned long)g.pressed_ms,
                  (unsigned)g.press2_index);
  }

  const auto mount = memo_queue::begin();
  const bool fs_ok = mount != memo_queue::MountResult::Failed;
  if (mount == memo_queue::MountResult::Reformatted && cause != ESP_SLEEP_WAKEUP_TIMER) {
    show("Storage reset", "it wouldn't mount, so it\nwas reformatted: any\nqueued memos are lost");
    delay(MESSAGE_MS * 2);
  }

  if (cause == ESP_SLEEP_WAKEUP_EXT1) { statusAndSleep(fs_ok); }
  if (recording) {
    saveRecording(mic_ok, fs_ok);
  } else if (cause != ESP_SLEEP_WAKEUP_TIMER) {
    char detail[96];
    snprintf(detail, sizeof(detail),
             "To record: press,\nrelease, then press\nand hold the front\nbutton.\n\n%u queued\nid %s",
             fs_ok ? (unsigned)memo_queue::count() : 0u, uploader::deviceId().c_str());
    show("M5Recorder", detail);
    delay(MESSAGE_MS);
  }

  if (!fs_ok) { sleepNow(); }
  // After a brownout or crash (not a normal power-on), don't start Wi-Fi
  // straight away: on a weak battery that could loop. Try again later.
  if (cause == ESP_SLEEP_WAKEUP_UNDEFINED) {
    const auto reason = esp_reset_reason();
    if (reason == ESP_RST_BROWNOUT || reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT ||
        reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT) {
      Serial.printf("reset reason %d: no Wi-Fi this time\n", (int)reason);
      if (memo_queue::count()) { retryIn(FAULT_RETRY_MINUTES); }
      sleepNow();
    }
  }
  uploadAndSleep(cause != ESP_SLEEP_WAKEUP_TIMER);
}

void loop() {}

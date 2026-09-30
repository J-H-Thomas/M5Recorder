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

// Kept through deep sleep.
RTC_DATA_ATTR bool     key1_wait_release = false;  // slept with KEY1 held: next ext0 wake is its release
RTC_DATA_ATTR bool     key2_wait_release = false;
RTC_DATA_ATTR uint32_t retry_minutes = RETRY_MINUTES;  // retry interval after the next failed round
RTC_DATA_ATTR uint32_t armed_minutes = 0;          // retry timer it last slept with (0 = none)
RTC_DATA_ATTR uint16_t ignored_presses = 0;        // rejected gestures since the status screen last showed them

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

uint32_t nextSeq() {
  Preferences prefs;
  prefs.begin("recorder", false);
  const uint32_t seq = prefs.getUInt("seq", 0) + 1;
  prefs.putUInt("seq", seq);
  prefs.end();
  return seq;
}

// Configures the wake sources and sleeps. Needs nothing initialised, so it's
// also used before M5.begin() and from the upload watchdog's timer task.
//
// A button that is held now would wake the stick straight back up, so it is
// armed to wake on its *release* instead (and that wake just sleeps again).
// key1_is_press: KEY1 is down because the user is starting the record gesture
// (pressed on the status screen or during an upload): wake at once on it.
// timer_minutes: retry-timer wake (0 = none).
[[noreturn]] void deepSleep(uint32_t timer_minutes, bool key1_is_press = false) {
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

  armed_minutes = timer_minutes;
  if (timer_minutes) { esp_sleep_enable_timer_wakeup(timer_minutes * 60ULL * 1000000ULL); }
  esp_deep_sleep_start();
}

// Normal sleep after M5.begin(): Wi-Fi, codec and display off first.
[[noreturn]] void sleepNow(uint32_t timer_minutes, bool key1_is_press = false) {
  uploader::shutdown();
  audio::codecOff();
  M5.Display.sleep();
  M5.Display.waitDisplay();
  Serial.flush();
  deepSleep(timer_minutes, key1_is_press);
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
  char detail[48];

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
    if (memo_queue::save(audio::samples(), n, audio::SAMPLE_RATE, nextSeq(), started)) {
      retry_minutes = RETRY_MINUTES;  // a new memo starts the retry backoff afresh
      const int32_t battery = M5.Power.getBatteryLevel();
      if (battery >= 0 && battery <= LOW_BATTERY_PCT) {
        snprintf(detail, sizeof(detail), "%.1f s\n\nBATTERY LOW: %ld%%", seconds, (long)battery);
      } else {
        snprintf(detail, sizeof(detail), "%.1f s", seconds);
      }
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
  Serial.printf("status: battery %ld%% %d mV charging %d, ignored presses %u\n", (long)level, mv,
                (int)charging, (unsigned)ignored_presses);
  ignored_presses = 0;
}

// Status screen for STATUS_MS, or until KEY2 is pressed again. A KEY1 press
// closes it and wakes straight into the record gesture.
[[noreturn]] void statusAndSleep(bool fs_ok) {
  M5.Display.setBrightness(BRIGHTNESS);
  showStatus(fs_ok);
  const uint32_t retry = fs_ok && memo_queue::count() > 0 ? retry_minutes : 0;
  bool key2_armed = false;  // ignore the gesture's second press until it's released
  const uint32_t start = millis();
  while (millis() - start < STATUS_MS) {
    if (digitalRead(PIN_KEY1) == LOW) { sleepNow(retry, true); }
    const bool key2_down = digitalRead(PIN_KEY2) == LOW;
    if (!key2_down) { key2_armed = true; }
    if (key2_armed && key2_down) { break; }
    delay(20);
  }
  sleepNow(retry);
}

void showProgress(size_t sent, size_t remaining) {
  char detail[40];
  snprintf(detail, sizeof(detail), "%u of %u", (unsigned)(sent + 1), (unsigned)remaining + (unsigned)sent);
  show("Sending...", detail);
}

// Upload watchdog (runs in the esp_timer task): a request blocked past its
// budget. Count it as a failure and sleep; the queue is retried later.
[[noreturn]] void uploadStuck() {
  const uint32_t retry_in = retry_minutes;
  retry_minutes = std::min(retry_minutes * 2, RETRY_MAX_MINUTES);
  esp_wifi_stop();
  deepSleep(retry_in);
}

[[noreturn]] void uploadAndSleep(bool visible) {
  if (memo_queue::count() == 0) { sleepNow(0); }

  const auto r = uploader::sendQueue(PIN_KEY1, visible ? showProgress : nullptr, uploadStuck);
  const size_t left = memo_queue::count();

  if (r.outcome == uploader::Outcome::Interrupted) {
    // Not a failure. KEY1 is down: wake straight back into the record gesture.
    sleepNow(left ? retry_minutes : 0, true);
  }
  // On failure, retry after retry_in minutes, and back off for the round after.
  const uint32_t retry_in = retry_minutes;
  if (left == 0) {
    retry_minutes = RETRY_MINUTES;
  } else {
    retry_minutes = std::min(retry_minutes * 2, RETRY_MAX_MINUTES);
  }
  Serial.printf("upload: outcome %d, sent %u, left %u, http %d, retry in %lu min\n",
                (int)r.outcome, (unsigned)r.sent, (unsigned)left, r.http_status,
                (unsigned long)(left ? retry_in : 0));

  if (visible) {
    char detail[64];
    switch (r.outcome) {
      case uploader::Outcome::AllSent:
        snprintf(detail, sizeof(detail), "%u memo%s, %.0f KB/s", (unsigned)r.sent, r.sent == 1 ? "" : "s",
                 r.upload_ms ? r.bytes / 1.024f / r.upload_ms : 0.0f);
        show("Sent", detail);
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
  sleepNow(left ? retry_in : 0);
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
    deepSleep(armed_minutes);
  }
  if (cause == ESP_SLEEP_WAKEUP_EXT1 && key2_wait_release) {
    key2_wait_release = false;
    deepSleep(armed_minutes);
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
      deepSleep(armed_minutes);
    }
  }
  if (recording) {
    mic_ok = audio::start(PIN_KEY1);
    g = detectGesture(PIN_KEY1);
    if (!g.ok) {
      ++ignored_presses;
      audio::stop();
      audio::release();
      audio::codecOff();
      deepSleep(armed_minutes);
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
  if (recording) {
    Serial.printf("gesture: down at start %d, released %lu ms, pressed %lu ms, index %u\n",
                  (int)g.down_at_start, (unsigned long)g.released_ms, (unsigned long)g.pressed_ms,
                  (unsigned)g.press2_index);
  }

  const bool fs_ok = memo_queue::begin();

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

  if (!fs_ok) { sleepNow(0); }
  uploadAndSleep(cause != ESP_SLEEP_WAKEUP_TIMER);
}

void loop() {}

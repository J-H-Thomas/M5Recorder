// M5Recorder: push-to-talk voice memos on an M5StickS3.
//
// Hold the front button (KEY1) to record; release to stop. The memo is saved
// to flash, then uploaded over Wi-Fi (home network or phone hotspot) to the
// receiver on the home server, which transcribes it into an Obsidian note.
// Memos that can't be sent stay queued; the stick wakes every
// RETRY_MINUTES to try again while any are waiting.
//
// Wake paths:
//   KEY1 (ext0)  record -> save -> upload -> sleep
//   KEY2 (ext1)  status screen (battery, storage, queue) -> sleep
//   timer        upload queued memos with the screen off -> sleep
//   power on     show status, upload anything queued -> sleep
//
// Recording starts before M5.begin() (see audio.cpp): with the codec rail
// (L3B) left on in sleep, audio is live ~55 ms after the app starts.

#include <M5Unified.h>
#include <Preferences.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>

#include "audio.h"
#include "memo_queue.h"
#include "uploader.h"

namespace {

constexpr gpio_num_t PIN_KEY1   = GPIO_NUM_11;  // front: record
constexpr gpio_num_t PIN_KEY2   = GPIO_NUM_12;  // side: status screen
constexpr gpio_num_t PIN_LCD_BL = GPIO_NUM_38;

constexpr size_t   MIN_SAMPLES     = audio::SAMPLE_RATE / 2;  // presses under 0.5 s are ignored
constexpr uint32_t RETRY_MINUTES   = 15;
constexpr uint32_t MESSAGE_MS      = 2000;
constexpr uint32_t STATUS_MS       = 5000;
constexpr int32_t  LOW_BATTERY_PCT = 20;
constexpr uint32_t BYTES_PER_SECOND = audio::SAMPLE_RATE * sizeof(int16_t);
constexpr uint8_t  BRIGHTNESS      = 80;
constexpr time_t   CLOCK_VALID_AFTER = 1700000000;  // Nov 2023

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

// Deep sleep until KEY1 (and, if memos are waiting, the retry timer).
// wait_release=false is used when KEY1 is already down: the stick wakes again
// at once and goes straight into recording.
[[noreturn]] void sleepNow(bool retry_timer, bool wait_release = true) {
  uploader::shutdown();
  audio::codecOff();
  M5.Display.sleep();
  M5.Display.waitDisplay();

  if (wait_release) {
    while (digitalRead(PIN_KEY1) == LOW) { delay(10); }
  }
  // A held KEY2 would wake the stick straight back up into the status screen.
  while (digitalRead(PIN_KEY2) == LOW) { delay(10); }
  delay(50);

  // Keep the backlight off while the digital pads are unpowered.
  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, LOW);
  gpio_hold_en(PIN_LCD_BL);
  gpio_deep_sleep_hold_en();

  rtc_gpio_pullup_en(PIN_KEY1);
  rtc_gpio_pulldown_dis(PIN_KEY1);
  esp_sleep_enable_ext0_wakeup(PIN_KEY1, 0);
  rtc_gpio_pullup_en(PIN_KEY2);
  rtc_gpio_pulldown_dis(PIN_KEY2);
  esp_sleep_enable_ext1_wakeup(1ULL << PIN_KEY2, ESP_EXT1_WAKEUP_ANY_LOW);
  if (retry_timer) { esp_sleep_enable_timer_wakeup(RETRY_MINUTES * 60ULL * 1000000ULL); }
  Serial.flush();
  esp_deep_sleep_start();
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
  } else if (n < MIN_SAMPLES) {
    // A tap, not a memo.
  } else if (!fs_ok) {
    show("Storage error", "memo not saved");
    delay(MESSAGE_MS);
  } else {
    const time_t now = time(nullptr);
    const uint32_t started = now > CLOCK_VALID_AFTER ? (uint32_t)(now - (time_t)seconds) : 0;
    show("Saving...");
    if (memo_queue::save(audio::samples(), n, audio::SAMPLE_RATE, nextSeq(), started)) {
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
// reads high while charging), storage left for memos, and the queue.
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
  Serial.printf("status: battery %ld%% %d mV charging %d\n", (long)level, mv, (int)charging);
}

// Status screen for STATUS_MS, or until KEY2 is pressed again. KEY1 goes
// straight to recording.
[[noreturn]] void statusAndSleep(bool fs_ok) {
  M5.Display.setBrightness(BRIGHTNESS);
  showStatus(fs_ok);
  bool key2_armed = false;  // ignore the press that woke us until it's released
  const uint32_t start = millis();
  while (millis() - start < STATUS_MS) {
    if (digitalRead(PIN_KEY1) == LOW) { sleepNow(fs_ok && memo_queue::count() > 0, false); }
    const bool key2_down = digitalRead(PIN_KEY2) == LOW;
    if (!key2_down) { key2_armed = true; }
    if (key2_armed && key2_down) { break; }
    delay(20);
  }
  sleepNow(fs_ok && memo_queue::count() > 0);
}

void showProgress(size_t sent, size_t remaining) {
  char detail[40];
  snprintf(detail, sizeof(detail), "%u of %u", (unsigned)(sent + 1), (unsigned)remaining + (unsigned)sent);
  show("Sending...", detail);
}

[[noreturn]] void uploadAndSleep(bool visible) {
  if (memo_queue::count() == 0) { sleepNow(false); }

  const auto r = uploader::sendQueue(PIN_KEY1, visible ? showProgress : nullptr);
  const size_t left = memo_queue::count();
  Serial.printf("upload: outcome %d, sent %u, left %u, http %d\n",
                (int)r.outcome, (unsigned)r.sent, (unsigned)left, r.http_status);

  if (r.outcome == uploader::Outcome::Interrupted) {
    sleepNow(true, false);  // KEY1 is down: wake straight back up and record
  }
  if (visible) {
    char detail[48];
    switch (r.outcome) {
      case uploader::Outcome::AllSent:
        snprintf(detail, sizeof(detail), "%u memo%s", (unsigned)r.sent, r.sent == 1 ? "" : "s");
        show("Sent", detail);
        break;
      case uploader::Outcome::NoWifi:
        snprintf(detail, sizeof(detail), "%u queued, retry in %lu min", (unsigned)left, (unsigned long)RETRY_MINUTES);
        show("No Wi-Fi", detail);
        break;
      case uploader::Outcome::BadToken:
        show("Bad token", "check secrets.h");
        break;
      default:
        snprintf(detail, sizeof(detail), "HTTP %d, %u queued", r.http_status, (unsigned)left);
        show("Upload failed", detail);
        break;
    }
    delay(MESSAGE_MS);
  }
  sleepNow(left > 0);
}

}  // namespace

void setup() {
  // Read KEY1 and start the mic before anything slow happens.
  gpio_hold_dis(PIN_LCD_BL);
  gpio_deep_sleep_hold_dis();
  rtc_gpio_deinit(PIN_KEY1);
  rtc_gpio_deinit(PIN_KEY2);
  pinMode(PIN_KEY1, INPUT_PULLUP);
  pinMode(PIN_KEY2, INPUT_PULLUP);
  const auto cause = esp_sleep_get_wakeup_cause();
  const bool recording = cause == ESP_SLEEP_WAKEUP_EXT0;
  const bool mic_ok = recording && audio::start(PIN_KEY1);

  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  cfg.internal_imu = false;
  cfg.internal_rtc = false;
  cfg.internal_mic = false;  // audio.cpp owns the mic's I2S port
  M5.begin(cfg);
  M5.Display.setBrightness(cause == ESP_SLEEP_WAKEUP_TIMER ? 0 : BRIGHTNESS);
  M5.Display.setFont(&fonts::Font0);

  const bool fs_ok = memo_queue::begin();

  if (cause == ESP_SLEEP_WAKEUP_EXT1) { statusAndSleep(fs_ok); }
  if (recording) {
    saveRecording(mic_ok, fs_ok);
  } else if (cause != ESP_SLEEP_WAKEUP_TIMER) {
    char detail[64];
    snprintf(detail, sizeof(detail), "Hold front button\nto record.\n\n%u queued\nid %s",
             fs_ok ? (unsigned)memo_queue::count() : 0u, uploader::deviceId().c_str());
    show("M5Recorder", detail);
    delay(MESSAGE_MS);
  }

  if (!fs_ok) { sleepNow(false); }
  uploadAndSleep(cause != ESP_SLEEP_WAKEUP_TIMER);
}

void loop() {}

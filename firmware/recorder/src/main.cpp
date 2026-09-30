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

constexpr gpio_num_t PIN_KEY1   = GPIO_NUM_11;
constexpr gpio_num_t PIN_LCD_BL = GPIO_NUM_38;

constexpr size_t   MIN_SAMPLES     = audio::SAMPLE_RATE / 2;  // presses under 0.5 s are ignored
constexpr uint32_t RETRY_MINUTES   = 15;
constexpr uint32_t MESSAGE_MS      = 2000;
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
    delay(50);
  }

  // Keep the backlight off while the digital pads are unpowered.
  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, LOW);
  gpio_hold_en(PIN_LCD_BL);
  gpio_deep_sleep_hold_en();

  rtc_gpio_pullup_en(PIN_KEY1);
  rtc_gpio_pulldown_dis(PIN_KEY1);
  esp_sleep_enable_ext0_wakeup(PIN_KEY1, 0);
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
      snprintf(detail, sizeof(detail), "%.1f s", seconds);
      show("Saved", detail);
    } else {
      show("Queue full", "memo not saved");
      delay(MESSAGE_MS);
    }
  }
  audio::release();
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
  pinMode(PIN_KEY1, INPUT_PULLUP);
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

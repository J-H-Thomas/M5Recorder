// M5StickS3 wake-latency test.
//
// Measures how much of a push-to-talk memo is lost between pressing KEY1
// (front button) and the microphone delivering real audio, when waking from
// ESP32 deep sleep.
//
// Flow:
//   1. Device sleeps. Press and HOLD KEY1 and immediately say "one two three".
//   2. Screen turns red while recording; release KEY1 to stop.
//   3. The recording is played back (listen for a clipped "one") and the
//      timings are shown on screen and printed over USB serial.
//   4. KEY2 (side button) toggles whether the audio codec / LCD rail (L3B)
//      stays powered during sleep, to see if that shortens codec warm-up.
//      KEY1 goes back to sleep for the next test; otherwise it sleeps after 30 s.
//
// Timings are milliseconds since the app started; ROM + bootloader time before
// that is not visible to the app, which is what the playback test is for.

#include <M5Unified.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>

namespace {

constexpr gpio_num_t PIN_KEY1   = GPIO_NUM_11;
constexpr gpio_num_t PIN_LCD_BL = GPIO_NUM_38;

constexpr uint8_t  PM1_ADDR      = 0x6E;
constexpr uint32_t PM1_FREQ      = 100000;
constexpr uint8_t  PM1_GPIO_OUT  = 0x11;
constexpr uint8_t  PM1_WAKE_SRC  = 0x05;
constexpr uint8_t  PM1_L3B_BIT   = 1 << 2;  // PM1 GPIO2 enables the L3B rail (LCD + ES8311)

constexpr uint32_t SAMPLE_RATE  = 16000;
constexpr size_t   CHUNK        = 256;  // 16 ms per record() request
constexpr uint32_t MAX_SECONDS  = 30;
constexpr size_t   MAX_SAMPLES  = SAMPLE_RATE * MAX_SECONDS;
constexpr int16_t  SPEECH_LEVEL = 2000; // rough "someone is talking" amplitude
constexpr uint8_t  PLAY_VOLUME  = 160;  // stay under ~75% on battery (docs: higher can brown out)
constexpr uint32_t RESULT_TIMEOUT_MS = 30000;

// Survive deep sleep.
RTC_DATA_ATTR bool     keep_l3b_in_sleep = false;
RTC_DATA_ATTR uint32_t test_count = 0;

struct Result {
  bool     key1_at_start;
  uint32_t t_setup;        // app start -> setup()
  uint32_t t_begin;        // M5.begin() done
  uint32_t t_mic;          // Mic.begin() done
  uint32_t t_release;      // KEY1 released (or buffer full)
  size_t   samples;
  int32_t  first_nonzero_ms;  // after t_mic; -1 = never
  int32_t  first_speech_ms;   // after t_mic; -1 = never
  uint8_t  pm_wake_src;
};

int32_t firstIndexMs(const int16_t* buf, size_t n, int16_t level) {
  for (size_t i = 0; i < n; ++i) {
    if (abs(buf[i]) > level) { return (int32_t)(i * 1000 / SAMPLE_RATE); }
  }
  return -1;
}

void printResult(const Result& r, bool timer_wake) {
  Serial.printf("\n=== test %lu (%s) ===\n", (unsigned long)test_count,
                timer_wake ? "wake" : "cold boot");
  Serial.printf("L3B kept on in sleep : %s\n", keep_l3b_in_sleep ? "yes" : "no");
  Serial.printf("KEY1 held at start   : %s\n", r.key1_at_start ? "yes" : "no");
  Serial.printf("PM1 WAKE_SRC         : 0x%02X\n", r.pm_wake_src);
  Serial.printf("setup() at           : %lu ms\n", (unsigned long)r.t_setup);
  Serial.printf("M5.begin() done      : %lu ms\n", (unsigned long)r.t_begin);
  Serial.printf("Mic.begin() done     : %lu ms\n", (unsigned long)r.t_mic);
  Serial.printf("first non-zero audio : %ld ms after mic start\n", (long)r.first_nonzero_ms);
  Serial.printf("first speech-level   : %ld ms after mic start\n", (long)r.first_speech_ms);
  Serial.printf("released at          : %lu ms\n", (unsigned long)r.t_release);
  Serial.printf("recorded             : %u ms\n", (unsigned)(r.samples * 1000 / SAMPLE_RATE));
}

void drawResult(const Result& r) {
  auto& d = M5.Display;
  d.fillScreen(TFT_BLACK);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.setCursor(0, 0);
  d.printf("Test #%lu\n", (unsigned long)test_count);
  d.printf("L3B in sleep: %s\n\n", keep_l3b_in_sleep ? "ON" : "off");
  d.printf("KEY1 held: %s\n", r.key1_at_start ? "yes" : "NO");
  d.printf("WAKE_SRC: 0x%02X\n\n", r.pm_wake_src);
  d.printf("setup  %5lu ms\n", (unsigned long)r.t_setup);
  d.printf("begin  %5lu ms\n", (unsigned long)r.t_begin);
  d.printf("mic    %5lu ms\n", (unsigned long)r.t_mic);
  d.printf("audio +%5ld ms\n", (long)r.first_nonzero_ms);
  d.printf("speech+%5ld ms\n", (long)r.first_speech_ms);
  d.printf("rec    %5u ms\n\n", (unsigned)(r.samples * 1000 / SAMPLE_RATE));
  d.setTextColor(TFT_YELLOW, TFT_BLACK);
  d.printf("KEY1: sleep/next\nKEY2: toggle L3B\n");
}

void drawIntro() {
  auto& d = M5.Display;
  d.fillScreen(TFT_BLACK);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.setCursor(0, 0);
  d.printf("Wake test\n\n");
  d.printf("After sleep:\n");
  d.printf("HOLD front button\n");
  d.printf("and at once say\n");
  d.printf("\"one two three\".\n");
  d.printf("Release to stop.\n\n");
  d.printf("L3B in sleep: %s\n\n", keep_l3b_in_sleep ? "ON" : "off");
  d.setTextColor(TFT_YELLOW, TFT_BLACK);
  d.printf("KEY1: sleep now\nKEY2: toggle L3B\n");
}

[[noreturn]] void goToSleep() {
  M5.Mic.end();
  M5.Speaker.end();
  M5.Display.sleep();
  M5.Display.waitDisplay();

  // Don't sleep with the button down, or we'd wake straight back up.
  while (digitalRead(PIN_KEY1) == LOW) { delay(10); }
  delay(50);

  if (!keep_l3b_in_sleep) {
    M5.In_I2C.bitOff(PM1_ADDR, PM1_GPIO_OUT, PM1_L3B_BIT, PM1_FREQ);
  }

  // Keep the backlight off while the digital pads are unpowered.
  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, LOW);
  gpio_hold_en(PIN_LCD_BL);
  gpio_deep_sleep_hold_en();

  rtc_gpio_pullup_en(PIN_KEY1);
  rtc_gpio_pulldown_dis(PIN_KEY1);
  esp_sleep_enable_ext0_wakeup(PIN_KEY1, 0);
  Serial.flush();
  esp_deep_sleep_start();
}

}  // namespace

void setup() {
  Result r{};
  r.t_setup = millis();

  // Read KEY1 before anything slow happens.
  gpio_hold_dis(PIN_LCD_BL);
  gpio_deep_sleep_hold_dis();
  rtc_gpio_deinit(PIN_KEY1);
  pinMode(PIN_KEY1, INPUT_PULLUP);
  r.key1_at_start = digitalRead(PIN_KEY1) == LOW;
  const bool woke_by_key = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0;

  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  cfg.internal_imu = false;  // not needed; saves init time
  cfg.internal_rtc = false;
  M5.begin(cfg);
  r.t_begin = millis();
  r.pm_wake_src = M5.In_I2C.readRegister8(PM1_ADDR, PM1_WAKE_SRC, PM1_FREQ);

  M5.Display.setBrightness(96);
  M5.Display.setFont(&fonts::Font0);
  M5.Display.setTextSize(1);

  if (!woke_by_key) {
    drawIntro();
  } else {
    ++test_count;

    // Speaker and mic share the ES8311 / I2S port.
    M5.Speaker.end();
    M5.Mic.begin();
    r.t_mic = millis();
    M5.Display.fillScreen(TFT_RED);
    M5.Display.setTextColor(TFT_WHITE, TFT_RED);
    M5.Display.setTextSize(3);
    M5.Display.drawCenterString("REC", M5.Display.width() / 2, M5.Display.height() / 2 - 12);
    M5.Display.setTextSize(1);

    auto* buf = (int16_t*)heap_caps_malloc(MAX_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    size_t queued = 0;
    if (buf) {
      while (queued < MAX_SAMPLES && digitalRead(PIN_KEY1) == LOW) {
        if (M5.Mic.isRecording() < 2) {
          const size_t n = std::min(CHUNK, MAX_SAMPLES - queued);
          if (M5.Mic.record(buf + queued, n, SAMPLE_RATE)) { queued += n; }
        } else {
          delay(1);
        }
      }
    }
    r.t_release = millis();
    while (M5.Mic.isRecording()) { delay(1); }
    M5.Mic.end();

    r.samples = queued;
    r.first_nonzero_ms = buf ? firstIndexMs(buf, queued, 0) : -1;
    r.first_speech_ms  = buf ? firstIndexMs(buf, queued, SPEECH_LEVEL) : -1;
    printResult(r, true);
    drawResult(r);

    if (buf && queued) {
      M5.Speaker.begin();
      M5.Speaker.setVolume(PLAY_VOLUME);
      M5.Speaker.playRaw(buf, queued, SAMPLE_RATE, false, 1, 0, true);
      while (M5.Speaker.isPlaying()) { delay(10); }
      M5.Speaker.end();
    }
    free(buf);
  }

  // Wait for KEY1 (sleep for next test) or KEY2 (toggle L3B mode).
  const uint32_t start = millis();
  bool key1_armed = digitalRead(PIN_KEY1) == HIGH;  // ignore a press still held from recording
  while (millis() - start < RESULT_TIMEOUT_MS) {
    M5.update();
    if (!key1_armed && digitalRead(PIN_KEY1) == HIGH) { key1_armed = true; }
    if (key1_armed && M5.BtnA.wasPressed()) { break; }
    if (M5.BtnB.wasPressed()) {
      keep_l3b_in_sleep = !keep_l3b_in_sleep;
      if (woke_by_key) { drawResult(r); } else { drawIntro(); }
    }
    delay(10);
  }
  goToSleep();
}

void loop() {}

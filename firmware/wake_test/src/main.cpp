// M5StickS3 wake-latency test (fast-wake version).
//
// Measures how much of a push-to-talk memo is lost between pressing KEY1
// (front button) and the microphone delivering real audio, when waking from
// ESP32 deep sleep.
//
// Results from the first version: with the codec / LCD rail (L3B) off in sleep
// the ES8311 sends zeros for ~1 s after power-up; with L3B kept on, audio is
// live as soon as the mic starts, but M5.begin() (~400 ms) delayed that start.
// So this version keeps L3B on in sleep by default and, on a KEY1 wake, sets up
// the ES8311 and I2S itself and starts recording from a background task
// *before* M5.begin(). The display comes up while it is already recording.
//
// Flow:
//   1. Device sleeps. Press and HOLD KEY1 and immediately say "one two three".
//   2. Screen turns red while recording; release KEY1 to stop.
//   3. The recording is played back (listen for a clipped "one") and the
//      timings are shown on screen and printed over USB serial.
//   4. KEY2 (side button) toggles whether L3B stays powered during sleep.
//      KEY1 goes back to sleep for the next test; otherwise it sleeps after 30 s.
//
// Timings are milliseconds since the app started; ROM + bootloader time before
// that is not visible to the app, which is what the playback test is for.

#include <M5Unified.h>
#include <driver/i2s.h>
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

// ES8311 codec and its I2S pins (same as M5Unified's StickS3 mic setup).
constexpr uint8_t    ES8311_ADDR = 0x18;
constexpr uint32_t   ES8311_FREQ = 400000;
constexpr i2s_port_t I2S_PORT    = I2S_NUM_1;
constexpr int PIN_I2S_MCK = 18;
constexpr int PIN_I2S_BCK = 17;
constexpr int PIN_I2S_WS  = 15;
constexpr int PIN_I2S_DIN = 16;

// Register writes copied from M5Unified's _microphone_enabled_cb_sticks3().
constexpr uint8_t ES8311_MIC_ON[][2] = {
  {0x00, 0x80},  // CSM power on
  {0x01, 0xBA},  // MCLK taken from BCLK
  {0x02, 0x18},  // MULT_PRE = 3 (x8): BCLK 32*fs -> MCLK 256*fs
  {0x0D, 0x01},  // power up analog circuitry
  {0x0E, 0x02},  // enable analog PGA and ADC modulator
  {0x14, 0x10},  // select Mic1p-Mic1n, minimum PGA gain
  {0x17, 0xFF},  // ADC volume max
  {0x1C, 0x6A},  // ADC equalizer bypass, cancel DC offset
};
constexpr uint8_t ES8311_MIC_OFF[][2] = {
  {0x0D, 0xFC},  // power down analog circuitry
  {0x0E, 0x6A},
  {0x00, 0x00},  // CSM power down
};

constexpr uint32_t SAMPLE_RATE  = 16000;
constexpr size_t   READ_FRAMES  = 256;  // stereo frames per i2s_read (16 ms)
constexpr int32_t  MIC_GAIN     = 8;    // matches M5Unified's default (magnification 16, over_sampling 2)
constexpr uint32_t MAX_SECONDS  = 30;
constexpr size_t   MAX_SAMPLES  = SAMPLE_RATE * MAX_SECONDS;
constexpr int16_t  SPEECH_LEVEL = 2000; // rough "someone is talking" amplitude
constexpr size_t   GAP_SAMPLES  = SAMPLE_RATE / 50;  // zero runs >= 20 ms count as dropouts
constexpr uint8_t  PLAY_VOLUME  = 160;  // stay under ~75% on battery (docs: higher can brown out)
constexpr uint32_t RESULT_TIMEOUT_MS = 30000;

// Survive deep sleep. L3B on is the default now: it removed the ~1 s of codec
// silence in the first version's tests.
RTC_DATA_ATTR bool     keep_l3b_in_sleep = true;
RTC_DATA_ATTR uint32_t test_count = 0;

struct Result {
  bool     key1_at_start;
  bool     codec_ok;         // all ES8311 register writes acknowledged
  uint32_t t_setup;          // app start -> setup()
  uint32_t t_mic;            // I2S running, capture task started
  uint32_t t_begin;          // M5.begin() done (now after the mic)
  uint32_t t_release;        // KEY1 released (or buffer full)
  size_t   samples;
  int32_t  first_nonzero_ms; // after t_mic; -1 = never
  int32_t  first_speech_ms;  // after t_mic; -1 = never
  int32_t  longest_gap_ms;   // longest run of zeros after audio started
  uint8_t  pm_wake_src;
};

// Filled by the capture task; read by setup() once `done` is set.
struct Capture {
  int16_t*         buf = nullptr;
  volatile size_t  count = 0;
  volatile bool    stop = false;
  volatile bool    done = false;
} cap;

int32_t firstIndexMs(const int16_t* buf, size_t n, int16_t level) {
  for (size_t i = 0; i < n; ++i) {
    if (abs(buf[i]) > level) { return (int32_t)(i * 1000 / SAMPLE_RATE); }
  }
  return -1;
}

// Longest run of exact zeros after the first non-zero sample, if at least
// GAP_SAMPLES long: a codec reset or DMA overrun during recording shows here.
int32_t longestGapMs(const int16_t* buf, size_t n) {
  size_t i = 0;
  while (i < n && buf[i] == 0) { ++i; }
  size_t longest = 0, run = 0;
  for (; i < n; ++i) {
    run = buf[i] == 0 ? run + 1 : 0;
    if (run > longest) { longest = run; }
  }
  return longest >= GAP_SAMPLES ? (int32_t)(longest * 1000 / SAMPLE_RATE) : 0;
}

bool writeCodec(const uint8_t (*regs)[2], size_t n) {
  bool ok = true;
  for (size_t i = 0; i < n; ++i) {
    ok &= M5.In_I2C.writeRegister8(ES8311_ADDR, regs[i][0], regs[i][1], ES8311_FREQ);
  }
  return ok;
}

bool startI2s() {
  i2s_config_t c{};
  c.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  c.sample_rate          = SAMPLE_RATE;
  c.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
  c.channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT;  // both slots: BCLK stays at 32*fs
  c.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  c.dma_buf_count        = 8;
  c.dma_buf_len          = 256;  // 8 x 16 ms of buffering, covers short stalls
  c.mclk_multiple        = I2S_MCLK_MULTIPLE_256;
  c.bits_per_chan        = I2S_BITS_PER_CHAN_16BIT;
  if (i2s_driver_install(I2S_PORT, &c, 0, nullptr) != ESP_OK) { return false; }

  i2s_pin_config_t p{};
  p.mck_io_num   = PIN_I2S_MCK;
  p.bck_io_num   = PIN_I2S_BCK;
  p.ws_io_num    = PIN_I2S_WS;
  p.data_out_num = I2S_PIN_NO_CHANGE;
  p.data_in_num  = PIN_I2S_DIN;
  if (i2s_set_pin(I2S_PORT, &p) != ESP_OK) {
    i2s_driver_uninstall(I2S_PORT);
    return false;
  }
  return true;
}

// Reads stereo frames and keeps the louder slot of each (the mic is on one of
// them; the other is zero or a copy), with M5Unified's default gain.
void captureTask(void*) {
  static int16_t raw[READ_FRAMES * 2];
  while (!cap.stop && cap.count < MAX_SAMPLES) {
    size_t got = 0;
    i2s_read(I2S_PORT, raw, sizeof(raw), &got, pdMS_TO_TICKS(100));
    const size_t frames = std::min(got / (2 * sizeof(int16_t)), MAX_SAMPLES - cap.count);
    size_t n = cap.count;
    for (size_t f = 0; f < frames; ++f) {
      const int16_t a = raw[f * 2], b = raw[f * 2 + 1];
      const int32_t s = (abs(a) >= abs(b) ? a : b) * MIC_GAIN;
      cap.buf[n++] = (int16_t)std::max<int32_t>(-32768, std::min<int32_t>(32767, s));
    }
    cap.count = n;
  }
  cap.done = true;
  vTaskDelete(nullptr);
}

void printResult(const Result& r) {
  Serial.printf("\n=== test %lu ===\n", (unsigned long)test_count);
  Serial.printf("L3B kept on in sleep : %s\n", keep_l3b_in_sleep ? "yes" : "no");
  Serial.printf("KEY1 held at start   : %s\n", r.key1_at_start ? "yes" : "no");
  Serial.printf("codec writes ok      : %s\n", r.codec_ok ? "yes" : "no");
  Serial.printf("PM1 WAKE_SRC         : 0x%02X\n", r.pm_wake_src);
  Serial.printf("setup() at           : %lu ms\n", (unsigned long)r.t_setup);
  Serial.printf("mic running          : %lu ms\n", (unsigned long)r.t_mic);
  Serial.printf("M5.begin() done      : %lu ms\n", (unsigned long)r.t_begin);
  Serial.printf("first non-zero audio : %ld ms after mic start\n", (long)r.first_nonzero_ms);
  Serial.printf("first speech-level   : %ld ms after mic start\n", (long)r.first_speech_ms);
  Serial.printf("longest dropout      : %ld ms\n", (long)r.longest_gap_ms);
  Serial.printf("released at          : %lu ms\n", (unsigned long)r.t_release);
  Serial.printf("recorded             : %u ms\n", (unsigned)(r.samples * 1000 / SAMPLE_RATE));
}

void drawResult(const Result& r) {
  auto& d = M5.Display;
  d.fillScreen(TFT_BLACK);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.setCursor(0, 0);
  d.printf("Test #%lu (fast)\n", (unsigned long)test_count);
  d.printf("L3B in sleep: %s\n\n", keep_l3b_in_sleep ? "ON" : "off");
  d.printf("KEY1 held: %s\n", r.key1_at_start ? "yes" : "NO");
  d.printf("codec: %s\n", r.codec_ok ? "ok" : "FAILED");
  d.printf("WAKE_SRC: 0x%02X\n\n", r.pm_wake_src);
  d.printf("setup  %5lu ms\n", (unsigned long)r.t_setup);
  d.printf("mic    %5lu ms\n", (unsigned long)r.t_mic);
  d.printf("begin  %5lu ms\n", (unsigned long)r.t_begin);
  d.printf("audio +%5ld ms\n", (long)r.first_nonzero_ms);
  d.printf("speech+%5ld ms\n", (long)r.first_speech_ms);
  d.printf("gap    %5ld ms\n", (long)r.longest_gap_ms);
  d.printf("rec    %5u ms\n\n", (unsigned)(r.samples * 1000 / SAMPLE_RATE));
  d.setTextColor(TFT_YELLOW, TFT_BLACK);
  d.printf("KEY1: sleep/next\nKEY2: toggle L3B\n");
}

void drawIntro() {
  auto& d = M5.Display;
  d.fillScreen(TFT_BLACK);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.setCursor(0, 0);
  d.printf("Wake test (fast)\n\n");
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
  M5.Speaker.end();
  M5.Display.sleep();
  M5.Display.waitDisplay();

  // Don't sleep with the button down, or we'd wake straight back up.
  while (digitalRead(PIN_KEY1) == LOW) { delay(10); }
  delay(50);

  // Power down the codec's analog side; it restarts instantly if L3B stays on.
  writeCodec(ES8311_MIC_OFF, sizeof(ES8311_MIC_OFF) / sizeof(ES8311_MIC_OFF[0]));
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

  // Fast path: start recording before M5.begin().
  bool capturing = false;
  if (woke_by_key) {
    ++test_count;
    cap.buf = (int16_t*)heap_caps_malloc(MAX_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    M5.In_I2C.begin(I2C_NUM_1, GPIO_NUM_47, GPIO_NUM_48);
    // L3B is normally still on from sleep; this is a no-op then.
    M5.In_I2C.bitOn(PM1_ADDR, PM1_GPIO_OUT, PM1_L3B_BIT, PM1_FREQ);
    r.codec_ok = writeCodec(ES8311_MIC_ON, sizeof(ES8311_MIC_ON) / sizeof(ES8311_MIC_ON[0]));
    M5.In_I2C.release();  // M5GFX probes these pins during M5.begin()
    if (cap.buf && startI2s()) {
      capturing = xTaskCreatePinnedToCore(captureTask, "capture", 4096, nullptr,
                                          configMAX_PRIORITIES - 2, nullptr, 0) == pdPASS;
    }
    r.t_mic = millis();
  }

  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  cfg.internal_imu = false;  // not needed; saves init time
  cfg.internal_rtc = false;
  cfg.internal_mic = false;  // the fast path owns the mic's I2S port
  M5.begin(cfg);
  r.t_begin = millis();
  r.pm_wake_src = M5.In_I2C.readRegister8(PM1_ADDR, PM1_WAKE_SRC, PM1_FREQ);

  M5.Display.setBrightness(96);
  M5.Display.setFont(&fonts::Font0);
  M5.Display.setTextSize(1);

  if (!woke_by_key) {
    drawIntro();
  } else {
    M5.Display.fillScreen(TFT_RED);
    M5.Display.setTextColor(TFT_WHITE, TFT_RED);
    M5.Display.setTextSize(3);
    M5.Display.drawCenterString("REC", M5.Display.width() / 2, M5.Display.height() / 2 - 12);
    M5.Display.setTextSize(1);

    while (capturing && !cap.done && digitalRead(PIN_KEY1) == LOW) { delay(5); }
    r.t_release = millis();
    cap.stop = true;
    while (capturing && !cap.done) { delay(1); }
    i2s_driver_uninstall(I2S_PORT);

    const size_t n = capturing ? cap.count : 0;
    r.samples = n;
    r.first_nonzero_ms = n ? firstIndexMs(cap.buf, n, 0) : -1;
    r.first_speech_ms  = n ? firstIndexMs(cap.buf, n, SPEECH_LEVEL) : -1;
    r.longest_gap_ms   = n ? longestGapMs(cap.buf, n) : 0;
    printResult(r);
    drawResult(r);

    if (n) {
      M5.Speaker.begin();
      M5.Speaker.setVolume(PLAY_VOLUME);
      M5.Speaker.playRaw(cap.buf, n, SAMPLE_RATE, false, 1, 0, true);
      while (M5.Speaker.isPlaying()) { delay(10); }
      M5.Speaker.end();
    }
    free(cap.buf);
    cap.buf = nullptr;
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

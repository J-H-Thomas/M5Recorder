#include "audio.h"

#include <M5Unified.h>
#include <driver/i2s.h>

namespace audio {
namespace {

constexpr uint8_t  PM1_ADDR     = 0x6E;
constexpr uint32_t PM1_FREQ     = 100000;
constexpr uint8_t  PM1_GPIO_OUT = 0x11;
constexpr uint8_t  PM1_L3B_BIT  = 1 << 2;  // PM1 GPIO2 enables the L3B rail (LCD + ES8311)

constexpr uint8_t    ES8311_ADDR = 0x18;
constexpr uint32_t   ES8311_FREQ = 400000;
constexpr i2s_port_t I2S_PORT    = I2S_NUM_1;
constexpr int PIN_I2S_MCK = 18;
constexpr int PIN_I2S_BCK = 17;
constexpr int PIN_I2S_WS  = 15;
constexpr int PIN_I2S_DIN = 16;

// Register writes copied from M5Unified's _microphone_enabled_cb_sticks3().
constexpr uint8_t MIC_ON[][2] = {
  {0x00, 0x80},  // CSM power on
  {0x01, 0xBA},  // MCLK taken from BCLK
  {0x02, 0x18},  // MULT_PRE = 3 (x8): BCLK 32*fs -> MCLK 256*fs
  {0x0D, 0x01},  // power up analog circuitry
  {0x0E, 0x02},  // enable analog PGA and ADC modulator
  {0x14, 0x10},  // select Mic1p-Mic1n, minimum PGA gain
  {0x17, 0xFF},  // ADC volume max
  {0x1C, 0x6A},  // ADC equalizer bypass, cancel DC offset
};
constexpr uint8_t MIC_OFF[][2] = {
  {0x0D, 0xFC},  // power down analog circuitry
  {0x0E, 0x6A},
  {0x00, 0x00},  // CSM power down
};
// REG0D VMIDSEL (bits 1:0): 01 = start up VMID with normal-speed charge (what
// M5Unified uses), 11 = start up with fast charge. After a cold power-up of the
// codec (L3B was off) the normal charge left about 1 s of silence.
constexpr uint8_t REG0D_VMID_NORMAL = 0x01;
constexpr uint8_t REG0D_VMID_FAST   = 0x03;

constexpr size_t  READ_FRAMES   = 256;  // stereo frames per i2s_read (16 ms)
constexpr int32_t GAIN          = 8;    // matches M5Unified's default (magnification 16, over_sampling 2)
constexpr int     RELEASE_READS = 2;    // KEY1 up for 2 reads in a row (~32 ms) ends the memo
// Room for the gesture (press, release, press) before the memo starts.
constexpr size_t  BUFFER_SAMPLES = MAX_SAMPLES + SAMPLE_RATE * 3;

int16_t*          buf = nullptr;
volatile size_t   count = 0;
volatile size_t   start_at = 0;
volatile bool     armed = false;
volatile bool     stop_req = false;
volatile bool     done = true;
bool              running = false;
bool              codec_ok = false;
bool              cold_start = false;  // the codec was powered up this wake (L3B was off)
gpio_num_t        key = GPIO_NUM_NC;

bool writeRegs(const uint8_t (*regs)[2], size_t n) {
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
// them; the other is zero or a copy). Once armed, stops itself when KEY1 is
// released or the memo reaches MAX_SECONDS.
void captureTask(void*) {
  static int16_t raw[READ_FRAMES * 2];
  int released = 0;
  while (!stop_req && count < BUFFER_SAMPLES) {
    size_t got = 0;
    i2s_read(I2S_PORT, raw, sizeof(raw), &got, pdMS_TO_TICKS(100));
    size_t limit = BUFFER_SAMPLES;
    if (armed) { limit = std::min(limit, start_at + MAX_SAMPLES); }
    const size_t frames = std::min(got / (2 * sizeof(int16_t)), limit - count);
    size_t n = count;
    for (size_t f = 0; f < frames; ++f) {
      const int16_t a = raw[f * 2], b = raw[f * 2 + 1];
      const int32_t s = (abs(a) >= abs(b) ? a : b) * GAIN;
      buf[n++] = (int16_t)std::max<int32_t>(-32768, std::min<int32_t>(32767, s));
    }
    count = n;
    if (!armed) { continue; }
    if (count >= limit) { break; }
    released = digitalRead(key) == HIGH ? released + 1 : 0;
    if (released >= RELEASE_READS) { break; }
  }
  done = true;
  vTaskDelete(nullptr);
}

}  // namespace

bool start(gpio_num_t key_pin) {
  key = key_pin;
  count = 0;
  start_at = 0;
  armed = false;
  stop_req = false;
  done = true;
  buf = (int16_t*)heap_caps_malloc(BUFFER_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);

  M5.In_I2C.begin(I2C_NUM_1, GPIO_NUM_47, GPIO_NUM_48);
  // L3B normally stays on through sleep; this is a no-op then. If it was off
  // (L3B_OFF_IN_SLEEP builds), give the ES8311 a moment to power up before
  // talking to it.
  const bool l3b_was_on = M5.In_I2C.readRegister8(PM1_ADDR, PM1_GPIO_OUT, PM1_FREQ) & PM1_L3B_BIT;
  M5.In_I2C.bitOn(PM1_ADDR, PM1_GPIO_OUT, PM1_L3B_BIT, PM1_FREQ);
  cold_start = !l3b_was_on;
  if (cold_start) {
    delay(20);
    // After a cold power-up the codec delivered its first audio at exactly
    // 1026 ms every time (fast VMID charge made no difference): a fixed
    // power-up timer, not a capacitor. Espressif's ES8311 driver zeroes the
    // power-up/down timing registers before starting the codec; do the same.
    M5.In_I2C.writeRegister8(ES8311_ADDR, 0x0B, 0x00, ES8311_FREQ);
    M5.In_I2C.writeRegister8(ES8311_ADDR, 0x0C, 0x00, ES8311_FREQ);
  }
  codec_ok = writeRegs(MIC_ON, sizeof(MIC_ON) / sizeof(MIC_ON[0]));
  if (cold_start) {
    // Fast-charge the codec's reference; finishPowerUp() returns it to normal.
    codec_ok &= M5.In_I2C.writeRegister8(ES8311_ADDR, 0x0D, REG0D_VMID_FAST, ES8311_FREQ);
  }
  M5.In_I2C.release();  // M5GFX probes these pins during M5.begin()

  if (!buf || !codec_ok || !startI2s()) { return false; }
  running = true;
  done = false;
  if (xTaskCreatePinnedToCore(captureTask, "capture", 4096, nullptr,
                              configMAX_PRIORITIES - 2, nullptr, 0) != pdPASS) {
    done = true;
    stop();
    return false;
  }
  return true;
}

size_t currentIndex() { return count; }

bool coldStart() { return cold_start; }
size_t startIndex() { return start_at; }

void finishPowerUp() {
  if (!cold_start) { return; }
  M5.In_I2C.writeRegister8(ES8311_ADDR, 0x0D, REG0D_VMID_NORMAL, ES8311_FREQ);
}

int32_t firstAudioMs() {
  if (!buf) { return -1; }
  for (size_t i = 0; i < count; ++i) {
    if (buf[i] != 0) { return (int32_t)(i * 1000 / SAMPLE_RATE); }
  }
  return -1;
}

void arm(size_t start_index) {
  start_at = std::min(start_index, (size_t)count);
  armed = true;
}

bool finished() { return done; }

void stop() {
  stop_req = true;
  while (!done) { delay(1); }
  if (running) {
    i2s_driver_uninstall(I2S_PORT);
    running = false;
  }
}

const int16_t* samples() { return buf ? buf + start_at : nullptr; }
size_t sampleCount() { return armed && count > start_at ? count - start_at : 0; }
bool codecOk() { return codec_ok; }

void release() {
  free(buf);
  buf = nullptr;
  count = 0;
  start_at = 0;
  armed = false;
}

void codecOff() {
  // Works whether or not M5.begin() has run (a rejected press skips it).
  M5.In_I2C.begin(I2C_NUM_1, GPIO_NUM_47, GPIO_NUM_48);
  writeRegs(MIC_OFF, sizeof(MIC_OFF) / sizeof(MIC_OFF[0]));
#if L3B_OFF_IN_SLEEP
  // Experiment: power the LCD + codec rail down for sleep, to measure what
  // keeping it on costs (recordings then lose ~1 s to codec warm-up).
  M5.In_I2C.bitOff(PM1_ADDR, PM1_GPIO_OUT, PM1_L3B_BIT, PM1_FREQ);
#endif
}

}  // namespace audio

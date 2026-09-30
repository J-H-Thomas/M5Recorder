// Fast-start recording: ES8311 + I2S set up directly (before M5.begin()),
// captured by a background task into PSRAM until KEY1 is released.
#pragma once

#include <Arduino.h>

namespace audio {

constexpr uint32_t SAMPLE_RATE = 16000;
constexpr uint32_t MAX_SECONDS = 120;
constexpr size_t   MAX_SAMPLES = SAMPLE_RATE * MAX_SECONDS;

// Powers up the codec and starts capturing. Uses M5.In_I2C (begun and
// released here), so call it before M5.begin(). False if anything failed.
bool start(gpio_num_t key_pin);

// True once capture has stopped (KEY1 released, buffer full, or stop()).
bool finished();

// Stops capture (if still running) and frees the I2S driver.
void stop();

const int16_t* samples();
size_t sampleCount();
bool codecOk();

// Frees the PSRAM buffer.
void release();

// Powers down the codec's analog side (needs M5.In_I2C, i.e. after M5.begin()).
void codecOff();

}  // namespace audio

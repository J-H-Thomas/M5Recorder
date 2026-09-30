// Fast-start recording: ES8311 + I2S set up directly (before M5.begin()),
// captured by a background task into PSRAM.
//
// Capture starts at wake (the first press of the record gesture) so the mic is
// warm by the time the gesture is confirmed. arm() marks where the memo
// starts (the second press minus a little pre-roll); after that, releasing
// KEY1 ends the capture.
#pragma once

#include <Arduino.h>

namespace audio {

constexpr uint32_t SAMPLE_RATE = 16000;
constexpr uint32_t MAX_SECONDS = 60;                   // longest memo
constexpr size_t   MAX_SAMPLES = SAMPLE_RATE * MAX_SECONDS;

// Powers up the codec and starts capturing. Uses M5.In_I2C (begun and
// released here), so call it before M5.begin(). False if anything failed.
bool start(gpio_num_t key_pin);

// Samples captured so far (index of the next one).
size_t currentIndex();

// Marks the memo as starting at sample `start_index`; from now on releasing
// KEY1 (debounced) or reaching MAX_SECONDS ends the capture.
void arm(size_t start_index);

// True once capture has stopped (KEY1 released after arm(), memo at
// MAX_SECONDS, buffer full, or stop()).
bool finished();

// Stops capture (if still running) and frees the I2S driver.
void stop();

// The memo: from the arm() start index to the end of capture.
const int16_t* samples();
size_t sampleCount();
bool codecOk();

// Frees the PSRAM buffer.
void release();

// Powers down the codec's analog side. Safe before or after M5.begin().
void codecOff();

}  // namespace audio

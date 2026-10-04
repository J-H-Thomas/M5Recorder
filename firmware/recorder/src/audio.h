// Fast-start recording: ES8311 + I2S set up directly (before M5.begin()),
// captured by a background task into PSRAM.
//
// Capture starts at wake (the first press of the record gesture) so the mic is
// warm by the time the gesture is confirmed. arm() marks where the memo
// starts (the second press minus a little pre-roll); after that, releasing
// KEY1 ends the capture.
#pragma once

#include <Arduino.h>

// 1 (default) = switch the LCD + codec rail (L3B) off in sleep. Keeping it on
// cost about 3 mA (a charge lasted ~2.5 days instead of weeks). With the
// ES8311's power-up timing zeroed (see audio::start()), the codec delivers
// audio ~2 ms after power-up, so nothing is lost by switching it off.
#ifndef L3B_OFF_IN_SLEEP
#define L3B_OFF_IN_SLEEP 1
#endif

namespace audio {

constexpr uint32_t SAMPLE_RATE = 16000;
constexpr uint32_t MAX_SECONDS = 60;                   // longest memo
constexpr size_t   MAX_SAMPLES = SAMPLE_RATE * MAX_SECONDS;

// Powers up the codec and starts capturing. Uses M5.In_I2C (begun and
// released here), so call it before M5.begin(). False if anything failed.
bool start(gpio_num_t key_pin);

// Samples captured so far (index of the next one).
size_t currentIndex();

// True if the codec was powered up this wake (L3B was off in sleep). Then its
// reference is fast-charging, and finishPowerUp() (after M5.begin(), when the
// internal I2C bus is ours again) returns it to normal.
bool coldStart();
void finishPowerUp();

// ms from capture start to the first non-zero sample (-1: none): how long the
// codec took to deliver audio.
int32_t firstAudioMs();

// Sample index where the memo starts (set by arm()).
size_t startIndex();

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

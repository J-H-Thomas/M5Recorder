// Memos waiting to upload, as WAV files in LittleFS: /q/<seq>_<unix time>.wav
// (time 0 = the clock wasn't set when it was recorded).
#pragma once

#include <Arduino.h>

namespace memo_queue {

struct Entry {
  String   path;
  uint32_t seq;
  uint32_t time;
};

bool begin();                 // mounts LittleFS (formats it on first use)
size_t count();
size_t freeBytes();

// Writes a 16 kHz mono 16-bit WAV. False if there isn't room or the write failed.
bool save(const int16_t* samples, size_t n, uint32_t sample_rate, uint32_t seq, uint32_t time);

bool oldest(Entry& out);      // lowest sequence number first
bool remove(const Entry& e);

}  // namespace memo_queue

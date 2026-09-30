// Memos waiting to upload, as WAV files in LittleFS:
//   /q/<seq>_<unix time>_<epoch hex>.wav   (time 0 = the clock wasn't set)
//   /q/<seq>_<unix time>.wav               (queued before epochs existed)
// Memos the receiver rejects are moved to /bad/ (kept, but out of the queue).
#pragma once

#include <Arduino.h>

namespace memo_queue {

struct Entry {
  String   path;
  uint32_t seq;
  uint32_t time;
  uint32_t epoch;  // 0 for memos queued before epochs existed
};

enum class MountResult { Ok, Reformatted, Failed };

// Mounts LittleFS (formatting only if it won't mount twice) and removes
// leftovers of interrupted saves.
MountResult begin();

size_t count();          // memos waiting to send
size_t badCount();       // memos set aside after the receiver rejected them
size_t freeBytes();      // room left for memos
size_t capacityBytes();  // room for memos when the queue is empty

// Writes a 16 kHz mono 16-bit WAV. False if there isn't room or the write failed.
bool save(const int16_t* samples, size_t n, uint32_t sample_rate, uint32_t seq, uint32_t time, uint32_t epoch);

bool oldest(Entry& out);      // lowest sequence number first
bool remove(const Entry& e);
bool setAside(const Entry& e);  // move to /bad/

}  // namespace memo_queue

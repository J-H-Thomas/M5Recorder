#include "memo_queue.h"

#include <LittleFS.h>

#include <vector>

namespace memo_queue {
namespace {

constexpr const char* DIR = "/q";
constexpr const char* BAD_DIR = "/bad";
constexpr size_t WAV_HEADER = 44;
constexpr size_t SPARE = 16 * 1024;  // LittleFS needs some free blocks to work in

void put16(uint8_t* p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) { p[i] = v >> (8 * i); } }

void wavHeader(uint8_t* h, uint32_t data_bytes, uint32_t rate) {
  memcpy(h, "RIFF", 4);
  put32(h + 4, 36 + data_bytes);
  memcpy(h + 8, "WAVEfmt ", 8);
  put32(h + 16, 16);          // fmt chunk size
  put16(h + 20, 1);           // PCM
  put16(h + 22, 1);           // mono
  put32(h + 24, rate);
  put32(h + 28, rate * 2);    // byte rate
  put16(h + 32, 2);           // block align
  put16(h + 34, 16);          // bits per sample
  memcpy(h + 36, "data", 4);
  put32(h + 40, data_bytes);
}

// "<seq>_<time>_<epoch hex>.wav", or "<seq>_<time>.wav" from before epochs
// (epoch 0). Anything else (e.g. a .part left by a crash) isn't a queue entry.
bool parseName(const char* name, uint32_t& seq, uint32_t& time, uint32_t& epoch) {
  unsigned long s = 0, t = 0, e = 0;
  int end = 0;
  if (sscanf(name, "%lu_%lu_%lx.wav%n", &s, &t, &e, &end) == 3 && name[end] == '\0' && end > 0) {
    seq = s; time = t; epoch = e;
    return true;
  }
  end = 0;
  if (sscanf(name, "%lu_%lu.wav%n", &s, &t, &end) == 2 && name[end] == '\0' && end > 0) {
    seq = s; time = t; epoch = 0;
    return true;
  }
  return false;
}

size_t sizeOfDir(const char* path) {
  size_t total = 0;
  File dir = LittleFS.open(path);
  if (!dir) { return 0; }
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) { total += f.size(); }
  return total;
}

size_t countDir(const char* path, bool queue_entries_only) {
  size_t n = 0;
  File dir = LittleFS.open(path);
  if (!dir) { return 0; }
  uint32_t seq, time, epoch;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (!queue_entries_only || parseName(f.name(), seq, time, epoch)) { ++n; }
  }
  return n;
}

}  // namespace

MountResult begin() {
  // Never format on the first failure: that would silently throw away the
  // queue. Retry once, and format only if the filesystem really won't mount.
  MountResult r = MountResult::Ok;
  if (!LittleFS.begin(false) && !LittleFS.begin(false)) {
    Serial.println("queue: LittleFS won't mount, formatting");
    if (!LittleFS.begin(true)) { return MountResult::Failed; }
    r = MountResult::Reformatted;
  }
  if (!LittleFS.exists(DIR)) { LittleFS.mkdir(DIR); }
  if (!LittleFS.exists(BAD_DIR)) { LittleFS.mkdir(BAD_DIR); }

  // Leftovers from a save interrupted by a crash or a flat battery.
  std::vector<String> strays;
  File dir = LittleFS.open(DIR);
  uint32_t seq, time, epoch;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (!parseName(f.name(), seq, time, epoch)) { strays.push_back(String(DIR) + "/" + f.name()); }
  }
  dir.close();
  for (const auto& s : strays) {
    Serial.printf("queue: removing stray %s\n", s.c_str());
    LittleFS.remove(s);
  }
  return r;
}

size_t count() { return countDir(DIR, true); }
size_t badCount() { return countDir(BAD_DIR, false); }

size_t freeBytes() {
  const size_t total = LittleFS.totalBytes(), used = LittleFS.usedBytes();
  return used + SPARE < total ? total - used - SPARE : 0;
}

size_t capacityBytes() {
  // LittleFS itself uses a few blocks, so this is measured against an empty
  // queue rather than the raw partition size.
  return freeBytes() + sizeOfDir(DIR) + sizeOfDir(BAD_DIR);
}

bool save(const int16_t* samples, size_t n, uint32_t sample_rate, uint32_t seq, uint32_t time, uint32_t epoch) {
  const size_t data_bytes = n * sizeof(int16_t);
  if (data_bytes + WAV_HEADER > freeBytes()) { return false; }

  char name[64], tmp[48];
  snprintf(name, sizeof(name), "%s/%08lu_%lu_%08lx.wav", DIR, (unsigned long)seq, (unsigned long)time,
           (unsigned long)epoch);
  snprintf(tmp, sizeof(tmp), "%s/%08lu.part", DIR, (unsigned long)seq);

  File f = LittleFS.open(tmp, FILE_WRITE);
  if (!f) { return false; }
  uint8_t header[WAV_HEADER];
  wavHeader(header, data_bytes, sample_rate);
  bool ok = f.write(header, WAV_HEADER) == WAV_HEADER;
  const uint8_t* p = (const uint8_t*)samples;
  for (size_t off = 0; ok && off < data_bytes; off += 4096) {
    const size_t len = std::min<size_t>(4096, data_bytes - off);
    ok = f.write(p + off, len) == len;
  }
  f.close();
  // A crash mid-write leaves only a .part file, which isn't a queue entry.
  if (!ok || !LittleFS.rename(tmp, name)) {
    LittleFS.remove(tmp);
    return false;
  }
  return true;
}

bool oldest(Entry& out) {
  bool found = false;
  File dir = LittleFS.open(DIR);
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    uint32_t seq, time, epoch;
    if (!parseName(f.name(), seq, time, epoch)) { continue; }
    if (!found || seq < out.seq) {
      out = {String(DIR) + "/" + f.name(), seq, time, epoch};
      found = true;
    }
  }
  return found;
}

bool remove(const Entry& e) { return LittleFS.remove(e.path); }

bool setAside(const Entry& e) {
  const String name = e.path.substring(e.path.lastIndexOf('/') + 1);
  return LittleFS.rename(e.path, String(BAD_DIR) + "/" + name);
}

}  // namespace memo_queue

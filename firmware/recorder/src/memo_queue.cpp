#include "memo_queue.h"

#include <LittleFS.h>

#include <vector>

namespace memo_queue {
namespace {

constexpr const char* DIR = "/q";
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

bool parseName(const char* name, uint32_t& seq, uint32_t& time) {
  unsigned long s = 0, t = 0;
  if (sscanf(name, "%lu_%lu.wav", &s, &t) != 2) { return false; }
  seq = s;
  time = t;
  return true;
}

}  // namespace

bool begin() {
  if (!LittleFS.begin(true)) { return false; }
  if (!LittleFS.exists(DIR)) { LittleFS.mkdir(DIR); }
  return true;
}

size_t count() {
  size_t n = 0;
  File dir = LittleFS.open(DIR);
  uint32_t seq, time;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (parseName(f.name(), seq, time)) { ++n; }
  }
  return n;
}

size_t freeBytes() {
  const size_t total = LittleFS.totalBytes(), used = LittleFS.usedBytes();
  return used + SPARE < total ? total - used - SPARE : 0;
}

size_t capacityBytes() {
  // LittleFS itself uses a few blocks (root and /q directories), so this is
  // measured against an empty queue rather than the raw partition size.
  size_t queued = 0;
  File dir = LittleFS.open(DIR);
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) { queued += f.size(); }
  return freeBytes() + queued;
}

bool save(const int16_t* samples, size_t n, uint32_t sample_rate, uint32_t seq, uint32_t time) {
  const size_t data_bytes = n * sizeof(int16_t);
  if (data_bytes + WAV_HEADER > freeBytes()) { return false; }

  char name[48], tmp[48];
  snprintf(name, sizeof(name), "%s/%08lu_%lu.wav", DIR, (unsigned long)seq, (unsigned long)time);
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
  std::vector<String> strays;  // leftovers from an interrupted save
  File dir = LittleFS.open(DIR);
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    uint32_t seq, time;
    const String path = String(DIR) + "/" + f.name();
    if (!parseName(f.name(), seq, time)) {
      strays.push_back(path);
    } else if (!found || seq < out.seq) {
      out = {path, seq, time};
      found = true;
    }
  }
  dir.close();
  for (const auto& s : strays) { LittleFS.remove(s); }
  return found;
}

bool remove(const Entry& e) { return LittleFS.remove(e.path); }

}  // namespace memo_queue

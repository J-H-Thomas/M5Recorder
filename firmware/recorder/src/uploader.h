// Sends queued memos to the receiver over Wi-Fi + HTTPS.
#pragma once

#include <Arduino.h>

namespace uploader {

enum class Outcome {
  AllSent,     // queue is empty
  NoWifi,      // none of the networks connected
  Failed,      // connected, but an upload failed (server down, network error)
  BadToken,    // the receiver rejected the token: fix secrets.h
  Interrupted, // KEY1 was pressed
};

struct Result {
  Outcome outcome;
  size_t  sent;
  int     http_status;  // last HTTP status (or negative HTTPClient error)
};

// Progress callback: (sent so far, remaining in queue).
using Progress = void (*)(size_t sent, size_t remaining);

// Connects, syncs the clock, and uploads the queue oldest first, deleting each
// memo once the receiver accepts it. Stops early if KEY1 goes down.
Result sendQueue(gpio_num_t key_pin, Progress progress);

// Turns Wi-Fi off (before deep sleep).
void shutdown();

// 12 hex digits of the factory MAC, used in memo ids.
String deviceId();

}  // namespace uploader

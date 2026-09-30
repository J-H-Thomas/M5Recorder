// Sends queued memos to the receiver over Wi-Fi + HTTPS.
#pragma once

#include <Arduino.h>

namespace uploader {

enum class Outcome {
  AllSent,     // queue is empty
  NoWifi,      // none of the networks connected
  NoInternet,  // joined Wi-Fi but couldn't reach the server (DNS, connect, timeout)
  Failed,      // reached the server, but it returned an error
  BadToken,    // the receiver rejected the token: fix secrets.h
  Interrupted, // KEY1 was pressed
};

struct Result {
  Outcome  outcome;
  size_t   sent;
  int      http_status;  // last HTTP status (or negative HTTPClient error)
  size_t   bytes;        // sent successfully
  uint32_t upload_ms;    // time spent on those requests (incl. the server's reply)
  String   network;      // the Wi-Fi network joined, if any
};

// Progress callback: (sent so far, remaining in queue).
using Progress = void (*)(size_t sent, size_t remaining);

// Called from a timer task if one request runs past its time budget (a
// blocking DNS lookup or handshake that ignores its timeout). It must not
// return: it should put the stick to sleep.
using Stuck = void (*)();

// Connects, syncs the clock, and uploads the queue oldest first, deleting each
// memo once the receiver accepts it. Stops early if KEY1 goes down.
Result sendQueue(gpio_num_t key_pin, Progress progress, Stuck on_stuck);

// Turns Wi-Fi off (before deep sleep).
void shutdown();

// 12 hex digits of the factory MAC, used in memo ids.
String deviceId();

}  // namespace uploader

// Sends queued memos to the receiver over Wi-Fi + HTTPS.
#pragma once

#include <Arduino.h>

namespace uploader {

enum class Outcome {
  AllSent,     // queue is empty
  NoWifi,      // none of the networks connected
  NoInternet,  // joined Wi-Fi but couldn't reach the server (DNS, connect, timeout)
  CertError,   // reached a server whose certificate didn't verify (CA list out of date?)
  Failed,      // reached the server, but it returned an error (or a memo couldn't be read)
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
  size_t   set_aside;    // memos the receiver rejected, moved out of the queue
  int32_t  clock_shift;  // seconds NTP moved the clock this round (0 if not synced)
};

// Progress callback: (sent so far, remaining in queue).
using Progress = void (*)(size_t sent, size_t remaining);

// The stick's state, sent with every request so the receiver (and Home
// Assistant) can track battery life and the queue.
struct Telemetry {
  int16_t  battery_mv;   // <= 0: unknown
  int32_t  battery_pct;  // < 0: unknown
  bool     charging;
  uint32_t ignored;      // presses ignored since the last successful report
};

// Called from a timer task if one request runs past its time budget (a
// blocking DNS lookup or handshake that ignores its timeout). It must not
// return: it should put the stick to sleep.
using Stuck = void (*)();

// Connects, syncs the clock, and uploads the queue oldest first, deleting each
// memo once the receiver accepts it. Stops early if KEY1 goes down.
Result sendQueue(gpio_num_t key_pin, Progress progress, Stuck on_stuck, const Telemetry& telemetry);

// Connects and reports the stick's state (POST /heartbeat), for check-ins
// when there's nothing to upload. Outcome is AllSent on success.
Result heartbeat(gpio_num_t key_pin, const Telemetry& telemetry);

// Turns Wi-Fi off (before deep sleep).
void shutdown();

// 12 hex digits of the factory MAC, used in memo ids.
String deviceId();

}  // namespace uploader

#include "uploader.h"

#include <HTTPClient.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_sntp.h>

#include "ca_certs.h"
#include "memo_queue.h"
#include "secrets.h"

namespace uploader {
namespace {

constexpr uint32_t CONNECT_TIMEOUT_MS = 8000;  // per network
constexpr uint32_t STALE_STATUS_MS    = 1000;  // ignore "not found" this soon after begin()
constexpr uint32_t NTP_TIMEOUT_MS     = 3000;
constexpr int      NETWORK_COUNT      = sizeof(WIFI_NETWORKS) / sizeof(WIFI_NETWORKS[0]);

// The network that last worked, tried first next time (survives deep sleep).
RTC_DATA_ATTR int8_t last_network = -1;
constexpr uint32_t HTTP_TIMEOUT_MS    = 20000;
constexpr time_t   CLOCK_VALID_AFTER  = 1700000000;  // Nov 2023

bool keyDown(gpio_num_t key) { return digitalRead(key) == LOW; }

// Joins one network by name. The Wi-Fi driver scans for it itself and reports
// "no such network" after a couple of seconds, so an absent network costs
// that, not the full timeout. False on failure, timeout or KEY1.
bool tryNetwork(gpio_num_t key, const WifiNetwork& net) {
  const uint32_t t0 = millis();
  WiFi.begin(net.ssid, net.password);
  while (millis() - t0 < CONNECT_TIMEOUT_MS) {
    const wl_status_t s = WiFi.status();
    if (s == WL_CONNECTED) {
      Serial.printf("wifi: connected to %s in %lu ms (RSSI %d)\n", net.ssid,
                    (unsigned long)(millis() - t0), WiFi.RSSI());
      return true;
    }
    if (millis() - t0 > STALE_STATUS_MS && (s == WL_NO_SSID_AVAIL || s == WL_CONNECT_FAILED)) {
      Serial.printf("wifi: %s %s after %lu ms\n", net.ssid,
                    s == WL_NO_SSID_AVAIL ? "not found" : "refused (password?)",
                    (unsigned long)(millis() - t0));
      break;
    }
    if (keyDown(key)) { break; }
    delay(20);
  }
  if (millis() - t0 >= CONNECT_TIMEOUT_MS) { Serial.printf("wifi: %s timed out\n", net.ssid); }
  WiFi.disconnect();
  return false;
}

// Tries the network that worked last time first (so away from home the
// hotspot goes first, at home the home network), then the rest in
// WIFI_NETWORKS order. (A scan-first approach was dropped: the Arduino
// core gives up on a scan after 20x the per-channel time, 2.4 s at
// 120 ms, and reported that as zero networks.)
bool connect(gpio_num_t key) {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // modem sleep throttles uploads badly; it's off only while awake anyway

  int order[NETWORK_COUNT];
  int n = 0;
  if (last_network >= 0 && last_network < NETWORK_COUNT) { order[n++] = last_network; }
  for (int i = 0; i < NETWORK_COUNT; ++i) {
    if (i != last_network) { order[n++] = i; }
  }
  for (int k = 0; k < n; ++k) {
    if (tryNetwork(key, WIFI_NETWORKS[order[k]])) {
      last_network = order[k];
      return true;
    }
    if (keyDown(key)) { return false; }
  }
  return false;
}

// The ESP32's clock keeps running through deep sleep, so one sync per upload
// round is plenty; memos record time(nullptr) when they're made.
void syncClock() {
  configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
  const uint32_t start = millis();
  while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && millis() - start < NTP_TIMEOUT_MS) {
    delay(50);
  }
}

}  // namespace

String deviceId() {
  const uint64_t mac = ESP.getEfuseMac();  // bytes in transmission order, LSB first
  char id[13];
  for (int i = 0; i < 6; ++i) {
    snprintf(id + i * 2, 3, "%02x", (unsigned)((mac >> (8 * i)) & 0xFF));
  }
  return String(id);
}

Result sendQueue(gpio_num_t key, Progress progress) {
  Result r{Outcome::AllSent, 0, 0, 0, 0};
  size_t remaining = memo_queue::count();
  if (remaining == 0) { return r; }
  if (!connect(key)) {
    r.outcome = keyDown(key) ? Outcome::Interrupted : Outcome::NoWifi;
    return r;
  }
  if (time(nullptr) < CLOCK_VALID_AFTER) { syncClock(); }

  const String device = deviceId();
  WiFiClientSecure tls;
  tls.setCACert(CA_CERTS);
  HTTPClient http;
  http.setReuse(true);  // one TLS handshake for the whole queue
  http.setTimeout(HTTP_TIMEOUT_MS);

  memo_queue::Entry e;
  while (memo_queue::oldest(e)) {
    if (keyDown(key)) {
      r.outcome = Outcome::Interrupted;
      break;
    }
    if (progress) { progress(r.sent, remaining); }

    // Read the whole memo into PSRAM and send it in one write: streaming from
    // the file goes out in 1.4 KB pieces, one TLS record each, which is slow.
    File f = LittleFS.open(e.path, FILE_READ);
    const size_t size = f ? f.size() : 0;
    uint8_t* body = size ? (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM) : nullptr;
    const bool read_ok = body && f.read(body, size) == size;
    if (f) { f.close(); }
    if (!read_ok) {  // unreadable: drop it rather than retry forever
      free(body);
      memo_queue::remove(e);
      --remaining;
      continue;
    }
    http.begin(tls, UPLOAD_URL);
    http.addHeader("Authorization", String("Bearer ") + MEMO_TOKEN);
    http.addHeader("Content-Type", "audio/wav");
    http.addHeader("X-Memo-Id", device + "-" + e.seq);
    http.addHeader("X-Memo-Time", String(e.time));
    http.addHeader("X-Memo-Device", device);
    const uint32_t t0 = millis();
    const int status = http.sendRequest("POST", body, size);
    const uint32_t ms = millis() - t0;
    http.end();
    free(body);
    r.http_status = status;
    Serial.printf("upload %s -> %d, %u bytes in %lu ms (%.0f KB/s)\n", e.path.c_str(), status,
                  (unsigned)size, (unsigned long)ms, ms ? size / 1.024f / ms : 0.0f);

    if (status >= 200 && status < 300) {
      memo_queue::remove(e);
      ++r.sent;
      --remaining;
      r.bytes += size;
      r.upload_ms += ms;
    } else {
      r.outcome = status == 401 ? Outcome::BadToken : Outcome::Failed;
      break;
    }
  }
  if (r.outcome == Outcome::AllSent && memo_queue::count() > 0) { r.outcome = Outcome::Failed; }
  return r;
}

void shutdown() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

}  // namespace uploader

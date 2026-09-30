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

constexpr uint32_t SCAN_TIMEOUT_MS    = 5000;
constexpr uint32_t CONNECT_TIMEOUT_MS = 8000;
constexpr uint32_t NTP_TIMEOUT_MS     = 3000;
constexpr uint32_t HTTP_TIMEOUT_MS    = 20000;
constexpr time_t   CLOCK_VALID_AFTER  = 1700000000;  // Nov 2023

bool keyDown(gpio_num_t key) { return digitalRead(key) == LOW; }

// Scans once and joins the first network in WIFI_NETWORKS that is in range
// (list order = preference), on the channel and access point the scan found.
// If none is in range it gives up straight away rather than waiting on each.
// Hidden networks don't show in a scan, so they aren't supported.
bool connect(gpio_num_t key) {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // modem sleep throttles uploads badly; it's off only while awake anyway

  const uint32_t t0 = millis();
  WiFi.scanNetworks(true, false, false, 120);  // async, active scan, 120 ms per channel
  int16_t found = WIFI_SCAN_RUNNING;
  while ((found = WiFi.scanComplete()) == WIFI_SCAN_RUNNING && millis() - t0 < SCAN_TIMEOUT_MS) {
    if (keyDown(key)) { WiFi.scanDelete(); return false; }
    delay(20);
  }
  if (found < 0) { found = 0; }

  const WifiNetwork* chosen = nullptr;
  int best = -1;
  for (const auto& net : WIFI_NETWORKS) {
    for (int i = 0; i < found; ++i) {
      if (WiFi.SSID(i) == net.ssid && (best < 0 || WiFi.RSSI(i) > WiFi.RSSI(best))) { best = i; }
    }
    if (best >= 0) { chosen = &net; break; }
  }
  Serial.printf("wifi: scan %lu ms, %d networks, using %s\n", (unsigned long)(millis() - t0),
                found, chosen ? chosen->ssid : "none");
  if (!chosen) {
    WiFi.scanDelete();
    return false;
  }

  const int32_t channel = WiFi.channel(best);
  uint8_t bssid[6];
  memcpy(bssid, WiFi.BSSID(best), sizeof(bssid));
  WiFi.scanDelete();
  WiFi.begin(chosen->ssid, chosen->password, channel, bssid);
  const uint32_t t1 = millis();
  while (millis() - t1 < CONNECT_TIMEOUT_MS) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("wifi: connected to %s in %lu ms (RSSI %d)\n", chosen->ssid,
                    (unsigned long)(millis() - t1), WiFi.RSSI());
      return true;
    }
    if (keyDown(key)) { return false; }
    delay(20);
  }
  Serial.printf("wifi: %s didn't connect\n", chosen->ssid);
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

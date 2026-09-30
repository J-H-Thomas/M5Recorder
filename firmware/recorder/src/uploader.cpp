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

constexpr uint32_t CONNECT_TIMEOUT_MS = 8000;   // per network
constexpr uint32_t NTP_TIMEOUT_MS     = 3000;
constexpr uint32_t HTTP_TIMEOUT_MS    = 20000;
constexpr time_t   CLOCK_VALID_AFTER  = 1700000000;  // Nov 2023

bool keyDown(gpio_num_t key) { return digitalRead(key) == LOW; }

bool connect(gpio_num_t key) {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  for (const auto& net : WIFI_NETWORKS) {
    WiFi.begin(net.ssid, net.password);
    const uint32_t start = millis();
    while (millis() - start < CONNECT_TIMEOUT_MS) {
      if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("wifi: connected to %s\n", net.ssid);
        return true;
      }
      if (keyDown(key)) { return false; }
      delay(50);
    }
    Serial.printf("wifi: %s not available\n", net.ssid);
    WiFi.disconnect();
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
  Result r{Outcome::AllSent, 0, 0};
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

    File f = LittleFS.open(e.path, FILE_READ);
    if (!f) {  // unreadable: drop it rather than retry forever
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
    const int status = http.sendRequest("POST", &f, f.size());
    f.close();
    http.end();
    r.http_status = status;
    Serial.printf("upload %s -> %d\n", e.path.c_str(), status);

    if (status >= 200 && status < 300) {
      memo_queue::remove(e);
      ++r.sent;
      --remaining;
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

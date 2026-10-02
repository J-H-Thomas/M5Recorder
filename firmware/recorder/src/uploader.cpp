#include "uploader.h"

#include <HTTPClient.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_sntp.h>
#include <esp_timer.h>

#include "ca_certs.h"
#include "memo_queue.h"
#include "secrets.h"

namespace uploader {
namespace {

constexpr uint32_t CONNECT_TIMEOUT_MS = 8000;   // first attempt at each network
constexpr uint32_t RETRY_TIMEOUT_MS   = 12000;  // second attempt at one that timed out
constexpr uint32_t STALE_STATUS_MS    = 1000;   // ignore "not found" this soon after begin()
constexpr uint32_t NTP_TIMEOUT_MS     = 3000;   // clock unset: wait this long before uploading
constexpr uint32_t NTP_FINISH_MS      = 2000;   // clock set: wait at most this after uploading
constexpr int      TLS_CERT_VERIFY_FAILED = -0x2700;  // MBEDTLS_ERR_X509_CERT_VERIFY_FAILED
constexpr size_t   WAV_HEADER_BYTES   = 44;
constexpr uint32_t TLS_HANDSHAKE_S    = 10;
constexpr uint32_t TLS_TIMEOUT_S      = 15;
constexpr int32_t  HTTP_CONNECT_TIMEOUT_MS = 8000;
constexpr uint16_t HTTP_TIMEOUT_MS    = 15000;
// Per-request watchdog: 20 s plus 20 KB/s for the body (a 2-minute memo, about
// 3.8 MB, still gets over 3 minutes).
constexpr uint32_t WATCHDOG_BASE_MS   = 20000;
constexpr uint32_t WATCHDOG_MIN_BYTES_PER_MS = 20;
constexpr time_t   CLOCK_VALID_AFTER  = 1700000000;  // Nov 2023
constexpr int      NETWORK_COUNT      = sizeof(WIFI_NETWORKS) / sizeof(WIFI_NETWORKS[0]);

// The network that last worked, tried first next time (survives deep sleep).
RTC_DATA_ATTR int8_t last_network = -1;

// Latest disconnect reason from the Wi-Fi driver (wifi_err_reason_t), for the log.
volatile uint8_t last_reason = 0;

enum class Attempt { Connected, NotFound, Refused, TimedOut, Aborted };

// KEY1 interrupts an upload only after it has been seen released: a memo that
// ran to the 60 s cap arrives here with the button still held, and that
// press isn't a new one.
bool key_released = false;

bool keyDown(gpio_num_t key) {
  if (digitalRead(key) == HIGH) {
    key_released = true;
    return false;
  }
  return key_released;
}

void logDisconnects() {
  static bool registered = false;
  if (registered) { return; }
  registered = true;
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
    last_reason = info.wifi_sta_disconnected.reason;
    Serial.printf("wifi: disconnected, reason %u (%s)\n", last_reason,
                  WiFi.disconnectReasonName((wifi_err_reason_t)last_reason));
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
}

// Joins one network by name. The Wi-Fi driver scans for it itself and reports
// "no such network" after a couple of seconds, so an absent network costs
// that, not the full timeout.
Attempt tryNetwork(gpio_num_t key, const WifiNetwork& net, uint32_t timeout_ms) {
  const uint32_t t0 = millis();
  last_reason = 0;
  WiFi.begin(net.ssid, net.password);
  Attempt result = Attempt::TimedOut;
  while (millis() - t0 < timeout_ms) {
    const wl_status_t s = WiFi.status();
    if (s == WL_CONNECTED) {
      Serial.printf("wifi: connected to %s in %lu ms (RSSI %d)\n", net.ssid,
                    (unsigned long)(millis() - t0), WiFi.RSSI());
      return Attempt::Connected;
    }
    if (millis() - t0 > STALE_STATUS_MS && (s == WL_NO_SSID_AVAIL || s == WL_CONNECT_FAILED)) {
      result = s == WL_NO_SSID_AVAIL ? Attempt::NotFound : Attempt::Refused;
      break;
    }
    if (keyDown(key)) {
      result = Attempt::Aborted;
      break;
    }
    delay(20);
  }
  static const char* const names[] = {"connected", "not found", "refused", "timed out", "aborted"};
  Serial.printf("wifi: %s %s after %lu ms (last reason %u)\n", net.ssid, names[(int)result],
                (unsigned long)(millis() - t0), last_reason);
  // Reset the driver's connection state so the next attempt starts clean.
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  return result;
}

// Tries the network that worked last time first (so away from home the
// hotspot goes first, at home the home network), then the rest in
// WIFI_NETWORKS order. Then any that timed out (in range but slow, e.g. a
// phone hotspot still clearing the previous connection) get one longer try.
// (A scan-first approach was dropped: the Arduino core gives up on a scan
// after 20x the per-channel time, 2.4 s at 120 ms, and reported that as zero
// networks.)
bool connect(gpio_num_t key) {
  logDisconnects();
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // modem sleep throttles uploads badly; it's off only while awake anyway

  int order[NETWORK_COUNT];
  int n = 0;
  if (last_network >= 0 && last_network < NETWORK_COUNT) { order[n++] = last_network; }
  for (int i = 0; i < NETWORK_COUNT; ++i) {
    if (i != last_network) { order[n++] = i; }
  }
  bool timed_out[NETWORK_COUNT] = {};
  for (int k = 0; k < n; ++k) {
    const Attempt a = tryNetwork(key, WIFI_NETWORKS[order[k]], CONNECT_TIMEOUT_MS);
    if (a == Attempt::Connected) {
      last_network = order[k];
      return true;
    }
    if (a == Attempt::Aborted) { return false; }
    timed_out[k] = a == Attempt::TimedOut;
  }
  for (int k = 0; k < n; ++k) {
    if (!timed_out[k]) { continue; }
    Serial.printf("wifi: retrying %s\n", WIFI_NETWORKS[order[k]].ssid);
    const Attempt a = tryNetwork(key, WIFI_NETWORKS[order[k]], RETRY_TIMEOUT_MS);
    if (a == Attempt::Connected) {
      last_network = order[k];
      return true;
    }
    if (a == Attempt::Aborted) { return false; }
  }
  return false;
}

// NTP on every upload round: the clock keeps running through deep sleep, but
// on the internal RC oscillator, which drifts. Memos record time(nullptr) when
// they're made; the receiver also corrects them using X-Device-Now.
bool ntp_synced = false;

void startClockSync() {
  ntp_synced = false;
  configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
}

// Waits up to `ms` for the sync started above. sntp_get_sync_status() reports
// COMPLETED only once, so remember it.
bool waitClockSync(uint32_t ms) {
  const uint32_t start = millis();
  while (!ntp_synced && millis() - start < ms) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) { ntp_synced = true; break; }
    delay(50);
  }
  return ntp_synced;
}

void configureTls(WiFiClientSecure& tls, HTTPClient& http) {
  tls.setCACert(CA_CERTS);
  // The core's defaults (120 s handshake, 30 s reads) left the stick stuck on
  // "Sending..." for minutes on a hotspot with no internet.
  tls.setHandshakeTimeout(TLS_HANDSHAKE_S);
  tls.setTimeout(TLS_TIMEOUT_S);
  http.setReuse(true);  // one TLS handshake for the whole queue
  http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
}

void addCommonHeaders(HTTPClient& http, const String& device, const Telemetry& t, size_t queued) {
  const time_t now = time(nullptr);
  http.addHeader("Authorization", String("Bearer ") + MEMO_TOKEN);
  http.addHeader("X-Memo-Device", device);
  http.addHeader("X-Device-Now", String(now > CLOCK_VALID_AFTER ? (uint32_t)now : 0));
  if (t.battery_mv > 0) { http.addHeader("X-Battery-mV", String(t.battery_mv)); }
  if (t.battery_pct >= 0) { http.addHeader("X-Battery-Pct", String(t.battery_pct)); }
  http.addHeader("X-Charging", t.charging ? "1" : "0");
  http.addHeader("X-Queue", String((unsigned)queued));
  http.addHeader("X-Set-Aside", String((unsigned)memo_queue::badCount()));
  http.addHeader("X-Ignored", String((unsigned)t.ignored));
#if defined(L3B_OFF_IN_SLEEP) && L3B_OFF_IN_SLEEP
  http.addHeader("X-Firmware", FW_VERSION "-l3boff");  // experiment build: tell its readings apart in HA
#else
  http.addHeader("X-Firmware", FW_VERSION);
#endif
}

// Where to send heartbeats: the upload URL with /upload replaced.
String heartbeatUrl() {
  String url = UPLOAD_URL;
  const int at = url.lastIndexOf("/upload");
  return (at >= 0 ? url.substring(0, at) : url) + "/heartbeat";
}

// Negative HTTPClient codes: tell a certificate failure from no internet.
Outcome transportError(WiFiClientSecure& tls, int status) {
  char err[80];
  const int tls_error = tls.lastError(err, sizeof(err));
  Serial.printf("upload: transport error %d, TLS error %d (%s)\n", status, tls_error, err);
  return tls_error == TLS_CERT_VERIFY_FAILED ? Outcome::CertError : Outcome::NoInternet;
}

String memoId(const String& device, const memo_queue::Entry& e) {
  if (e.epoch == 0) { return device + "-" + e.seq; }  // queued before epochs existed
  char epoch[9];
  snprintf(epoch, sizeof(epoch), "%08lx", (unsigned long)e.epoch);
  return device + "-" + epoch + "-" + e.seq;
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

Result sendQueue(gpio_num_t key, Progress progress, Stuck on_stuck, const Telemetry& telemetry) {
  Result r{Outcome::AllSent, 0, 0, 0, 0, String(), 0, 0};
  key_released = false;
  size_t remaining = memo_queue::count();
  if (remaining == 0) { return r; }
  if (!connect(key)) {
    r.outcome = keyDown(key) ? Outcome::Interrupted : Outcome::NoWifi;
    return r;
  }
  r.network = WiFi.SSID();
  const time_t clock_before = time(nullptr);
  const uint32_t millis_before = millis();
  startClockSync();
  if (clock_before < CLOCK_VALID_AFTER) { waitClockSync(NTP_TIMEOUT_MS); }

  // Backstop for a request that blocks past every timeout below.
  esp_timer_handle_t watchdog = nullptr;
  if (on_stuck) {
    esp_timer_create_args_t args{};
    args.callback = [](void* fn) {
      Serial.println("upload: watchdog fired, forcing sleep");
      ((Stuck)fn)();
    };
    args.arg = (void*)on_stuck;
    args.name = "upload-wd";
    esp_timer_create(&args, &watchdog);
  }

  const String device = deviceId();
  WiFiClientSecure tls;
  HTTPClient http;
  configureTls(tls, http);

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
    if (f && size < WAV_HEADER_BYTES) {  // not even a header: can never be sent
      f.close();
      Serial.printf("upload: %s is only %u bytes, removing\n", e.path.c_str(), (unsigned)size);
      memo_queue::remove(e);
      --remaining;
      continue;
    }
    uint8_t* body = size ? (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM) : nullptr;
    const bool read_ok = body && f.read(body, size) == size;
    if (f) { f.close(); }
    if (!read_ok) {  // probably transient (memory, flash): keep it and try next round
      free(body);
      Serial.printf("upload: couldn't read %s, keeping it\n", e.path.c_str());
      r.outcome = Outcome::Failed;
      break;
    }
    http.begin(tls, UPLOAD_URL);
    // Queue as it will be once this memo is in: what's still waiting on the stick.
    addCommonHeaders(http, device, telemetry, remaining - 1);
    http.addHeader("Content-Type", "audio/wav");
    http.addHeader("X-Memo-Id", memoId(device, e));
    http.addHeader("X-Memo-Time", String(e.time));
    const uint32_t t0 = millis();
    if (watchdog) {
      esp_timer_start_once(watchdog, (WATCHDOG_BASE_MS + size / WATCHDOG_MIN_BYTES_PER_MS) * 1000ULL);
    }
    const int status = http.sendRequest("POST", body, size);
    if (watchdog) { esp_timer_stop(watchdog); }
    const uint32_t ms = millis() - t0;
    const String reply = status > 0 ? http.getString() : String();
    http.end();
    free(body);
    r.http_status = status;
    Serial.printf("upload %s -> %d %s, %u bytes in %lu ms (%.0f KB/s)\n", e.path.c_str(), status,
                  reply.c_str(), (unsigned)size, (unsigned long)ms, ms ? size / 1.024f / ms : 0.0f);

    if (status >= 200 && status < 300) {
      // Only the receiver's own answer counts: a 200 from something else in
      // the way (a misrouted proxy, a login page) mustn't delete the memo.
      if (reply.indexOf("\"queued\"") < 0 && reply.indexOf("\"duplicate\"") < 0) {
        r.outcome = Outcome::Failed;
        break;
      }
      memo_queue::remove(e);
      ++r.sent;
      --remaining;
      r.bytes += size;
      r.upload_ms += ms;
    } else if (status == 401) {
      r.outcome = Outcome::BadToken;
      break;
    } else if (status >= 400 && status < 500 && status != 408 && status != 429 &&
               reply.indexOf("\"detail\"") >= 0) {
      // The receiver itself (its errors carry "detail") will never take this
      // memo: bad format, too large, or its id was used for different audio.
      // Keep it, but out of the way, so it doesn't block the memos behind it.
      // A 4xx from anything else (e.g. the proxy's login) just fails the round.
      Serial.printf("upload: %s rejected (%d), set aside\n", e.path.c_str(), status);
      memo_queue::setAside(e);
      ++r.set_aside;
      --remaining;
    } else if (status < 0) {
      // HTTPClient's own codes (DNS or connect failure, timeout). A certificate
      // that doesn't verify shows up as one too; tell the two apart.
      r.outcome = transportError(tls, status);
      break;
    } else {
      r.outcome = Outcome::Failed;  // 5xx, 408, 429: try again later
      break;
    }
  }
  if (watchdog) { esp_timer_delete(watchdog); }
  if (r.outcome == Outcome::AllSent && memo_queue::count() > 0) { r.outcome = Outcome::Failed; }

  // Let the clock sync finish if it hasn't, and report how far it moved the
  // clock (the caller keeps its retry deadline on the same clock).
  if (waitClockSync(NTP_FINISH_MS)) {
    r.clock_shift = (int32_t)((time(nullptr) - clock_before) - (int32_t)((millis() - millis_before) / 1000));
    if (r.clock_shift > 2 || r.clock_shift < -2) { Serial.printf("clock: corrected by %ld s\n", (long)r.clock_shift); }
  }
  return r;
}

Result heartbeat(gpio_num_t key, const Telemetry& telemetry) {
  Result r{Outcome::AllSent, 0, 0, 0, 0, String(), 0, 0};
  key_released = false;
  if (!connect(key)) {
    r.outcome = keyDown(key) ? Outcome::Interrupted : Outcome::NoWifi;
    return r;
  }
  r.network = WiFi.SSID();
  const time_t clock_before = time(nullptr);
  const uint32_t millis_before = millis();
  startClockSync();
  if (clock_before < CLOCK_VALID_AFTER) { waitClockSync(NTP_TIMEOUT_MS); }

  WiFiClientSecure tls;
  HTTPClient http;
  configureTls(tls, http);
  http.begin(tls, heartbeatUrl());
  addCommonHeaders(http, deviceId(), telemetry, memo_queue::count());
  const int status = http.sendRequest("POST", (uint8_t*)nullptr, 0);
  const String reply = status > 0 ? http.getString() : String();
  http.end();
  r.http_status = status;
  Serial.printf("heartbeat -> %d %s\n", status, reply.c_str());
  if (status >= 200 && status < 300 && reply.indexOf("\"ok\"") >= 0) {
    r.outcome = Outcome::AllSent;
  } else if (status == 401) {
    r.outcome = Outcome::BadToken;
  } else if (status < 0) {
    r.outcome = transportError(tls, status);
  } else {
    r.outcome = Outcome::Failed;
  }
  if (waitClockSync(NTP_FINISH_MS)) {
    r.clock_shift = (int32_t)((time(nullptr) - clock_before) - (int32_t)((millis() - millis_before) / 1000));
  }
  return r;
}

void shutdown() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

}  // namespace uploader

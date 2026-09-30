// Copy to secrets.h (gitignored) and fill in.
#pragma once

// Wi-Fi networks. The one that worked last time is tried first, then these
// in order. The ESP32 only does 2.4 GHz; on the phone hotspot pick the
// 2.4 GHz band and WPA2.
struct WifiNetwork { const char* ssid; const char* password; };
static const WifiNetwork WIFI_NETWORKS[] = {
  {"phone-hotspot-ssid", "hotspot-password"},
  {"home-ssid", "home-password"},
};

// Receiver upload URL (through Pangolin) and its MEMO_TOKEN.
static const char UPLOAD_URL[] = "https://memos.example.com/upload";
static const char MEMO_TOKEN[] = "same-long-random-string-as-the-server";

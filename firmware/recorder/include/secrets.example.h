// Copy to secrets.h (gitignored) and fill in.
#pragma once

// Wi-Fi networks, tried in order. The ESP32 only does 2.4 GHz; on the phone
// hotspot pick the 2.4 GHz band and WPA2.
struct WifiNetwork { const char* ssid; const char* password; };
static const WifiNetwork WIFI_NETWORKS[] = {
  {"home-ssid", "home-password"},
  {"phone-hotspot-ssid", "hotspot-password"},
};

// Receiver upload URL (through Pangolin) and its MEMO_TOKEN.
static const char UPLOAD_URL[] = "https://memos.example.com/upload";
static const char MEMO_TOKEN[] = "same-long-random-string-as-the-server";

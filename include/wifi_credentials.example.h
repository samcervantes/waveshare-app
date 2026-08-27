#pragma once

// Copy this file to wifi_credentials.h (gitignored, so real credentials
// never get committed) and fill in your network's details. Used by
// clock_app.cpp to connect just long enough to sync real time via NTP.
#define WIFI_SSID "your-network-name"
#define WIFI_PASSWORD "your-network-password"

// Tried if WIFI_SSID isn't reachable - see wifi_status.cpp's fallback
// timeout logic. If your fallback network is open (no password), adjust
// the WiFi.begin() calls in wifi_status.cpp to drop the password
// argument - the WiFi app's Scan page shows each nearby network's auth
// type, which is the quickest way to check.
#define WIFI_SSID_FALLBACK "your-fallback-network-name"
#define WIFI_PASSWORD_FALLBACK "your-fallback-network-password"

#include "wifi_status.h"

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>

#include "wifi_credentials.h"

namespace {

constexpr uint32_t POLL_MS = 1000;

// America/Los_Angeles (Pacific Time) - handles PST/PDT DST transitions
// automatically. Update if this board moves elsewhere. (Only clock_app
// reads the resulting wall-clock time, but the sync itself lives here so
// it can happen in the background instead of blocking clock_app's UI.)
constexpr char TZ_STRING[] = "PST8PDT,M3.2.0/2,M11.1.0/2";
constexpr char NTP_SERVER1[] = "pool.ntp.org";
constexpr char NTP_SERVER2[] = "time.nist.gov";

// Any epoch time before this (~2023-11-14) means NTP hasn't landed yet -
// an unsynced ESP32 clock reads back near 1970. Cheap non-blocking stand-in
// for getLocalTime()'s blocking wait-and-check loop.
constexpr time_t SANE_EPOCH_THRESHOLD = 1700000000;

lv_obj_t *icon = nullptr;
lv_timer_t *poll_timer = nullptr;
bool ntp_configured = false;
bool time_synced = false;
uint32_t connect_started_ms = 0;
bool tried_fallback = false;
WifiNetwork active_network = WifiNetwork::PRIMARY;
int last_primary_wl_status = WL_IDLE_STATUS;

// Same dBm thresholds as wifi_app/bluetooth_app's signal meters, so
// "strong/medium/weak" means the same color everywhere in this project.
lv_color_t rssi_to_color(int rssi) {
  if (rssi >= -60) return lv_color_hex(0x30D158);  // strong
  if (rssi >= -75) return lv_color_hex(0xFFD60A);  // medium
  return lv_color_hex(0xFF453A);                   // weak
}

void poll(lv_timer_t * /*t*/) {
  bool connected = (WiFi.status() == WL_CONNECTED);
  lv_obj_set_style_text_color(icon, connected ? rssi_to_color(WiFi.RSSI()) : lv_color_hex(0x555555), 0);

  // One-shot fallback, not a retry loop: WiFi.begin() only ever gets
  // called again here once, when the primary network hasn't connected
  // within WIFI_PRIMARY_TIMEOUT_MS. An earlier version of this file called
  // WiFi.begin() repeatedly on a timer whenever disconnected, which
  // destabilized the WiFi driver badly enough to eventually crash the
  // board (see wifi_status_init's comment) - setAutoReconnect handles
  // ongoing reconnection to whichever network this successfully lands
  // on, so this only needs to fire at most once per boot.
  if (!connected && !tried_fallback && millis() - connect_started_ms > WIFI_PRIMARY_TIMEOUT_MS) {
    last_primary_wl_status = WiFi.status();
    tried_fallback = true;
    active_network = WifiNetwork::FALLBACK;
    // WPA2, confirmed via the WiFi app's Scan page (shows "[WPA2]" next
    // to WIFI_SSID_FALLBACK) - an earlier version of this tried it as an
    // open network instead, on the theory that a WL_DISCONNECTED status
    // that never resolved meant an auth-type mismatch, but the scan
    // result rules that out; the real culprit for a stalled connection
    // was more likely the -77dBm signal (borderline weak, see
    // rssi_to_color) plus this being a shared/guest network.
    WiFi.begin(WIFI_SSID_FALLBACK, WIFI_PASSWORD_FALLBACK);
    connect_started_ms = millis();
  }

  if (!connected) return;

  if (!ntp_configured) {
    ntp_configured = true;
    configTzTime(TZ_STRING, NTP_SERVER1, NTP_SERVER2);  // returns immediately; SNTP syncs in the background
  }

  if (!time_synced && time(nullptr) > SANE_EPOCH_THRESHOLD) {
    time_synced = true;
  }
}

}  // namespace

void wifi_status_init(lv_obj_t *icon_obj) {
  icon = icon_obj;

  // Connect once, then hand reconnection off to the IDF's own built-in
  // auto-reconnect instead of hand-rolling a retry loop: an earlier
  // version here called WiFi.disconnect()+WiFi.begin() on a timer every
  // ~20s whenever disconnected, which - confirmed via serial over a long
  // run - destabilized the WiFi driver badly enough to eventually crash
  // the whole board (Guru Meditation / Load access fault, then a crash
  // loop on reboot). setAutoReconnect is the officially supported
  // mechanism for "reconnect to whatever I last connected to" and doesn't
  // re-touch the STA config on every attempt the way our loop did.
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  connect_started_ms = millis();

  poll_timer = lv_timer_create(poll, POLL_MS, nullptr);
  poll(nullptr);
}

bool wifi_status_time_synced() {
  return time_synced;
}

WifiNetwork wifi_status_active_network() {
  return active_network;
}

bool wifi_status_fallback_attempted() {
  return tried_fallback;
}

int wifi_status_last_primary_wl_status() {
  return last_primary_wl_status;
}

uint32_t wifi_status_connect_started_ms() {
  return connect_started_ms;
}

void wifi_status_reconnect() {
  // Re-issues WiFi.begin() for whichever network is currently active,
  // without resetting tried_fallback/active_network - e.g. after wifi_app
  // borrows the radio for a scan (which calls WiFi.disconnect()) and
  // needs to hand it back in the same state it found it in, rather than
  // leaving the radio off/disconnected until the next reboot.
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  if (active_network == WifiNetwork::FALLBACK) {
    WiFi.begin(WIFI_SSID_FALLBACK, WIFI_PASSWORD_FALLBACK);
  } else {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
  connect_started_ms = millis();
}

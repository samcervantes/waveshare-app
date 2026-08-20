#include "wifi_status.h"

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>

#include "wifi_credentials.h"

namespace {

constexpr uint32_t POLL_MS = 1000;
constexpr uint32_t RECONNECT_COOLDOWN_MS = 10000;

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
uint32_t last_connect_attempt_ms = 0;
bool ntp_configured = false;
bool time_synced = false;

void poll(lv_timer_t * /*t*/) {
  bool connected = (WiFi.status() == WL_CONNECTED);
  lv_obj_set_style_text_color(icon, connected ? lv_color_hex(0x30D158) : lv_color_hex(0x555555), 0);

  if (!connected) {
    // WiFi.begin() is non-blocking (the handshake happens in the
    // background WiFi task) - this cooldown just avoids restarting the
    // attempt every poll while one's already in flight.
    uint32_t now_ms = millis();
    if (now_ms - last_connect_attempt_ms > RECONNECT_COOLDOWN_MS) {
      last_connect_attempt_ms = now_ms;
      WiFi.mode(WIFI_STA);
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
    return;
  }

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
  last_connect_attempt_ms = millis() - RECONNECT_COOLDOWN_MS;  // let the first poll connect right away
  poll_timer = lv_timer_create(poll, POLL_MS, nullptr);
  poll(nullptr);
}

bool wifi_status_time_synced() {
  return time_synced;
}

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
bool pinned = false;
WifiNetwork active_network = WifiNetwork::PRIMARY;
int last_primary_wl_status = WL_IDLE_STATUS;
int last_fallback_wl_status = WL_IDLE_STATUS;

// Same dBm thresholds as wifi_app/bluetooth_app's signal meters, so
// "strong/medium/weak" means the same color everywhere in this project.
lv_color_t rssi_to_color(int rssi) {
  if (rssi >= -60) return lv_color_hex(0x30D158);  // strong
  if (rssi >= -75) return lv_color_hex(0xFFD60A);  // medium
  return lv_color_hex(0xFF453A);                   // weak
}

// Issues WiFi.begin() for whichever network active_network currently is.
// Always disconnects first - reading the actual Arduino-ESP32 driver
// source (STAClass::connect() in STA.cpp) confirms WiFi.begin() only
// auto-disconnects if the STA is fully WL_CONNECTED; if it's still
// mid-handshake to a *different* network (exactly the state
// setAutoReconnect(true) keeps it in while it keeps retrying whatever
// was last configured, e.g. a Primary that's out of range), begin() for
// a new network gets silently rejected by the IDF driver instead (
// confirmed via serial: "sta is connecting, cannot set config" /
// ESP_ERR_WIFI_STATE) - our own active_network/pinned state updates
// regardless, so nothing here looked wrong, but the radio never actually
// got the new command. This is exactly why poll()'s automatic
// PRIMARY->FALLBACK alternation below silently never took effect on its
// own (Primary's own auto-reconnect kept the STA permanently "busy"),
// while manually visiting/leaving the WiFi app's Fox Hunt page worked
// around it purely by accident (it disconnects to scan, which happens to
// clear the same busy state).
//
// This does mean every caller here - including poll()'s own periodic
// retry - now does the disconnect()+begin() pairing that a documented
// past version of this file ran too frequently and crashed the board
// with (see wifi_status_init's comment). Mitigated, not eliminated: see
// WIFI_PRIMARY_TIMEOUT_MS's comment for why that interval was widened
// alongside this change. Worth watching for instability over a long
// uptime if this ever needs revisiting.
void begin_active_network(const char *reason) {
  WiFi.disconnect();
  if (active_network == WifiNetwork::FALLBACK) {
    Serial.printf("[wifi_status] %s -> FALLBACK (%s)\n", reason, WIFI_SSID_FALLBACK);
    WiFi.begin(WIFI_SSID_FALLBACK, WIFI_PASSWORD_FALLBACK);
  } else {
    Serial.printf("[wifi_status] %s -> PRIMARY (%s)\n", reason, WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
  connect_started_ms = millis();
}

void poll(lv_timer_t * /*t*/) {
  bool connected = (WiFi.status() == WL_CONNECTED);
  lv_obj_set_style_text_color(icon, connected ? rssi_to_color(WiFi.RSSI()) : lv_color_hex(0x555555), 0);

  static bool was_connected = false;
  if (connected != was_connected) {
    was_connected = connected;
    if (connected) {
      Serial.printf("[wifi_status] connected to %s, rssi=%d\n", WiFi.SSID().c_str(), WiFi.RSSI());
    } else {
      Serial.printf("[wifi_status] disconnected, status=%d\n", WiFi.status());
    }
  }

  if (connected) {
    // Keep resetting this while connected, rather than only setting it at
    // the moment a WiFi.begin() call goes out: if this network later
    // drops, the elapsed-time check below should give it a fresh
    // WIFI_PRIMARY_TIMEOUT_MS grace period to reconnect on its own (via
    // setAutoReconnect) before switching away, not treat a connection
    // that's been solid for hours as instantly overdue for a switch the
    // moment it blips.
    connect_started_ms = millis();
  } else if (millis() - connect_started_ms > WIFI_PRIMARY_TIMEOUT_MS) {
    // Alternates indefinitely between the two networks, not a one-shot
    // primary->fallback switch: neither network is guaranteed reachable
    // at any given time (e.g. the primary's fine at home but unreachable
    // elsewhere, and vice versa for the fallback), so if whichever one is
    // currently active hasn't connected within WIFI_PRIMARY_TIMEOUT_MS,
    // try the other one instead - and keep alternating for as long as
    // neither connects. begin_active_network's disconnect()+begin() pair
    // is the same shape as the hand-rolled loop that previously
    // destabilized the WiFi driver badly enough to crash the board (see
    // its own comment and wifi_status_init's) - kept survivable by
    // spacing, not by avoiding the pattern: each call here is at least
    // WIFI_PRIMARY_TIMEOUT_MS apart, never tighter.
    //
    // Unless pinned (see wifi_status_pin_network) - then skip the
    // alternation and just retry the same network again, since the user
    // has explicitly asked to stick with it rather than have a slow/
    // spotty connection keep getting abandoned for the other one.
    if (active_network == WifiNetwork::PRIMARY) {
      last_primary_wl_status = WiFi.status();
      if (!pinned) {
        tried_fallback = true;
        active_network = WifiNetwork::FALLBACK;
      }
    } else {
      last_fallback_wl_status = WiFi.status();
      if (!pinned) active_network = WifiNetwork::PRIMARY;
    }
    begin_active_network("poll retry");
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

int wifi_status_last_fallback_wl_status() {
  return last_fallback_wl_status;
}

uint32_t wifi_status_connect_started_ms() {
  return connect_started_ms;
}

void wifi_status_pin_network(WifiNetwork net) {
  pinned = true;
  active_network = net;
  if (net == WifiNetwork::FALLBACK) tried_fallback = true;
  begin_active_network("pin");
}

void wifi_status_reconnect() {
  // Re-issues WiFi.begin() for whichever network is currently active,
  // without resetting tried_fallback/active_network - e.g. after wifi_app
  // borrows the radio for a scan (which calls WiFi.disconnect()) and
  // needs to hand it back in the same state it found it in, rather than
  // leaving the radio off/disconnected until the next reboot.
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  begin_active_network("reconnect");
}

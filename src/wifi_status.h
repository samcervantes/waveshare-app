#pragma once

#include <lvgl.h>

// Keeps a best-effort WiFi connection alive in the background, falling
// back from the primary network to a secondary one if the primary isn't
// reachable, and reflects the current connection state as this icon's
// color. Call once after creating the icon object; icon's own visibility
// is the caller's responsibility (launcher.cpp parents it under
// launcher_root so it only shows on the home screen).
//
// Also configures NTP once connected and tracks whether it's completed -
// see wifi_status_time_synced. Everything here runs off a periodic timer
// and never blocks: clock_app used to connect and sync inline in
// on_open(), which froze all input for several seconds (the Arduino
// loop() that polls the button/touch is blocked for as long as on_open()
// runs) - polling a background flag instead means opening the clock is
// instant, and it just shows real time a moment later once sync finishes.
void wifi_status_init(lv_obj_t *icon);

// True once NTP has actually landed a real timestamp (not just "WiFi is
// connected") - see time_sync_threshold in wifi_status.cpp for how that's
// detected. Safe to call anytime after wifi_status_init.
bool wifi_status_time_synced();

// Which credential set (see wifi_credentials.h) wifi_status is currently
// trying or connected to - starts PRIMARY every boot, and moves to
// FALLBACK at most once, if PRIMARY hasn't connected within
// WIFI_PRIMARY_TIMEOUT_MS. Used by the WiFi app's connection-status page.
enum class WifiNetwork { PRIMARY, FALLBACK };
WifiNetwork wifi_status_active_network();

// How long wifi_status gives the primary network before trying the
// fallback - see wifi_status.cpp's poll().
constexpr uint32_t WIFI_PRIMARY_TIMEOUT_MS = 15000;

// True once the one-shot PRIMARY -> FALLBACK switch has happened this
// boot (whether or not the fallback itself has connected yet).
bool wifi_status_fallback_attempted();

// The raw WiFi.status() code (WL_CONNECTED, WL_NO_SSID_AVAIL, etc.)
// captured for the primary network right before giving up on it and
// switching to the fallback - only meaningful once
// wifi_status_fallback_attempted() is true. Lets the connection-status
// page show *why* the primary network didn't connect (out of range vs.
// wrong password vs. still associating), not just that it didn't.
int wifi_status_last_primary_wl_status();

// millis() timestamp of the most recent WiFi.begin() call (primary at
// boot, or fallback after the one-shot switch) - for computing elapsed/
// remaining time on the connection-status page.
uint32_t wifi_status_connect_started_ms();

// Re-issues WiFi.begin() for whichever network is currently active,
// without resetting the PRIMARY -> FALLBACK state. wifi_app borrows the
// radio for its network scan (WiFi.disconnect() + scanNetworks()); call
// this after closing that app to hand the radio back in the same state
// it was in before, rather than leaving it disconnected.
void wifi_status_reconnect();

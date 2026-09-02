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
// trying or connected to - starts PRIMARY every boot. If whichever one is
// active hasn't connected within WIFI_PRIMARY_TIMEOUT_MS, this alternates
// to the other one and keeps alternating for as long as neither connects
// (e.g. primary reachable at home but not elsewhere, and vice versa for
// the fallback) - not a one-way, one-shot switch. Used by the WiFi app's
// connection-status page.
enum class WifiNetwork { PRIMARY, FALLBACK };
WifiNetwork wifi_status_active_network();

// How long wifi_status gives whichever network is active before trying
// the other one - see wifi_status.cpp's poll().
constexpr uint32_t WIFI_PRIMARY_TIMEOUT_MS = 15000;

// True once the fallback has been tried at least once this boot (whether
// or not it's currently active/connected) - used only to distinguish
// "haven't tried it yet" from later states on the connection-status page.
bool wifi_status_fallback_attempted();

// The raw WiFi.status() code (WL_CONNECTED, WL_NO_SSID_AVAIL, etc.)
// captured for each network right before giving up on it (for now) and
// switching to the other one. Lets the connection-status page show *why*
// a network didn't connect (out of range vs. wrong password vs. still
// associating), not just that it didn't. wifi_status_last_primary_wl_status
// is only meaningful once wifi_status_active_network() has left PRIMARY
// at least once; wifi_status_last_fallback_wl_status likewise for FALLBACK.
int wifi_status_last_primary_wl_status();
int wifi_status_last_fallback_wl_status();

// millis() timestamp of the most recent WiFi.begin() call, or of the most
// recent poll() tick observed as connected (see poll()'s comment on why
// it keeps resetting while connected) - for computing elapsed/remaining
// time on the connection-status page.
uint32_t wifi_status_connect_started_ms();

// Re-issues WiFi.begin() for whichever network is currently active,
// without resetting the PRIMARY/FALLBACK alternation state. wifi_app
// borrows the radio for its network scan (WiFi.disconnect() +
// scanNetworks()); call this after closing that app to hand the radio
// back in the same state it was in before, rather than leaving it
// disconnected.
void wifi_status_reconnect();

// Pins connection attempts to one specific network, stopping the
// automatic PRIMARY/FALLBACK alternation - poll() will keep retrying
// just this one (re-issuing WiFi.begin() every WIFI_PRIMARY_TIMEOUT_MS
// as usual) instead of giving up and trying the other one. Used by the
// WiFi app's Scan page: tapping a row for a network matching
// WIFI_SSID/WIFI_SSID_FALLBACK pins to it, so a spotty network doesn't
// keep getting abandoned mid-attempt in favor of the other one. Runtime
// state only (resets to auto-alternating on reboot); stays pinned across
// closing/reopening the WiFi app until a different network is pinned -
// there's no explicit unpin, since nothing here currently needs to go
// back to auto-alternating once a preference's been set.
void wifi_status_pin_network(WifiNetwork net);

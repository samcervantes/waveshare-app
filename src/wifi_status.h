#pragma once

#include <lvgl.h>

// Keeps a best-effort WiFi connection alive in the background - retries
// periodically whenever disconnected (e.g. after wifi_app borrows the
// radio and turns it off when it's done) - and reflects the current
// connection state as this icon's color. Call once after creating the
// icon object; icon's own visibility is the caller's responsibility
// (launcher.cpp parents it under launcher_root so it only shows on the
// home screen).
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

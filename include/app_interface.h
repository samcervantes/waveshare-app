#pragma once

#include <lvgl.h>

// The contract every launcher app implements. See CLAUDE.md for the full
// "how to add a new app" walkthrough - the short version is: copy
// src/apps/counter_app.cpp/.h, rename things, register it in
// src/app_registry.cpp.
struct AppDescriptor {
  // Shown under the icon on the home screen. Keep it short - the icons are
  // small (2 per page on a 172px-wide screen).
  const char *name;

  // An LVGL symbol macro (LV_SYMBOL_...) or a short UTF-8 string used as
  // the icon glyph, and the background color of the icon tile.
  const char *icon_symbol;
  lv_color_t icon_color;

  // Builds the app's UI as children of `parent`, a full-screen container
  // the launcher creates for it. Called once each time the app is opened.
  void (*on_open)(lv_obj_t *parent);

  // Called once when the user backs out to the launcher (long press).
  // The launcher deletes `parent` (and thus all LVGL widgets under it)
  // right after this returns, so on_close only needs to clean up things
  // that outlive widget deletion - e.g. lv_timer_t timers you created, or
  // hardware you reconfigured (see rgb_app for an example).
  void (*on_close)();

  // Optional: called on a short press while this app is in the
  // foreground, for whatever per-app interaction makes sense. Set to
  // nullptr if the app doesn't use short press.
  void (*on_short_press)();
};

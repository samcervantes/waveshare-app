#include "clock_app.h"

#include <Arduino.h>
#include <lvgl.h>
#include <time.h>

#include "config.h"
#include "wifi_status.h"

namespace {

// Digit colors to cycle through on short press.
constexpr uint32_t COLORS[] = {0xFFFFFF, 0x0A84FF, 0x30D158, 0xFF9F0A, 0xFF375F};
constexpr size_t COLOR_COUNT = sizeof(COLORS) / sizeof(COLORS[0]);

const char *const WEEKDAYS[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char *const MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

lv_obj_t *time_label = nullptr;
lv_obj_t *meridiem_label = nullptr;
lv_obj_t *seconds_label = nullptr;
lv_obj_t *date_label = nullptr;
lv_timer_t *tick_timer = nullptr;
size_t color_idx = 0;

void apply_color() {
  lv_color_t c = lv_color_hex(COLORS[color_idx]);
  lv_obj_set_style_text_color(time_label, c, 0);
  lv_obj_set_style_text_color(meridiem_label, c, 0);
}

// hour24 is 0-23 either way, whether it came from the real synced clock or
// (see refresh_time) the seconds-since-boot fallback - keeps the 12h/AM
// and PM formatting in one place instead of duplicated per source.
void set_time_labels(int hour24, int minute, int second) {
  int hour12 = hour24 % 12;
  if (hour12 == 0) hour12 = 12;
  lv_label_set_text_fmt(time_label, "%d:%02d", hour12, minute);
  lv_label_set_text(meridiem_label, hour24 < 12 ? "AM" : "PM");
  lv_label_set_text_fmt(seconds_label, ":%02d", second);
}

void refresh_time(lv_timer_t * /*t*/) {
  if (wifi_status_time_synced()) {
    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    set_time_labels(timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    lv_label_set_text_fmt(date_label, "%s, %s %d", WEEKDAYS[timeinfo.tm_wday], MONTHS[timeinfo.tm_mon],
                           timeinfo.tm_mday);
  } else {
    // WiFi/NTP sync (see wifi_status.cpp) hasn't landed yet this boot -
    // fall back to seconds-since-boot wrapped to a 24h face rather than
    // blocking here waiting for it; this label will start showing the
    // real date the moment wifi_status_time_synced() flips true.
    lv_label_set_text(date_label, "Syncing time...");
    uint32_t s = millis() / 1000;
    set_time_labels((s / 3600) % 24, (s / 60) % 60, s % 60);
  }
}

void on_open(lv_obj_t *parent) {
  color_idx = 0;

  // Full screen width and centered, rather than sized to its own content -
  // besides being as big as this project's fonts get (48 is the largest
  // Montserrat size built in, see lv_conf.h), a fixed-width box keeps the
  // digits from visibly shifting left/right as the hour changes between
  // 1 and 2 characters (e.g. "1:05" vs "12:59").
  time_label = lv_label_create(parent);
  lv_obj_set_style_text_font(time_label, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_align(time_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(time_label, LCD_PANEL_WIDTH);

  // AM/PM + seconds share a row below the time instead of AM/PM squeezing
  // in beside the big digits - this is secondary information, the time
  // itself is the star of the screen now.
  lv_obj_t *sub_row = lv_obj_create(parent);
  lv_obj_remove_style_all(sub_row);
  lv_obj_set_size(sub_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(sub_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(sub_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(sub_row, 8, 0);
  lv_obj_clear_flag(sub_row, LV_OBJ_FLAG_SCROLLABLE);

  // Created in this order (seconds, then AM/PM) since a flex row lays
  // children out left-to-right - seconds on the left, AM/PM on the right.
  seconds_label = lv_label_create(sub_row);
  lv_obj_set_style_text_font(seconds_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(seconds_label, lv_color_white(), 0);

  meridiem_label = lv_label_create(sub_row);
  lv_obj_set_style_text_font(meridiem_label, &lv_font_montserrat_20, 0);
  apply_color();

  date_label = lv_label_create(parent);
  // Bigger and white, not the dim gray this used to be - reported hard to
  // read (see CLAUDE.md's text-contrast note). Full screen width + centered
  // text, same technique as time_label, rather than a content-sized box
  // aligned relative to sub_row - keeps it reliably centered regardless of
  // how wide any particular date string happens to render.
  lv_obj_set_style_text_font(date_label, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(date_label, lv_color_white(), 0);
  lv_obj_set_style_text_align(date_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(date_label, LCD_PANEL_WIDTH);

  lv_obj_t *hint = lv_label_create(parent);
  lv_label_set_text(hint, ACTION_WORD ": color  |  " HOME_HINT);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(hint, lv_color_hex(0x555555), 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -16);

  // Real text content has to exist before the align calls below, since
  // sub_row and date_label are sized to their content - aligning first
  // would anchor them to empty-label sizes.
  refresh_time(nullptr);

  lv_obj_align(time_label, LV_ALIGN_CENTER, 0, -30);
  lv_obj_align_to(sub_row, time_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
  lv_obj_align_to(date_label, sub_row, LV_ALIGN_OUT_BOTTOM_MID, 0, 12);

  tick_timer = lv_timer_create(refresh_time, 1000, nullptr);
}

void on_close() {
  if (tick_timer) {
    lv_timer_del(tick_timer);
    tick_timer = nullptr;
  }
  time_label = meridiem_label = seconds_label = date_label = nullptr;
}

void on_short_press() {
  color_idx = (color_idx + 1) % COLOR_COUNT;
  apply_color();
}

}  // namespace

const AppDescriptor clock_app = {
    .name = "Clock",
    .icon_symbol = LV_SYMBOL_LOOP,
    .icon_color = lv_color_hex(0x0A84FF),
    .on_open = on_open,
    .on_close = on_close,
    .on_short_press = on_short_press,
};

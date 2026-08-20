#include "clock_app.h"

#include <Arduino.h>
#include <WiFi.h>
#include <lvgl.h>
#include <time.h>

#include "wifi_credentials.h"

namespace {

// America/Los_Angeles (Pacific Time) - handles PST/PDT DST transitions
// automatically. Update if this board moves elsewhere.
constexpr char TZ_STRING[] = "PST8PDT,M3.2.0/2,M11.1.0/2";
constexpr char NTP_SERVER1[] = "pool.ntp.org";
constexpr char NTP_SERVER2[] = "time.nist.gov";
constexpr uint32_t WIFI_TIMEOUT_MS = 15000;
constexpr uint32_t NTP_TIMEOUT_MS = 10000;

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

// This board has no battery-backed RTC, so real time only exists once
// synced over WiFi/NTP. Both flags are deliberately sticky for the whole
// boot session (not reset in on_close/on_open) - a successful sync keeps
// ticking correctly off the chip's internal clock without WiFi, and a
// failed attempt isn't retried on every reopen (that would mean eating
// the ~15s connect timeout each time); power-cycle the board to retry.
bool time_synced = false;
bool sync_attempted = false;

void apply_color() {
  lv_color_t c = lv_color_hex(COLORS[color_idx]);
  lv_obj_set_style_text_color(time_label, c, 0);
  lv_obj_set_style_text_color(meridiem_label, c, 0);
}

void sync_time() {
  sync_attempted = true;
  lv_label_set_text(date_label, "Connecting to WiFi...");
  lv_timer_handler();  // flush status text to the panel before blocking below

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    delay(100);
  }

  if (WiFi.status() != WL_CONNECTED) {
    lv_label_set_text(date_label, "No WiFi - showing uptime");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    return;
  }

  lv_label_set_text(date_label, "Syncing time...");
  lv_timer_handler();

  configTzTime(TZ_STRING, NTP_SERVER1, NTP_SERVER2);

  struct tm timeinfo;
  bool got_time = getLocalTime(&timeinfo, NTP_TIMEOUT_MS);

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  if (!got_time) {
    lv_label_set_text(date_label, "Time sync failed - showing uptime");
    return;
  }

  time_synced = true;
}

// hour24 is 0-23 either way, whether it came from a real synced clock or
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
  if (time_synced) {
    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    set_time_labels(timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    lv_label_set_text_fmt(date_label, "%s, %s %d", WEEKDAYS[timeinfo.tm_wday], MONTHS[timeinfo.tm_mon],
                           timeinfo.tm_mday);
  } else {
    // No successful sync yet (or this boot never had WiFi) - fall back to
    // seconds-since-boot wrapped to a 24h face, same as before this app
    // could reach real time at all. date_label is left alone here: it's
    // already showing whatever status sync_time() set.
    uint32_t s = millis() / 1000;
    set_time_labels((s / 3600) % 24, (s / 60) % 60, s % 60);
  }
}

void on_open(lv_obj_t *parent) {
  color_idx = 0;

  // time_label + meridiem_label share a row so "PM" sits right beside the
  // big digits rather than needing hand-tuned pixel offsets.
  lv_obj_t *time_row = lv_obj_create(parent);
  lv_obj_remove_style_all(time_row);
  lv_obj_set_size(time_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(time_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(time_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(time_row, 4, 0);
  lv_obj_clear_flag(time_row, LV_OBJ_FLAG_SCROLLABLE);

  time_label = lv_label_create(time_row);
  lv_obj_set_style_text_font(time_label, &lv_font_montserrat_48, 0);

  meridiem_label = lv_label_create(time_row);
  lv_obj_set_style_text_font(meridiem_label, &lv_font_montserrat_20, 0);
  lv_obj_set_style_pad_bottom(meridiem_label, 6, 0);  // nudge toward the big digits' baseline
  apply_color();

  seconds_label = lv_label_create(parent);
  lv_obj_set_style_text_font(seconds_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(seconds_label, lv_color_hex(0x666666), 0);

  date_label = lv_label_create(parent);
  lv_obj_set_style_text_font(date_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(date_label, lv_color_hex(0x888888), 0);

  lv_obj_t *hint = lv_label_create(parent);
  lv_label_set_text(hint, "short: color  |  hold: home");
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(hint, lv_color_hex(0x555555), 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -16);

  // Real text content has to exist before the align calls below, since
  // time_row and its neighbors are sized to their content (LV_SIZE_CONTENT)
  // - aligning first would anchor them to empty-label sizes.
  refresh_time(nullptr);
  if (!sync_attempted) sync_time();
  refresh_time(nullptr);

  lv_obj_align(time_row, LV_ALIGN_CENTER, 0, -20);
  lv_obj_align_to(seconds_label, time_row, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);
  lv_obj_align_to(date_label, seconds_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

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

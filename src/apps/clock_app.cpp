#include "clock_app.h"

#include <Arduino.h>
#include <lvgl.h>

namespace {

// Digit colors to cycle through on short press.
constexpr uint32_t COLORS[] = {0xFFFFFF, 0x0A84FF, 0x30D158, 0xFF9F0A, 0xFF375F};
constexpr size_t COLOR_COUNT = sizeof(COLORS) / sizeof(COLORS[0]);

lv_obj_t *time_label = nullptr;
lv_timer_t *tick_timer = nullptr;
size_t color_idx = 0;

// Simple uptime clock - no RTC or WiFi/NTP on this build, so "time" is
// just seconds since boot, wrapped to a 24h clock face.
void refresh_time(lv_timer_t * /*t*/) {
  uint32_t s = millis() / 1000;
  uint32_t hh = (s / 3600) % 24;
  uint32_t mm = (s / 60) % 60;
  uint32_t ss = s % 60;
  lv_label_set_text_fmt(time_label, "%02u:%02u:%02u", hh, mm, ss);
}

void on_open(lv_obj_t *parent) {
  color_idx = 0;

  lv_obj_t *title = lv_label_create(parent);
  lv_label_set_text(title, "Clock");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0x888888), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

  time_label = lv_label_create(parent);
  lv_obj_set_style_text_font(time_label, &lv_font_montserrat_32, 0);
  lv_obj_set_style_text_color(time_label, lv_color_hex(COLORS[color_idx]), 0);
  lv_obj_center(time_label);

  lv_obj_t *hint = lv_label_create(parent);
  lv_label_set_text(hint, "short: color  |  hold: home");
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(hint, lv_color_hex(0x555555), 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -16);

  refresh_time(nullptr);
  tick_timer = lv_timer_create(refresh_time, 1000, nullptr);
}

void on_close() {
  if (tick_timer) {
    lv_timer_del(tick_timer);
    tick_timer = nullptr;
  }
  time_label = nullptr;
}

void on_short_press() {
  color_idx = (color_idx + 1) % COLOR_COUNT;
  lv_obj_set_style_text_color(time_label, lv_color_hex(COLORS[color_idx]), 0);
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

#include "stopwatch_app.h"

#include <Arduino.h>
#include <lvgl.h>

#include "config.h"

namespace {

lv_obj_t *state_label = nullptr;
lv_obj_t *time_label = nullptr;
lv_timer_t *tick_timer = nullptr;

bool running = false;
uint32_t accumulated_ms = 0;  // time banked from previous start/stop cycles
uint32_t start_ms = 0;        // millis() when the current run began

uint32_t elapsed_ms() {
  return accumulated_ms + (running ? millis() - start_ms : 0);
}

void refresh(lv_timer_t * /*t*/) {
  uint32_t e = elapsed_ms();
  uint32_t mm = e / 60000;
  uint32_t ss = (e / 1000) % 60;
  uint32_t ds = (e / 100) % 10;  // tenths
  lv_label_set_text_fmt(time_label, "%02u:%02u.%u", mm, ss, ds);
}

void on_open(lv_obj_t *parent) {
  running = false;
  accumulated_ms = 0;

  state_label = lv_label_create(parent);
  lv_label_set_text(state_label, "Stopped");
  lv_obj_set_style_text_font(state_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(state_label, lv_color_hex(0x888888), 0);
  lv_obj_align(state_label, LV_ALIGN_TOP_MID, 0, 16);

  time_label = lv_label_create(parent);
  lv_obj_set_style_text_font(time_label, &lv_font_montserrat_32, 0);
  lv_obj_set_style_text_color(time_label, lv_color_hex(0xFF9F0A), 0);
  lv_obj_center(time_label);

  lv_obj_t *hint = lv_label_create(parent);
  lv_label_set_text(hint, ACTION_WORD ": start/stop  |  " HOME_HINT);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(hint, lv_color_hex(0x555555), 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -16);

  refresh(nullptr);
  tick_timer = lv_timer_create(refresh, 100, nullptr);
}

void on_close() {
  if (tick_timer) {
    lv_timer_del(tick_timer);
    tick_timer = nullptr;
  }
  state_label = nullptr;
  time_label = nullptr;
}

void on_short_press() {
  if (running) {
    accumulated_ms += millis() - start_ms;
    running = false;
    lv_label_set_text(state_label, "Stopped");
  } else {
    start_ms = millis();
    running = true;
    lv_label_set_text(state_label, "Running");
  }
}

}  // namespace

const AppDescriptor stopwatch_app = {
    .name = "Stopwatch",
    .icon_symbol = LV_SYMBOL_PLAY,
    .icon_color = lv_color_hex(0xFF9F0A),
    .on_open = on_open,
    .on_close = on_close,
    .on_short_press = on_short_press,
};

#include "breathe_app.h"

#include <lvgl.h>

#include "config.h"

namespace {

constexpr uint32_t INHALE_MS = 4000;
constexpr uint32_t EXHALE_MS = 6000;

// The bar runs nearly the full screen height (just a small top/bottom
// margin) and sits toward the left edge, leaving a column on the right for
// the title/phase/hint text - there's no room left above or below the bar
// for them once it's this tall.
constexpr lv_coord_t BAR_WIDTH = 48;
constexpr lv_coord_t BAR_X = 14;
constexpr lv_coord_t BAR_TOP_MARGIN = 20;
constexpr lv_coord_t BAR_BOTTOM_MARGIN = 20;
constexpr lv_coord_t BAR_HEIGHT = LCD_PANEL_HEIGHT - BAR_TOP_MARGIN - BAR_BOTTOM_MARGIN;

lv_obj_t *bar = nullptr;
lv_obj_t *phase_label = nullptr;
lv_timer_t *phase_timer = nullptr;
bool inhaling = true;

// The bar's built-in value animation (driven by lv_bar_set_value's
// LV_ANIM_ON) runs over the style anim_time set here, so this both starts
// the visual fill/drain and re-arms its duration for the new phase.
void start_phase() {
  uint32_t duration = inhaling ? INHALE_MS : EXHALE_MS;
  lv_obj_set_style_anim_time(bar, duration, LV_PART_MAIN);
  lv_bar_set_value(bar, inhaling ? 100 : 0, LV_ANIM_ON);
  lv_label_set_text(phase_label, inhaling ? "Inhale" : "Exhale");
}

void phase_timer_cb(lv_timer_t *t) {
  inhaling = !inhaling;
  start_phase();
  lv_timer_set_period(t, inhaling ? INHALE_MS : EXHALE_MS);
}

void on_open(lv_obj_t *parent) {
  // Vertical bar: lv_bar auto-detects orientation from height > width, and
  // fills from the bottom up toward the max value, which matches "rises on
  // inhale" for free - no manual axis flipping needed. It runs nearly the
  // full screen height, so all the text lives in the column to its right
  // instead of above/below it.
  bar = lv_bar_create(parent);
  lv_obj_set_size(bar, BAR_WIDTH, BAR_HEIGHT);
  lv_obj_set_pos(bar, BAR_X, BAR_TOP_MARGIN);
  lv_bar_set_range(bar, 0, 100);
  lv_obj_set_style_bg_color(bar, lv_color_hex(0x2C2C2E), LV_PART_MAIN);
  // A thick, light border on the track (inset from the indicator via
  // pad_all) so the top of the bar - and how much headroom is left - stays
  // clearly visible even when the indicator is nearly full, instead of
  // just fading into the black screen background.
  lv_obj_set_style_border_width(bar, 4, LV_PART_MAIN);
  lv_obj_set_style_border_color(bar, lv_color_hex(0x8E8E93), LV_PART_MAIN);
  lv_obj_set_style_border_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_all(bar, 5, LV_PART_MAIN);
  lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(bar, lv_color_hex(0x5AC8FA), LV_PART_INDICATOR);
  lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
  lv_bar_set_value(bar, 0, LV_ANIM_OFF);

  lv_coord_t text_x = BAR_X + BAR_WIDTH + 12;

  lv_obj_t *title = lv_label_create(parent);
  lv_label_set_text(title, "Breathe");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0x888888), 0);
  lv_obj_set_pos(title, text_x, BAR_TOP_MARGIN);

  phase_label = lv_label_create(parent);
  lv_obj_set_style_text_font(phase_label, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(phase_label, lv_color_hex(0x5AC8FA), 0);
  lv_obj_align(phase_label, LV_ALIGN_LEFT_MID, text_x, 0);

  lv_obj_t *hint = lv_label_create(parent);
  lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(hint, LCD_PANEL_WIDTH - text_x - 4);
  lv_label_set_text(hint, HOME_HINT);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(hint, lv_color_hex(0x555555), 0);
  lv_obj_set_pos(hint, text_x, LCD_PANEL_HEIGHT - BAR_BOTTOM_MARGIN - 30);

  inhaling = true;
  start_phase();
  phase_timer = lv_timer_create(phase_timer_cb, INHALE_MS, nullptr);
}

void on_close() {
  if (phase_timer) {
    lv_timer_del(phase_timer);
    phase_timer = nullptr;
  }
  bar = nullptr;
  phase_label = nullptr;
}

}  // namespace

const AppDescriptor breathe_app = {
    .name = "Breathe",
    .icon_symbol = LV_SYMBOL_TINT,
    .icon_color = lv_color_hex(0x5AC8FA),
    .on_open = on_open,
    .on_close = on_close,
    .on_short_press = nullptr,
};

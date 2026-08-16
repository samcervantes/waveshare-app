#include "bounce_app.h"

#include <lvgl.h>
#include <stdlib.h>

#include "config.h"

namespace {

constexpr uint32_t COLORS[] = {0xFF3B30, 0x30D158, 0x0A84FF, 0xFFD60A, 0xBF5AF2, 0x64D2FF};
constexpr size_t COLOR_COUNT = sizeof(COLORS) / sizeof(COLORS[0]);
constexpr int SPEEDS[] = {2, 4, 6};
constexpr size_t SPEED_COUNT = sizeof(SPEEDS) / sizeof(SPEEDS[0]);

constexpr lv_coord_t BALL_SIZE = 24;
constexpr lv_coord_t AREA_TOP = 36;    // below the title
constexpr lv_coord_t AREA_BOTTOM = 296;  // above the hint text
constexpr lv_coord_t AREA_LEFT = 0;
constexpr lv_coord_t AREA_RIGHT = LCD_PANEL_WIDTH;

lv_obj_t *ball = nullptr;
lv_timer_t *tick_timer = nullptr;
int x, y, dx, dy;
size_t color_idx = 0;
size_t speed_idx = 1;

void move_ball(lv_timer_t * /*t*/) {
  x += dx;
  y += dy;

  bool bounced = false;
  if (x <= AREA_LEFT) {
    x = AREA_LEFT;
    dx = -dx;
    bounced = true;
  } else if (x >= AREA_RIGHT - BALL_SIZE) {
    x = AREA_RIGHT - BALL_SIZE;
    dx = -dx;
    bounced = true;
  }
  if (y <= AREA_TOP) {
    y = AREA_TOP;
    dy = -dy;
    bounced = true;
  } else if (y >= AREA_BOTTOM - BALL_SIZE) {
    y = AREA_BOTTOM - BALL_SIZE;
    dy = -dy;
    bounced = true;
  }

  if (bounced) {
    color_idx = (color_idx + 1) % COLOR_COUNT;
    lv_obj_set_style_bg_color(ball, lv_color_hex(COLORS[color_idx]), 0);
  }

  lv_obj_set_pos(ball, x, y);
}

void on_open(lv_obj_t *parent) {
  lv_obj_t *title = lv_label_create(parent);
  lv_label_set_text(title, "Bounce");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0x888888), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

  ball = lv_obj_create(parent);
  lv_obj_set_size(ball, BALL_SIZE, BALL_SIZE);
  lv_obj_set_style_radius(ball, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(ball, 0, 0);
  lv_obj_clear_flag(ball, LV_OBJ_FLAG_SCROLLABLE);

  color_idx = 0;
  speed_idx = 1;
  x = AREA_LEFT + 10;
  y = AREA_TOP + 10;
  dx = SPEEDS[speed_idx];
  dy = SPEEDS[speed_idx] - 1;
  lv_obj_set_style_bg_color(ball, lv_color_hex(COLORS[color_idx]), 0);
  lv_obj_set_pos(ball, x, y);

  lv_obj_t *hint = lv_label_create(parent);
  lv_label_set_text(hint, "short: speed  |  hold: home");
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(hint, lv_color_hex(0x555555), 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -16);

  tick_timer = lv_timer_create(move_ball, 30, nullptr);
}

void on_close() {
  if (tick_timer) {
    lv_timer_del(tick_timer);
    tick_timer = nullptr;
  }
  ball = nullptr;
}

void on_short_press() {
  speed_idx = (speed_idx + 1) % SPEED_COUNT;
  int s = SPEEDS[speed_idx];
  dx = (dx < 0 ? -s : s);
  dy = (dy < 0 ? -s : s);
}

}  // namespace

const AppDescriptor bounce_app = {
    .name = "Bounce",
    .icon_symbol = LV_SYMBOL_SHUFFLE,
    .icon_color = lv_color_hex(0xFF375F),
    .on_open = on_open,
    .on_close = on_close,
    .on_short_press = on_short_press,
};

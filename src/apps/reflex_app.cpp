#include "reflex_app.h"

#include <Arduino.h>
#include <lvgl.h>

#include "config.h"

namespace {

constexpr lv_coord_t CURSOR_H = 8;
constexpr lv_coord_t ZONE_W = LCD_PANEL_WIDTH;
constexpr lv_coord_t ZONE_H_START = 60;
constexpr lv_coord_t ZONE_H_MIN = 24;
constexpr lv_coord_t ZONE_H_STEP = 4;
constexpr float CURSOR_SPEED_START = 3.0f;
constexpr float CURSOR_SPEED_MAX = 8.0f;
constexpr float CURSOR_SPEED_STEP = 0.3f;
constexpr int START_LIVES = 3;

constexpr lv_coord_t AREA_TOP = 44;      // below the score/lives label
constexpr lv_coord_t AREA_BOTTOM = 296;  // above the hint text

enum class State { PLAYING, GAME_OVER };

lv_obj_t *status_label = nullptr;
lv_obj_t *hint_label = nullptr;
lv_obj_t *zone = nullptr;
lv_obj_t *cursor = nullptr;
lv_timer_t *tick_timer = nullptr;

State state = State::PLAYING;
float cursor_y;
float cursor_speed;
int cursor_dir;
lv_coord_t zone_h;
lv_coord_t zone_top;
int score;
int lives;

void update_status() {
  lv_label_set_text_fmt(status_label, "Score: %d   Lives: %d", score, lives);
}

void place_zone() {
  zone_top = random(AREA_TOP, AREA_BOTTOM - zone_h);
  lv_obj_set_pos(zone, 0, zone_top);
  lv_obj_set_size(zone, ZONE_W, zone_h);
}

void reset_game() {
  state = State::PLAYING;
  score = 0;
  lives = START_LIVES;
  zone_h = ZONE_H_START;
  cursor_speed = CURSOR_SPEED_START;
  cursor_dir = 1;
  cursor_y = AREA_TOP;

  update_status();
  lv_label_set_text(hint_label, "short: stop it in the zone  |  hold: home");
  place_zone();
  lv_obj_set_pos(cursor, 0, static_cast<lv_coord_t>(cursor_y));
}

void die() {
  state = State::GAME_OVER;
  lv_label_set_text_fmt(status_label, "Game Over - Score %d", score);
  lv_label_set_text(hint_label, "short: retry  |  hold: home");
}

void game_tick(lv_timer_t * /*t*/) {
  if (state != State::PLAYING) return;

  cursor_y += cursor_speed * cursor_dir;
  if (cursor_y <= AREA_TOP) {
    cursor_y = AREA_TOP;
    cursor_dir = 1;
  } else if (cursor_y >= AREA_BOTTOM - CURSOR_H) {
    cursor_y = AREA_BOTTOM - CURSOR_H;
    cursor_dir = -1;
  }

  lv_obj_set_pos(cursor, 0, static_cast<lv_coord_t>(cursor_y));
}

void on_open(lv_obj_t *parent) {
  status_label = lv_label_create(parent);
  lv_obj_set_style_text_font(status_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(status_label, lv_color_hex(0xCCCCCC), 0);
  lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 12);

  zone = lv_obj_create(parent);
  lv_obj_set_style_bg_color(zone, lv_color_hex(0x30D158), 0);
  lv_obj_set_style_border_width(zone, 0, 0);
  lv_obj_set_style_radius(zone, 0, 0);
  lv_obj_clear_flag(zone, LV_OBJ_FLAG_SCROLLABLE);

  cursor = lv_obj_create(parent);
  lv_obj_set_size(cursor, ZONE_W, CURSOR_H);
  lv_obj_set_style_bg_color(cursor, lv_color_hex(0xFFCC00), 0);
  lv_obj_set_style_border_width(cursor, 0, 0);
  lv_obj_set_style_radius(cursor, 0, 0);
  lv_obj_clear_flag(cursor, LV_OBJ_FLAG_SCROLLABLE);

  hint_label = lv_label_create(parent);
  lv_obj_set_style_text_font(hint_label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(hint_label, lv_color_hex(0x555555), 0);
  lv_obj_align(hint_label, LV_ALIGN_BOTTOM_MID, 0, -16);

  randomSeed(micros());
  reset_game();

  tick_timer = lv_timer_create(game_tick, 20, nullptr);
}

void on_close() {
  if (tick_timer) {
    lv_timer_del(tick_timer);
    tick_timer = nullptr;
  }
  status_label = hint_label = zone = cursor = nullptr;
}

void on_short_press() {
  if (state != State::PLAYING) {
    reset_game();
    return;
  }

  lv_coord_t cursor_center = static_cast<lv_coord_t>(cursor_y) + CURSOR_H / 2;
  bool hit = cursor_center >= zone_top && cursor_center <= zone_top + zone_h;

  if (hit) {
    score++;
    zone_h = max(static_cast<lv_coord_t>(zone_h - ZONE_H_STEP), ZONE_H_MIN);
    cursor_speed = min(cursor_speed + CURSOR_SPEED_STEP, CURSOR_SPEED_MAX);
    place_zone();
    update_status();
  } else {
    lives--;
    if (lives <= 0) {
      die();
      return;
    }
    place_zone();
    update_status();
  }
}

}  // namespace

const AppDescriptor reflex_app = {
    .name = "Reflex",
    .icon_symbol = LV_SYMBOL_OK,
    .icon_color = lv_color_hex(0xFF453A),
    .on_open = on_open,
    .on_close = on_close,
    .on_short_press = on_short_press,
};

#include "stack_app.h"

#include <Arduino.h>
#include <lvgl.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "config.h"

// A block-stacking game (the classic mobile "Stack"): a block slides back
// and forth above the tower; press to drop it. Only the part that
// overlaps the block below survives, so the tower narrows with every
// imperfect drop - land within PERFECT_TOLERANCE_PX of a perfect match
// and the width carries over unchanged instead of shrinking. One miss
// (zero overlap) ends the run. Works the same via a physical short press
// or a touch-board tap, since it only needs the one action.

namespace {

constexpr lv_coord_t PLAY_TOP = 36;
constexpr lv_coord_t PLAY_BOTTOM = 296;
constexpr lv_coord_t ROW_H = 20;
constexpr lv_coord_t BLOCK_H = 16;  // < ROW_H, leaves a visible gap between rows
constexpr lv_coord_t INITIAL_WIDTH = 120;
constexpr lv_coord_t PERFECT_TOLERANCE_PX = 4;

constexpr float INITIAL_SPEED = 1.6f;
constexpr float MAX_SPEED = 3.6f;
constexpr float SPEED_STEP = 0.06f;

constexpr uint32_t PALETTE[] = {0xFF453A, 0xFF9F0A, 0xFFD60A, 0x30D158, 0x64D2FF, 0x0A84FF, 0xBF5AF2, 0xFF375F};
constexpr size_t PALETTE_COUNT = sizeof(PALETTE) / sizeof(PALETTE[0]);

enum class State { PLAYING, GAME_OVER };

lv_obj_t *app_parent = nullptr;
lv_obj_t *status_label = nullptr;
lv_obj_t *hint_label = nullptr;
lv_obj_t *cur_block = nullptr;
lv_timer_t *tick_timer = nullptr;
std::vector<lv_obj_t *> placed;

State state = State::PLAYING;
int score = 0;
float cur_x = 0;
float cur_speed = INITIAL_SPEED;
lv_coord_t cur_width = INITIAL_WIDTH;
lv_coord_t spawn_y = 0;
float last_x = 0;
float last_width = LCD_PANEL_WIDTH;

lv_obj_t *make_block(lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_color_t color) {
  lv_obj_t *o = lv_obj_create(app_parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_size(o, w, BLOCK_H);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_style_bg_color(o, color, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(o, 3, 0);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

void update_status(const char *prefix = nullptr) {
  if (prefix) {
    lv_label_set_text_fmt(status_label, "%s  Score: %d", prefix, score);
  } else {
    lv_label_set_text_fmt(status_label, "Score: %d", score);
  }
}

void reset_game() {
  state = State::PLAYING;
  score = 0;

  for (lv_obj_t *w : placed) lv_obj_del(w);
  placed.clear();

  last_x = 0;
  last_width = LCD_PANEL_WIDTH;
  cur_width = INITIAL_WIDTH;
  cur_x = 0;
  cur_speed = INITIAL_SPEED;
  spawn_y = PLAY_BOTTOM - ROW_H;

  lv_obj_set_size(cur_block, cur_width, BLOCK_H);
  lv_obj_set_pos(cur_block, static_cast<lv_coord_t>(cur_x), spawn_y);
  lv_obj_set_style_bg_color(cur_block, lv_color_hex(PALETTE[0]), 0);
  lv_obj_clear_flag(cur_block, LV_OBJ_FLAG_HIDDEN);

  update_status();
  lv_label_set_text(hint_label, "short: drop  |  hold: home");
}

void end_game() {
  state = State::GAME_OVER;
  lv_obj_add_flag(cur_block, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text_fmt(status_label, "Game Over - Score %d", score);
  lv_label_set_text(hint_label, "short: retry  |  hold: home");
}

void game_tick(lv_timer_t * /*t*/) {
  if (state != State::PLAYING) return;

  cur_x += cur_speed;
  if (cur_x <= 0) {
    cur_x = 0;
    cur_speed = -cur_speed;
  } else if (cur_x + cur_width >= LCD_PANEL_WIDTH) {
    cur_x = LCD_PANEL_WIDTH - cur_width;
    cur_speed = -cur_speed;
  }
  lv_obj_set_x(cur_block, static_cast<lv_coord_t>(cur_x));
}

void on_open(lv_obj_t *parent) {
  app_parent = parent;

  status_label = lv_label_create(parent);
  lv_obj_set_style_text_font(status_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(status_label, lv_color_hex(0xCCCCCC), 0);
  lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 8);

  cur_block = lv_obj_create(parent);
  lv_obj_remove_style_all(cur_block);
  lv_obj_set_style_bg_opa(cur_block, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(cur_block, 3, 0);
  lv_obj_clear_flag(cur_block, LV_OBJ_FLAG_SCROLLABLE);

  hint_label = lv_label_create(parent);
  lv_obj_set_style_text_font(hint_label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(hint_label, lv_color_hex(0x555555), 0);
  lv_obj_align(hint_label, LV_ALIGN_BOTTOM_MID, 0, -16);

  reset_game();
  tick_timer = lv_timer_create(game_tick, 20, nullptr);
}

void on_close() {
  if (tick_timer) {
    lv_timer_del(tick_timer);
    tick_timer = nullptr;
  }
  for (lv_obj_t *w : placed) lv_obj_del(w);
  placed.clear();
  app_parent = status_label = hint_label = cur_block = nullptr;
}

void on_short_press() {
  if (state != State::PLAYING) {
    reset_game();
    return;
  }

  float new_left = std::max(cur_x, last_x);
  float new_right = std::min(cur_x + cur_width, last_x + last_width);
  float overlap = new_right - new_left;

  if (overlap <= 0) {
    end_game();
    return;
  }

  bool perfect = fabsf(cur_x - last_x) <= PERFECT_TOLERANCE_PX && fabsf((cur_x + cur_width) - (last_x + last_width)) <= PERFECT_TOLERANCE_PX;
  lv_coord_t placed_x = static_cast<lv_coord_t>(perfect ? last_x : new_left);
  lv_coord_t placed_width = static_cast<lv_coord_t>(perfect ? last_width : overlap);

  lv_color_t color = lv_color_hex(PALETTE[score % PALETTE_COUNT]);
  placed.push_back(make_block(placed_x, spawn_y, placed_width, color));

  score++;
  last_x = placed_x;
  last_width = placed_width;
  cur_width = placed_width;
  cur_speed = (cur_speed < 0 ? -1.0f : 1.0f) * std::min(fabsf(cur_speed) + SPEED_STEP, MAX_SPEED);

  // Tower grows upward (spawn_y decreases); once that would go above the
  // play area, shift everything down by one row instead - old blocks that
  // land below PLAY_BOTTOM are off-screen and get deleted, which is what
  // keeps memory bounded no matter how long a run goes on.
  lv_coord_t next_spawn_y = spawn_y - ROW_H;
  if (next_spawn_y < PLAY_TOP) {
    lv_coord_t shift = PLAY_TOP - next_spawn_y;
    next_spawn_y += shift;
    for (size_t i = 0; i < placed.size();) {
      lv_coord_t y = lv_obj_get_y(placed[i]) + shift;
      if (y > PLAY_BOTTOM) {
        lv_obj_del(placed[i]);
        placed.erase(placed.begin() + static_cast<long>(i));
      } else {
        lv_obj_set_y(placed[i], y);
        i++;
      }
    }
  }
  spawn_y = next_spawn_y;

  lv_obj_set_size(cur_block, cur_width, BLOCK_H);
  lv_obj_set_pos(cur_block, static_cast<lv_coord_t>(cur_x), spawn_y);
  lv_obj_set_style_bg_color(cur_block, lv_color_hex(PALETTE[score % PALETTE_COUNT]), 0);

  update_status(perfect ? "PERFECT!" : nullptr);
}

}  // namespace

const AppDescriptor stack_app = {
    .name = "Stack",
    .icon_symbol = LV_SYMBOL_BARS,
    .icon_color = lv_color_hex(0xAC8E68),
    .on_open = on_open,
    .on_close = on_close,
    .on_short_press = on_short_press,
};

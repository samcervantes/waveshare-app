#include "flappy_app.h"

#include <Arduino.h>
#include <lvgl.h>

#include "config.h"

namespace {

constexpr lv_coord_t BIRD_SIZE = 16;
constexpr lv_coord_t BIRD_X = 30;
constexpr lv_coord_t PIPE_W = 22;
constexpr lv_coord_t GAP_H = 80;
constexpr lv_coord_t PIPE_SPEED = 3;
constexpr float GRAVITY = 0.6f;
constexpr float FLAP_VEL = -6.0f;

constexpr lv_coord_t AREA_TOP = 44;      // below the score label
constexpr lv_coord_t AREA_BOTTOM = 296;  // above the hint text
constexpr lv_coord_t AREA_RIGHT = LCD_PANEL_WIDTH;

enum class State { PLAYING, GAME_OVER };

lv_obj_t *status_label = nullptr;
lv_obj_t *hint_label = nullptr;
lv_obj_t *bird = nullptr;
lv_obj_t *pipe_top = nullptr;
lv_obj_t *pipe_bottom = nullptr;
lv_timer_t *tick_timer = nullptr;

State state = State::PLAYING;
float bird_y;
float bird_vel;
int pipe_x;
int gap_top;
bool scored;
int score;

void layout_pipe() {
  lv_obj_set_pos(pipe_top, pipe_x, AREA_TOP);
  lv_obj_set_size(pipe_top, PIPE_W, gap_top - AREA_TOP);

  lv_obj_set_pos(pipe_bottom, pipe_x, gap_top + GAP_H);
  lv_obj_set_size(pipe_bottom, PIPE_W, AREA_BOTTOM - (gap_top + GAP_H));
}

void spawn_pipe() {
  pipe_x = AREA_RIGHT;
  gap_top = random(AREA_TOP + 10, AREA_BOTTOM - GAP_H - 10);
  scored = false;
  layout_pipe();
}

void reset_game() {
  state = State::PLAYING;
  bird_y = (AREA_TOP + AREA_BOTTOM) / 2;
  bird_vel = 0;
  score = 0;
  spawn_pipe();

  lv_label_set_text(status_label, "Score: 0");
  lv_label_set_text(hint_label, "short: flap  |  hold: home");
  lv_obj_set_pos(bird, BIRD_X, static_cast<lv_coord_t>(bird_y));
}

void die() {
  state = State::GAME_OVER;
  lv_label_set_text_fmt(status_label, "Game Over - Score %d", score);
  lv_label_set_text(hint_label, "short: retry  |  hold: home");
}

void game_tick(lv_timer_t * /*t*/) {
  if (state != State::PLAYING) return;

  bird_vel += GRAVITY;
  bird_y += bird_vel;

  if (bird_y <= AREA_TOP || bird_y + BIRD_SIZE >= AREA_BOTTOM) {
    bird_y = constrain(bird_y, static_cast<float>(AREA_TOP), static_cast<float>(AREA_BOTTOM - BIRD_SIZE));
    die();
    lv_obj_set_pos(bird, BIRD_X, static_cast<lv_coord_t>(bird_y));
    return;
  }

  pipe_x -= PIPE_SPEED;
  if (pipe_x + PIPE_W < 0) {
    spawn_pipe();
  } else {
    bool overlapping_x = (BIRD_X + BIRD_SIZE > pipe_x) && (BIRD_X < pipe_x + PIPE_W);
    if (overlapping_x && (bird_y < gap_top || bird_y + BIRD_SIZE > gap_top + GAP_H)) {
      die();
      lv_obj_set_pos(bird, BIRD_X, static_cast<lv_coord_t>(bird_y));
      return;
    }
    if (!scored && pipe_x + PIPE_W < BIRD_X) {
      scored = true;
      score++;
      lv_label_set_text_fmt(status_label, "Score: %d", score);
    }
    layout_pipe();
  }

  lv_obj_set_pos(bird, BIRD_X, static_cast<lv_coord_t>(bird_y));
}

void on_open(lv_obj_t *parent) {
  status_label = lv_label_create(parent);
  lv_obj_set_style_text_font(status_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(status_label, lv_color_hex(0xCCCCCC), 0);
  lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 12);

  pipe_top = lv_obj_create(parent);
  lv_obj_set_style_bg_color(pipe_top, lv_color_hex(0x30D158), 0);
  lv_obj_set_style_border_width(pipe_top, 0, 0);
  lv_obj_set_style_radius(pipe_top, 0, 0);
  lv_obj_clear_flag(pipe_top, LV_OBJ_FLAG_SCROLLABLE);

  pipe_bottom = lv_obj_create(parent);
  lv_obj_set_style_bg_color(pipe_bottom, lv_color_hex(0x30D158), 0);
  lv_obj_set_style_border_width(pipe_bottom, 0, 0);
  lv_obj_set_style_radius(pipe_bottom, 0, 0);
  lv_obj_clear_flag(pipe_bottom, LV_OBJ_FLAG_SCROLLABLE);

  bird = lv_obj_create(parent);
  lv_obj_set_size(bird, BIRD_SIZE, BIRD_SIZE);
  lv_obj_set_style_radius(bird, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(bird, lv_color_hex(0xFFCC00), 0);
  lv_obj_set_style_border_width(bird, 0, 0);
  lv_obj_clear_flag(bird, LV_OBJ_FLAG_SCROLLABLE);

  hint_label = lv_label_create(parent);
  lv_obj_set_style_text_font(hint_label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(hint_label, lv_color_hex(0x555555), 0);
  lv_obj_align(hint_label, LV_ALIGN_BOTTOM_MID, 0, -16);

  randomSeed(micros());
  reset_game();

  tick_timer = lv_timer_create(game_tick, 30, nullptr);
}

void on_close() {
  if (tick_timer) {
    lv_timer_del(tick_timer);
    tick_timer = nullptr;
  }
  status_label = hint_label = bird = pipe_top = pipe_bottom = nullptr;
}

void on_short_press() {
  if (state == State::PLAYING) {
    bird_vel = FLAP_VEL;
  } else {
    reset_game();
  }
}

}  // namespace

const AppDescriptor flappy_app = {
    .name = "Flappy",
    .icon_symbol = LV_SYMBOL_UP,
    .icon_color = lv_color_hex(0xFFCC00),
    .on_open = on_open,
    .on_close = on_close,
    .on_short_press = on_short_press,
};

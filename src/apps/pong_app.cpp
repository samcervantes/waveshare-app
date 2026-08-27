#include "pong_app.h"

#include <Arduino.h>
#include <lvgl.h>

#include "config.h"

// Pong against a (beatable) AI, played portrait: the ball rallies quickly
// left-right across the narrow 172px width while paddles have the full
// 320px height to maneuver in. The physical button always works: short
// press gives the paddle an upward kick, gravity pulls it back down the
// rest of the time (mirrors Flappy's feel, which this app replaces). On
// the touch board, dragging a finger up/down directly follows the paddle
// instead (see drag_area below) - wants_raw_touch opts out of the
// launcher's generic tap-anywhere overlay so a raw drag gesture reaches
// this app at all; gravity still applies between drag updates but at
// ~50Hz that's imperceptible, so the paddle just tracks the finger.

namespace {

constexpr lv_coord_t AREA_TOP = 36;
constexpr lv_coord_t AREA_BOTTOM = 296;

constexpr lv_coord_t PADDLE_W = 6;
constexpr lv_coord_t PADDLE_H = 44;
constexpr lv_coord_t PADDLE_MARGIN = 8;
constexpr lv_coord_t AI_X = PADDLE_MARGIN;
constexpr lv_coord_t PLAYER_X = LCD_PANEL_WIDTH - PADDLE_MARGIN - PADDLE_W;
constexpr lv_coord_t BALL_SIZE = 8;

constexpr float GRAVITY = 0.35f;
constexpr float BOOST_VEL = -3.6f;

constexpr float BALL_START_SPEED = 2.0f;
constexpr float BALL_SPEEDUP = 1.06f;
constexpr float BALL_MAX_SPEED = 5.5f;
constexpr float AI_SPEED = 2.0f;  // capped below what a fast ball needs, so it's beatable

constexpr int START_LIVES = 3;

enum class State { PLAYING, GAME_OVER };

lv_obj_t *status_label = nullptr;
lv_obj_t *hint_label = nullptr;
lv_obj_t *player_paddle = nullptr;
lv_obj_t *ai_paddle = nullptr;
lv_obj_t *ball = nullptr;
lv_timer_t *tick_timer = nullptr;
#if defined(BOARD_TOUCH_LCD147)
lv_obj_t *drag_area = nullptr;
#endif

State state = State::PLAYING;
float player_y, player_vy;
float ai_y;
float ball_x, ball_y, ball_vx, ball_vy;
float ball_speed;
int score, lives;

void update_status() {
  lv_label_set_text_fmt(status_label, "Score: %d   Lives: %d", score, lives);
}

void reset_ball() {
  ball_x = LCD_PANEL_WIDTH / 2.0f - BALL_SIZE / 2.0f;
  ball_y = (AREA_TOP + AREA_BOTTOM) / 2.0f - BALL_SIZE / 2.0f;
  ball_vx = ball_speed;  // always serve toward the player, so they lead every rally
  ball_vy = ((random(0, 200) - 100) / 100.0f) * (ball_speed * 0.6f);
}

void reset_game() {
  state = State::PLAYING;
  score = 0;
  lives = START_LIVES;
  ball_speed = BALL_START_SPEED;
  player_y = (AREA_TOP + AREA_BOTTOM) / 2.0f - PADDLE_H / 2.0f;
  player_vy = 0;
  ai_y = player_y;
  reset_ball();

  update_status();
#if defined(BOARD_TOUCH_LCD147)
  lv_label_set_text(hint_label, "drag: move paddle  |  " HOME_HINT);
#else
  lv_label_set_text(hint_label, "short: paddle up  |  " HOME_HINT);
#endif
  lv_obj_set_pos(player_paddle, PLAYER_X, static_cast<lv_coord_t>(player_y));
  lv_obj_set_pos(ai_paddle, AI_X, static_cast<lv_coord_t>(ai_y));
  lv_obj_clear_flag(ball, LV_OBJ_FLAG_HIDDEN);
}

void end_game() {
  state = State::GAME_OVER;
  lv_obj_add_flag(ball, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text_fmt(status_label, "Game Over - Score %d", score);
#if defined(BOARD_TOUCH_LCD147)
  lv_label_set_text(hint_label, "tap: retry  |  " HOME_HINT);
#else
  lv_label_set_text(hint_label, "short: retry  |  " HOME_HINT);
#endif
}

void game_tick(lv_timer_t * /*t*/) {
  if (state != State::PLAYING) return;

  player_vy += GRAVITY;
  player_y += player_vy;
  player_y = constrain(player_y, static_cast<float>(AREA_TOP), static_cast<float>(AREA_BOTTOM - PADDLE_H));

  float ai_center = ai_y + PADDLE_H / 2.0f;
  float ball_center = ball_y + BALL_SIZE / 2.0f;
  if (ai_center < ball_center - 2) {
    ai_y += AI_SPEED;
  } else if (ai_center > ball_center + 2) {
    ai_y -= AI_SPEED;
  }
  ai_y = constrain(ai_y, static_cast<float>(AREA_TOP), static_cast<float>(AREA_BOTTOM - PADDLE_H));

  ball_x += ball_vx;
  ball_y += ball_vy;

  if (ball_y <= AREA_TOP) {
    ball_y = AREA_TOP;
    ball_vy = -ball_vy;
  } else if (ball_y + BALL_SIZE >= AREA_BOTTOM) {
    ball_y = AREA_BOTTOM - BALL_SIZE;
    ball_vy = -ball_vy;
  }

  // AI paddle (left) - only relevant while the ball is heading toward it
  if (ball_vx < 0 && ball_x <= AI_X + PADDLE_W && ball_x + BALL_SIZE >= AI_X && ball_y + BALL_SIZE >= ai_y &&
      ball_y <= ai_y + PADDLE_H) {
    ball_x = AI_X + PADDLE_W;
    ball_speed = min(ball_speed * BALL_SPEEDUP, BALL_MAX_SPEED);
    ball_vx = ball_speed;
    float rel = ((ball_y + BALL_SIZE / 2.0f) - (ai_y + PADDLE_H / 2.0f)) / (PADDLE_H / 2.0f);
    ball_vy = rel * ball_speed;
  }

  // Player paddle (right)
  if (ball_vx > 0 && ball_x + BALL_SIZE >= PLAYER_X && ball_x <= PLAYER_X + PADDLE_W &&
      ball_y + BALL_SIZE >= player_y && ball_y <= player_y + PADDLE_H) {
    ball_x = PLAYER_X - BALL_SIZE;
    ball_speed = min(ball_speed * BALL_SPEEDUP, BALL_MAX_SPEED);
    ball_vx = -ball_speed;
    float rel = ((ball_y + BALL_SIZE / 2.0f) - (player_y + PADDLE_H / 2.0f)) / (PADDLE_H / 2.0f);
    ball_vy = rel * ball_speed;
  }

  if (ball_x + BALL_SIZE < 0) {
    score++;
    update_status();
    reset_ball();
  } else if (ball_x > LCD_PANEL_WIDTH) {
    lives--;
    if (lives <= 0) {
      end_game();
      return;
    }
    update_status();
    reset_ball();
  }

  lv_obj_set_pos(player_paddle, PLAYER_X, static_cast<lv_coord_t>(player_y));
  lv_obj_set_pos(ai_paddle, AI_X, static_cast<lv_coord_t>(ai_y));
  lv_obj_set_pos(ball, static_cast<lv_coord_t>(ball_x), static_cast<lv_coord_t>(ball_y));
}

#if defined(BOARD_TOUCH_LCD147)
void on_drag(lv_event_t * /*e*/) {
  if (state != State::PLAYING) return;
  lv_point_t point;
  lv_indev_get_point(lv_indev_get_act(), &point);
  player_y = point.y - PADDLE_H / 2.0f;
  player_y = constrain(player_y, static_cast<float>(AREA_TOP), static_cast<float>(AREA_BOTTOM - PADDLE_H));
  player_vy = 0;  // don't let gravity fight the finger on the next tick
}

// drag_area only drives paddle movement while PLAYING (see on_drag) - a
// tap during GAME_OVER retries instead, since wants_raw_touch means the
// launcher's generic tap-anywhere-is-a-short-press overlay (which would
// otherwise reach on_short_press's retry branch) is skipped for this app.
void on_drag_area_tap(lv_event_t * /*e*/) {
  if (state == State::GAME_OVER) reset_game();
}
#endif

lv_obj_t *make_paddle(lv_obj_t *parent, uint32_t color) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_set_size(o, PADDLE_W, PADDLE_H);
  lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_radius(o, 2, 0);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

void on_open(lv_obj_t *parent) {
  status_label = lv_label_create(parent);
  lv_obj_set_style_text_font(status_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(status_label, lv_color_hex(0xCCCCCC), 0);
  lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 8);

  player_paddle = make_paddle(parent, 0xFFCC00);
  ai_paddle = make_paddle(parent, 0x555555);

  ball = lv_obj_create(parent);
  lv_obj_set_size(ball, BALL_SIZE, BALL_SIZE);
  lv_obj_set_style_radius(ball, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(ball, lv_color_white(), 0);
  lv_obj_set_style_border_width(ball, 0, 0);
  lv_obj_clear_flag(ball, LV_OBJ_FLAG_SCROLLABLE);

  hint_label = lv_label_create(parent);
  lv_obj_set_style_text_font(hint_label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(hint_label, lv_color_hex(0x555555), 0);
  lv_obj_align(hint_label, LV_ALIGN_BOTTOM_MID, 0, -16);

#if defined(BOARD_TOUCH_LCD147)
  // Covers just the play field (not the score/hint text) so a drag
  // anywhere in it moves the paddle. Going home is physical-button-only
  // (see launcher.cpp), so unlike an earlier version of this there's no
  // need to bubble a touch long-press anywhere - holding a finger still
  // to track the ball is normal play here, not a "go home" gesture.
  drag_area = lv_obj_create(parent);
  lv_obj_remove_style_all(drag_area);
  lv_obj_set_size(drag_area, LCD_PANEL_WIDTH, AREA_BOTTOM - AREA_TOP);
  lv_obj_set_pos(drag_area, 0, AREA_TOP);
  lv_obj_clear_flag(drag_area, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(drag_area, on_drag, LV_EVENT_PRESSED, nullptr);
  lv_obj_add_event_cb(drag_area, on_drag, LV_EVENT_PRESSING, nullptr);
  lv_obj_add_event_cb(drag_area, on_drag_area_tap, LV_EVENT_SHORT_CLICKED, nullptr);
#endif

  randomSeed(micros());
  reset_game();

  tick_timer = lv_timer_create(game_tick, 20, nullptr);
}

void on_close() {
  if (tick_timer) {
    lv_timer_del(tick_timer);
    tick_timer = nullptr;
  }
  status_label = hint_label = player_paddle = ai_paddle = ball = nullptr;
#if defined(BOARD_TOUCH_LCD147)
  drag_area = nullptr;
#endif
}

void on_short_press() {
  if (state == State::PLAYING) {
    player_vy = BOOST_VEL;
  } else {
    reset_game();
  }
}

}  // namespace

const AppDescriptor pong_app = {
    .name = "Pong",
    .icon_symbol = LV_SYMBOL_UP,
    .icon_color = lv_color_hex(0xFFCC00),
    .on_open = on_open,
    .on_close = on_close,
    .on_short_press = on_short_press,
    .wants_raw_touch = true,
};

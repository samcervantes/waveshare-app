#include "whack_app.h"

#include <Arduino.h>
#include <lvgl.h>

#include "config.h"

// A touch-native game: the target is a real clickable widget, tapped
// directly (wants_raw_touch = true below opts out of the launcher's
// generic tap-anywhere-is-a-short-press overlay so this app's own
// widgets can receive clicks - see app_interface.h). Touch board only
// (app_registry.cpp registers it there only); on_short_press is unused
// since there's no meaningful "physical button" version of tapping a
// specific spot on screen.

namespace {

constexpr lv_coord_t AREA_TOP = 44;      // below the score/time label
constexpr lv_coord_t AREA_BOTTOM = 296;  // above the hint text

constexpr lv_coord_t TARGET_SIZE_START = 56;
constexpr lv_coord_t TARGET_SIZE_MIN = 28;
constexpr lv_coord_t TARGET_SIZE_STEP = 2;

constexpr uint32_t GAME_DURATION_MS = 20000;
constexpr uint32_t TICK_MS = 100;

constexpr uint32_t TARGET_COLORS[] = {0xFF453A, 0x30D158, 0x0A84FF, 0xFFD60A, 0xBF5AF2, 0x64D2FF};
constexpr size_t TARGET_COLOR_COUNT = sizeof(TARGET_COLORS) / sizeof(TARGET_COLORS[0]);

enum class State { PLAYING, GAME_OVER };

lv_obj_t *status_label = nullptr;
lv_obj_t *hint_label = nullptr;
lv_obj_t *target = nullptr;

State state = State::PLAYING;
int score;
lv_coord_t target_size;
uint32_t time_left_ms;
lv_timer_t *tick_timer = nullptr;

void update_status() {
  lv_label_set_text_fmt(status_label, "Score: %d   %d.%ds", score, time_left_ms / 1000, (time_left_ms / 100) % 10);
}

void place_target() {
  lv_obj_set_size(target, target_size, target_size);
  lv_obj_set_pos(target, random(4, LCD_PANEL_WIDTH - target_size - 4), random(AREA_TOP, AREA_BOTTOM - target_size));
  lv_obj_set_style_bg_color(target, lv_color_hex(TARGET_COLORS[random(0, TARGET_COLOR_COUNT)]), 0);
}

void reset_game() {
  state = State::PLAYING;
  score = 0;
  target_size = TARGET_SIZE_START;
  time_left_ms = GAME_DURATION_MS;

  lv_obj_clear_flag(target, LV_OBJ_FLAG_HIDDEN);
  update_status();
  lv_label_set_text(hint_label, "tap the dot  |  hold: home");
  place_target();
}

void end_game() {
  state = State::GAME_OVER;
  lv_obj_add_flag(target, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text_fmt(status_label, "Game Over - Score %d", score);
  lv_label_set_text(hint_label, "tap anywhere to retry  |  hold: home");
}

void game_tick(lv_timer_t * /*t*/) {
  if (state != State::PLAYING) return;

  if (time_left_ms <= TICK_MS) {
    time_left_ms = 0;
    end_game();
    return;
  }
  time_left_ms -= TICK_MS;
  update_status();
}

void on_target_tap(lv_event_t * /*e*/) {
  score++;
  target_size = max(static_cast<lv_coord_t>(target_size - TARGET_SIZE_STEP), TARGET_SIZE_MIN);
  update_status();
  place_target();
}

// Game-over state hides the target, so app_root itself (which fills the
// rest of the screen behind it) catches the "tap anywhere to retry" tap.
// LV_EVENT_SHORT_CLICKED (not CLICKED) matters here specifically: app_root
// also has the launcher's long-press-to-home handler on it (see
// launcher.cpp), and LVGL still fires CLICKED on release even after a
// long press already fired - SHORT_CLICKED is the one gated on that not
// having happened. Without this, holding on empty space to go home would
// call reset_game() right after close_app() already tore the app down.
void on_background_tap(lv_event_t * /*e*/) {
  if (state == State::GAME_OVER) reset_game();
}

void on_open(lv_obj_t *parent) {
  status_label = lv_label_create(parent);
  lv_obj_set_style_text_font(status_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(status_label, lv_color_hex(0xCCCCCC), 0);
  lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 12);

  lv_obj_add_event_cb(parent, on_background_tap, LV_EVENT_SHORT_CLICKED, nullptr);

  target = lv_obj_create(parent);
  lv_obj_set_style_radius(target, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(target, 0, 0);
  lv_obj_clear_flag(target, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(target, on_target_tap, LV_EVENT_SHORT_CLICKED, nullptr);

  hint_label = lv_label_create(parent);
  lv_obj_set_style_text_font(hint_label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(hint_label, lv_color_hex(0x555555), 0);
  lv_obj_align(hint_label, LV_ALIGN_BOTTOM_MID, 0, -16);

  randomSeed(micros());
  reset_game();

  tick_timer = lv_timer_create(game_tick, TICK_MS, nullptr);
}

void on_close() {
  if (tick_timer) {
    lv_timer_del(tick_timer);
    tick_timer = nullptr;
  }
  status_label = hint_label = target = nullptr;
}

}  // namespace

const AppDescriptor whack_app = {
    .name = "Whack",
    .icon_symbol = LV_SYMBOL_GPS,
    .icon_color = lv_color_hex(0x5E5CE6),
    .on_open = on_open,
    .on_close = on_close,
    .on_short_press = nullptr,
    .wants_raw_touch = true,
};

#include "snake_app.h"

#include <Arduino.h>
#include <lvgl.h>

#include "config.h"

// Classic grid snake, sized to fit this panel's 172x320 portrait shape -
// about the most natural screen ratio there is for this genre. Each board
// gets a control scheme suited to what it actually has:
//   - Non-touch: short press rotates the heading 90 clockwise (the
//     standard single-button snake control), long press home.
//   - Touch: the snake continuously steers toward wherever a finger is
//     currently pressed (see steer_toward_touch), not swipe gestures -
//     swipe-direction detection turned out unreliable on this hardware
//     (see wifi_app.cpp's Scan/Status tabs history), while continuous
//     press-position tracking has been solid ever since Pong's paddle
//     drag. Physical button on this board is home-only, per this
//     project's touch-is-the-interface convention (see launcher.cpp).

namespace {

constexpr lv_coord_t CELL = 8;
constexpr int COLS = 21;                 // 21*8=168px, centered in the 172px panel
constexpr int ROWS = 33;                 // 33*8=264px play area height
constexpr lv_coord_t GRID_X0 = (LCD_PANEL_WIDTH - COLS * CELL) / 2;
constexpr lv_coord_t AREA_TOP = 28;      // status label
constexpr lv_coord_t GRID_Y0 = AREA_TOP;

constexpr int MAX_LEN = 150;  // pre-allocated segment pool - COLS*ROWS=693 is the real ceiling
constexpr int START_LEN = 3;

constexpr uint32_t TICK_START_MS = 150;
constexpr uint32_t TICK_MIN_MS = 70;
constexpr uint32_t TICK_STEP_MS = 2;  // speeds up this much per food eaten

enum class Dir { UP, DOWN, LEFT, RIGHT };
// Indexed by current Dir - gives the heading 90 clockwise from it, used by
// the non-touch board's single-button control.
constexpr Dir CLOCKWISE[4] = {Dir::RIGHT, Dir::LEFT, Dir::UP, Dir::DOWN};

struct Cell {
  int8_t x, y;
};

enum class State { PLAYING, GAME_OVER };

lv_obj_t *status_label = nullptr;
lv_obj_t *hint_label = nullptr;
lv_obj_t *segments[MAX_LEN] = {nullptr};
lv_obj_t *food_widget = nullptr;
#if defined(BOARD_TOUCH_LCD147)
lv_obj_t *drag_area = nullptr;
bool touch_active = false;
lv_coord_t touch_x = 0, touch_y = 0;
#endif
lv_timer_t *tick_timer = nullptr;

State state = State::PLAYING;
Cell body[MAX_LEN];
int length = START_LEN;
Dir dir = Dir::UP;
Cell food = {0, 0};
int score = 0;
int best_score = 0;
uint32_t tick_ms = TICK_START_MS;

bool is_opposite(Dir a, Dir b) {
  return (a == Dir::UP && b == Dir::DOWN) || (a == Dir::DOWN && b == Dir::UP) ||
         (a == Dir::LEFT && b == Dir::RIGHT) || (a == Dir::RIGHT && b == Dir::LEFT);
}

void update_status() {
  lv_label_set_text_fmt(status_label, "Score: %d   Best: %d", score, best_score);
}

void place_food() {
  bool occupied;
  int8_t fx, fy;
  do {
    fx = static_cast<int8_t>(random(0, COLS));
    fy = static_cast<int8_t>(random(0, ROWS));
    occupied = false;
    for (int i = 0; i < length; i++) {
      if (body[i].x == fx && body[i].y == fy) {
        occupied = true;
        break;
      }
    }
  } while (occupied);
  food = {fx, fy};
  lv_obj_set_pos(food_widget, GRID_X0 + fx * CELL, GRID_Y0 + fy * CELL);
}

void reset_game() {
  state = State::PLAYING;
  score = 0;
  tick_ms = TICK_START_MS;
  length = START_LEN;
  dir = Dir::UP;
#if defined(BOARD_TOUCH_LCD147)
  touch_active = false;
#endif

  int8_t hx = COLS / 2;
  int8_t hy = (ROWS * 2) / 3;
  for (int i = 0; i < length; i++) body[i] = {hx, static_cast<int8_t>(hy + i)};  // tail below head, heading up

  for (int i = 0; i < length; i++) {
    lv_obj_set_pos(segments[i], GRID_X0 + body[i].x * CELL, GRID_Y0 + body[i].y * CELL);
    lv_obj_clear_flag(segments[i], LV_OBJ_FLAG_HIDDEN);
  }
  for (int i = length; i < MAX_LEN; i++) lv_obj_add_flag(segments[i], LV_OBJ_FLAG_HIDDEN);

  place_food();
  update_status();
#if defined(BOARD_TOUCH_LCD147)
  lv_label_set_text(hint_label, "drag: steer  |  " HOME_HINT);
#else
  lv_label_set_text(hint_label, "short: turn  |  " HOME_HINT);
#endif
  if (tick_timer) lv_timer_set_period(tick_timer, tick_ms);
}

void end_game() {
  state = State::GAME_OVER;
  if (score > best_score) best_score = score;
  update_status();
  lv_obj_add_flag(food_widget, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text_fmt(status_label, "Game Over - Score %d", score);
#if defined(BOARD_TOUCH_LCD147)
  lv_label_set_text(hint_label, "tap: retry  |  " HOME_HINT);
#else
  lv_label_set_text(hint_label, "short: retry  |  " HOME_HINT);
#endif
}

#if defined(BOARD_TOUCH_LCD147)
// Continuously steers toward wherever the finger currently is, rather
// than requiring a swipe gesture - see the file header comment for why.
// Only ever turns 90 from the current heading (a snake can't reverse
// into itself in one tick), same physical constraint the single-button
// rotate control has for free.
void steer_toward_touch() {
  if (!touch_active) return;

  lv_coord_t head_px_x = GRID_X0 + body[0].x * CELL + CELL / 2;
  lv_coord_t head_px_y = GRID_Y0 + body[0].y * CELL + CELL / 2;
  int dx = touch_x - head_px_x;
  int dy = touch_y - head_px_y;
  int adx = dx < 0 ? -dx : dx;
  int ady = dy < 0 ? -dy : dy;
  if (adx < CELL && ady < CELL) return;  // touch is right on the head - no clear direction yet

  Dir desired = (adx > ady) ? (dx > 0 ? Dir::RIGHT : Dir::LEFT) : (dy > 0 ? Dir::DOWN : Dir::UP);
  if (!is_opposite(desired, dir)) dir = desired;
}
#endif

void game_tick(lv_timer_t * /*t*/) {
  if (state != State::PLAYING) return;

#if defined(BOARD_TOUCH_LCD147)
  steer_toward_touch();
#endif

  Cell new_head = {static_cast<int8_t>(body[0].x + (dir == Dir::RIGHT) - (dir == Dir::LEFT)),
                    static_cast<int8_t>(body[0].y + (dir == Dir::DOWN) - (dir == Dir::UP))};

  if (new_head.x < 0 || new_head.x >= COLS || new_head.y < 0 || new_head.y >= ROWS) {
    end_game();
    return;
  }
  for (int i = 0; i < length; i++) {
    if (body[i].x == new_head.x && body[i].y == new_head.y) {
      end_game();
      return;
    }
  }

  bool grew = (new_head.x == food.x && new_head.y == food.y);
  int new_length = grew ? min(length + 1, MAX_LEN) : length;
  for (int i = new_length - 1; i > 0; i--) body[i] = body[i - 1];
  body[0] = new_head;
  length = new_length;

  for (int i = 0; i < length; i++) {
    lv_obj_set_pos(segments[i], GRID_X0 + body[i].x * CELL, GRID_Y0 + body[i].y * CELL);
    lv_obj_clear_flag(segments[i], LV_OBJ_FLAG_HIDDEN);
  }

  if (grew) {
    score++;
    update_status();
    place_food();
    tick_ms = tick_ms > TICK_MIN_MS + TICK_STEP_MS ? tick_ms - TICK_STEP_MS : TICK_MIN_MS;
    lv_timer_set_period(tick_timer, tick_ms);
  }
}

#if defined(BOARD_TOUCH_LCD147)
void on_touch_move(lv_event_t * /*e*/) {
  lv_point_t p;
  lv_indev_get_point(lv_indev_get_act(), &p);
  touch_x = p.x;
  touch_y = p.y;
  touch_active = true;
}

void on_touch_release(lv_event_t * /*e*/) {
  touch_active = false;
}

void on_drag_area_tap(lv_event_t * /*e*/) {
  if (state == State::GAME_OVER) reset_game();
}
#endif

void on_open(lv_obj_t *parent) {
  status_label = lv_label_create(parent);
  lv_obj_set_style_text_font(status_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(status_label, lv_color_hex(0xDDDDDD), 0);
  lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 6);

  for (int i = 0; i < MAX_LEN; i++) {
    lv_obj_t *seg = lv_obj_create(parent);
    lv_obj_remove_style_all(seg);
    lv_obj_set_size(seg, CELL - 1, CELL - 1);
    lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(seg, lv_color_hex(i == 0 ? 0x30D158 : 0x1E7A38), 0);
    lv_obj_set_style_radius(seg, 1, 0);
    lv_obj_clear_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(seg, LV_OBJ_FLAG_HIDDEN);
    segments[i] = seg;
  }

  food_widget = lv_obj_create(parent);
  lv_obj_remove_style_all(food_widget);
  lv_obj_set_size(food_widget, CELL - 1, CELL - 1);
  lv_obj_set_style_bg_opa(food_widget, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(food_widget, lv_color_hex(0xFF453A), 0);
  lv_obj_set_style_radius(food_widget, LV_RADIUS_CIRCLE, 0);
  lv_obj_clear_flag(food_widget, LV_OBJ_FLAG_SCROLLABLE);

  hint_label = lv_label_create(parent);
  lv_obj_set_style_text_font(hint_label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(hint_label, lv_color_hex(0xDDDDDD), 0);
  lv_obj_align(hint_label, LV_ALIGN_BOTTOM_MID, 0, -6);

#if defined(BOARD_TOUCH_LCD147)
  // Covers just the play field (not the status/hint text) so a drag
  // anywhere in it steers the snake - see steer_toward_touch(). Tapping
  // it during GAME_OVER retries, since wants_raw_touch means the
  // launcher's generic tap-anywhere overlay is skipped for this app.
  drag_area = lv_obj_create(parent);
  lv_obj_remove_style_all(drag_area);
  lv_obj_set_size(drag_area, LCD_PANEL_WIDTH, ROWS * CELL);
  lv_obj_set_pos(drag_area, 0, GRID_Y0);
  lv_obj_clear_flag(drag_area, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(drag_area, on_touch_move, LV_EVENT_PRESSED, nullptr);
  lv_obj_add_event_cb(drag_area, on_touch_move, LV_EVENT_PRESSING, nullptr);
  lv_obj_add_event_cb(drag_area, on_touch_release, LV_EVENT_RELEASED, nullptr);
  lv_obj_add_event_cb(drag_area, on_drag_area_tap, LV_EVENT_SHORT_CLICKED, nullptr);
#endif

  randomSeed(micros());
  best_score = 0;
  reset_game();

  tick_timer = lv_timer_create(game_tick, tick_ms, nullptr);
}

void on_close() {
  if (tick_timer) {
    lv_timer_del(tick_timer);
    tick_timer = nullptr;
  }
  status_label = hint_label = food_widget = nullptr;
  for (int i = 0; i < MAX_LEN; i++) segments[i] = nullptr;
#if defined(BOARD_TOUCH_LCD147)
  drag_area = nullptr;
#endif
}

void on_short_press() {
  if (state == State::PLAYING) {
    dir = CLOCKWISE[static_cast<int>(dir)];
  } else {
    reset_game();
  }
}

}  // namespace

const AppDescriptor snake_app = {
    .name = "Snake",
    .icon_symbol = LV_SYMBOL_REFRESH,
    .icon_color = lv_color_hex(0x30D158),
    .on_open = on_open,
    .on_close = on_close,
    .on_short_press = on_short_press,
    // Touch board only (no-op elsewhere, see app_interface.h) - needed so
    // the play field can be dragged on directly for steering.
    .wants_raw_touch = true,
};

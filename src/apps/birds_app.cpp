#include "birds_app.h"

#include <Arduino.h>
#include <lvgl.h>

#include <cmath>

#include "config.h"

// Angry-Birds-style slingshot: pull the bird back from the anchor and
// release to launch it in an arc (gravity-affected) at a pig, smashing
// through a block obstacle along the way. Controls differ genuinely by
// board, not just in how the same aim is expressed:
//   - Touch: full 2D pull-and-release, same as the real thing - reads the
//     press position continuously while held (the same reliable
//     technique Pong's paddle drag and Snake's steering use), not a
//     swipe gesture.
//   - Non-touch: a single button can't express a 2D pull, so instead a
//     power meter oscillates continuously and a short press launches at
//     whatever power it reads at that instant (the same timing-skill
//     shape as Stack's falling block). The launch angle/horizontal drift
//     is fixed, and the pig spawns within the range that's actually
//     reachable that way - see NT_VX/NT_MAX_POWER below.
//
// Runs in landscape: the panel itself is a fixed 172x320 portrait strip
// (display.cpp doesn't support switching that per app), so rather than a
// cramped ~172px-wide arc, all of this app's own game state lives in a
// separate 320x172 *game* coordinate space (GAME_W x GAME_H - the panel's
// dimensions swapped) and gets rotated 90 into physical pixels only at
// the point each widget is positioned (game_to_px/set_game_rect) - and
// the reverse for touch input (px_to_game). Play the game with the board
// turned 90 clockwise from its normal orientation (see those functions'
// comments for the exact convention). The score/hint text is rotated to
// match (see HUD_TEXT_W/HUD_X0 below) rather than staying upright at the
// physical top/bottom of the panel - LVGL 8 can rotate a label like any
// other object via the transform_angle/pivot style properties, the same
// mechanism the bird's beak/eyebrow already use to turn plain rects into
// angled shapes, it's just not the obvious place to look for it.

namespace {

constexpr float GAME_W = LCD_PANEL_HEIGHT;  // 320 - landscape width (the travel axis)
constexpr float GAME_H = LCD_PANEL_WIDTH;   // 172 - landscape height (the gravity axis)

// Keeps gameplay off the extreme gx (landscape-horizontal) ends - not
// for the HUD text anymore (that now lives in a gy-axis strip instead,
// see HUD_TEXT_W/HUD_X0 below), but ANCHOR_X still needs this clearance
// from PLAY_X_MIN/MAX for MAX_PULL (see its own comment).
constexpr float PLAY_X_MIN = 26;
constexpr float PLAY_X_MAX = GAME_W - 26;

// The status/hint text reads along the landscape/game orientation (see
// the file header), which rules out a plain rotated lv_label: LVGL 8 can
// only rotate a *live* object (lv_obj_set_style_transform_angle, same as
// the bird's beak/eyebrow below) by rendering it into an offscreen layer
// sized to its own on-screen footprint first - for a label that footprint
// is dozens of px on a side, and at this project's LV_MEM_SIZE (64KB,
// see lv_conf.h) that layer allocation fails outright, so the object
// silently doesn't draw at all (LVGL logs a warning, but nothing reaches
// the panel or a monitor that isn't already attached).
//
// Instead, HUD text is pre-rotated *pixel data*, not a rotated live
// object: draw_hud_text() renders normal upright text into a small
// scratch lv_canvas (hud_scratch_buf), then lv_canvas_transform() rotates
// that bitmap into a second, on-screen canvas (hud_status_buf/
// hud_hint_buf) - a directly-into-the-destination-buffer software
// rotation that doesn't need a layer at all, so it's cheap regardless of
// text length. The scratch canvas is HUD_SRC_W x HUD_SRC_H (upright);
// each destination canvas is the swapped HUD_DST_W x HUD_DST_H (rotated),
// positioned on screen with a plain lv_obj_set_pos - no pivot math needed
// since the rotation is already baked into the pixels.
//
// angle -900 (rotate_source_to_dest's `angle` param takes it directly,
// no need to wrap to a positive value the way object styles do) rotates
// text to advance in the landscape-forward (+gx) direction - see
// game_to_px's comment for why physical "up" (decreasing physical y) is
// landscape-forward. offset_y=HUD_SRC_W in draw_hud_text() is what maps
// the rotated-about-its-own-top-left-corner source into the positive
// [0,HUD_DST_W]x[0,HUD_DST_H] area the destination canvas actually
// covers - see the source comment there for the corner-pivot math.
constexpr lv_coord_t HUD_SRC_W = 140;
constexpr lv_coord_t HUD_SRC_H = 20;
constexpr lv_coord_t HUD_DST_W = HUD_SRC_H;
constexpr lv_coord_t HUD_DST_H = HUD_SRC_W;
constexpr lv_coord_t HUD_X0 = 3;  // physical-x inset from the sky edge (gy ~3..~23)

lv_color_t hud_scratch_buf[LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(HUD_SRC_W, HUD_SRC_H)];
lv_color_t hud_status_buf[LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(HUD_DST_W, HUD_DST_H)];
lv_color_t hud_hint_buf[LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(HUD_DST_W, HUD_DST_H)];

// ANCHOR_X needs at least MAX_PULL of clearance from PLAY_X_MIN/MAX (too
// close to either and the touch point hits the physical screen boundary
// before reaching the intended max pull distance, capping shots well
// short of MAX_PULL regardless of what this file's constants say).
// ANCHOR_Y is dead center of GAME_H - the panel's *narrower* physical
// axis (172px, vs. 320 for GAME_W), so once the panel is turned sideways
// to play, this is the screen's actual vertical center - which caps how
// big MAX_PULL can be to begin with (GAME_H/2, minus a few px of buffer).
constexpr float ANCHOR_X = 100;
constexpr float ANCHOR_Y = GAME_H / 2;
constexpr float BIRD_R = 21 * 0.8f;  // 20% smaller than the original size
constexpr float PIG_R = BIRD_R;      // same size as the (resized) bird
constexpr float BLOCK_W = 16;
constexpr float BLOCK_H = 22;
constexpr float GROUND_H = 14;  // strip along the large-gy edge of game space

// The wooden Y-fork stand: a post from the ground up to a fork junction,
// then two prongs spreading toward the sky (smaller gy) on either side of
// ANCHOR_X. The rubber bands run from each prong tip to wherever the bird
// currently is.
constexpr float PRONG_SPREAD = 14;
constexpr float PRONG_TIP_Y = ANCHOR_Y - 22;
constexpr float FORK_Y = ANCHOR_Y + 6;
constexpr float POST_BASE_Y = GAME_H - GROUND_H;

constexpr float GRAVITY = 0.22f;
constexpr uint32_t TICK_MS = 30;
constexpr int START_BIRDS = 6;

constexpr float MAX_PULL = 74;           // touch - ANCHOR_X - MAX_PULL == PLAY_X_MIN
constexpr float POWER_SCALE = 0.15f;     // touch
constexpr float NT_VX = 0.55f;           // non-touch: fixed drift along GAME_W per shot
constexpr float NT_MAX_POWER = 9.0f;     // non-touch: launch speed at full meter
constexpr uint32_t NT_METER_PERIOD_MS = 1300;
constexpr float METER_STEP = TICK_MS / (NT_METER_PERIOD_MS / 2.0f);

constexpr int MAX_PIGS_PER_LEVEL = 2;
constexpr int MAX_BLOCKS_PER_LEVEL = 2;

struct LevelPig {
  float x, y;
};
struct LevelBlock {
  float x, y;
};
struct Level {
  int pig_count;
  LevelPig pigs[MAX_PIGS_PER_LEVEL];
  int block_count;
  LevelBlock blocks[MAX_BLOCKS_PER_LEVEL];
};

// Fixed layouts, replacing the old fully-random single pig/block spawn.
// Coordinates are game-space (see GAME_W/GAME_H above) and stay inside the
// same box the old random spawn constrained itself to - pig x in
// [ANCHOR_X+30, PLAY_X_MAX], y in [10, GAME_H-20] - so a shot on the
// non-touch board's fixed launch angle/power range (NT_VX/NT_MAX_POWER)
// can still reach every pig; the touch board's full 2D aim can reach
// further, so these stay conservative for both rather than tuned per
// board. Cycles back to level 0 after the last one - birds_remaining
// running out is what actually ends the run, not the level list.
constexpr Level LEVELS[] = {
    // 1: one pig behind a single block - the original layout. Pushed
    // toward PLAY_X_MAX (the pigs used to sit too close to ANCHOR_X,
    // making the shot too easy) - block shifted along with its pig so it
    // still guards the same way.
    {1, {{280, 90}}, 1, {{220, 79}}},
    // 2: two pigs spread apart, one block guarding the nearer one.
    {2, {{240, 50}, {280, 130}}, 1, {{215, 70}}},
    // 3: one pig behind a two-block tower.
    {1, {{288, 100}}, 2, {{228, 144}, {228, 122}}},
    // 4: two pigs, each with its own block.
    {2, {{220, 40}, {290, 140}}, 2, {{190, 30}, {250, 125}}},
    // 5: two pigs close together, one block up front.
    {2, {{280, 70}, {290, 120}}, 1, {{240, 95}}},
};
constexpr int NUM_LEVELS = sizeof(LEVELS) / sizeof(LEVELS[0]);

enum class State { AIMING, FLYING, GAME_OVER };

lv_obj_t *hud_scratch = nullptr;  // offscreen, never shown - see HUD comment above
lv_obj_t *status_canvas = nullptr;
lv_obj_t *hint_canvas = nullptr;
lv_obj_t *bird = nullptr;
lv_obj_t *pigs[MAX_PIGS_PER_LEVEL] = {nullptr};
lv_obj_t *blocks[MAX_BLOCKS_PER_LEVEL] = {nullptr};
lv_obj_t *ground = nullptr;
lv_timer_t *tick_timer = nullptr;

// The fork is static scenery and the bands track the bird - both boards
// draw them (not just touch), since aiming with a fixed-position bird on
// the non-touch board still means "a bird loaded in a sling," visually.
// Point arrays are namespace-scope because lv_line_set_points stores the
// pointer, not a copy.
lv_obj_t *fork_post = nullptr;
lv_obj_t *fork_left = nullptr;
lv_obj_t *fork_right = nullptr;
lv_point_t fork_post_pts[2];
lv_point_t fork_left_pts[2];
lv_point_t fork_right_pts[2];
lv_obj_t *band_line_l = nullptr;
lv_obj_t *band_line_r = nullptr;
lv_point_t band_points_l[2];
lv_point_t band_points_r[2];

// Touch-only widgets/state - harmless unused nullptr/0 on the non-touch
// build, same pattern as wifi_app.cpp's tab_scan/tab_status.
lv_obj_t *drag_area = nullptr;
bool pulling = false;

// Non-touch-only state - likewise harmless unused on the touch build.
lv_obj_t *meter_bar = nullptr;
float meter_value = 0;
int meter_dir = 1;

State state = State::AIMING;
float bx, by, bvx, bvy;  // all in game space
bool pig_alive[MAX_PIGS_PER_LEVEL];
float pig_x[MAX_PIGS_PER_LEVEL], pig_y[MAX_PIGS_PER_LEVEL];
bool block_alive[MAX_BLOCKS_PER_LEVEL];
float block_x[MAX_BLOCKS_PER_LEVEL], block_y[MAX_BLOCKS_PER_LEVEL];
int pigs_left = 0;
int current_level = 0;
int score = 0;
int birds_remaining = START_BIRDS;

// Rotates a game-space point into physical panel pixels. Convention:
// play this app with the panel turned 90 clockwise from normal - i.e.
// the panel's physical left edge becomes the top of what you're looking
// at. (gx=0,gy=0), the game's top-left, lands at the panel's
// bottom-left; (gx=0,gy=GAME_H), the game's bottom-left, lands at the
// panel's bottom-right - so "gravity" (which pulls toward larger gy)
// visually pulls toward the panel's physical right edge, which is
// "down" once the panel is turned that way.
lv_point_t game_to_px(float gx, float gy) {
  return {static_cast<lv_coord_t>(gy), static_cast<lv_coord_t>((LCD_PANEL_HEIGHT - 1) - gx)};
}

// Inverse of game_to_px, for touch input.
void px_to_game(lv_coord_t px, lv_coord_t py, float &gx, float &gy) {
  gx = (LCD_PANEL_HEIGHT - 1) - py;
  gy = px;
}

// Positions+sizes a widget from a game-space axis-aligned rect (top-left
// corner + size). A 90 rotation swaps which axis is "width" - the
// object's physical width becomes the game rect's height and vice versa.
void set_game_rect(lv_obj_t *obj, float gx0, float gy0, float gw, float gh) {
  lv_obj_set_size(obj, static_cast<lv_coord_t>(gh), static_cast<lv_coord_t>(gw));
  lv_point_t p = game_to_px(gx0 + gw, gy0);
  lv_obj_set_pos(obj, p.x, p.y);
}

// Renders `text` upright into the shared scratch canvas, then rotates
// that bitmap into `dst` (status_canvas or hint_canvas) - see the HUD
// comment up top for why this goes through pixel data instead of a
// rotated live label.
//
// lv_canvas_transform rotates around (pivot_x,pivot_y) *in source
// coordinates*; passing (0,0) pivots on the source's own top-left
// corner, which - like rotating a rectangle by hinging it at one
// corner - sweeps the far corner (HUD_SRC_W,HUD_SRC_H) around to land at
// (HUD_SRC_H,-HUD_SRC_W) relative to that same pivot (rotating -90:
// (dx,dy) -> (dy,-dx)). offset_y=HUD_SRC_W shifts that whole swept
// rectangle by (0,+HUD_SRC_W), landing it exactly on [0,HUD_DST_W] x
// [0,HUD_DST_H] - the destination canvas's own bounds - rather than
// partly off into negative destination coordinates.
void draw_hud_text(lv_obj_t *dst, const lv_font_t *font, const char *text) {
  lv_canvas_fill_bg(hud_scratch, lv_color_black(), LV_OPA_TRANSP);
  lv_draw_label_dsc_t label_dsc;
  lv_draw_label_dsc_init(&label_dsc);
  label_dsc.font = font;
  label_dsc.color = lv_color_hex(0xDDDDDD);
  lv_canvas_draw_text(hud_scratch, 0, 0, HUD_SRC_W, &label_dsc, text);

  lv_canvas_fill_bg(dst, lv_color_black(), LV_OPA_TRANSP);
  lv_canvas_transform(dst, lv_canvas_get_img(hud_scratch), -900, LV_IMG_ZOOM_NONE, 0, HUD_SRC_W, 0, 0, true);
}

void update_status() {
  char buf[48];
  snprintf(buf, sizeof(buf), "Score: %d  Lvl: %d  Birds: %d", score, current_level + 1, birds_remaining);
  draw_hud_text(status_canvas, &lv_font_montserrat_16, buf);
}

// Points each rubber band from its prong tip to the bird's current game
// position (bx, by) and makes sure they're visible - called whenever the
// bird is loaded/resting in the sling, whether that's a fresh shot or
// mid-drag on the touch board.
void update_bands() {
  lv_obj_clear_flag(band_line_l, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(band_line_r, LV_OBJ_FLAG_HIDDEN);
  band_points_l[0] = game_to_px(ANCHOR_X - PRONG_SPREAD, PRONG_TIP_Y);
  band_points_l[1] = game_to_px(bx, by);
  lv_line_set_points(band_line_l, band_points_l, 2);
  band_points_r[0] = game_to_px(ANCHOR_X + PRONG_SPREAD, PRONG_TIP_Y);
  band_points_r[1] = game_to_px(bx, by);
  lv_line_set_points(band_line_r, band_points_r, 2);
}

void place_bird_at_anchor() {
  bx = ANCHOR_X;
  by = ANCHOR_Y;
  set_game_rect(bird, bx - BIRD_R, by - BIRD_R, BIRD_R * 2, BIRD_R * 2);
  update_bands();
}

// Loads a fixed layout from LEVELS[idx]: shows/positions exactly
// lvl.pig_count pigs and lvl.block_count blocks, hides the remaining
// unused slots up to MAX_PIGS_PER_LEVEL/MAX_BLOCKS_PER_LEVEL.
void spawn_level(int idx) {
  const Level &lvl = LEVELS[idx];
  for (int i = 0; i < MAX_PIGS_PER_LEVEL; i++) {
    if (i < lvl.pig_count) {
      pig_alive[i] = true;
      pig_x[i] = lvl.pigs[i].x;
      pig_y[i] = lvl.pigs[i].y;
      set_game_rect(pigs[i], pig_x[i] - PIG_R, pig_y[i] - PIG_R, PIG_R * 2, PIG_R * 2);
      lv_obj_clear_flag(pigs[i], LV_OBJ_FLAG_HIDDEN);
    } else {
      pig_alive[i] = false;
      lv_obj_add_flag(pigs[i], LV_OBJ_FLAG_HIDDEN);
    }
  }
  for (int i = 0; i < MAX_BLOCKS_PER_LEVEL; i++) {
    if (i < lvl.block_count) {
      block_alive[i] = true;
      block_x[i] = lvl.blocks[i].x;
      block_y[i] = lvl.blocks[i].y;
      set_game_rect(blocks[i], block_x[i], block_y[i], BLOCK_W, BLOCK_H);
      lv_obj_clear_flag(blocks[i], LV_OBJ_FLAG_HIDDEN);
    } else {
      block_alive[i] = false;
      lv_obj_add_flag(blocks[i], LV_OBJ_FLAG_HIDDEN);
    }
  }
  pigs_left = lvl.pig_count;
}

void reset_game() {
  state = State::AIMING;
  score = 0;
  birds_remaining = START_BIRDS;
  pulling = false;
  meter_value = 0;
  meter_dir = 1;

  place_bird_at_anchor();  // also shows/positions the bands
  lv_obj_clear_flag(bird, LV_OBJ_FLAG_HIDDEN);
  current_level = 0;
  spawn_level(current_level);
  update_status();

#if defined(BOARD_TOUCH_LCD147)
  draw_hud_text(hint_canvas, &lv_font_montserrat_14, "drag: aim  |  " HOME_HINT);
#else
  draw_hud_text(hint_canvas, &lv_font_montserrat_14, "short: launch  |  " HOME_HINT);
#endif
}

void end_game() {
  state = State::GAME_OVER;
  lv_obj_add_flag(bird, LV_OBJ_FLAG_HIDDEN);
  for (int i = 0; i < MAX_PIGS_PER_LEVEL; i++) lv_obj_add_flag(pigs[i], LV_OBJ_FLAG_HIDDEN);
  for (int i = 0; i < MAX_BLOCKS_PER_LEVEL; i++) lv_obj_add_flag(blocks[i], LV_OBJ_FLAG_HIDDEN);
  char buf[48];
  snprintf(buf, sizeof(buf), "Game Over - Score %d", score);
  draw_hud_text(status_canvas, &lv_font_montserrat_16, buf);
#if defined(BOARD_TOUCH_LCD147)
  draw_hud_text(hint_canvas, &lv_font_montserrat_14, "tap: retry  |  " HOME_HINT);
#else
  draw_hud_text(hint_canvas, &lv_font_montserrat_14, "short: retry  |  " HOME_HINT);
#endif
}

void launch(float vx, float vy) {
  bvx = vx;
  bvy = vy;
  state = State::FLYING;
  // The bird leaves the pouch - hide the bands until it's back in a sling.
  lv_obj_add_flag(band_line_l, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(band_line_r, LV_OBJ_FLAG_HIDDEN);
}

#if defined(BOARD_TOUCH_LCD147)
// Reads the press position continuously while held, same technique as
// Pong's paddle drag and Snake's steer-toward-touch - not a swipe
// gesture (see the file header comment for why this project avoids
// those for gameplay). The bird jumps straight to wherever the finger
// is (clamped to MAX_PULL from the anchor) rather than requiring a
// precise initial grab on the small bird widget. The touch point is
// converted to game space (px_to_game) before any of the pull math, so
// this reads the same regardless of the panel's physical rotation.
void on_pull(lv_event_t * /*e*/) {
  if (state != State::AIMING) return;
  pulling = true;

  lv_point_t p;
  lv_indev_get_point(lv_indev_get_act(), &p);
  float gx, gy;
  px_to_game(p.x, p.y, gx, gy);

  float dx = gx - ANCHOR_X;
  float dy = gy - ANCHOR_Y;
  float dist = sqrtf(dx * dx + dy * dy);
  if (dist > MAX_PULL) {
    float s = MAX_PULL / dist;
    dx *= s;
    dy *= s;
  }
  bx = ANCHOR_X + dx;
  by = ANCHOR_Y + dy;
  set_game_rect(bird, bx - BIRD_R, by - BIRD_R, BIRD_R * 2, BIRD_R * 2);
  update_bands();
}

void on_release(lv_event_t * /*e*/) {
  if (!pulling) return;
  pulling = false;
  if (state != State::AIMING) return;

  float pull_x = bx - ANCHOR_X;
  float pull_y = by - ANCHOR_Y;
  float dist = sqrtf(pull_x * pull_x + pull_y * pull_y);
  if (dist < 10) {
    place_bird_at_anchor();  // too small a pull to count as a shot
    return;
  }
  launch(-pull_x * POWER_SCALE, -pull_y * POWER_SCALE);
}

void on_area_tap(lv_event_t * /*e*/) {
  if (state == State::GAME_OVER) reset_game();
}
#endif

void game_tick(lv_timer_t * /*t*/) {
  if (state == State::AIMING) {
#if !defined(BOARD_TOUCH_LCD147)
    meter_value += meter_dir * METER_STEP;
    if (meter_value >= 1.0f) {
      meter_value = 1.0f;
      meter_dir = -1;
    } else if (meter_value <= 0.0f) {
      meter_value = 0.0f;
      meter_dir = 1;
    }
    lv_bar_set_value(meter_bar, static_cast<int>(meter_value * 100), LV_ANIM_OFF);
#endif
    return;
  }
  if (state != State::FLYING) return;

  bvy += GRAVITY;
  bx += bvx;
  by += bvy;
  set_game_rect(bird, bx - BIRD_R, by - BIRD_R, BIRD_R * 2, BIRD_R * 2);

  for (int i = 0; i < MAX_PIGS_PER_LEVEL; i++) {
    if (!pig_alive[i]) continue;
    float pdx = bx - pig_x[i];
    float pdy = by - pig_y[i];
    if (pdx * pdx + pdy * pdy > (BIRD_R + PIG_R) * (BIRD_R + PIG_R)) continue;
    pig_alive[i] = false;
    lv_obj_add_flag(pigs[i], LV_OBJ_FLAG_HIDDEN);
    score++;
    pigs_left--;
    state = State::AIMING;
    place_bird_at_anchor();
    if (pigs_left <= 0) {
      current_level = (current_level + 1) % NUM_LEVELS;
      spawn_level(current_level);
    }
    update_status();
    return;
  }

  for (int i = 0; i < MAX_BLOCKS_PER_LEVEL; i++) {
    if (!block_alive[i]) continue;
    if (bx + BIRD_R > block_x[i] && bx - BIRD_R < block_x[i] + BLOCK_W && by + BIRD_R > block_y[i] &&
        by - BIRD_R < block_y[i] + BLOCK_H) {
      block_alive[i] = false;
      lv_obj_add_flag(blocks[i], LV_OBJ_FLAG_HIDDEN);
      bvx *= 0.5f;
      bvy *= 0.5f;
    }
  }

  if (by > GAME_H + 20 || bx < -20 || bx > GAME_W + 20) {
    birds_remaining--;
    update_status();
    if (birds_remaining <= 0) {
      end_game();
      return;
    }
    state = State::AIMING;
    place_bird_at_anchor();
  }
}

// Builds one block widget (base rect + top/bottom bevel strips), hidden
// and unpositioned - spawn_level shows/positions it per the active level.
lv_obj_t *build_block(lv_obj_t *parent) {
  lv_obj_t *block = lv_obj_create(parent);
  lv_obj_remove_style_all(block);
  lv_obj_set_style_bg_opa(block, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(block, lv_color_hex(0x9C7A4C), 0);
  lv_obj_set_style_radius(block, 2, 0);
  lv_obj_clear_flag(block, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(block, LV_OBJ_FLAG_HIDDEN);
  // Set now (matching what set_game_rect will set later) rather than
  // leaving it at LVGL's default size, since the bevel strips below are
  // aligned relative to it immediately - lv_obj_align computes against
  // whatever size the parent has *right now*, not whatever it's resized
  // to later, so aligning before this would put them in the wrong place.
  lv_obj_set_size(block, static_cast<lv_coord_t>(BLOCK_H), static_cast<lv_coord_t>(BLOCK_W));

  lv_obj_t *block_hi = lv_obj_create(block);
  lv_obj_remove_style_all(block_hi);
  lv_obj_set_size(block_hi, LV_PCT(100), 3);
  lv_obj_set_style_bg_opa(block_hi, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(block_hi, lv_color_hex(0xC7A876), 0);
  lv_obj_clear_flag(block_hi, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(block_hi, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(block_hi, LV_ALIGN_TOP_MID, 0, 0);

  lv_obj_t *block_lo = lv_obj_create(block);
  lv_obj_remove_style_all(block_lo);
  lv_obj_set_size(block_lo, LV_PCT(100), 3);
  lv_obj_set_style_bg_opa(block_lo, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(block_lo, lv_color_hex(0x6B4F2E), 0);
  lv_obj_clear_flag(block_lo, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(block_lo, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(block_lo, LV_ALIGN_BOTTOM_MID, 0, 0);

  return block;
}

// Builds one pig widget (body + snout/nostrils + eyes/pupils), hidden and
// unpositioned - spawn_level shows/positions it per the active level.
//
// Snout on the ground-facing side (RIGHT_MID, same edge as the bird's
// belly), eyes spread along the sky-facing side (LEFT_MID) - the same
// physical-to-landscape rotation applied to the bird's face below, so
// the pig also reads right-side-up once the panel is turned sideways.
// Width/height and offsets are swapped/rotated accordingly (a part
// that was wide-and-short along the old horizontal axis becomes
// tall-and-narrow along the new one, since the visual rotation swaps
// which physical axis reads as "horizontal" once the panel is turned).
lv_obj_t *build_pig(lv_obj_t *parent) {
  lv_obj_t *pig = lv_obj_create(parent);
  lv_obj_remove_style_all(pig);
  lv_obj_set_style_bg_opa(pig, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(pig, lv_color_hex(0x30D158), 0);
  lv_obj_set_style_radius(pig, LV_RADIUS_CIRCLE, 0);
  lv_obj_clear_flag(pig, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(pig, LV_OBJ_FLAG_HIDDEN);
  // See the same-purpose comment on block's sizing above.
  lv_obj_set_size(pig, static_cast<lv_coord_t>(PIG_R * 2), static_cast<lv_coord_t>(PIG_R * 2));

  lv_obj_t *pig_snout = lv_obj_create(pig);
  lv_obj_remove_style_all(pig_snout);
  lv_obj_set_size(pig_snout, 13, 17);
  lv_obj_set_style_bg_opa(pig_snout, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(pig_snout, lv_color_hex(0x9BE8AE), 0);
  lv_obj_set_style_radius(pig_snout, 4, 0);
  lv_obj_clear_flag(pig_snout, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(pig_snout, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(pig_snout, LV_ALIGN_RIGHT_MID, -4, 0);

  for (int i = 0; i < 2; i++) {
    lv_obj_t *nostril = lv_obj_create(pig_snout);
    lv_obj_remove_style_all(nostril);
    lv_obj_set_size(nostril, 4, 4);
    lv_obj_set_style_bg_opa(nostril, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(nostril, lv_color_hex(0x1E7A38), 0);
    lv_obj_set_style_radius(nostril, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(nostril, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(nostril, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(nostril, i == 0 ? LV_ALIGN_BOTTOM_MID : LV_ALIGN_TOP_MID, 0, i == 0 ? -2 : 2);
  }

  for (int i = 0; i < 2; i++) {
    lv_obj_t *eye = lv_obj_create(pig);
    lv_obj_remove_style_all(eye);
    lv_obj_set_size(eye, 7, 7);
    lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(eye, lv_color_white(), 0);
    lv_obj_set_style_radius(eye, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(eye, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(eye, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(eye, i == 0 ? LV_ALIGN_BOTTOM_LEFT : LV_ALIGN_TOP_LEFT, 4, i == 0 ? -2 : 2);

    lv_obj_t *pupil = lv_obj_create(eye);
    lv_obj_remove_style_all(pupil);
    lv_obj_set_size(pupil, 4, 4);
    lv_obj_set_style_bg_opa(pupil, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(pupil, lv_color_black(), 0);
    lv_obj_set_style_radius(pupil, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(pupil, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(pupil, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(pupil);
  }

  return pig;
}

void on_open(lv_obj_t *parent) {
  hud_scratch = lv_canvas_create(parent);
  lv_canvas_set_buffer(hud_scratch, hud_scratch_buf, HUD_SRC_W, HUD_SRC_H, LV_IMG_CF_TRUE_COLOR_ALPHA);
  lv_obj_add_flag(hud_scratch, LV_OBJ_FLAG_HIDDEN);  // only ever used as a pixel source, never drawn itself

  status_canvas = lv_canvas_create(parent);
  lv_canvas_set_buffer(status_canvas, hud_status_buf, HUD_DST_W, HUD_DST_H, LV_IMG_CF_TRUE_COLOR_ALPHA);
  lv_obj_set_pos(status_canvas, HUD_X0, 10);  // footprint: physical y [10, 10+HUD_SRC_W]

  // Ground: a static strip along the large-gy edge of game space (where
  // gravity pulls things toward), created first so everything else draws
  // on top of it. Its child (the grass line) sits on the *small-gy* edge
  // of the strip, which set_game_rect's rotation puts at local
  // LEFT_MID - see game_to_px: increasing gy increases physical x, so the
  // smaller-gy/surface edge of a ground rect ends up at the smaller-local-x
  // side of the widget, i.e. LEFT_MID, not TOP/BOTTOM.
  ground = lv_obj_create(parent);
  lv_obj_remove_style_all(ground);
  lv_obj_set_style_bg_opa(ground, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(ground, lv_color_hex(0x8B6B3D), 0);
  lv_obj_clear_flag(ground, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(ground, LV_OBJ_FLAG_CLICKABLE);
  set_game_rect(ground, 0, GAME_H - GROUND_H, GAME_W, GROUND_H);

  lv_obj_t *grass = lv_obj_create(ground);
  lv_obj_remove_style_all(grass);
  lv_obj_set_size(grass, 4, LV_PCT(100));
  lv_obj_set_style_bg_opa(grass, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(grass, lv_color_hex(0x5DBB4C), 0);
  lv_obj_clear_flag(grass, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(grass, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(grass, LV_ALIGN_LEFT_MID, 0, 0);

  // Block: a plain rect base plus a lighter top-edge/darker bottom-edge
  // strip for a simple beveled "wood crate" look. Children use ordinary
  // relative alignment (not the game_to_px rotation), so once positioned
  // here they just ride along with the parent for free whenever it's
  // repositioned in game space (spawn_level never touches them again).
  for (int i = 0; i < MAX_BLOCKS_PER_LEVEL; i++) blocks[i] = build_block(parent);

  // Pig: body plus a snout (with nostrils) and a pair of eyes. Same
  // relative-children-ride-along-for-free approach as the block above.
  for (int i = 0; i < MAX_PIGS_PER_LEVEL; i++) pigs[i] = build_pig(parent);

  // Slingshot: a static wooden Y (one post, two prongs) planted at the
  // anchor - drawn once here and never repositioned. Created before the
  // bird below so the bird's body draws on top of the pouch area, like
  // it's actually resting in the fork.
  fork_post_pts[0] = game_to_px(ANCHOR_X, POST_BASE_Y);
  fork_post_pts[1] = game_to_px(ANCHOR_X, FORK_Y);
  fork_post = lv_line_create(parent);
  lv_obj_set_style_line_color(fork_post, lv_color_hex(0x6B4F2E), 0);
  lv_obj_set_style_line_width(fork_post, 4, 0);
  lv_obj_set_style_line_rounded(fork_post, true, 0);
  lv_line_set_points(fork_post, fork_post_pts, 2);

  fork_left_pts[0] = game_to_px(ANCHOR_X, FORK_Y);
  fork_left_pts[1] = game_to_px(ANCHOR_X - PRONG_SPREAD, PRONG_TIP_Y);
  fork_left = lv_line_create(parent);
  lv_obj_set_style_line_color(fork_left, lv_color_hex(0x6B4F2E), 0);
  lv_obj_set_style_line_width(fork_left, 4, 0);
  lv_obj_set_style_line_rounded(fork_left, true, 0);
  lv_line_set_points(fork_left, fork_left_pts, 2);

  fork_right_pts[0] = game_to_px(ANCHOR_X, FORK_Y);
  fork_right_pts[1] = game_to_px(ANCHOR_X + PRONG_SPREAD, PRONG_TIP_Y);
  fork_right = lv_line_create(parent);
  lv_obj_set_style_line_color(fork_right, lv_color_hex(0x6B4F2E), 0);
  lv_obj_set_style_line_width(fork_right, 4, 0);
  lv_obj_set_style_line_rounded(fork_right, true, 0);
  lv_line_set_points(fork_right, fork_right_pts, 2);

  // Rubber bands - repositioned by update_bands() every time the bird
  // moves in the sling (place_bird_at_anchor/on_pull), and hidden while
  // the bird's actually in flight (launch()).
  band_line_l = lv_line_create(parent);
  lv_obj_set_style_line_color(band_line_l, lv_color_hex(0xC9A876), 0);
  lv_obj_set_style_line_width(band_line_l, 2, 0);

  band_line_r = lv_line_create(parent);
  lv_obj_set_style_line_color(band_line_r, lv_color_hex(0xC9A876), 0);
  lv_obj_set_style_line_width(band_line_r, 2, 0);

  // Bird: modeled on the classic Angry Birds "Red" - round body, cream
  // belly, orange beak, a single angry eye with an angled eyebrow, and a
  // tuft of head feathers. Doesn't dynamically rotate to face its
  // direction of travel (LVGL 8 makes that a lot more involved for a
  // composite sprite like this - would mean re-deriving every child's
  // offset from the velocity angle each tick, not just setting it once
  // here), so it always "faces" the same fixed way; a cosmetic
  // simplification, not a physics one.
  //
  // Children are laid out with plain LVGL alignment, which is relative to
  // the *physical, unrotated* panel - not game space. Per game_to_px,
  // physical "up" (toward TOP_MID) is game +X, i.e. landscape-forward
  // (the direction the bird actually flies, toward the pig); physical
  // "right" (RIGHT_MID) is game +Y, i.e. landscape-down (the ground);
  // physical "left" (LEFT_MID) is landscape-up (the sky). So the beak
  // faces TOP_MID, the belly sits at RIGHT_MID, and the head tuft sits at
  // LEFT_MID - that's what makes the bird read right-side-up and
  // forward-facing once the panel is actually turned sideways to play.
  bird = lv_obj_create(parent);
  lv_obj_remove_style_all(bird);
  lv_obj_set_style_bg_opa(bird, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(bird, lv_color_hex(0xE8452C), 0);
  lv_obj_set_style_radius(bird, LV_RADIUS_CIRCLE, 0);
  lv_obj_clear_flag(bird, LV_OBJ_FLAG_SCROLLABLE);
  // See the same-purpose comment on block's sizing above.
  lv_obj_set_size(bird, static_cast<lv_coord_t>(BIRD_R * 2), static_cast<lv_coord_t>(BIRD_R * 2));

  // Belly: lighter patch on the ground-facing side (RIGHT_MID).
  lv_obj_t *bird_belly = lv_obj_create(bird);
  lv_obj_remove_style_all(bird_belly);
  lv_obj_set_size(bird_belly, 20, 20);
  lv_obj_set_style_bg_opa(bird_belly, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(bird_belly, lv_color_hex(0xF5D9A8), 0);
  lv_obj_set_style_radius(bird_belly, LV_RADIUS_CIRCLE, 0);
  lv_obj_clear_flag(bird_belly, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(bird_belly, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(bird_belly, LV_ALIGN_RIGHT_MID, -4, 0);

  // Beak: forward-facing (TOP_MID), pokes out past the body edge.
  lv_obj_t *bird_beak = lv_obj_create(bird);
  lv_obj_remove_style_all(bird_beak);
  lv_obj_set_size(bird_beak, 12, 12);
  lv_obj_set_style_bg_opa(bird_beak, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(bird_beak, lv_color_hex(0xFFA500), 0);
  lv_obj_clear_flag(bird_beak, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(bird_beak, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_transform_angle(bird_beak, 450, 0);  // 45deg square -> diamond, pokes out like a beak
  lv_obj_align(bird_beak, LV_ALIGN_TOP_MID, 0, -7);

  // Head tuft: two small feather spikes on the sky-facing side (LEFT_MID).
  lv_obj_t *bird_feather1 = lv_obj_create(bird);
  lv_obj_remove_style_all(bird_feather1);
  lv_obj_set_size(bird_feather1, 8, 8);
  lv_obj_set_style_bg_opa(bird_feather1, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(bird_feather1, lv_color_hex(0x1A1A1A), 0);
  lv_obj_clear_flag(bird_feather1, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(bird_feather1, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_transform_angle(bird_feather1, 450, 0);
  lv_obj_align(bird_feather1, LV_ALIGN_LEFT_MID, -3, -7);

  lv_obj_t *bird_feather2 = lv_obj_create(bird);
  lv_obj_remove_style_all(bird_feather2);
  lv_obj_set_size(bird_feather2, 6, 6);
  lv_obj_set_style_bg_opa(bird_feather2, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(bird_feather2, lv_color_hex(0x1A1A1A), 0);
  lv_obj_clear_flag(bird_feather2, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(bird_feather2, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_transform_angle(bird_feather2, 450, 0);
  lv_obj_align(bird_feather2, LV_ALIGN_LEFT_MID, -1, 4);

  // Eye: forward+sky corner (TOP_LEFT) - the side facing both "ahead" and
  // "up", where a bird's eye actually sits.
  lv_obj_t *bird_eye = lv_obj_create(bird);
  lv_obj_remove_style_all(bird_eye);
  lv_obj_set_size(bird_eye, 12, 12);
  lv_obj_set_style_bg_opa(bird_eye, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(bird_eye, lv_color_white(), 0);
  lv_obj_set_style_radius(bird_eye, LV_RADIUS_CIRCLE, 0);
  lv_obj_clear_flag(bird_eye, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(bird_eye, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(bird_eye, LV_ALIGN_TOP_LEFT, 2, 2);

  lv_obj_t *bird_pupil = lv_obj_create(bird_eye);
  lv_obj_remove_style_all(bird_pupil);
  lv_obj_set_size(bird_pupil, 5, 5);
  lv_obj_set_style_bg_opa(bird_pupil, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(bird_pupil, lv_color_black(), 0);
  lv_obj_set_style_radius(bird_pupil, LV_RADIUS_CIRCLE, 0);
  lv_obj_clear_flag(bird_pupil, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(bird_pupil, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_center(bird_pupil);

  // Eyebrow: angled slash above the eye (further toward the sky/LEFT_MID
  // corner) for the signature Angry Birds scowl.
  lv_obj_t *bird_eyebrow = lv_obj_create(bird);
  lv_obj_remove_style_all(bird_eyebrow);
  lv_obj_set_size(bird_eyebrow, 11, 4);
  lv_obj_set_style_bg_opa(bird_eyebrow, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(bird_eyebrow, lv_color_hex(0x1A1A1A), 0);
  lv_obj_clear_flag(bird_eyebrow, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(bird_eyebrow, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_transform_angle(bird_eyebrow, 3300, 0);  // -30deg slant
  lv_obj_align(bird_eyebrow, LV_ALIGN_TOP_LEFT, -1, -3);

  hint_canvas = lv_canvas_create(parent);
  lv_canvas_set_buffer(hint_canvas, hud_hint_buf, HUD_DST_W, HUD_DST_H, LV_IMG_CF_TRUE_COLOR_ALPHA);
  // footprint: physical y [GAME_W-10-HUD_SRC_W, GAME_W-10] - well clear of
  // status_canvas's [10, 10+HUD_SRC_W] band above.
  lv_obj_set_pos(hint_canvas, HUD_X0, static_cast<lv_coord_t>(GAME_W) - 10 - HUD_SRC_W);

#if defined(BOARD_TOUCH_LCD147)
  // Covers the whole panel below/above the text strips so a press
  // anywhere starts a pull (not just a precise grab on the small bird
  // widget) - see on_pull(). wants_raw_touch means the launcher's
  // generic tap-anywhere overlay is skipped for this app, so this app
  // has to handle its own taps too.
  drag_area = lv_obj_create(parent);
  lv_obj_remove_style_all(drag_area);
  lv_obj_set_size(drag_area, LCD_PANEL_WIDTH, LCD_PANEL_HEIGHT - 48);
  lv_obj_set_pos(drag_area, 0, 24);
  lv_obj_clear_flag(drag_area, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(drag_area, on_pull, LV_EVENT_PRESSED, nullptr);
  lv_obj_add_event_cb(drag_area, on_pull, LV_EVENT_PRESSING, nullptr);
  lv_obj_add_event_cb(drag_area, on_release, LV_EVENT_RELEASED, nullptr);
  lv_obj_add_event_cb(drag_area, on_area_tap, LV_EVENT_SHORT_CLICKED, nullptr);
#else
  meter_bar = lv_bar_create(parent);
  lv_obj_set_size(meter_bar, 140, 14);
  lv_obj_align(meter_bar, LV_ALIGN_BOTTOM_MID, 0, -30);
  lv_bar_set_range(meter_bar, 0, 100);
  lv_obj_set_style_bg_color(meter_bar, lv_color_hex(0x333333), 0);
  lv_obj_set_style_bg_color(meter_bar, lv_color_hex(0xE8452C), LV_PART_INDICATOR);
#endif

  randomSeed(micros());
  reset_game();

  tick_timer = lv_timer_create(game_tick, TICK_MS, nullptr);
}

void on_close() {
  if (tick_timer) {
    lv_timer_del(tick_timer);
    tick_timer = nullptr;
  }
  hud_scratch = status_canvas = hint_canvas = bird = ground = nullptr;
  for (int i = 0; i < MAX_PIGS_PER_LEVEL; i++) pigs[i] = nullptr;
  for (int i = 0; i < MAX_BLOCKS_PER_LEVEL; i++) blocks[i] = nullptr;
  fork_post = fork_left = fork_right = band_line_l = band_line_r = nullptr;
#if defined(BOARD_TOUCH_LCD147)
  drag_area = nullptr;
#else
  meter_bar = nullptr;
#endif
}

// Non-touch board only in practice: wants_raw_touch means the touch
// board's generic tap-anywhere overlay is skipped for this app, and its
// physical button is solely a quick-press-to-home shortcut (see
// launcher_handle_button) - so on the touch board nothing ever calls
// this.
void on_short_press() {
  if (state == State::AIMING) {
    launch(NT_VX, -(meter_value * NT_MAX_POWER));
  } else if (state == State::GAME_OVER) {
    reset_game();
  }
}

// Home-screen icon: a small bird face built from the same primitive-shape
// technique as the in-game bird above (rotated squares for the diamond
// beak/feather tufts, a circle eye+pupil, an angled eyebrow bar) - no
// single LV_SYMBOL_* glyph reads as "bird", so this draws one directly
// into the icon tile instead (see build_icon in app_interface.h). `tile`
// is a TILE_SIZE (100x100) square already colored icon_color below, which
// doubles as the bird's body - upright/facing right, unlike the in-game
// bird's landscape-rotated convention, since a static home-screen icon
// has no "played sideways" orientation to match.
void build_bird_icon(lv_obj_t *tile) {
  auto diamond = [&](lv_coord_t cx, lv_coord_t cy, lv_coord_t size, lv_color_t color) {
    lv_obj_t *d = lv_obj_create(tile);
    lv_obj_remove_style_all(d);
    lv_obj_set_size(d, size, size);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(d, color, 0);
    lv_obj_set_style_radius(d, 2, 0);
    lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(d, cx - size / 2, cy - size / 2);
    lv_obj_set_style_transform_angle(d, 450, 0);  // 45deg square -> diamond
  };

  // Head tuft feathers, upper-left.
  diamond(26, 22, 14, lv_color_hex(0x1A1A1A));
  diamond(20, 34, 10, lv_color_hex(0x1A1A1A));

  // Belly: light patch, lower-center.
  lv_obj_t *belly = lv_obj_create(tile);
  lv_obj_remove_style_all(belly);
  lv_obj_set_size(belly, 52, 44);
  lv_obj_set_style_bg_opa(belly, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(belly, lv_color_hex(0xF5D9A8), 0);
  lv_obj_set_style_radius(belly, LV_RADIUS_CIRCLE, 0);
  lv_obj_clear_flag(belly, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(belly, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_pos(belly, 20, 52);

  // Beak, pointing right.
  diamond(76, 58, 24, lv_color_hex(0xFFA500));

  // Eye, upper-right.
  lv_obj_t *eye = lv_obj_create(tile);
  lv_obj_remove_style_all(eye);
  lv_obj_set_size(eye, 26, 26);
  lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(eye, lv_color_white(), 0);
  lv_obj_set_style_radius(eye, LV_RADIUS_CIRCLE, 0);
  lv_obj_clear_flag(eye, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(eye, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_pos(eye, 45, 19);

  lv_obj_t *pupil = lv_obj_create(eye);
  lv_obj_remove_style_all(pupil);
  lv_obj_set_size(pupil, 12, 12);
  lv_obj_set_style_bg_opa(pupil, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(pupil, lv_color_black(), 0);
  lv_obj_set_style_radius(pupil, LV_RADIUS_CIRCLE, 0);
  lv_obj_clear_flag(pupil, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(pupil, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(pupil, LV_ALIGN_CENTER, 4, 0);

  // Eyebrow: angled slash above the eye, for the signature scowl.
  lv_obj_t *eyebrow = lv_obj_create(tile);
  lv_obj_remove_style_all(eyebrow);
  lv_obj_set_size(eyebrow, 22, 6);
  lv_obj_set_style_bg_opa(eyebrow, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(eyebrow, lv_color_hex(0x1A1A1A), 0);
  lv_obj_set_style_radius(eyebrow, 3, 0);
  lv_obj_clear_flag(eyebrow, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(eyebrow, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_pos(eyebrow, 41, 13);
  lv_obj_set_style_transform_angle(eyebrow, 3350, 0);  // -25deg slant
}

}  // namespace

const AppDescriptor birds_app = {
    .name = "Birds",
    .icon_symbol = nullptr,  // unused - build_icon below draws the icon instead
    .icon_color = lv_color_hex(0xE8452C),
    .build_icon = build_bird_icon,
    .on_open = on_open,
    .on_close = on_close,
    .on_short_press = on_short_press,
    // Touch board only (no-op elsewhere, see app_interface.h) - needed so
    // the play field can be pulled on directly to aim.
    .wants_raw_touch = true,
};

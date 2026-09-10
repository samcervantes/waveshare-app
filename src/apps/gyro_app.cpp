#include "gyro_app.h"

#include <Arduino.h>
#include <lvgl.h>
#include <math.h>

#include "config.h"
#include "earth_data/earth.h"
#include "imu.h"

// Two swipeable pages, both driven by the QMI8658A (imu.cpp/.h):
//  - Level: a bubble-level style visualization of the accelerometer - a
//    dot moves within a frame, its X/Y position directly tracking the
//    accelerometer's X/Y axes, and its color tracking the Z axis (green
//    when flat, red as the device tips onto its edge). Raw accel/gyro
//    numbers are printed below it.
//  - Ball: a shaded, textured 3D sphere whose orientation directly
//    mirrors the device's current pitch/roll (the same values the Level
//    page shows as text) - tilt the device, the ball tilts with it.
//
// This app used to be a single-page airplane-style rotating attitude
// indicator (rotating ground/sky disc for pitch+roll). Dropped for two
// reasons, both found via real-hardware testing - see git history for the
// gory details:
//  1. Rotating a large filled LVGL object triggers LVGL 8's software
//     "layer" compositing path (lv_draw_sw_layer_create), which mallocs
//     an offscreen buffer sized to the rotated object. A ~260x260 object
//     needed 100KB+, which this board's ~130KB free heap couldn't supply
//     - the failed allocation crashed the board (Guru Meditation "Store
//     access fault"). A smaller rotated element (~80x4px) avoided the
//     crash, but:
//  2. Even with the crash fixed, a hand tilting the board is never a
//     clean single-axis rotation, so two derived values (pitch from one
//     formula, roll from another) driving two different widgets was
//     genuinely hard to read - rolling the device visibly moved the
//     *pitch* indicator, which looked like a bug but was really just
//     pitch/roll cross-talk from an imprecise hand tilt.
// The Level page's position+color encoding sidesteps both: nothing
// rotates (no layer, no crash risk ever), and each accelerometer axis
// maps to one visibly distinct property.
//
// The Ball page needs an actual shaded, textured sphere ("maybe a 3D
// earth"), not just a wireframe - LVGL's own widgets can't draw that, so
// this renders it pixel-by-pixel into an lv_canvas: for each pixel, treat
// it as a point on a sphere seen in orthographic projection (nx,ny are the
// pixel's position scaled to [-1,1], nz = sqrt(1-nx^2-ny^2) is how far
// toward the viewer that point on the sphere surface is), rotate that
// point by the device's pitch/roll, convert the rotated point to a
// latitude/longitude pair and sample a real NASA public-domain earth
// texture at that spot (see earth_data/earth.h - a first version used a
// procedural checkerboard pattern instead, reported "lame"), and shade it
// by how much it faces a fixed light direction. This still avoids LVGL's
// transform_angle/layer system entirely (see above) - it's plain
// per-pixel writes into a canvas buffer, not a rotated LVGL object.
//
// The canvas buffer (ball_canvas_buf below) is a fixed-size global array,
// not a runtime heap allocation - same reasoning as avoiding the rotated-
// object crash: a heap allocation this large (the buffer is ~20KB) could
// fail/fragment on this board's tight RAM, but a compile-time-sized global
// array is reserved once, always in the same spot, with no allocation
// call (and thus no allocation failure) at app-open time.
//
// The accel-axis-to-screen-axis mapping on the Level page (does tilting
// right move the dot right or left) was tuned against this specific board
// by trial and error on real hardware - both axes ended up negated.
//
// The Ball page went through two other designs before this one - a
// gyroscope-angular-*speed* version (always spun the same direction
// regardless of which way the device twisted - read as "broken"), then a
// single-signed-gyro-axis version (spun, but not around an axis the user
// could actually feel by twisting the device) - both replaced with a
// direct mapping instead: the ball's rendered orientation is just the
// same instantaneous pitch/roll the Level page already computes from the
// accelerometer, not anything integrated over time. Tilt the device, the
// ball is tilted exactly that much, right now - no accumulation, no
// axis-of-rotation guessing about what "twist" means, and it reuses the
// exact pitch/roll math (and sign conventions) already tuned against this
// board on the Level page.

namespace {

constexpr lv_coord_t FRAME_SIZE = 140;
constexpr lv_coord_t FRAME_RADIUS = FRAME_SIZE / 2;
constexpr lv_coord_t FRAME_TOP = 16;
constexpr lv_coord_t BUBBLE_SIZE = 22;
constexpr lv_coord_t BUBBLE_RADIUS = BUBBLE_SIZE / 2;
// Pixels of travel per g - the bubble reaches the edge of its allowed
// travel area at roughly 1g of tilt on that axis.
constexpr float BUBBLE_PX_PER_G = 70.0f;
constexpr lv_coord_t BUBBLE_MAX_OFFSET = FRAME_RADIUS - BUBBLE_RADIUS - 2;
constexpr uint32_t POLL_MS = 50;

constexpr float DEG_PER_RAD = 180.0f / static_cast<float>(M_PI);
constexpr float RAD_PER_DEG = static_cast<float>(M_PI) / 180.0f;

// Kept modest on purpose: the buffer below is BALL_SIZE*BALL_SIZE*2 bytes
// (100x100 -> ~20KB) of *permanent* static RAM (see the file header
// comment), and the render loop below is one sqrtf plus a handful of
// multiplies per pixel *inside* the circle - both scale with the square of
// this number, and this board has neither much RAM nor a hardware FPU
// (ESP32-C6 is RV32IMAC - no F extension - so every float op here runs
// through software emulation).
constexpr int BALL_SIZE = 100;
constexpr int BALL_RADIUS = BALL_SIZE / 2;
// Redraw the sphere every other IMU poll (~10 times/second). Briefly
// tried every tick (~20/s, matching POLL_MS) to make the spin look
// smoother, but that made the physical BOOT button ("go home") miss
// presses on this page specifically - the render call is long enough
// that it can block the main loop() past the point where input.cpp's
// polled button debounce would otherwise catch a quick press-then-release
// edge (see input.cpp's own comment on why a polled digitalRead() can
// miss a full press cycle that happens between two poll() calls). Going
// home working reliably matters more than this page's smoothness, so
// this stays throttled - if the render gets meaningfully cheaper later
// (or input handling becomes interrupt-driven instead of polled), it's
// safe to revisit.
constexpr int BALL_RENDER_EVERY_N_TICKS = 2;

lv_obj_t *root = nullptr;
lv_obj_t *page_level = nullptr;
lv_obj_t *page_ball = nullptr;
int current_page = 0;  // 0 = Level, 1 = Ball

lv_obj_t *bubble = nullptr;
lv_obj_t *pitch_roll_label = nullptr;
lv_obj_t *accel_label = nullptr;
lv_obj_t *gyro_label = nullptr;

// Precomputed once in on_open (see below), not called as powf() per pixel
// per channel in render_ball - a texture's channel value only has 256
// possible inputs, so a 256-entry table computed once up front is exact
// and free at render time, versus 3 powf() calls (one of the more
// expensive software-float functions on this FPU-less chip) for every one
// of the ~7800 pixels inside the sphere, every render.
uint8_t gamma_lut[256];

lv_obj_t *ball_canvas = nullptr;
// Permanent static storage, not heap - see the file header comment.
lv_color_t ball_canvas_buf[BALL_SIZE * BALL_SIZE];
// Latest pitch/roll, read by render_ball() - stored here rather than
// passed as an argument because rendering is throttled to every other
// tick (see BALL_RENDER_EVERY_N_TICKS) and needs the most recent values
// on ticks where it doesn't get a fresh call.
float ball_pitch_deg = 0.0f;
float ball_roll_deg = 0.0f;
int poll_tick_count = 0;

lv_timer_t *poll_timer = nullptr;

void show_page(int idx) {
  current_page = idx;
  if (idx == 0) {
    lv_obj_clear_flag(page_level, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(page_ball, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(page_level, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(page_ball, LV_OBJ_FLAG_HIDDEN);
  }
}

// Swipe direction doesn't matter much with only 2 pages (either one just
// toggles), but LV_DIR_LEFT advancing/LV_DIR_RIGHT going back matches the
// convention used elsewhere in this project (launcher.cpp's page swipe,
// stock_app's timescale swipe).
void gesture_cb(lv_event_t * /*e*/) {
  lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
  if (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT) {
    show_page(current_page == 0 ? 1 : 0);
  }
}

void update_level(const ImuSample &s) {
  // Both negated per user feedback on real hardware.
  float dx = -s.accel_x * BUBBLE_PX_PER_G;
  float dy = -s.accel_y * BUBBLE_PX_PER_G;
  float dist = sqrtf(dx * dx + dy * dy);
  if (dist > BUBBLE_MAX_OFFSET) {
    float scale = BUBBLE_MAX_OFFSET / dist;
    dx *= scale;
    dy *= scale;
  }
  lv_obj_set_pos(bubble, FRAME_RADIUS - BUBBLE_RADIUS + static_cast<lv_coord_t>(dx),
                 FRAME_RADIUS - BUBBLE_RADIUS + static_cast<lv_coord_t>(dy));

  // Green when level (|accel_z| close to 1g), red as the device tips onto
  // its edge (accel_z close to 0).
  float level_t = 1.0f - fabsf(s.accel_z);
  if (level_t < 0.0f) level_t = 0.0f;
  if (level_t > 1.0f) level_t = 1.0f;
  // mix is the weight of the first color (red) - so it should grow as
  // level_t (how far from level) grows.
  lv_color_t level_color =
      lv_color_mix(lv_color_hex(0xFF453A), lv_color_hex(0x30D158), static_cast<lv_opa_t>(level_t * 255.0f));
  lv_obj_set_style_bg_color(bubble, level_color, 0);

  float roll_deg = atan2f(s.accel_y, s.accel_z) * DEG_PER_RAD;
  float pitch_deg = atan2f(-s.accel_x, sqrtf(s.accel_y * s.accel_y + s.accel_z * s.accel_z)) * DEG_PER_RAD;
  // Shared with the Ball page - see ball_pitch_deg/ball_roll_deg's comment.
  ball_pitch_deg = pitch_deg;
  ball_roll_deg = roll_deg;

  // Plain libc snprintf, not lv_label_set_text_fmt: this project's
  // lv_conf.h has LV_SPRINTF_USE_FLOAT set to 0, so LVGL's own minimal
  // printf doesn't support %f - it prints a literal 'f' and, worse,
  // doesn't consume the argument, silently shifting every argument after
  // it (see stock_app.cpp's apply_data for the first time this bit us).
  // Single spaces (not the wider double/triple spacing this started with) -
  // keeps these reliably on one line at 172px wide even with a couple of
  // negative signs; see the on_open comment for why each label also has a
  // fixed height regardless, so an occasional wrap (e.g. right after a
  // sharp knock making all three axes read large) still can't shift the
  // label below it.
  char pitch_roll_buf[40], accel_buf[48], gyro_buf[48];
  snprintf(pitch_roll_buf, sizeof(pitch_roll_buf), "Pitch %.0f\xC2\xB0 Roll %.0f\xC2\xB0", pitch_deg, roll_deg);
  lv_label_set_text(pitch_roll_label, pitch_roll_buf);
  snprintf(accel_buf, sizeof(accel_buf), "Accel %.2f %.2f %.2f g", s.accel_x, s.accel_y, s.accel_z);
  lv_label_set_text(accel_label, accel_buf);
  snprintf(gyro_buf, sizeof(gyro_buf), "Gyro %.0f %.0f %.0f \xC2\xB0/s", s.gyro_x, s.gyro_y, s.gyro_z);
  lv_label_set_text(gyro_label, gyro_buf);
}

// Renders the sphere into ball_canvas_buf and marks the canvas for
// redraw - see the file header comment for the projection/shading math
// and why this is a plain pixel buffer rather than a rotated LVGL object.
void render_ball() {
  // Pitch/roll swapped per user feedback on real hardware - the ball's
  // rotation axes read backwards relative to the Level page's labels.
  // Pitch's sign has been flipped twice since - first because it read
  // backwards, then again because changing roll's rotation axis (see
  // rz_roll below) flipped pitch's effective direction back along with
  // it.
  float pitch_rad = ball_roll_deg * RAD_PER_DEG;
  float roll_rad = ball_pitch_deg * RAD_PER_DEG;
  float cos_roll = cosf(roll_rad), sin_roll = sinf(roll_rad);
  float cos_pitch = cosf(pitch_rad), sin_pitch = sinf(pitch_rad);

  for (int py = 0; py < BALL_SIZE; py++) {
    // Bail out of a render in progress the instant the physical BOOT
    // button is held down, rather than finishing all BALL_SIZE rows first.
    // input.cpp's button debounce is polled from the main loop(), not
    // interrupt-driven (see its own comment) - so a render that runs long
    // enough can make a quick "go home" press-then-release happen entirely
    // in the gap between two loop() iterations and never register at all,
    // which is exactly what got reported here. A raw digitalRead() is
    // effectively free next to the per-pixel float math below, so
    // checking it every row (not just every render, or every few rows)
    // costs nothing worth measuring but bounds how long this can ever
    // block the main loop to about one row's worth of work. Leaving the
    // canvas half-updated when this fires is fine - the app is about to
    // be torn down anyway.
    if (digitalRead(PIN_BOOT_BUTTON) == LOW) return;

    float ny = (py - BALL_RADIUS + 0.5f) / BALL_RADIUS;
    for (int px = 0; px < BALL_SIZE; px++) {
      float nx = (px - BALL_RADIUS + 0.5f) / BALL_RADIUS;
      float r2 = nx * nx + ny * ny;

      lv_color_t color;
      if (r2 > 1.0f) {
        color = lv_color_black();  // outside the sphere - matches the app's bg
      } else {
        // nz is how far this point on the sphere's surface faces the
        // viewer (1 = dead center, 0 = grazing edge) - the classic
        // orthographic-projection sphere formula.
        float nz = sqrtf(1.0f - r2);
        // Two plain 2D rotations of floats already in hand (not an LVGL
        // transform). Roll rotates (nx,nz) around the vertical (y) axis -
        // tipping the ball around its own vertical axis, the way rolling
        // a marble sideways would, rather than spinning it flat like a
        // coin (an earlier version rotated (nx,ny) around the viewer-
        // facing axis instead, which is what produced that "spins flat"
        // look reported on real hardware). Pitch then rotates (ny, the
        // roll-rotated z) around the horizontal (x) axis, tipping the
        // ball toward/away from the viewer.
        float rx = nx * cos_roll + nz * sin_roll;
        float rz_roll = -nx * sin_roll + nz * cos_roll;
        float ry = ny * cos_pitch + rz_roll * sin_pitch;
        float rz = -ny * sin_pitch + rz_roll * cos_pitch;

        // Sample the real earth texture (earth_data/earth.h) instead of a
        // procedural pattern - reported "lame" as a checkerboard. (rx,ry,rz)
        // is a point on a unit sphere, so this is the standard spherical-
        // to-equirectangular mapping: ry (the rotated "up" component) gives
        // latitude via asin, rx/rz (the two components perpendicular to
        // "up") give longitude via atan2.
        float lat = asinf(ry < -1.0f ? -1.0f : (ry > 1.0f ? 1.0f : ry));
        // Offset so the device lying flat (rx=0, rz=~1, lon=0) looks at
        // the Americas - purely this particular texture's inherent
        // alignment (which longitude ends up at its horizontal center),
        // found by trial and error on real hardware, not a real-world
        // convention. Went 0 (Indian Ocean) -> 180 (Africa) -> 270
        // (Europe, the Americas being a half-turn - 180 deg - further
        // around from there either direction) -> 90.
        constexpr float LON_OFFSET_DEG = 90.0f;
        float lon = atan2f(rx, rz) + LON_OFFSET_DEG * RAD_PER_DEG;
        int u = static_cast<int>((lon / (2.0f * static_cast<float>(M_PI)) + 0.5f) * EARTH_TEX_W);
        int v = static_cast<int>((0.5f - lat / static_cast<float>(M_PI)) * EARTH_TEX_H);
        u = ((u % EARTH_TEX_W) + EARTH_TEX_W) % EARTH_TEX_W;  // wrap the longitude seam
        if (v < 0) v = 0;
        if (v >= EARTH_TEX_H) v = EARTH_TEX_H - 1;
        const uint8_t *texel = &EARTH_TEX_RGB[(v * EARTH_TEX_W + u) * 3];

        // No more Lambertian darkening at all - the procedural colors this
        // shading was tuned against were bright/saturated to begin with,
        // but the real photo texture's natural tones (deep ocean blue,
        // dark forest green) read as "way too dark, barely visible" even
        // at full (1.0) brightness once shading was involved at all.
        //
        // A flat multiplier (tried first) barely helps the darkest blues
        // and greens - doubling a very small channel value is still a
        // small value. A gamma curve (out = 255*(in/255)^gamma) instead
        // lifts dark tones much more than already-bright ones, and the
        // smaller the exponent, the stronger that lift. Went through
        // gamma=0.5 (sqrt), 1/3 (cbrt), and 0.25, all still reported too
        // dark - this uses gamma=0.18: a channel value of 20 now rises to
        // ~166, 200 still only rises to ~246. gamma_lut precomputes this
        // once in on_open (see its own comment) instead of calling powf()
        // here per pixel per channel.
        color = lv_color_make(gamma_lut[texel[0]], gamma_lut[texel[1]], gamma_lut[texel[2]]);
      }
      ball_canvas_buf[py * BALL_SIZE + px] = color;
    }
  }
  lv_obj_invalidate(ball_canvas);
}

// update_level() already refreshed ball_pitch_deg/ball_roll_deg - this
// just throttles the (relatively expensive) full-canvas render, see
// BALL_RENDER_EVERY_N_TICKS. Skipped entirely unless the Ball page is the
// one actually visible - this used to run unconditionally on every poll
// regardless of which page was showing, which meant the expensive
// per-pixel render (a handful of trig/pow calls per pixel, ~7800 pixels)
// was periodically hogging the CPU even while looking at the Level page,
// starving touch/gesture polling and reading as "the whole app is
// laggy and swipes stopped registering" - not just a Ball-page problem.
void update_ball() {
  if (current_page != 1) return;
  if (poll_tick_count % BALL_RENDER_EVERY_N_TICKS == 0) render_ball();
}

void poll_imu(lv_timer_t * /*t*/) {
  poll_tick_count++;
  ImuSample sample;
  if (!imu_read(&sample)) return;
  update_level(sample);
  update_ball();
}

void on_open(lv_obj_t *parent) {
  // See gamma_lut's own comment - computed once here rather than calling
  // powf() per pixel per channel in render_ball().
  constexpr float GAMMA = 0.18f;
  for (int i = 0; i < 256; i++) {
    gamma_lut[i] = static_cast<uint8_t>(powf(i / 255.0f, GAMMA) * 255.0f);
  }

  // A gyro_app-owned wrapper, not `parent` directly - `parent` is shared
  // and reused across every app, so an event callback added straight to
  // it would silently accumulate a fresh duplicate registration every
  // time this app reopens (see stock_app.cpp's on_open for the same
  // reasoning, found the hard way there).
  root = lv_obj_create(parent);
  lv_obj_remove_style_all(root);
  lv_obj_set_size(root, LCD_PANEL_WIDTH, LCD_PANEL_HEIGHT);
  lv_obj_set_pos(root, 0, 0);
  lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
  // Gesture events bubble by default (LV_OBJ_FLAG_GESTURE_BUBBLE), so
  // without clearing it here the swipe handled below would keep bubbling
  // past `root` up to app_root and the screen, where nothing listens.
  lv_obj_clear_flag(root, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(root, gesture_cb, LV_EVENT_GESTURE, nullptr);

  page_level = lv_obj_create(root);
  lv_obj_remove_style_all(page_level);
  lv_obj_set_size(page_level, LCD_PANEL_WIDTH, LCD_PANEL_HEIGHT);
  lv_obj_clear_flag(page_level, LV_OBJ_FLAG_SCROLLABLE);

  page_ball = lv_obj_create(root);
  lv_obj_remove_style_all(page_ball);
  lv_obj_set_size(page_ball, LCD_PANEL_WIDTH, LCD_PANEL_HEIGHT);
  lv_obj_clear_flag(page_ball, LV_OBJ_FLAG_SCROLLABLE);

  // --- Level page ---

  // Plain square frame, not a clipped circle - clip_corner + children is a
  // known rough edge in LVGL 8.x (children can fail to render at all, which
  // is exactly what happened here the first time this used clip_corner).
  // Not worth the risk for a cosmetic rounded bezel.
  lv_obj_t *level_frame = lv_obj_create(page_level);
  lv_obj_remove_style_all(level_frame);
  lv_obj_set_size(level_frame, FRAME_SIZE, FRAME_SIZE);
  lv_obj_align(level_frame, LV_ALIGN_TOP_MID, 0, FRAME_TOP);
  lv_obj_set_style_border_width(level_frame, 3, 0);
  lv_obj_set_style_border_color(level_frame, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(level_frame, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(level_frame, lv_color_hex(0x2C2C2E), 0);
  lv_obj_clear_flag(level_frame, LV_OBJ_FLAG_SCROLLABLE);

  // Fixed crosshair marking dead center (accel_x == accel_y == 0).
  lv_obj_t *h_line = lv_obj_create(level_frame);
  lv_obj_remove_style_all(h_line);
  lv_obj_set_size(h_line, FRAME_SIZE, 1);
  lv_obj_align(h_line, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_opa(h_line, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(h_line, lv_color_hex(0x555555), 0);
  lv_obj_clear_flag(h_line, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *v_line = lv_obj_create(level_frame);
  lv_obj_remove_style_all(v_line);
  lv_obj_set_size(v_line, 1, FRAME_SIZE);
  lv_obj_align(v_line, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_opa(v_line, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(v_line, lv_color_hex(0x555555), 0);
  lv_obj_clear_flag(v_line, LV_OBJ_FLAG_SCROLLABLE);

  // The bubble - only its position (X/Y) and bg_color change, both plain
  // property writes with no LVGL "layer" involved, unlike a rotation
  // would be (see the file header comment).
  //
  // Positioned with lv_obj_set_pos, not lv_obj_align: lv_obj_align()
  // (confirmed by reading lv_obj_pos.c) sets a *persistent* alignment
  // style and stores its x/y args as an offset from that alignment
  // anchor, not an absolute position - it doesn't just move the object
  // once. update_level()'s later lv_obj_set_pos calls write the same
  // underlying x/y style properties, so if this had been created with
  // lv_obj_align(..., LV_ALIGN_CENTER, 0, 0), every later "absolute"
  // position would actually land as an offset *from center*, which is
  // exactly what pinned the bubble to the frame's corner on real hardware
  // even when accel_x/accel_y were both nearly 0.
  bubble = lv_obj_create(level_frame);
  lv_obj_remove_style_all(bubble);
  lv_obj_set_size(bubble, BUBBLE_SIZE, BUBBLE_SIZE);
  lv_obj_set_style_radius(bubble, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(bubble, lv_color_hex(0x30D158), 0);
  lv_obj_set_pos(bubble, FRAME_RADIUS - BUBBLE_RADIUS, FRAME_RADIUS - BUBBLE_RADIUS);
  lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);

  // Fixed width AND fixed height + center align + wrap, not content-sized
  // alignment - two related LVGL gotchas fixed here, both found on real
  // hardware:
  //  1. These labels are empty at creation time (real text lands later, in
  //     update_level), and lv_obj_align_to reads an object's width at
  //     call time - aligning an empty label then filling it in afterward
  //     anchors it from an empty-content position and lets it grow off
  //     the right edge of the screen once real (and wider than 172px)
  //     text arrives. Fixed width fixes this.
  //  2. Even with fixed width, LV_LABEL_LONG_WRAP still lets a label grow
  //     *taller* (2 lines instead of 1) whenever the text happens to be a
  //     little longer - e.g. an extra '-' sign on a negative reading.
  //     Since accel_label/gyro_label are each aligned relative to the
  //     PREVIOUS label's box, and that alignment is computed once at open
  //     time, a later runtime height change (1 line vs 2) doesn't move
  //     anything below it - it just overlaps. Fixed height reserves room
  //     for the 2-line case up front, so the box size (and therefore
  //     every later sibling's position) never changes at runtime, even
  //     though the actual line count inside it still does.
  constexpr lv_coord_t LABEL_WIDTH = LCD_PANEL_WIDTH - 8;
  constexpr lv_coord_t LABEL_H_1LINE = 22;
  constexpr lv_coord_t LABEL_H_2LINE = 40;
  pitch_roll_label = lv_label_create(page_level);
  lv_obj_set_style_text_font(pitch_roll_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(pitch_roll_label, lv_color_white(), 0);
  lv_obj_set_width(pitch_roll_label, LABEL_WIDTH);
  lv_obj_set_height(pitch_roll_label, LABEL_H_1LINE);
  lv_obj_set_style_text_align(pitch_roll_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(pitch_roll_label, LV_LABEL_LONG_WRAP);
  lv_obj_align_to(pitch_roll_label, level_frame, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

  accel_label = lv_label_create(page_level);
  lv_obj_set_style_text_font(accel_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(accel_label, lv_color_white(), 0);
  lv_obj_set_width(accel_label, LABEL_WIDTH);
  lv_obj_set_height(accel_label, LABEL_H_2LINE);
  lv_obj_set_style_text_align(accel_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(accel_label, LV_LABEL_LONG_WRAP);
  lv_obj_align_to(accel_label, pitch_roll_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);

  gyro_label = lv_label_create(page_level);
  lv_obj_set_style_text_font(gyro_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(gyro_label, lv_color_white(), 0);
  lv_obj_set_width(gyro_label, LABEL_WIDTH);
  lv_obj_set_height(gyro_label, LABEL_H_2LINE);
  lv_obj_set_style_text_align(gyro_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(gyro_label, LV_LABEL_LONG_WRAP);
  lv_obj_align_to(gyro_label, accel_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);

  // --- Ball page ---

  // The sphere is entirely pixels rendered into this canvas by
  // render_ball() (see its comment for the projection/shading math) -
  // no border/frame object needed since the render already draws the
  // sphere's own circular silhouette against a black background matching
  // the app's own bg, so it just floats there. Centered on the screen -
  // this page has nothing else on it now that the hint text is gone.
  ball_canvas = lv_canvas_create(page_ball);
  lv_canvas_set_buffer(ball_canvas, ball_canvas_buf, BALL_SIZE, BALL_SIZE, LV_IMG_CF_TRUE_COLOR);
  lv_obj_center(ball_canvas);

  // --- Shared ---

  lv_obj_t *hint = lv_label_create(root);
  lv_label_set_text(hint, "swipe: switch view  |  " HOME_HINT);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(hint, lv_color_hex(0x555555), 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -16);

  ball_pitch_deg = ball_roll_deg = 0.0f;
  show_page(0);

  ImuSample sample;
  if (imu_read(&sample)) {
    update_level(sample);
    update_ball();
  } else {
    lv_label_set_text(pitch_roll_label, "IMU not detected");
  }
  poll_timer = lv_timer_create(poll_imu, POLL_MS, nullptr);
}

void on_close() {
  if (poll_timer) {
    lv_timer_del(poll_timer);
    poll_timer = nullptr;
  }
  root = page_level = page_ball = nullptr;
  bubble = pitch_roll_label = accel_label = gyro_label = nullptr;
  ball_canvas = nullptr;
  poll_tick_count = 0;
}

}  // namespace

const AppDescriptor gyro_app = {
    .name = "Gyro",
    .icon_symbol = LV_SYMBOL_GPS,
    .icon_color = lv_color_hex(0x30D158),
    .on_open = on_open,
    .on_close = on_close,
    .on_short_press = nullptr,
    // Touch board only (no-op elsewhere, see app_interface.h) - needed so
    // the swipe-between-pages gesture reaches this app's own root object
    // directly instead of the launcher's generic tap-anywhere overlay
    // swallowing it first (that overlay doesn't forward gestures).
    .wants_raw_touch = true,
};

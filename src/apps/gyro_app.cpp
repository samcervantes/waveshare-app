#include "gyro_app.h"

#include <Arduino.h>
#include <lvgl.h>
#include <math.h>

#include "config.h"
#include "earth_data/earth.h"
#include "imu.h"

// Three swipeable pages, all driven by the QMI8658A (imu.cpp/.h):
//  - Level: a bubble-level style visualization of the accelerometer - a
//    dot moves within a frame, its X/Y position directly tracking the
//    accelerometer's X/Y axes, and its color tracking the Z axis (green
//    when flat, red as the device tips onto its edge). Raw accel/gyro
//    numbers are printed below it.
//  - Ball: a shaded, textured 3D sphere whose orientation directly
//    mirrors the device's current pitch/roll (the same values the Level
//    page shows as text) - tilt the device, the ball tilts with it.
//  - Attitude: an airplane-style flight attitude indicator - the same 3D
//    sphere as the Ball page, but colored as a sky/ground horizon instead
//    of textured with a map, with a fixed yellow aircraft symbol
//    overlaid on top. This app actually *started* as exactly this kind of
//    single flat rotating horizon disc (see below for why that got
//    replaced) - this page is a deliberate return to that idea, but
//    rendered the same safe way the Ball page is (see render_attitude's
//    own comment for specifics).
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
// The canvas buffer (ball_canvas_buf below) is heap-allocated in on_open
// and freed in on_close, not a permanent global array like an earlier
// version of this comment recommended. That version's reasoning (a fixed
// global can't fail the way a runtime allocation can) is true but
// incomplete: a permanent ~20KB static reservation is gone for the entire
// time the board is powered on, whether or not this page is ever visited
// in a given session - it was found starving *other* apps' heap (the
// Stocks app's TLS handshake, specifically, on a board where free heap
// with WiFi connected runs in the tens-of-KB) even while this one wasn't
// open. Allocating on open and freeing on close gives that 20KB back to
// everything else the vast majority of the time this app isn't the one on
// screen, at the cost of on_open needing to handle the (now real, if
// still rare) case where the allocation itself fails - see its own
// handling below.
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

constexpr int PAGE_COUNT = 3;  // Level, Ball, Attitude

lv_obj_t *root = nullptr;
lv_obj_t *page_level = nullptr;
lv_obj_t *page_ball = nullptr;
lv_obj_t *page_attitude = nullptr;
int current_page = 0;  // 0 = Level, 1 = Ball, 2 = Attitude

lv_obj_t *bubble = nullptr;
lv_obj_t *pitch_roll_label = nullptr;
lv_obj_t *accel_label = nullptr;
lv_obj_t *gyro_label = nullptr;

// Bank-angle pointer on the Attitude page - a small dot that slides along
// the fixed tick-mark scale at the top of that page's frame, the way a
// real attitude indicator's roll pointer does. Moved with lv_obj_set_pos
// (never lv_obj_align - see the Level page's bubble comment for why
// mixing the two on the same object caused real bugs before), so unlike
// the fixed tick marks it can safely be repositioned every frame.
lv_obj_t *bank_pointer = nullptr;

// Precomputed once in on_open (see below), not called as powf() per pixel
// per channel in render_ball - a texture's channel value only has 256
// possible inputs, so a 256-entry table computed once up front is exact
// and free at render time, versus 3 powf() calls (one of the more
// expensive software-float functions on this FPU-less chip) for every one
// of the ~7800 pixels inside the sphere, every render.
uint8_t gamma_lut[256];

lv_obj_t *ball_canvas = nullptr;
// Allocated in on_open, freed in on_close - see the file header comment
// for why this isn't a permanent static array.
lv_color_t *ball_canvas_buf = nullptr;
lv_obj_t *attitude_canvas = nullptr;
// Same reasoning and same lifecycle as ball_canvas_buf - allocated
// alongside it in on_open, freed alongside it in on_close, not held onto
// for the full time gyro_app happens to be open across all 3 pages.
// That's a real, deliberate trade-off, not an oversight: freeing/
// reallocating on every page switch instead would give back another
// ~20KB while looking at the Level page specifically, but adds real
// complexity (the lv_canvas object's buffer pointer would need to be
// re-attached each time) for a benefit already covered by on_close - this
// app is closed far more of the time than any single one of its pages is
// hidden mid-visit. Revisit if this app's per-open memory footprint ever
// becomes its own reported problem.
lv_color_t *attitude_canvas_buf = nullptr;
// Latest pitch/roll, read by render_ball()/render_attitude() - stored
// here rather than passed as an argument because rendering is throttled
// to every other tick (see BALL_RENDER_EVERY_N_TICKS) and needs the most
// recent values on ticks where it doesn't get a fresh call.
float ball_pitch_deg = 0.0f;
float ball_roll_deg = 0.0f;
// Separate pitch/roll for the Attitude page only, referenced to holding
// the device vertically (screen facing the viewer, like a phone) instead
// of flat on a table - the Level/Ball pages must keep using
// ball_pitch_deg/ball_roll_deg's flat-table reference unchanged. Computed
// with the accel_y/accel_z roles swapped relative to the flat-reference
// formula below (accel_y becomes the "which way is down" axis instead of
// accel_z), which is a first guess at the right axis mapping, not yet
// confirmed on hardware - may need a sign or axis swap once tested.
float attitude_pitch_deg = 0.0f;
float attitude_roll_deg = 0.0f;
int poll_tick_count = 0;

lv_timer_t *poll_timer = nullptr;

void show_page(int idx) {
  current_page = idx;
  lv_obj_add_flag(page_level, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(page_ball, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(page_attitude, LV_OBJ_FLAG_HIDDEN);
  lv_obj_t *shown = page_level;
  if (idx == 1) shown = page_ball;
  if (idx == 2) shown = page_attitude;
  lv_obj_clear_flag(shown, LV_OBJ_FLAG_HIDDEN);
}

// LV_DIR_LEFT advances/LV_DIR_RIGHT goes back, wrapping around PAGE_COUNT -
// matches the convention used elsewhere in this project (launcher.cpp's
// page swipe, stock_app's timescale swipe).
void gesture_cb(lv_event_t * /*e*/) {
  lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
  if (dir == LV_DIR_LEFT) {
    show_page((current_page + 1) % PAGE_COUNT);
  } else if (dir == LV_DIR_RIGHT) {
    show_page((current_page + PAGE_COUNT - 1) % PAGE_COUNT);
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

  // Attitude-page-only pitch/roll, referenced to "held vertically" - see
  // attitude_pitch_deg/attitude_roll_deg's declaration comment. Same
  // atan2 formula shape as above, with accel_y taking accel_z's role as
  // the reference ("down") axis and accel_z taking accel_y's role.
  attitude_roll_deg = atan2f(s.accel_z, s.accel_y) * DEG_PER_RAD;
  attitude_pitch_deg = atan2f(-s.accel_x, sqrtf(s.accel_z * s.accel_z + s.accel_y * s.accel_y)) * DEG_PER_RAD;

  // Bank-angle pointer on the Attitude page - updated here (every tick,
  // not gated to that page's canvas render throttle) since moving one
  // small object is negligible cost regardless of which page is actually
  // visible. Uses attitude_pitch_deg, not attitude_roll_deg - matches
  // render_attitude()'s local roll_rad, which (see its own comment) is
  // assigned from the "pitch_deg"-named value due to the earlier swap fix;
  // this keeps the pointer showing the same "roll" the sphere itself
  // rotates by, instead of introducing a second, inconsistent mapping.
  if (bank_pointer) {
    constexpr float POINTER_RADIUS = BALL_RADIUS - 4.0f;
    constexpr float MAX_BANK_DEG = 60.0f;
    float bank = attitude_pitch_deg;
    if (bank > MAX_BANK_DEG) bank = MAX_BANK_DEG;
    if (bank < -MAX_BANK_DEG) bank = -MAX_BANK_DEG;
    float bank_rad = bank * RAD_PER_DEG;
    lv_coord_t dx = static_cast<lv_coord_t>(POINTER_RADIUS * sinf(bank_rad));
    lv_coord_t dy = static_cast<lv_coord_t>(-POINTER_RADIUS * cosf(bank_rad));
    lv_obj_set_pos(bank_pointer, LCD_PANEL_WIDTH / 2 - 3 + dx, LCD_PANEL_HEIGHT / 2 - 3 + dy);
  }

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
  if (!ball_canvas_buf) return;  // allocation failed in on_open - see its own handling
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

// Same sphere-in-orthographic-projection setup as render_ball() (see its
// own comment for the full explanation and why this is a canvas render
// rather than a rotated LVGL object) - the only real difference is what
// color a point on the sphere gets, once its rotated position is known:
// render_ball() looks up a texture pixel, this just splits on the sign of
// the rotated "up" component (ry). ry is 0 dead center exactly when the
// device is level (pitch=roll=0) - checked by hand against the rotation
// math - which is what makes it the right value to draw the horizon line
// on. No texture/gamma lookup needed, so this is noticeably cheaper per
// pixel than render_ball() despite doing the same rotation.
//
// Which side (ry>0 or ry<0) is "sky" is a first guess, not yet confirmed
// on real hardware the way the Ball/Level pages' axis directions were -
// if pitching or rolling the device moves the horizon the wrong way, flip
// the comparison below.
//
// Pitch ladder lines (the short white rungs at +-10/20/30 degrees above
// and below the horizon on a real attitude indicator) are drawn the same
// way as the horizon line itself: ry is sin(pitch angle of that point), so
// a rung at N degrees is just "ry is close to sin(N deg)" - no extra
// per-pixel trig needed, sin(10/20/30 deg) are precomputed constants.
// Restricted to a horizontal band near center (|rx| small) so they read as
// short rungs rather than full-width lines like the horizon itself.
void render_attitude() {
  if (!attitude_canvas_buf) return;  // allocation failed in on_open - see its own handling
  // attitude_pitch_deg/attitude_roll_deg, not ball_*: this page uses its
  // own "held vertically = level" reference - see their declaration comment.
  float pitch_rad = attitude_roll_deg * RAD_PER_DEG;
  float roll_rad = attitude_pitch_deg * RAD_PER_DEG;
  float cos_roll = cosf(roll_rad), sin_roll = sinf(roll_rad);
  float cos_pitch = cosf(pitch_rad), sin_pitch = sinf(pitch_rad);

  constexpr float LINE_HALF_WIDTH = 0.014f;
  constexpr float LADDER_10 = 0.17365f;  // sin(10deg)
  constexpr float LADDER_20 = 0.34202f;  // sin(20deg)
  constexpr float LADDER_30 = 0.5f;      // sin(30deg)
  constexpr float LADDER_40 = 0.64279f;  // sin(40deg)
  constexpr float LADDER_50 = 0.76604f;  // sin(50deg)
  constexpr float LADDER_60 = 0.86603f;  // sin(60deg)
  // 10/20/40/50deg rungs are short and plain, like a real horizon's minor
  // graduations; 30/60deg are the "major" rungs - longer, and get a small
  // perpendicular end-tick (see below) so they read as the ones worth
  // glancing at without needing numbers this display has no room to draw.
  constexpr float MINOR_RX_HALF_WIDTH = 0.22f;
  constexpr float MAJOR_RX_HALF_WIDTH = 0.4f;
  constexpr float END_TICK_HALF_LEN = 0.09f;
  lv_color_t sky = lv_color_hex(0x2E86DE);
  lv_color_t ground = lv_color_hex(0x8B5E3C);
  lv_color_t white = lv_color_white();

  for (int py = 0; py < BALL_SIZE; py++) {
    // See render_ball()'s identical check for why this is here - a render
    // that runs long enough can make the physical "go home" button miss a
    // quick press entirely (input.cpp's debounce is polled, not
    // interrupt-driven).
    if (digitalRead(PIN_BOOT_BUTTON) == LOW) return;

    float ny = (py - BALL_RADIUS + 0.5f) / BALL_RADIUS;
    for (int px = 0; px < BALL_SIZE; px++) {
      float nx = (px - BALL_RADIUS + 0.5f) / BALL_RADIUS;
      float r2 = nx * nx + ny * ny;

      lv_color_t color;
      if (r2 > 1.0f) {
        color = lv_color_black();  // outside the sphere - matches the app's bg
      } else {
        float nz = sqrtf(1.0f - r2);
        // Pitch first: tilts the horizon up/down by rotating (ny, nz)
        // around the screen's horizontal axis. +nz*sin_pitch (not -), to
        // match the sign of the original ry = ny*cos_pitch + rz_roll*sin_pitch
        // (at roll=0, rz_roll reduced to nz) - the first version of this
        // rewrite flipped that sign by accident, which is why pitch
        // direction broke when this composition replaced the old one.
        float ny_pitched = ny * cos_pitch + nz * sin_pitch;
        // Roll second, as an in-plane (nx, ny) rotation - this banks the
        // horizon through the center regardless of pitch, like a real
        // attitude indicator. The old approach (rotating nx into nz, the
        // same composition render_ball() uses for the globe) left ry
        // independent of roll whenever pitch was ~0, which is why rolling
        // the device visually read as the view spinning/yawing instead of
        // banking - see the user report that prompted this fix. Sign
        // flipped (roll_rad negated) per user feedback that the initial
        // direction was reversed.
        float rx = nx * cos_roll + ny_pitched * sin_roll;
        float ry = -nx * sin_roll + ny_pitched * cos_roll;

        float aby = fabsf(ry);
        float abx = fabsf(rx);
        bool minor_rung = (fabsf(aby - LADDER_10) < LINE_HALF_WIDTH || fabsf(aby - LADDER_20) < LINE_HALF_WIDTH ||
                            fabsf(aby - LADDER_40) < LINE_HALF_WIDTH || fabsf(aby - LADDER_50) < LINE_HALF_WIDTH) &&
                           abx < MINOR_RX_HALF_WIDTH;
        bool major_rung =
            (fabsf(aby - LADDER_30) < LINE_HALF_WIDTH || fabsf(aby - LADDER_60) < LINE_HALF_WIDTH) && abx < MAJOR_RX_HALF_WIDTH;
        // Small vertical hash at each end of a major rung - checked
        // independent of abx's upper bound above, just pinned near
        // MAJOR_RX_HALF_WIDTH, so it draws past the rung's own end.
        bool end_tick = (fabsf(aby - LADDER_30) < END_TICK_HALF_LEN || fabsf(aby - LADDER_60) < END_TICK_HALF_LEN) &&
                         fabsf(abx - MAJOR_RX_HALF_WIDTH) < LINE_HALF_WIDTH;

        color = (ry > 0.0f) ? sky : ground;
        if (aby < LINE_HALF_WIDTH) {
          color = white;  // the horizon line itself, full width
        } else if (minor_rung || major_rung || end_tick) {
          color = white;  // a pitch ladder rung (or a major rung's end-tick)
        }
      }
      attitude_canvas_buf[py * BALL_SIZE + px] = color;
    }
  }
  lv_obj_invalidate(attitude_canvas);
}

// Same throttling/page-gating reasoning as update_ball() - see its own
// comment for why this matters (an unconditional render starved input
// handling even while a different page was showing).
void update_attitude() {
  if (current_page != 2) return;
  if (poll_tick_count % BALL_RENDER_EVERY_N_TICKS == 0) render_attitude();
}

void poll_imu(lv_timer_t * /*t*/) {
  poll_tick_count++;
  ImuSample sample;
  if (!imu_read(&sample)) return;
  update_level(sample);
  update_ball();
  update_attitude();
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

  page_attitude = lv_obj_create(root);
  lv_obj_remove_style_all(page_attitude);
  lv_obj_set_size(page_attitude, LCD_PANEL_WIDTH, LCD_PANEL_HEIGHT);
  lv_obj_clear_flag(page_attitude, LV_OBJ_FLAG_SCROLLABLE);

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
  //
  // ball_canvas_buf is allocated here, not a permanent static array - see
  // the file header comment for why. That means this allocation can
  // genuinely fail on this board (unlike a compile-time array), so it's
  // checked - better to show a plain message on this one cosmetic page
  // than to write through a null pointer.
  ball_canvas_buf = static_cast<lv_color_t *>(malloc(static_cast<size_t>(BALL_SIZE) * BALL_SIZE * sizeof(lv_color_t)));
  if (ball_canvas_buf) {
    ball_canvas = lv_canvas_create(page_ball);
    lv_canvas_set_buffer(ball_canvas, ball_canvas_buf, BALL_SIZE, BALL_SIZE, LV_IMG_CF_TRUE_COLOR);
    lv_obj_center(ball_canvas);
  } else {
    lv_obj_t *oom_label = lv_label_create(page_ball);
    lv_label_set_text(oom_label, "Not enough free memory\nfor the globe right now");
    lv_obj_set_style_text_font(oom_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(oom_label, lv_color_white(), 0);
    lv_label_set_long_mode(oom_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(oom_label, LCD_PANEL_WIDTH - 24);
    lv_obj_set_style_text_align(oom_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(oom_label);
  }

  // --- Attitude page ---

  // Same allocate-here-not-permanently reasoning as ball_canvas_buf - see
  // its own comment and attitude_canvas_buf's declaration comment.
  attitude_canvas_buf =
      static_cast<lv_color_t *>(malloc(static_cast<size_t>(BALL_SIZE) * BALL_SIZE * sizeof(lv_color_t)));
  if (attitude_canvas_buf) {
    attitude_canvas = lv_canvas_create(page_attitude);
    lv_canvas_set_buffer(attitude_canvas, attitude_canvas_buf, BALL_SIZE, BALL_SIZE, LV_IMG_CF_TRUE_COLOR);
    lv_obj_center(attitude_canvas);

    // Fixed aircraft reference symbol - a gap-in-the-middle wing bar plus
    // a center dot, the classic look of a real attitude indicator's fixed
    // index mark. Children of page_attitude (not the canvas), created
    // after it so they draw on top of it, and never touched by
    // render_attitude - they stay put while the horizon moves underneath
    // them, same as the real instrument this is modeled on. Aligned once
    // here and never repositioned again, so (unlike the Level page's
    // bubble) there's no conflict with lv_obj_align's "persistent anchor"
    // behavior - see the Level page's bubble comment for why that
    // distinction has mattered here before.
    constexpr lv_coord_t WING_LEN = 36;
    constexpr lv_coord_t WING_GAP = 16;
    lv_obj_t *left_wing = lv_obj_create(page_attitude);
    lv_obj_remove_style_all(left_wing);
    lv_obj_set_size(left_wing, WING_LEN, 4);
    lv_obj_set_style_bg_opa(left_wing, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(left_wing, lv_color_hex(0xFFCC00), 0);
    lv_obj_align(left_wing, LV_ALIGN_CENTER, -(WING_GAP / 2 + WING_LEN / 2), 0);
    lv_obj_clear_flag(left_wing, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *right_wing = lv_obj_create(page_attitude);
    lv_obj_remove_style_all(right_wing);
    lv_obj_set_size(right_wing, WING_LEN, 4);
    lv_obj_set_style_bg_opa(right_wing, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(right_wing, lv_color_hex(0xFFCC00), 0);
    lv_obj_align(right_wing, LV_ALIGN_CENTER, WING_GAP / 2 + WING_LEN / 2, 0);
    lv_obj_clear_flag(right_wing, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *center_dot = lv_obj_create(page_attitude);
    lv_obj_remove_style_all(center_dot);
    lv_obj_set_size(center_dot, 8, 8);
    lv_obj_set_style_radius(center_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(center_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(center_dot, lv_color_hex(0xFFCC00), 0);
    lv_obj_align(center_dot, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(center_dot, LV_OBJ_FLAG_SCROLLABLE);

    // Bank-angle scale: fixed tick marks at 0/+-30/+-60 degrees around the
    // top of the ball, plus bank_pointer (created here, moved every tick
    // in update_level - see its own declaration comment) sliding along
    // the same arc, just inside the ticks. Real attitude indicators do
    // this with a rotating ball and a fixed pointer, or a fixed scale and
    // a pointer attached to the ball - this is the latter, since a
    // pointer is one small object to reposition each frame (cheap, safe)
    // where rotating the scale itself would mean either an LVGL transform
    // (the exact thing this whole app avoids - see the file header
    // comment) or redrawing it into the canvas as more per-pixel work.
    constexpr float TICK_RADIUS = BALL_RADIUS + 8.0f;
    constexpr float TICK_ANGLES_DEG[] = {0.0f, 30.0f, -30.0f, 60.0f, -60.0f};
    for (float angle_deg : TICK_ANGLES_DEG) {
      float angle_rad = angle_deg * RAD_PER_DEG;
      lv_coord_t dx = static_cast<lv_coord_t>(TICK_RADIUS * sinf(angle_rad));
      lv_coord_t dy = static_cast<lv_coord_t>(-TICK_RADIUS * cosf(angle_rad));
      // The 0 deg ("wings level") tick is a small bar, wider than the
      // +-30/+-60 dots, so it reads as the one to line the pointer up
      // with rather than just another tick.
      bool is_zero = (angle_deg == 0.0f);
      lv_obj_t *tick = lv_obj_create(page_attitude);
      lv_obj_remove_style_all(tick);
      lv_obj_set_size(tick, is_zero ? 8 : 4, is_zero ? 4 : 4);
      lv_obj_set_style_radius(tick, is_zero ? 1 : LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_bg_opa(tick, LV_OPA_COVER, 0);
      lv_obj_set_style_bg_color(tick, lv_color_white(), 0);
      lv_obj_align(tick, LV_ALIGN_CENTER, dx, dy);
      lv_obj_clear_flag(tick, LV_OBJ_FLAG_SCROLLABLE);
    }

    bank_pointer = lv_obj_create(page_attitude);
    lv_obj_remove_style_all(bank_pointer);
    lv_obj_set_size(bank_pointer, 6, 6);
    lv_obj_set_style_radius(bank_pointer, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(bank_pointer, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bank_pointer, lv_color_hex(0xFFCC00), 0);
    // Absolute lv_obj_set_pos here too, not lv_obj_align, even for this
    // first placement - see bank_pointer's own declaration comment.
    lv_obj_set_pos(bank_pointer, LCD_PANEL_WIDTH / 2 - 3, LCD_PANEL_HEIGHT / 2 - 3 - static_cast<lv_coord_t>(BALL_RADIUS - 4));
    lv_obj_clear_flag(bank_pointer, LV_OBJ_FLAG_SCROLLABLE);
  } else {
    lv_obj_t *oom_label = lv_label_create(page_attitude);
    lv_label_set_text(oom_label, "Not enough free memory\nfor the horizon right now");
    lv_obj_set_style_text_font(oom_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(oom_label, lv_color_white(), 0);
    lv_label_set_long_mode(oom_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(oom_label, LCD_PANEL_WIDTH - 24);
    lv_obj_set_style_text_align(oom_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(oom_label);
  }

  // --- Shared ---

  lv_obj_t *hint = lv_label_create(root);
  lv_label_set_text(hint, "swipe: switch view  |  " HOME_HINT);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(hint, lv_color_hex(0x555555), 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -16);

  ball_pitch_deg = ball_roll_deg = 0.0f;
  attitude_pitch_deg = attitude_roll_deg = 0.0f;
  show_page(0);

  ImuSample sample;
  if (imu_read(&sample)) {
    update_level(sample);
    update_ball();
    update_attitude();
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
  root = page_level = page_ball = page_attitude = nullptr;
  bubble = pitch_roll_label = accel_label = gyro_label = nullptr;
  bank_pointer = nullptr;
  ball_canvas = attitude_canvas = nullptr;
  // Freed here, not a permanent static array - see the file header
  // comment and ball_canvas_buf's/attitude_canvas_buf's own declaration
  // comments for why. free() on a null pointer (the allocation-failed
  // case) is a defined no-op.
  free(ball_canvas_buf);
  free(attitude_canvas_buf);
  ball_canvas_buf = attitude_canvas_buf = nullptr;
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

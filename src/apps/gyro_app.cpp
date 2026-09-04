#include "gyro_app.h"

#include <Arduino.h>
#include <lvgl.h>
#include <math.h>

#include "config.h"
#include "imu.h"

// Bubble-level style visualization of the QMI8658A accelerometer
// (imu.cpp/.h): a dot moves within a circular frame, its X/Y position
// directly tracking the accelerometer's X/Y axes (no rotation, no derived
// angle math to get backwards), and its color tracking the Z axis (green
// when the device is flat, red as it tips onto its edge). All 6 raw
// degrees of freedom (3-axis accel + 3-axis gyro) are also printed as
// numbers below it.
//
// This app used to be an airplane-style rotating attitude indicator
// (rotating ground/sky disc for pitch+roll). Dropped for two reasons,
// both found via real-hardware testing - see git history for the gory
// details:
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
//     pitch/roll cross-talk from an imprecise hand tilt plus (possibly)
//     an axis convention that doesn't match this chip's physical mounting
//     on this board (never fully confirmed - see below).
// A position+color encoding sidesteps both: nothing rotates (no layer,
// no crash risk ever), and each accelerometer axis maps to one visibly
// distinct property, so it's actually possible to tell from the screen
// which physical motion drives which axis.
//
// The accel-axis-to-screen-axis mapping below (does tilting right move
// the dot right or left, does tilting away move it up or down) is a
// first guess, not yet confirmed against this board's physical IMU
// mounting - if a motion moves the dot the "wrong" way, flip the
// relevant sign in update_from_sample below.

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

lv_obj_t *level_frame = nullptr;
lv_obj_t *bubble = nullptr;
lv_obj_t *pitch_roll_label = nullptr;
lv_obj_t *accel_label = nullptr;
lv_obj_t *gyro_label = nullptr;
lv_timer_t *poll_timer = nullptr;

void update_from_sample(const ImuSample &s) {
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

  float deg_per_rad = 180.0f / static_cast<float>(M_PI);
  float roll_deg = atan2f(s.accel_y, s.accel_z) * deg_per_rad;
  float pitch_deg = atan2f(-s.accel_x, sqrtf(s.accel_y * s.accel_y + s.accel_z * s.accel_z)) * deg_per_rad;

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

void poll_imu(lv_timer_t * /*t*/) {
  ImuSample sample;
  if (imu_read(&sample)) update_from_sample(sample);
}

void on_open(lv_obj_t *parent) {
  // Plain square frame, not a clipped circle - clip_corner + children is a
  // known rough edge in LVGL 8.x (children can fail to render at all, which
  // is exactly what happened here the first time this used clip_corner).
  // Not worth the risk for a cosmetic rounded bezel.
  level_frame = lv_obj_create(parent);
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
  // once. update_from_sample()'s later lv_obj_set_pos calls write the
  // same underlying x/y style properties, so if this had been created
  // with lv_obj_align(..., LV_ALIGN_CENTER, 0, 0), every later "absolute"
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
  //     update_from_sample), and lv_obj_align_to reads an object's width
  //     at call time - aligning an empty label then filling it in
  //     afterward anchors it from an empty-content position and lets it
  //     grow off the right edge of the screen once real (and wider than
  //     172px) text arrives. Fixed width fixes this.
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
  pitch_roll_label = lv_label_create(parent);
  lv_obj_set_style_text_font(pitch_roll_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(pitch_roll_label, lv_color_white(), 0);
  lv_obj_set_width(pitch_roll_label, LABEL_WIDTH);
  lv_obj_set_height(pitch_roll_label, LABEL_H_1LINE);
  lv_obj_set_style_text_align(pitch_roll_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(pitch_roll_label, LV_LABEL_LONG_WRAP);
  lv_obj_align_to(pitch_roll_label, level_frame, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

  accel_label = lv_label_create(parent);
  lv_obj_set_style_text_font(accel_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(accel_label, lv_color_white(), 0);
  lv_obj_set_width(accel_label, LABEL_WIDTH);
  lv_obj_set_height(accel_label, LABEL_H_2LINE);
  lv_obj_set_style_text_align(accel_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(accel_label, LV_LABEL_LONG_WRAP);
  lv_obj_align_to(accel_label, pitch_roll_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);

  gyro_label = lv_label_create(parent);
  lv_obj_set_style_text_font(gyro_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(gyro_label, lv_color_white(), 0);
  lv_obj_set_width(gyro_label, LABEL_WIDTH);
  lv_obj_set_height(gyro_label, LABEL_H_2LINE);
  lv_obj_set_style_text_align(gyro_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(gyro_label, LV_LABEL_LONG_WRAP);
  lv_obj_align_to(gyro_label, accel_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);

  lv_obj_t *hint = lv_label_create(parent);
  lv_label_set_text(hint, HOME_HINT);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(hint, lv_color_hex(0x555555), 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -16);

  ImuSample sample;
  if (imu_read(&sample)) {
    update_from_sample(sample);
  } else {
    lv_label_set_text(pitch_roll_label, "IMU not detected");
  }
  poll_timer = lv_timer_create(poll_imu, 50, nullptr);
}

void on_close() {
  if (poll_timer) {
    lv_timer_del(poll_timer);
    poll_timer = nullptr;
  }
  level_frame = bubble = nullptr;
  pitch_roll_label = accel_label = gyro_label = nullptr;
}

}  // namespace

const AppDescriptor gyro_app = {
    .name = "Gyro",
    .icon_symbol = LV_SYMBOL_GPS,
    .icon_color = lv_color_hex(0x30D158),
    .on_open = on_open,
    .on_close = on_close,
    .on_short_press = nullptr,
};

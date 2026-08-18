#include "photos_app.h"

#include <lvgl.h>

#include "config.h"

// This board has no filesystem/SD slot and only ~512KB SRAM/no PSRAM, so
// there's nowhere to load real JPEG/PNG photos from and no image decoder
// wired into lv_conf.h. Instead, each "photo" below is a small vector scene
// built from plain LVGL shapes (gradients, circles, rotated squares) - a
// handful of sample photos with zero flash/RAM cost beyond a few widgets.

namespace {

constexpr lv_coord_t AREA_TOP = 40;
constexpr lv_coord_t AREA_BOTTOM = 272;
constexpr lv_coord_t AREA_HEIGHT = AREA_BOTTOM - AREA_TOP;

lv_obj_t *scene = nullptr;
lv_obj_t *caption = nullptr;
lv_obj_t *dots_row = nullptr;

lv_obj_t *make_shape(lv_obj_t *parent, lv_coord_t w, lv_coord_t h, lv_color_t color) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_bg_color(o, color, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_radius(o, 0, 0);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

lv_obj_t *make_circle(lv_obj_t *parent, lv_coord_t d, lv_color_t color) {
  lv_obj_t *o = make_shape(parent, d, d, color);
  lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
  return o;
}

// Rotated square, clipped to a triangular "peak" by whatever opaque shape
// gets drawn after it (e.g. a ground strip covering its bottom half).
lv_obj_t *make_peak(lv_obj_t *parent, lv_coord_t size, lv_coord_t cx, lv_coord_t cy, lv_color_t color) {
  lv_obj_t *o = make_shape(parent, size, size, color);
  lv_obj_set_pos(o, cx - size / 2, cy - size / 2);
  lv_obj_set_style_transform_pivot_x(o, size / 2, 0);
  lv_obj_set_style_transform_pivot_y(o, size / 2, 0);
  lv_obj_set_style_transform_angle(o, 450, 0);  // 45.0 degrees
  return o;
}

void set_gradient(lv_obj_t *o, uint32_t top, uint32_t bottom) {
  lv_obj_set_style_bg_color(o, lv_color_hex(top), 0);
  lv_obj_set_style_bg_grad_color(o, lv_color_hex(bottom), 0);
  lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_radius(o, 0, 0);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

void render_sunset(lv_obj_t *s) {
  set_gradient(s, 0xFFC371, 0x4A1E5C);
  lv_obj_t *sun = make_circle(s, 72, lv_color_hex(0xFFE066));
  lv_obj_align(sun, LV_ALIGN_CENTER, 0, -46);
  lv_obj_t *sea = make_shape(s, LCD_PANEL_WIDTH, 56, lv_color_hex(0x1B1B3A));
  lv_obj_align(sea, LV_ALIGN_BOTTOM_MID, 0, 0);
}

void render_ocean(lv_obj_t *s) {
  set_gradient(s, 0xBFEFFF, 0x0A3D62);
  lv_obj_t *sun = make_circle(s, 34, lv_color_hex(0xFFF3B0));
  lv_obj_align(sun, LV_ALIGN_TOP_LEFT, 14, 14);
  const lv_coord_t wave_y[] = {AREA_HEIGHT - 90, AREA_HEIGHT - 58, AREA_HEIGHT - 26};
  const lv_opa_t wave_opa[] = {LV_OPA_30, LV_OPA_50, LV_OPA_70};
  for (int i = 0; i < 3; i++) {
    lv_obj_t *wave = lv_obj_create(s);
    lv_obj_set_size(wave, LCD_PANEL_WIDTH + 20, 20);
    lv_obj_set_pos(wave, -10, wave_y[i]);
    lv_obj_set_style_radius(wave, 10, 0);
    lv_obj_set_style_bg_color(wave, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(wave, wave_opa[i], 0);
    lv_obj_set_style_border_width(wave, 0, 0);
    lv_obj_clear_flag(wave, LV_OBJ_FLAG_SCROLLABLE);
  }
}

void render_mountains(lv_obj_t *s) {
  set_gradient(s, 0xFAD7A0, 0x6A3093);
  const lv_coord_t ground_y = AREA_HEIGHT - 60;
  make_peak(s, 130, 40, ground_y, lv_color_hex(0x8E7CC3));
  make_peak(s, 100, 120, ground_y, lv_color_hex(0x5B4B8A));
  make_peak(s, 90, 80, ground_y, lv_color_hex(0x3E3468));
  lv_obj_t *ground = make_shape(s, LCD_PANEL_WIDTH, AREA_HEIGHT - ground_y, lv_color_hex(0x1D1633));
  lv_obj_set_pos(ground, 0, ground_y);
}

void render_forest(lv_obj_t *s) {
  set_gradient(s, 0xCFE8CF, 0x2E5E3A);
  const lv_coord_t ground_y = AREA_HEIGHT - 40;
  lv_obj_t *ground = make_shape(s, LCD_PANEL_WIDTH, AREA_HEIGHT - ground_y, lv_color_hex(0x1E4028));
  lv_obj_set_pos(ground, 0, ground_y);
  const lv_coord_t tree_x[] = {20, 65, 110, 145};
  const lv_coord_t tree_d[] = {46, 62, 38, 50};
  for (int i = 0; i < 4; i++) {
    lv_coord_t trunk_h = 18;
    lv_obj_t *trunk = make_shape(s, 8, trunk_h, lv_color_hex(0x4A2E1A));
    lv_obj_set_pos(trunk, tree_x[i] - 4, ground_y - trunk_h + 4);
    lv_obj_t *foliage = make_circle(s, tree_d[i], lv_color_hex(0x1F6B3A));
    lv_obj_set_pos(foliage, tree_x[i] - tree_d[i] / 2, ground_y - trunk_h - tree_d[i] + 10);
  }
}

void render_night_sky(lv_obj_t *s) {
  set_gradient(s, 0x1B2A4A, 0x05050F);
  const lv_coord_t star_x[] = {20, 55, 90, 130, 150, 35, 110, 70};
  const lv_coord_t star_y[] = {20, 60, 15, 40, 90, 120, 140, 170};
  for (int i = 0; i < 8; i++) {
    lv_obj_t *star = make_circle(s, 3, lv_color_white());
    lv_obj_set_pos(star, star_x[i], star_y[i]);
  }
  lv_obj_t *moon = make_circle(s, 44, lv_color_hex(0xF5F0DC));
  lv_obj_align(moon, LV_ALIGN_TOP_RIGHT, -20, 20);
  lv_obj_t *bite = make_circle(s, 40, lv_color_hex(0x111327));
  lv_obj_align(bite, LV_ALIGN_TOP_RIGHT, -8, 12);
}

struct Photo {
  const char *name;
  void (*render)(lv_obj_t *);
};

constexpr Photo PHOTOS[] = {
    {"Sunset", render_sunset},
    {"Ocean", render_ocean},
    {"Mountains", render_mountains},
    {"Forest", render_forest},
    {"Night Sky", render_night_sky},
};
constexpr size_t PHOTO_COUNT = sizeof(PHOTOS) / sizeof(PHOTOS[0]);

size_t photo_idx = 0;

void show_photo() {
  lv_obj_clean(scene);
  PHOTOS[photo_idx].render(scene);
  lv_label_set_text_fmt(caption, "%s  (%d/%d)", PHOTOS[photo_idx].name, (int)photo_idx + 1, (int)PHOTO_COUNT);

  for (uint32_t i = 0; i < PHOTO_COUNT; i++) {
    lv_obj_t *dot = lv_obj_get_child(dots_row, i);
    bool active = (i == photo_idx);
    lv_obj_set_style_bg_color(dot, active ? lv_color_white() : lv_color_hex(0x555555), 0);
  }
}

void on_open(lv_obj_t *parent) {
  photo_idx = 0;

  caption = lv_label_create(parent);
  lv_obj_set_style_text_font(caption, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(caption, lv_color_hex(0xCCCCCC), 0);
  lv_obj_align(caption, LV_ALIGN_TOP_MID, 0, 12);

  scene = lv_obj_create(parent);
  lv_obj_set_size(scene, LCD_PANEL_WIDTH, AREA_HEIGHT);
  lv_obj_set_pos(scene, 0, AREA_TOP);
  lv_obj_set_style_border_width(scene, 0, 0);
  lv_obj_set_style_radius(scene, 0, 0);
  lv_obj_set_style_pad_all(scene, 0, 0);
  lv_obj_clear_flag(scene, LV_OBJ_FLAG_SCROLLABLE);

  dots_row = lv_obj_create(parent);
  lv_obj_set_size(dots_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(dots_row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(dots_row, 0, 0);
  lv_obj_set_style_pad_all(dots_row, 0, 0);
  lv_obj_set_style_pad_column(dots_row, 6, 0);
  lv_obj_set_flex_flow(dots_row, LV_FLEX_FLOW_ROW);
  lv_obj_clear_flag(dots_row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align(dots_row, LV_ALIGN_BOTTOM_MID, 0, -34);
  for (size_t i = 0; i < PHOTO_COUNT; i++) {
    lv_obj_t *d = lv_obj_create(dots_row);
    lv_obj_set_size(d, 6, 6);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(d, 0, 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE);
  }

  lv_obj_t *hint = lv_label_create(parent);
  lv_label_set_text(hint, "short: next  |  hold: home");
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(hint, lv_color_hex(0x555555), 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -16);

  show_photo();
}

void on_close() {
  scene = nullptr;
  caption = nullptr;
  dots_row = nullptr;
}

void on_short_press() {
  photo_idx = (photo_idx + 1) % PHOTO_COUNT;
  show_photo();
}

}  // namespace

const AppDescriptor photos_app = {
    .name = "Photos",
    .icon_symbol = LV_SYMBOL_IMAGE,
    .icon_color = lv_color_hex(0xFF2D55),
    .on_open = on_open,
    .on_close = on_close,
    .on_short_press = on_short_press,
};

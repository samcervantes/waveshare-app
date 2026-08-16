#include "rgb_app.h"

#include <Adafruit_NeoPixel.h>
#include <lvgl.h>

#include "config.h"

namespace {

struct ColorEntry {
  const char *name;
  uint8_t r, g, b;
  uint32_t hex;  // matching color for the on-screen swatch
};

constexpr ColorEntry PALETTE[] = {
    {"Red", 255, 0, 0, 0xFF3B30},      {"Green", 0, 255, 0, 0x30D158},
    {"Blue", 0, 0, 255, 0x0A84FF},     {"Yellow", 255, 200, 0, 0xFFD60A},
    {"Purple", 180, 0, 255, 0xBF5AF2}, {"Cyan", 0, 255, 255, 0x64D2FF},
    {"White", 255, 255, 255, 0xFFFFFF},
};
constexpr size_t PALETTE_LEN = sizeof(PALETTE) / sizeof(PALETTE[0]);

Adafruit_NeoPixel pixel(RGB_LED_COUNT, PIN_RGB_LED, NEO_GRB + NEO_KHZ800);
size_t color_idx = 0;
lv_obj_t *swatch = nullptr;
lv_obj_t *name_label = nullptr;

void apply_color() {
  const ColorEntry &c = PALETTE[color_idx];
  pixel.setPixelColor(0, pixel.Color(c.r, c.g, c.b));
  pixel.show();
  lv_obj_set_style_bg_color(swatch, lv_color_hex(c.hex), 0);
  lv_label_set_text(name_label, c.name);
}

void on_open(lv_obj_t *parent) {
  pixel.begin();
  pixel.setBrightness(80);
  color_idx = 0;

  lv_obj_t *title = lv_label_create(parent);
  lv_label_set_text(title, "RGB Light");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0x888888), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

  swatch = lv_obj_create(parent);
  lv_obj_set_size(swatch, 100, 100);
  lv_obj_set_style_radius(swatch, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(swatch, 0, 0);
  lv_obj_clear_flag(swatch, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align(swatch, LV_ALIGN_CENTER, 0, -30);

  name_label = lv_label_create(parent);
  lv_obj_set_style_text_font(name_label, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(name_label, lv_color_white(), 0);
  lv_obj_align(name_label, LV_ALIGN_CENTER, 0, 50);

  lv_obj_t *hint = lv_label_create(parent);
  lv_label_set_text(hint, "short: next color  |  hold: home");
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(hint, lv_color_hex(0x555555), 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -16);

  apply_color();
}

void on_close() {
  pixel.setPixelColor(0, 0);
  pixel.show();
  swatch = nullptr;
  name_label = nullptr;
}

void on_short_press() {
  color_idx = (color_idx + 1) % PALETTE_LEN;
  apply_color();
}

}  // namespace

const AppDescriptor rgb_app = {
    .name = "RGB Light",
    .icon_symbol = LV_SYMBOL_CHARGE,
    .icon_color = lv_color_hex(0xBF5AF2),
    .on_open = on_open,
    .on_close = on_close,
    .on_short_press = on_short_press,
};

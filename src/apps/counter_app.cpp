#include "counter_app.h"

#include <lvgl.h>

#include "config.h"

namespace {

int count = 0;
lv_obj_t *count_label = nullptr;

void on_open(lv_obj_t *parent) {
  count = 0;

  lv_obj_t *title = lv_label_create(parent);
  lv_label_set_text(title, "Counter");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0x888888), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

  count_label = lv_label_create(parent);
  lv_obj_set_style_text_font(count_label, &lv_font_montserrat_32, 0);
  lv_obj_set_style_text_color(count_label, lv_color_hex(0x30D158), 0);
  lv_label_set_text_fmt(count_label, "%d", count);
  lv_obj_center(count_label);

  lv_obj_t *hint = lv_label_create(parent);
  lv_label_set_text(hint, ACTION_WORD ": +1  |  " HOME_HINT);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(hint, lv_color_hex(0x555555), 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -16);
}

void on_close() {
  count_label = nullptr;
}

void on_short_press() {
  count++;
  lv_label_set_text_fmt(count_label, "%d", count);
}

}  // namespace

const AppDescriptor counter_app = {
    .name = "Counter",
    .icon_symbol = LV_SYMBOL_PLUS,
    .icon_color = lv_color_hex(0x30D158),
    .on_open = on_open,
    .on_close = on_close,
    .on_short_press = on_short_press,
};

#include "wifi_app.h"

#include <Arduino.h>
#include <WiFi.h>
#include <lvgl.h>

#include <algorithm>
#include <vector>

#include "config.h"

namespace {

constexpr size_t MAX_ROWS = 6;
constexpr lv_coord_t ROW_H = 40;
constexpr lv_coord_t AREA_TOP = 40;
constexpr lv_coord_t BAR_W = 36;
constexpr lv_coord_t BAR_H = 10;

lv_obj_t *status_label = nullptr;
lv_obj_t *hint_label = nullptr;
lv_obj_t *row_container[MAX_ROWS] = {nullptr};
lv_obj_t *row_label[MAX_ROWS] = {nullptr};
lv_obj_t *row_bar[MAX_ROWS] = {nullptr};

// Rough dBm -> 0-100% quality, same curve most OSes use for a signal meter.
int rssi_to_percent(int rssi) {
  if (rssi <= -100) return 0;
  if (rssi >= -50) return 100;
  return 2 * (rssi + 100);
}

lv_color_t rssi_to_color(int rssi) {
  if (rssi >= -60) return lv_color_hex(0x30D158);  // strong
  if (rssi >= -75) return lv_color_hex(0xFFD60A);  // medium
  return lv_color_hex(0xFF453A);                   // weak
}

void run_scan() {
  lv_label_set_text(status_label, "Scanning...");
  for (size_t i = 0; i < MAX_ROWS; i++) lv_obj_add_flag(row_container[i], LV_OBJ_FLAG_HIDDEN);
  lv_timer_handler();  // flush "Scanning..." to the panel before the blocking scan below

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  int n = WiFi.scanNetworks();

  if (n <= 0) {
    lv_label_set_text(status_label, n == 0 ? "No networks found" : "Scan failed");
    WiFi.scanDelete();
    return;
  }

  std::vector<int> order(n);
  for (int i = 0; i < n; i++) order[i] = i;
  std::sort(order.begin(), order.end(),
            [](int a, int b) { return WiFi.RSSI(a) > WiFi.RSSI(b); });

  size_t shown = std::min(static_cast<size_t>(n), MAX_ROWS);
  for (size_t r = 0; r < shown; r++) {
    int idx = order[r];
    int rssi = WiFi.RSSI(idx);
    lv_color_t c = rssi_to_color(rssi);

    lv_label_set_text_fmt(row_label[r], "%s  %ddBm", WiFi.SSID(idx).c_str(), rssi);
    lv_obj_set_style_text_color(row_label[r], c, 0);
    lv_bar_set_value(row_bar[r], rssi_to_percent(rssi), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(row_bar[r], c, LV_PART_INDICATOR);
    lv_obj_clear_flag(row_container[r], LV_OBJ_FLAG_HIDDEN);
  }

  lv_label_set_text_fmt(status_label, "%d network%s found", n, n == 1 ? "" : "s");
  WiFi.scanDelete();
}

void on_open(lv_obj_t *parent) {
  status_label = lv_label_create(parent);
  lv_obj_set_style_text_font(status_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(status_label, lv_color_hex(0x888888), 0);
  lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 8);

  for (size_t i = 0; i < MAX_ROWS; i++) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LCD_PANEL_WIDTH, ROW_H);
    lv_obj_set_pos(row, 0, AREA_TOP + static_cast<lv_coord_t>(i) * ROW_H);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(row, 8, 0);
    lv_obj_set_style_pad_right(row, 8, 0);
    row_container[i] = row;

    lv_obj_t *label = lv_label_create(row);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(label, LCD_PANEL_WIDTH - BAR_W - 24);
    row_label[i] = label;

    lv_obj_t *bar = lv_bar_create(row);
    lv_obj_set_size(bar, BAR_W, BAR_H);
    lv_bar_set_range(bar, 0, 100);
    row_bar[i] = bar;
  }

  hint_label = lv_label_create(parent);
  lv_label_set_text(hint_label, "short: rescan  |  hold: home");
  lv_obj_set_style_text_font(hint_label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(hint_label, lv_color_hex(0x555555), 0);
  lv_obj_align(hint_label, LV_ALIGN_BOTTOM_MID, 0, -16);

  run_scan();
}

void on_close() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  status_label = hint_label = nullptr;
  for (size_t i = 0; i < MAX_ROWS; i++) {
    row_container[i] = row_label[i] = row_bar[i] = nullptr;
  }
}

void on_short_press() {
  run_scan();
}

}  // namespace

const AppDescriptor wifi_app = {
    .name = "WiFi",
    .icon_symbol = LV_SYMBOL_WIFI,
    .icon_color = lv_color_hex(0x00C7BE),
    .on_open = on_open,
    .on_close = on_close,
    .on_short_press = on_short_press,
};

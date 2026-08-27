#include "bluetooth_app.h"

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <lvgl.h>

#include <algorithm>
#include <vector>

#include "config.h"

// This board only has BLE (Bluetooth Low Energy) radio hardware, not
// classic Bluetooth (BR/EDR) - so this scan finds BLE devices/beacons
// (headphones, fitness trackers, smart home gadgets, etc. that advertise
// over BLE) but not older classic-only devices.

namespace {

constexpr size_t MAX_ROWS = 6;
constexpr lv_coord_t ROW_H = 40;
constexpr lv_coord_t AREA_TOP = 40;
constexpr lv_coord_t BAR_W = 36;
constexpr lv_coord_t BAR_H = 10;
constexpr uint32_t SCAN_SECONDS = 4;

lv_obj_t *status_label = nullptr;
lv_obj_t *hint_label = nullptr;
lv_obj_t *row_container[MAX_ROWS] = {nullptr};
lv_obj_t *row_label[MAX_ROWS] = {nullptr};
lv_obj_t *row_bar[MAX_ROWS] = {nullptr};

struct FoundDevice {
  String label;
  int rssi;
};

// Same rough dBm -> 0-100% curve as wifi_app; BLE RSSI reads on a similar
// scale, so the same signal-strength meter feel carries over.
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

  BLEScan *scanner = BLEDevice::getScan();
  scanner->setActiveScan(true);
  BLEScanResults *results = scanner->start(SCAN_SECONDS, false);
  int n = results->getCount();

  if (n <= 0) {
    lv_label_set_text(status_label, "No devices found");
    scanner->clearResults();
    return;
  }

  std::vector<FoundDevice> found;
  found.reserve(n);
  for (int i = 0; i < n; i++) {
    BLEAdvertisedDevice d = results->getDevice(i);
    String label = d.haveName() ? String(d.getName().c_str()) : String(d.getAddress().toString().c_str());
    found.push_back({label, d.getRSSI()});
  }
  std::sort(found.begin(), found.end(), [](const FoundDevice &a, const FoundDevice &b) { return a.rssi > b.rssi; });

  size_t shown = std::min(found.size(), MAX_ROWS);
  for (size_t r = 0; r < shown; r++) {
    int rssi = found[r].rssi;
    lv_color_t c = rssi_to_color(rssi);

    lv_label_set_text_fmt(row_label[r], "%s  %ddBm", found[r].label.c_str(), rssi);
    lv_obj_set_style_text_color(row_label[r], c, 0);
    lv_bar_set_value(row_bar[r], rssi_to_percent(rssi), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(row_bar[r], c, LV_PART_INDICATOR);
    lv_obj_clear_flag(row_container[r], LV_OBJ_FLAG_HIDDEN);
  }

  lv_label_set_text_fmt(status_label, "%d device%s found", n, n == 1 ? "" : "s");
  scanner->clearResults();
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
  lv_label_set_text(hint_label, ACTION_WORD ": rescan  |  " HOME_HINT);
  lv_obj_set_style_text_font(hint_label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(hint_label, lv_color_hex(0x555555), 0);
  lv_obj_align(hint_label, LV_ALIGN_BOTTOM_MID, 0, -16);

  BLEDevice::init("");
  run_scan();
}

void on_close() {
  BLEDevice::deinit(true);
  status_label = hint_label = nullptr;
  for (size_t i = 0; i < MAX_ROWS; i++) {
    row_container[i] = row_label[i] = row_bar[i] = nullptr;
  }
}

void on_short_press() {
  run_scan();
}

}  // namespace

const AppDescriptor bluetooth_app = {
    .name = "Bluetooth",
    .icon_symbol = LV_SYMBOL_BLUETOOTH,
    .icon_color = lv_color_hex(0x64D2FF),
    .on_open = on_open,
    .on_close = on_close,
    .on_short_press = on_short_press,
};

#include "bluetooth_app.h"

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <lvgl.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "config.h"

// This board only has BLE (Bluetooth Low Energy) radio hardware, not
// classic Bluetooth (BR/EDR) - so this scan finds BLE devices/beacons
// (headphones, fitness trackers, smart home gadgets, etc. that advertise
// over BLE) but not older classic-only devices.
//
// Scanning is non-blocking, and devices populate the list live as they're
// found rather than all at once at the end - this app used to call
// BLEScan's blocking start(duration, is_continue) overload, which froze
// all button/touch input for the whole SCAN_SECONDS window (the Arduino
// loop() that polls input doesn't run again until it returns), the same
// class of bug wifi_app/clock_app/hn_app/stock_app already hit and fixed
// with a background task + mutex-guarded struct + polling timer. BLE
// doesn't need a whole separate FreeRTOS task for this though - the
// non-blocking start(duration, completionCallback) overload already runs
// the scan on the BLE stack's own task, invoking a completion callback
// when done and (via setAdvertisedDeviceCallbacks) a per-device callback
// as each one is found. Both callbacks run on that BLE task, not the
// LVGL/Arduino loop thread, so - same rule as the other apps' background
// tasks - they only ever write into a mutex-guarded struct, never touch
// LVGL directly; a periodic lv_timer on the normal thread polls it.

namespace {

constexpr lv_coord_t ROW_H = 40;
constexpr lv_coord_t AREA_TOP = 40;
constexpr lv_coord_t BAR_W = 36;
constexpr lv_coord_t BAR_H = 10;
constexpr uint32_t SCAN_SECONDS = 4;
constexpr uint32_t POLL_MS = 200;  // faster than the fetch-app apps' 500ms - live population wants to feel snappy

// Every discovered device gets a row now that the list scrolls (see
// list_container below) - MAX_ROWS == MAX_FOUND, not a smaller "top N by
// signal strength" cutoff like this used to have.
constexpr int MAX_FOUND = 24;
constexpr size_t MAX_ROWS = MAX_FOUND;

lv_obj_t *status_label = nullptr;
lv_obj_t *hint_label = nullptr;
lv_obj_t *list_container = nullptr;
lv_obj_t *row_container[MAX_ROWS] = {nullptr};
lv_obj_t *row_label[MAX_ROWS] = {nullptr};
lv_obj_t *row_bar[MAX_ROWS] = {nullptr};
lv_timer_t *poll_timer = nullptr;

// Detail view: tapping a row shows this device's advertised info (address,
// TX power/service UUID if present) instead of just its list-row summary.
// Not every device advertises TX power or a service UUID, so those are
// conditionally included - see show_detail. Sibling of list_container, not
// a child of it (same reasoning as the News app's detail_hint: children of
// a scrollable object scroll away with it, and this needs to cover the
// same area list_container normally occupies).
lv_obj_t *detail_container = nullptr;
lv_obj_t *detail_title_label = nullptr;
lv_obj_t *detail_body_label = nullptr;
lv_obj_t *detail_hint = nullptr;

struct FoundDevice {
  char label[32];
  char address[24];
  int rssi;
  bool has_tx_power;
  int8_t tx_power;
  bool has_service_uuid;
  char service_uuid[40];
};

SemaphoreHandle_t data_mutex = nullptr;
FoundDevice shared_found[MAX_FOUND];
int shared_found_count = 0;
uint32_t shared_generation = 0;
volatile bool scan_in_progress = false;

uint32_t shown_generation = 0;
bool was_scanning = false;

// The sorted, capped-to-MAX_ROWS snapshot currently shown in the list -
// row taps index into this (not shared_found directly) so a tapped row's
// detail matches what's actually on screen at that position, regardless
// of discovery order.
FoundDevice shown_devices[MAX_ROWS];
int shown_count = 0;

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

// Invoked on the BLE stack's own task, once per newly-discovered device -
// see the file header comment for why this only ever touches the
// mutex-guarded shared_found, never LVGL.
class DiscoveryCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice d) override {
    xSemaphoreTake(data_mutex, portMAX_DELAY);
    if (shared_found_count < MAX_FOUND) {
      FoundDevice &fd = shared_found[shared_found_count++];
      String label = d.haveName() ? String(d.getName().c_str()) : String(d.getAddress().toString().c_str());
      snprintf(fd.label, sizeof(fd.label), "%s", label.c_str());
      snprintf(fd.address, sizeof(fd.address), "%s", d.getAddress().toString().c_str());
      fd.rssi = d.getRSSI();
      fd.has_tx_power = d.haveTXPower();
      fd.tx_power = fd.has_tx_power ? d.getTXPower() : 0;
      fd.has_service_uuid = d.haveServiceUUID();
      if (fd.has_service_uuid) {
        snprintf(fd.service_uuid, sizeof(fd.service_uuid), "%s", d.getServiceUUID().toString().c_str());
      } else {
        fd.service_uuid[0] = '\0';
      }
      shared_generation++;
    }
    xSemaphoreGive(data_mutex);
  }
};
DiscoveryCallbacks discovery_callbacks;
BLEScan *scanner = nullptr;

// Also runs on the BLE stack's task - see the file header comment.
void on_scan_complete(BLEScanResults /*results*/) {
  scan_in_progress = false;
}

void apply_found() {
  xSemaphoreTake(data_mutex, portMAX_DELAY);
  FoundDevice snapshot[MAX_FOUND];
  int count = shared_found_count;
  memcpy(snapshot, shared_found, sizeof(FoundDevice) * count);
  xSemaphoreGive(data_mutex);

  std::vector<int> order(count);
  for (int i = 0; i < count; i++) order[i] = i;
  std::sort(order.begin(), order.end(),
            [&](int a, int b) { return snapshot[a].rssi > snapshot[b].rssi; });

  size_t shown = std::min(static_cast<size_t>(count), MAX_ROWS);
  for (size_t r = 0; r < shown; r++) {
    const FoundDevice &fd = snapshot[order[r]];
    shown_devices[r] = fd;
    lv_color_t c = rssi_to_color(fd.rssi);

    lv_label_set_text_fmt(row_label[r], "%s  %ddBm", fd.label, fd.rssi);
    lv_obj_set_style_text_color(row_label[r], c, 0);
    lv_bar_set_value(row_bar[r], rssi_to_percent(fd.rssi), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(row_bar[r], c, LV_PART_INDICATOR);
    lv_obj_clear_flag(row_container[r], LV_OBJ_FLAG_HIDDEN);
  }
  shown_count = static_cast<int>(shown);
  for (size_t r = shown; r < MAX_ROWS; r++) lv_obj_add_flag(row_container[r], LV_OBJ_FLAG_HIDDEN);

  lv_label_set_text_fmt(status_label, "%d device%s found", count, count == 1 ? "" : "s");
}

void start_scan() {
  if (scan_in_progress) return;

  xSemaphoreTake(data_mutex, portMAX_DELAY);
  shared_found_count = 0;
  shared_generation++;
  xSemaphoreGive(data_mutex);
  shown_generation = 0;

  lv_label_set_text(status_label, "Scanning...");
  for (size_t i = 0; i < MAX_ROWS; i++) lv_obj_add_flag(row_container[i], LV_OBJ_FLAG_HIDDEN);
  lv_obj_scroll_to_y(list_container, 0, LV_ANIM_OFF);  // back to the top for the new results

  scan_in_progress = true;
  // is_continue=false clears the scanner's own internal results each call
  // (see BLEScan::start's source) - nothing extra needed on our end for
  // that. Non-blocking: returns immediately, runs on the BLE stack's task.
  scanner->start(SCAN_SECONDS, on_scan_complete, false);
}

// Tap the header to rescan - the old "tap anywhere in the app" behavior
// moved here specifically now that tapping a *row* shows its detail
// instead (see on_row_tap below).
void on_status_tap(lv_event_t * /*e*/) {
  start_scan();
}

void show_detail(int idx) {
  if (idx < 0 || idx >= shown_count) return;
  const FoundDevice &fd = shown_devices[idx];

  lv_label_set_text(detail_title_label, fd.label);

  char body[160];
  int n = snprintf(body, sizeof(body), "Address: %s\nSignal: %d dBm", fd.address, fd.rssi);
  if (fd.has_tx_power) {
    n += snprintf(body + n, sizeof(body) - n, "\nTX power: %d dBm", fd.tx_power);
  }
  if (fd.has_service_uuid) {
    snprintf(body + n, sizeof(body) - n, "\nService: %s", fd.service_uuid);
  }
  lv_label_set_text(detail_body_label, body);
  // Text has to be set before aligning, not after - align_to reads the
  // title label's current (content-based) height, which doesn't reflect
  // new text until a layout pass happens; setting it after would
  // overlap a shorter-then-longer title with the body below it (same
  // LVGL gotcha as the News app's detail view - see its own comment).
  lv_obj_update_layout(detail_title_label);
  lv_obj_align_to(detail_body_label, detail_title_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 12);

  lv_obj_scroll_to_y(detail_container, 0, LV_ANIM_OFF);
  lv_obj_clear_flag(detail_container, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(detail_hint, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(list_container, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(status_label, LV_OBJ_FLAG_HIDDEN);
}

void hide_detail() {
  lv_obj_add_flag(detail_container, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(detail_hint, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(list_container, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(status_label, LV_OBJ_FLAG_HIDDEN);
}

void on_row_tap(lv_event_t *e) {
  int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
  if (lv_obj_has_flag(row_container[idx], LV_OBJ_FLAG_HIDDEN)) return;  // empty row
  show_detail(idx);
}

void on_detail_tap(lv_event_t * /*e*/) {
  hide_detail();
}

void poll_scan(lv_timer_t * /*t*/) {
  xSemaphoreTake(data_mutex, portMAX_DELAY);
  bool has_update = shared_generation != shown_generation;
  uint32_t gen = shared_generation;
  xSemaphoreGive(data_mutex);

  if (has_update) {
    shown_generation = gen;
    apply_found();
  }

  bool scanning = scan_in_progress;
  if (was_scanning && !scanning) {
    // Scan just finished - if nothing was ever found this cycle, apply_found()
    // never ran (shared_generation never moved off 0), so status_label is
    // still stuck on "Scanning..." without this.
    if (shown_generation == 0) lv_label_set_text(status_label, "No devices found");
  }
  was_scanning = scanning;
}

void on_open(lv_obj_t *parent) {
  if (!data_mutex) data_mutex = xSemaphoreCreateMutex();

  status_label = lv_label_create(parent);
  lv_obj_set_style_text_font(status_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(status_label, lv_color_white(), 0);
  lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 8);
  // Labels aren't clickable by default (unlike lv_obj_create's base
  // object) - needed for on_status_tap (tap the header to rescan) below.
  lv_obj_add_flag(status_label, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(status_label, on_status_tap, LV_EVENT_SHORT_CLICKED, nullptr);

  // Scrollable (vertical only) so more than a screenful of discovered
  // devices can be reached by swiping, with LVGL's default kinetic
  // scrolling - smooth for free, nothing extra needed for that part (same
  // technique as the News app's scrollable article detail view).
  list_container = lv_obj_create(parent);
  lv_obj_remove_style_all(list_container);
  lv_obj_set_size(list_container, LCD_PANEL_WIDTH, LCD_PANEL_HEIGHT - AREA_TOP - 40);
  lv_obj_set_pos(list_container, 0, AREA_TOP);
  lv_obj_set_scroll_dir(list_container, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(list_container, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_style_bg_opa(list_container, LV_OPA_COVER, LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_color(list_container, lv_color_hex(0x64D2FF), LV_PART_SCROLLBAR);
  lv_obj_set_style_width(list_container, 4, LV_PART_SCROLLBAR);

  for (size_t i = 0; i < MAX_ROWS; i++) {
    lv_obj_t *row = lv_obj_create(list_container);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LCD_PANEL_WIDTH, ROW_H);
    lv_obj_set_pos(row, 0, static_cast<lv_coord_t>(i) * ROW_H);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(row, 8, 0);
    lv_obj_set_style_pad_right(row, 8, 0);
    row_container[i] = row;
    lv_obj_add_event_cb(row, on_row_tap, LV_EVENT_SHORT_CLICKED,
                         reinterpret_cast<void *>(static_cast<intptr_t>(i)));

    lv_obj_t *label = lv_label_create(row);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    // Continuous horizontal scroll rather than dot-truncating, so a long
    // device name doesn't push the dBm reading off the visible row - same
    // technique as the WiFi app's Scan page rows.
    lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(label, LCD_PANEL_WIDTH - BAR_W - 24);
    row_label[i] = label;

    lv_obj_t *bar = lv_bar_create(row);
    lv_obj_set_size(bar, BAR_W, BAR_H);
    lv_bar_set_range(bar, 0, 100);
    row_bar[i] = bar;
  }

  // Device detail view - see show_detail/hide_detail. Scrollable for the
  // same reason as list_container (a service UUID line can run long).
  detail_container = lv_obj_create(parent);
  lv_obj_remove_style_all(detail_container);
  lv_obj_set_size(detail_container, LCD_PANEL_WIDTH, LCD_PANEL_HEIGHT - AREA_TOP - 40);
  lv_obj_set_pos(detail_container, 0, AREA_TOP);
  lv_obj_set_scroll_dir(detail_container, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(detail_container, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_style_bg_opa(detail_container, LV_OPA_COVER, LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_color(detail_container, lv_color_hex(0x64D2FF), LV_PART_SCROLLBAR);
  lv_obj_set_style_width(detail_container, 4, LV_PART_SCROLLBAR);
  lv_obj_add_flag(detail_container, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(detail_container, on_detail_tap, LV_EVENT_SHORT_CLICKED, nullptr);

  detail_title_label = lv_label_create(detail_container);
  lv_obj_set_style_text_font(detail_title_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(detail_title_label, lv_color_white(), 0);
  lv_label_set_long_mode(detail_title_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(detail_title_label, LCD_PANEL_WIDTH - 20);
  lv_obj_set_height(detail_title_label, LV_SIZE_CONTENT);
  lv_obj_set_pos(detail_title_label, 10, 4);

  detail_body_label = lv_label_create(detail_container);
  lv_obj_set_style_text_font(detail_body_label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(detail_body_label, lv_color_white(), 0);
  lv_label_set_long_mode(detail_body_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(detail_body_label, LCD_PANEL_WIDTH - 20);
  lv_obj_set_height(detail_body_label, LV_SIZE_CONTENT);
  // x/y set for real in show_detail (lv_obj_align_to, below the title).

  // Sibling of detail_container, not a child of it - see its own global
  // declaration comment for why (scrolls away otherwise).
  detail_hint = lv_label_create(parent);
  lv_label_set_text(detail_hint, "tap to go back");
  lv_obj_set_style_text_font(detail_hint, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(detail_hint, lv_color_hex(0x555555), 0);
  lv_obj_align(detail_hint, LV_ALIGN_BOTTOM_MID, 0, -16);
  lv_obj_add_flag(detail_hint, LV_OBJ_FLAG_HIDDEN);

  hint_label = lv_label_create(parent);
#if defined(BOARD_TOUCH_LCD147)
  lv_label_set_text(hint_label, "tap device: info  |  " HOME_HINT);
#else
  lv_label_set_text(hint_label, ACTION_WORD ": rescan  |  " HOME_HINT);
#endif
  lv_obj_set_style_text_font(hint_label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(hint_label, lv_color_hex(0x555555), 0);
  lv_obj_align(hint_label, LV_ALIGN_BOTTOM_MID, 0, -16);

  BLEDevice::init("");
  scanner = BLEDevice::getScan();
  scanner->setActiveScan(true);
  scanner->setAdvertisedDeviceCallbacks(&discovery_callbacks, false, true);

  was_scanning = false;
  poll_timer = lv_timer_create(poll_scan, POLL_MS, nullptr);
  start_scan();
}

void on_close() {
  if (poll_timer) {
    lv_timer_del(poll_timer);
    poll_timer = nullptr;
  }
  if (scan_in_progress && scanner) {
    scanner->stop();
    scan_in_progress = false;
  }
  BLEDevice::deinit(true);
  scanner = nullptr;
  status_label = hint_label = list_container = nullptr;
  detail_container = detail_title_label = detail_body_label = detail_hint = nullptr;
  for (size_t i = 0; i < MAX_ROWS; i++) {
    row_container[i] = row_label[i] = row_bar[i] = nullptr;
  }
}

// Non-touch board only in practice: wants_raw_touch means the touch
// board's generic tap-anywhere overlay is skipped for this app (see
// on_row_tap/on_status_tap/on_detail_tap instead), so on the touch board
// nothing ever calls this.
void on_short_press() {
  start_scan();
}

}  // namespace

const AppDescriptor bluetooth_app = {
    .name = "Bluetooth",
    .icon_symbol = LV_SYMBOL_BLUETOOTH,
    .icon_color = lv_color_hex(0x64D2FF),
    .on_open = on_open,
    .on_close = on_close,
    .on_short_press = on_short_press,
    // Touch board only (no-op elsewhere, see app_interface.h) - needed so
    // list_container's scroll and each row/status/detail tap reach their
    // own handlers directly instead of the launcher's generic
    // tap-anywhere-reaches-on_short_press overlay swallowing input first
    // (that overlay isn't scrollable and can't distinguish which row was
    // tapped).
    .wants_raw_touch = true,
};

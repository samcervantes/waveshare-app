#include "wifi_app.h"

#include <Arduino.h>
#include <WiFi.h>
#include <lvgl.h>

#include <algorithm>
#include <vector>

#include "config.h"
#include "wifi_credentials.h"
#include "wifi_status.h"

namespace {

constexpr size_t MAX_ROWS = 5;
constexpr lv_coord_t ROW_H = 40;
constexpr lv_coord_t AREA_TOP = 70;
constexpr lv_coord_t BAR_W = 36;
constexpr lv_coord_t BAR_H = 10;

enum class Page { SCAN, STATUS };
Page page = Page::SCAN;

lv_obj_t *tab_scan = nullptr;
lv_obj_t *tab_status = nullptr;

lv_obj_t *scan_page = nullptr;
lv_obj_t *status_label = nullptr;
lv_obj_t *row_container[MAX_ROWS] = {nullptr};
lv_obj_t *row_label[MAX_ROWS] = {nullptr};
lv_obj_t *row_bar[MAX_ROWS] = {nullptr};

lv_obj_t *status_page = nullptr;
lv_obj_t *primary_ssid_label = nullptr;
lv_obj_t *primary_state_label = nullptr;
lv_obj_t *fallback_ssid_label = nullptr;
lv_obj_t *fallback_state_label = nullptr;
lv_timer_t *status_timer = nullptr;

lv_obj_t *hint_label = nullptr;

// Async scan state machine - see run_scan()/poll_scan().
enum class ScanState { IDLE, WAITING_DISCONNECT, SCANNING };
ScanState scan_state = ScanState::IDLE;
uint32_t scan_state_started_ms = 0;
lv_timer_t *scan_timer = nullptr;

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

// Short form of a scanned network's auth type - shown per-row on the
// Scan page so a network that refuses to connect (see the Status page)
// can be diagnosed against what it actually requires, rather than
// guessing at open vs. WPA2 vs. WPA3.
const char *wifi_auth_str(wifi_auth_mode_t mode) {
  switch (mode) {
    case WIFI_AUTH_OPEN: return "open";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-Ent";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    case WIFI_AUTH_WAPI_PSK: return "WAPI";
    default: return "?";
  }
}

// Human-readable form of the handful of WiFi.status() codes actually
// useful for diagnosing a failed connection (wrong password vs. network
// simply not in range vs. still in progress).
const char *wl_status_str(int status) {
  switch (status) {
    case WL_IDLE_STATUS: return "idle";
    case WL_NO_SSID_AVAIL: return "network not found";
    case WL_SCAN_COMPLETED: return "scan done";
    case WL_CONNECTED: return "connected";
    case WL_CONNECT_FAILED: return "connect failed (bad password?)";
    case WL_CONNECTION_LOST: return "connection lost";
    case WL_DISCONNECTED: return "disconnected";
    default: return "unknown";
  }
}

void set_hint() {
  if (page == Page::SCAN) {
    lv_label_set_text(hint_label, ACTION_WORD ": rescan  |  " HOME_HINT);
  } else {
    lv_label_set_text(hint_label, HOME_HINT);
  }
}

void update_tabs() {
  bool on_scan = (page == Page::SCAN);
  lv_obj_set_style_text_color(tab_scan, lv_color_white(), 0);
  lv_obj_set_style_text_color(tab_status, lv_color_white(), 0);
  lv_obj_set_style_bg_color(tab_scan, on_scan ? lv_color_hex(0x0A84FF) : lv_color_hex(0x2A2A2A), 0);
  lv_obj_set_style_bg_color(tab_status, on_scan ? lv_color_hex(0x2A2A2A) : lv_color_hex(0x0A84FF), 0);
  lv_obj_clear_flag(on_scan ? scan_page : status_page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(on_scan ? status_page : scan_page, LV_OBJ_FLAG_HIDDEN);
  set_hint();
}

void run_scan();

void switch_to_scan(lv_event_t * /*e*/) {
  page = Page::SCAN;
  update_tabs();
  run_scan();
}

void switch_to_status(lv_event_t * /*e*/) {
  page = Page::STATUS;
  update_tabs();
}

// Swipe between the two pages - the tab labels are a small tap target on
// this panel, so this is the easier way to switch. LV_DIR_LEFT (finger
// dragging leftward) advances Status -> Scan, matching the tab order
// on-screen (Scan, then Status, left to right) the same way the launcher
// home screen's own page swipe (see launcher.cpp's page_gesture_cb)
// advances left-to-right.
void page_gesture_cb(lv_event_t * /*e*/) {
  lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
  if (dir == LV_DIR_LEFT && page == Page::STATUS) {
    switch_to_scan(nullptr);
  } else if (dir == LV_DIR_RIGHT && page == Page::SCAN) {
    switch_to_status(nullptr);
  }
}

// Live view of wifi_status.cpp's primary/fallback alternation, since the
// user reported the fallback network never seemed to connect and there
// was previously no visibility into why (out of range? wrong password?
// still waiting out the timeout?) - and later, that once it *did* fall
// back, it never tried the primary network again even after coming back
// in range of it (wifi_status.cpp now alternates indefinitely instead of
// switching once).
void update_status_page(lv_timer_t * /*t*/) {
  bool connected = (WiFi.status() == WL_CONNECTED);
  // Which network is *actually* associated right now, by SSID - not just
  // "is anything connected" combined with wifi_status_active_network().
  // Without this, reconnecting to the primary network could still show
  // the fallback row as "Connected" simply because *something* was
  // connected and the fallback had been tried at some point, regardless
  // of which network it actually was.
  bool on_primary = connected && WiFi.SSID() == WIFI_SSID;
  bool on_fallback = connected && WiFi.SSID() == WIFI_SSID_FALLBACK;
  WifiNetwork active = wifi_status_active_network();
  char buf[64];

  lv_label_set_text(primary_ssid_label, WIFI_SSID);
  if (on_primary) {
    snprintf(buf, sizeof(buf), "Connected  %ddBm", WiFi.RSSI());
    lv_obj_set_style_text_color(primary_state_label, lv_color_hex(0x30D158), 0);
  } else if (active == WifiNetwork::PRIMARY) {
    uint32_t elapsed_s = (millis() - wifi_status_connect_started_ms()) / 1000;
    snprintf(buf, sizeof(buf), "Connecting...  %us", elapsed_s);
    lv_obj_set_style_text_color(primary_state_label, lv_color_hex(0xFFD60A), 0);
  } else {
    // Not active right now (trying the fallback instead) - this is the
    // outcome of the *last* attempt, not a permanent verdict, since
    // wifi_status.cpp will circle back to try this network again.
    snprintf(buf, sizeof(buf), "Retrying later: %s", wl_status_str(wifi_status_last_primary_wl_status()));
    lv_obj_set_style_text_color(primary_state_label, lv_color_hex(0xFF453A), 0);
  }
  lv_label_set_text(primary_state_label, buf);

  lv_label_set_text(fallback_ssid_label, WIFI_SSID_FALLBACK);
  if (on_fallback) {
    snprintf(buf, sizeof(buf), "Connected  %ddBm", WiFi.RSSI());
    lv_obj_set_style_text_color(fallback_state_label, lv_color_hex(0x30D158), 0);
  } else if (!wifi_status_fallback_attempted()) {
    uint32_t remaining_s = (WIFI_PRIMARY_TIMEOUT_MS - (millis() - wifi_status_connect_started_ms())) / 1000;
    snprintf(buf, sizeof(buf), "Not tried yet  (in %us)", remaining_s);
    lv_obj_set_style_text_color(fallback_state_label, lv_color_hex(0xDDDDDD), 0);
  } else if (active == WifiNetwork::FALLBACK) {
    uint32_t elapsed_s = (millis() - wifi_status_connect_started_ms()) / 1000;
    snprintf(buf, sizeof(buf), "%s  (%us)", wl_status_str(WiFi.status()), elapsed_s);
    lv_obj_set_style_text_color(fallback_state_label, lv_color_hex(0xFFD60A), 0);
  } else {
    // Not active right now (trying the primary instead) - again, the
    // last attempt's outcome, not permanent.
    snprintf(buf, sizeof(buf), "Retrying later: %s", wl_status_str(wifi_status_last_fallback_wl_status()));
    lv_obj_set_style_text_color(fallback_state_label, lv_color_hex(0xFF453A), 0);
  }
  lv_label_set_text(fallback_state_label, buf);
}

// Kicks off a scan without blocking. run_scan() used to call the
// synchronous WiFi.scanNetworks() (plus a 100ms delay(), plus a manual
// lv_timer_handler() flush to get "Scanning..." on screen before that
// multi-second block) directly from on_short_press/on_open, which was
// harmless there - but now that switch_to_scan() also calls this from
// inside an LV_EVENT_SHORT_CLICKED callback, that call chain runs *from
// within* the outer lv_timer_handler() call (display_tick() -> the touch
// indev's own event dispatch), so the reentrant lv_timer_handler() call
// was genuinely reentrant (not just redundant) and the multi-second
// block sat in the middle of it - the likely cause of taps appearing to
// do nothing. WiFi.scanNetworks(true) (async) plus poll_scan() below,
// polling WiFi.scanComplete() the same way the rest of this app's timers
// poll state, avoids blocking entirely.
void run_scan() {
  lv_label_set_text(status_label, "Scanning...");
  for (size_t i = 0; i < MAX_ROWS; i++) lv_obj_add_flag(row_container[i], LV_OBJ_FLAG_HIDDEN);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  scan_state = ScanState::WAITING_DISCONNECT;
  scan_state_started_ms = millis();
}

void poll_scan(lv_timer_t * /*t*/) {
  if (scan_state == ScanState::WAITING_DISCONNECT) {
    // Same 100ms settle time the old blocking delay(100) gave the
    // disconnect before scanning, just not blocking this time.
    if (millis() - scan_state_started_ms < 100) return;
    WiFi.scanNetworks(true);
    scan_state = ScanState::SCANNING;
    return;
  }

  if (scan_state != ScanState::SCANNING) return;
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;
  scan_state = ScanState::IDLE;

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

    lv_label_set_text_fmt(row_label[r], "%s  %ddBm  %s", WiFi.SSID(idx).c_str(), rssi,
                           wifi_auth_str(WiFi.encryptionType(idx)));
    lv_obj_set_style_text_color(row_label[r], c, 0);
    lv_bar_set_value(row_bar[r], rssi_to_percent(rssi), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(row_bar[r], c, LV_PART_INDICATOR);
    lv_obj_clear_flag(row_container[r], LV_OBJ_FLAG_HIDDEN);
  }

  lv_label_set_text_fmt(status_label, "%d network%s found", n, n == 1 ? "" : "s");
  WiFi.scanDelete();
}

void on_open(lv_obj_t *parent) {
  page = Page::SCAN;

  // A wifi_app-owned wrapper, not `parent` (app_root) directly: app_root
  // is a single object shared and reused across every app (its children
  // get wiped by lv_obj_clean() on close, but the object itself and any
  // event callbacks added straight to it persist) - registering the
  // gesture handler below on `parent` itself would silently accumulate a
  // fresh duplicate registration every time this app reopens, and the
  // stale ones would go on firing (against now-null wifi_app globals)
  // while some other app is open. Parenting everything to `root` instead
  // means it - and its event callback - is destroyed along with the rest
  // of this app's widgets when the launcher cleans app_root on close.
  lv_obj_t *root = lv_obj_create(parent);
  lv_obj_remove_style_all(root);
  lv_obj_set_size(root, LCD_PANEL_WIDTH, LCD_PANEL_HEIGHT);
  lv_obj_set_pos(root, 0, 0);
  lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

#if defined(BOARD_TOUCH_LCD147)
  // Defaults to the status page, not a fresh scan: scanning requires
  // WiFi.disconnect() first (see run_scan), which would corrupt the very
  // primary/fallback connection state this page exists to show if it ran
  // automatically on every open.
  page = Page::STATUS;

  // See page_gesture_cb's comment for why LV_OBJ_FLAG_GESTURE_BUBBLE has
  // to be cleared here, same reasoning as launcher_root in launcher.cpp:
  // every object with a parent gets it set by default, so without
  // clearing it the event would keep bubbling right past `root` (where
  // the handler below is attached) up to app_root and the screen, where
  // nothing listens.
  lv_obj_clear_flag(root, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(root, page_gesture_cb, LV_EVENT_GESTURE, nullptr);

  // Swiping to change pages turned out unreliable on this hardware, so
  // these tabs are the primary way to switch (swipe still works as a
  // best-effort bonus, see page_gesture_cb) - sized as generous pill
  // buttons rather than plain small text, since the plain-text version
  // was too small to tap precisely. Fixed width/height rather than
  // content-based auto-sizing: font 20 + 20px of horizontal padding on
  // *each* of the two buttons doesn't fit side by side in this panel's
  // 172px width (it added up to well over twice that), which is what
  // caused the text to overflow the pills in the first place.
  // Width is fixed (that's what fits two of these side by side); height
  // is left content-based (font line height + vertical padding) so the
  // text is trivially vertically centered rather than needing to compute
  // that by hand against a separately fixed height.
  constexpr lv_coord_t TAB_W = 78;
  lv_obj_t *tabs = lv_obj_create(root);
  lv_obj_remove_style_all(tabs);
  lv_obj_set_size(tabs, LCD_PANEL_WIDTH, 40);
  lv_obj_set_pos(tabs, 0, 4);
  lv_obj_clear_flag(tabs, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(tabs, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(tabs, 8, 0);

  tab_scan = lv_label_create(tabs);
  lv_label_set_text(tab_scan, "Scan");
  lv_obj_set_style_text_font(tab_scan, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_align(tab_scan, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_bg_opa(tab_scan, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(tab_scan, 8, 0);
  lv_obj_set_style_pad_ver(tab_scan, 8, 0);
  lv_obj_set_width(tab_scan, TAB_W);
  lv_obj_add_flag(tab_scan, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(tab_scan, switch_to_scan, LV_EVENT_SHORT_CLICKED, nullptr);

  tab_status = lv_label_create(tabs);
  lv_label_set_text(tab_status, "Status");
  lv_obj_set_style_text_font(tab_status, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_align(tab_status, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_bg_opa(tab_status, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(tab_status, 8, 0);
  lv_obj_set_style_pad_ver(tab_status, 8, 0);
  lv_obj_set_width(tab_status, TAB_W);
  lv_obj_add_flag(tab_status, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(tab_status, switch_to_status, LV_EVENT_SHORT_CLICKED, nullptr);
#endif

  // --- Scan page: nearby networks, same as before. ---
  scan_page = lv_obj_create(root);
  lv_obj_remove_style_all(scan_page);
  lv_obj_set_size(scan_page, LCD_PANEL_WIDTH, LCD_PANEL_HEIGHT - AREA_TOP);
  lv_obj_set_pos(scan_page, 0, 0);
  lv_obj_clear_flag(scan_page, LV_OBJ_FLAG_SCROLLABLE);
  // scan_page's own bounding box spans y=0 (its children start lower, at
  // AREA_TOP, to leave room for the tabs - but the container itself
  // still geometrically overlaps the tabs' area). lv_obj_create()
  // defaults to clickable, and as a *later* sibling than `tabs` it sits
  // on top in z-order - without clearing this, tapping "Scan"/"Status"
  // would hit this invisible pass-through layer instead of the tab
  // underneath it.
  lv_obj_clear_flag(scan_page, LV_OBJ_FLAG_CLICKABLE);

  status_label = lv_label_create(scan_page);
  lv_obj_set_style_text_font(status_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(status_label, lv_color_hex(0xDDDDDD), 0);
  // scan_page spans the full height starting at y=0 - on the touch board
  // that means y=8 (this label's old, pre-tabs position) sits right in
  // the middle of the tabs row above it (y=4 to 44). Non-touch has no
  // tabs, so it keeps the original tighter offset.
#if defined(BOARD_TOUCH_LCD147)
  lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 46);
#else
  lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 8);
#endif

  for (size_t i = 0; i < MAX_ROWS; i++) {
    lv_obj_t *row = lv_obj_create(scan_page);
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
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    // Continuous horizontal scroll rather than dot-truncating, so a long
    // SSID doesn't push the dBm/auth-type info (see run_scan) off the
    // visible row - same technique as the Hacker News app's headlines.
    lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(label, LCD_PANEL_WIDTH - BAR_W - 24);
    row_label[i] = label;

    lv_obj_t *bar = lv_bar_create(row);
    lv_obj_set_size(bar, BAR_W, BAR_H);
    lv_bar_set_range(bar, 0, 100);
    row_bar[i] = bar;
  }

#if defined(BOARD_TOUCH_LCD147)
  // --- Status page: primary/fallback connection progress. ---
  status_page = lv_obj_create(root);
  lv_obj_remove_style_all(status_page);
  lv_obj_set_size(status_page, LCD_PANEL_WIDTH, LCD_PANEL_HEIGHT - AREA_TOP);
  lv_obj_set_pos(status_page, 0, 0);
  lv_obj_clear_flag(status_page, LV_OBJ_FLAG_SCROLLABLE);
  // See scan_page's comment - same issue, and this one's the one that
  // actually blocked taps in practice, since Status is the default page
  // and (being created after `tabs`) sits on top of it in z-order.
  lv_obj_clear_flag(status_page, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(status_page, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t *primary_header = lv_label_create(status_page);
  lv_label_set_text(primary_header, "Primary");
  lv_obj_set_style_text_font(primary_header, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(primary_header, lv_color_hex(0xDDDDDD), 0);
  lv_obj_set_pos(primary_header, 10, AREA_TOP);

  primary_ssid_label = lv_label_create(status_page);
  lv_obj_set_style_text_font(primary_ssid_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(primary_ssid_label, lv_color_white(), 0);
  lv_label_set_long_mode(primary_ssid_label, LV_LABEL_LONG_DOT);
  // LV_LABEL_LONG_DOT only truncates (rather than wrapping) once the
  // label's height, not just its width, is constrained to one line -
  // without this, a long SSID like "Red Thing Rides Again" wrapped to a
  // second line and overflowed into the state label below it.
  lv_obj_set_size(primary_ssid_label, LCD_PANEL_WIDTH - 20, 20);
  lv_obj_set_pos(primary_ssid_label, 10, AREA_TOP + 20);

  primary_state_label = lv_label_create(status_page);
  lv_obj_set_style_text_font(primary_state_label, &lv_font_montserrat_16, 0);
  // Continuous horizontal scroll rather than truncating, so a long status
  // message (e.g. "Retrying later: connect failed (bad password?)") is
  // fully readable instead of just cut off at the panel edge - same
  // technique as the Hacker News app's headlines and the Scan page's rows.
  lv_label_set_long_mode(primary_state_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_width(primary_state_label, LCD_PANEL_WIDTH - 20);
  lv_obj_set_pos(primary_state_label, 10, AREA_TOP + 44);

  lv_obj_t *fallback_header = lv_label_create(status_page);
  lv_label_set_text(fallback_header, "Fallback");
  lv_obj_set_style_text_font(fallback_header, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(fallback_header, lv_color_hex(0xDDDDDD), 0);
  lv_obj_set_pos(fallback_header, 10, AREA_TOP + 84);

  fallback_ssid_label = lv_label_create(status_page);
  lv_obj_set_style_text_font(fallback_ssid_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(fallback_ssid_label, lv_color_white(), 0);
  lv_label_set_long_mode(fallback_ssid_label, LV_LABEL_LONG_DOT);
  lv_obj_set_size(fallback_ssid_label, LCD_PANEL_WIDTH - 20, 20);
  lv_obj_set_pos(fallback_ssid_label, 10, AREA_TOP + 104);

  fallback_state_label = lv_label_create(status_page);
  lv_obj_set_style_text_font(fallback_state_label, &lv_font_montserrat_16, 0);
  lv_label_set_long_mode(fallback_state_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_width(fallback_state_label, LCD_PANEL_WIDTH - 20);
  lv_obj_set_pos(fallback_state_label, 10, AREA_TOP + 128);
#endif

  hint_label = lv_label_create(root);
  lv_obj_set_style_text_font(hint_label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(hint_label, lv_color_hex(0xDDDDDD), 0);
  lv_obj_align(hint_label, LV_ALIGN_BOTTOM_MID, 0, -16);

  scan_state = ScanState::IDLE;
  scan_timer = lv_timer_create(poll_scan, 100, nullptr);

#if defined(BOARD_TOUCH_LCD147)
  update_tabs();
  status_timer = lv_timer_create(update_status_page, 500, nullptr);
  update_status_page(nullptr);
#else
  lv_label_set_text(hint_label, ACTION_WORD ": rescan  |  " HOME_HINT);
  run_scan();
#endif
}

void on_close() {
  if (status_timer) {
    lv_timer_del(status_timer);
    status_timer = nullptr;
  }
  if (scan_timer) {
    lv_timer_del(scan_timer);
    scan_timer = nullptr;
  }
  scan_state = ScanState::IDLE;
  // wifi_app borrows the radio for scanNetworks() (which needs
  // WiFi.disconnect() first) - hand it back the way wifi_status left it
  // rather than turning the radio off, which used to strand the board
  // with no WiFi at all (and thus no way for the primary/fallback dance
  // above to ever run again) until the next reboot.
  wifi_status_reconnect();
  tab_scan = tab_status = nullptr;
  scan_page = status_label = nullptr;
  status_page = primary_ssid_label = primary_state_label = fallback_ssid_label = fallback_state_label = nullptr;
  hint_label = nullptr;
  for (size_t i = 0; i < MAX_ROWS; i++) {
    row_container[i] = row_label[i] = row_bar[i] = nullptr;
  }
}

// Non-touch board only in practice: wants_raw_touch means the touch
// board's generic tap-anywhere overlay is skipped for this app, and its
// physical button is solely a quick-press-to-home shortcut now (see
// launcher_handle_button) - so on the touch board nothing ever calls
// this. On the non-touch board `page` is always SCAN (no touch to reach
// the status page's tabs), so this is just "rescan".
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
    // Touch board only (no-op elsewhere, see app_interface.h) - needed so
    // the Scan/Status tab labels below can receive taps directly, instead
    // of the launcher's generic tap-anywhere-reaches-on_short_press
    // overlay swallowing them first.
    .wants_raw_touch = true,
};

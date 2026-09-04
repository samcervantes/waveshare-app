#include "wifi_app.h"

#include <Arduino.h>
#include <WiFi.h>
#include <lvgl.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "config.h"
#include "wifi_credentials.h"
#include "wifi_status.h"

namespace {

constexpr size_t MAX_ROWS = 5;
constexpr lv_coord_t ROW_H = 40;
#if defined(BOARD_TOUCH_LCD147)
constexpr lv_coord_t AREA_TOP = 40;  // no tabs anymore - just the status/count label above
#else
constexpr lv_coord_t AREA_TOP = 70;
#endif
constexpr lv_coord_t BAR_W = 36;
constexpr lv_coord_t BAR_H = 10;

enum class Page { SCAN, STATUS, FOX_HUNT };
constexpr int PAGE_COUNT = 3;
Page page = Page::SCAN;

lv_obj_t *scan_page = nullptr;
lv_obj_t *status_label = nullptr;
lv_obj_t *row_container[MAX_ROWS] = {nullptr};
lv_obj_t *row_label[MAX_ROWS] = {nullptr};
lv_obj_t *row_bar[MAX_ROWS] = {nullptr};
// The plain SSID behind each row's display text (which also has dBm/auth
// info baked in - see poll_scan) so a tapped row can be matched back to
// "which network is this" for the Fox Hunt page and the highlight below.
char row_ssid[MAX_ROWS][33];

lv_obj_t *status_page = nullptr;
// Each wraps its network's header/ssid/state labels so the whole block is
// one tap target (see on_known_network_tap) and one place to hang the
// same selection-highlight border the Scan page's rows use.
lv_obj_t *primary_row = nullptr;
lv_obj_t *fallback_row = nullptr;
lv_obj_t *primary_ssid_label = nullptr;
lv_obj_t *primary_state_label = nullptr;
lv_obj_t *fallback_ssid_label = nullptr;
lv_obj_t *fallback_state_label = nullptr;
lv_timer_t *status_timer = nullptr;

// The selected network - by SSID, not BSSID: connecting (wifi_status_pin_
// network) is inherently SSID-based, not tied to a specific AP's radio,
// and SSID is all the Status page's Primary/Fallback rows have to go on
// anyway (see on_known_network_tap - no scan/BSSID needed to select one
// of those). Set by tapping either a Scan-page row (on_row_tap) or a
// Status-page row (on_known_network_tap); see select_network. Also
// pins wifi_status's connection attempts to it, if it's one of the two
// networks this board actually has a password for (see select_network).
// Drives the Fox Hunt page's live signal-strength readout regardless of
// whether it matches a known network - "live" via the same scan-polling
// loop the Scan page uses (see poll_scan), just re-triggered continuously
// while that page stays open instead of once per tap/rescan.
lv_obj_t *fox_hunt_page = nullptr;
lv_obj_t *fox_target_label = nullptr;
lv_obj_t *fox_rssi_label = nullptr;
lv_obj_t *fox_bar = nullptr;
lv_obj_t *fox_status_label = nullptr;
bool has_target = false;
char target_ssid[33];

lv_obj_t *dots_row = nullptr;
lv_obj_t *dot[PAGE_COUNT] = {nullptr};

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

void run_scan();

// Highlights whichever currently-shown Scan-page row (if any) has the
// selected SSID, so it stays visually marked across rescans even as
// results reorder by signal strength.
void highlight_target_row() {
  for (size_t i = 0; i < MAX_ROWS; i++) {
    bool match = has_target && !lv_obj_has_flag(row_container[i], LV_OBJ_FLAG_HIDDEN) &&
                 strcmp(row_ssid[i], target_ssid) == 0;
    lv_obj_set_style_border_width(row_container[i], match ? 2 : 0, 0);
    lv_obj_set_style_border_color(row_container[i], lv_color_hex(0x5AC8FA), 0);
    lv_obj_set_style_border_opa(row_container[i], LV_OPA_COVER, 0);
  }
}

// Same highlight treatment, applied to the Status page's Primary/Fallback
// rows instead of a Scan-page row - whichever (if either) the selected
// SSID matches.
void highlight_status_rows() {
  if (!primary_row) return;  // Status page not built yet
  bool on_primary = has_target && strcmp(target_ssid, WIFI_SSID) == 0;
  bool on_fallback = has_target && strcmp(target_ssid, WIFI_SSID_FALLBACK) == 0;
  lv_obj_set_style_border_width(primary_row, on_primary ? 2 : 0, 0);
  lv_obj_set_style_border_color(primary_row, lv_color_hex(0x5AC8FA), 0);
  lv_obj_set_style_border_opa(primary_row, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(fallback_row, on_fallback ? 2 : 0, 0);
  lv_obj_set_style_border_color(fallback_row, lv_color_hex(0x5AC8FA), 0);
  lv_obj_set_style_border_opa(fallback_row, LV_OPA_COVER, 0);
}

// Shared by on_row_tap (Scan page) and on_known_network_tap (Status page)
// - either is "select this network", regardless of which page you tapped
// it from: highlight it everywhere it's shown, drive the Fox Hunt page,
// and pin the connection to it if it's one of the two networks this
// board actually has a password for (see wifi_status_pin_network's
// comment for why - a spotty network kept getting abandoned mid-attempt
// in favor of the other one). Networks without a saved password can
// still be Fox Hunt targets, just not something this board can join.
void select_network(const char *ssid) {
  strncpy(target_ssid, ssid, sizeof(target_ssid) - 1);
  target_ssid[sizeof(target_ssid) - 1] = '\0';
  has_target = true;
  highlight_target_row();
  highlight_status_rows();
  if (fox_target_label) lv_label_set_text(fox_target_label, target_ssid);

  if (strcmp(target_ssid, WIFI_SSID) == 0) {
    wifi_status_pin_network(WifiNetwork::PRIMARY);
  } else if (strcmp(target_ssid, WIFI_SSID_FALLBACK) == 0) {
    wifi_status_pin_network(WifiNetwork::FALLBACK);
  }
}

void on_row_tap(lv_event_t *e) {
  int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
  if (lv_obj_has_flag(row_container[idx], LV_OBJ_FLAG_HIDDEN)) return;  // empty row, nothing to select
  select_network(row_ssid[idx]);
}

void on_known_network_tap(lv_event_t *e) {
  auto net = static_cast<WifiNetwork>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
  select_network(net == WifiNetwork::PRIMARY ? WIFI_SSID : WIFI_SSID_FALLBACK);
}

// Shows/hides the right page and updates the dot row - same white-active/
// gray-inactive convention as the launcher's own home-screen page dots
// (see launcher.cpp's update_highlight).
void update_page() {
  lv_obj_add_flag(scan_page, LV_OBJ_FLAG_HIDDEN);
  if (status_page) lv_obj_add_flag(status_page, LV_OBJ_FLAG_HIDDEN);
  if (fox_hunt_page) lv_obj_add_flag(fox_hunt_page, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t *current = scan_page;
  if (page == Page::STATUS) current = status_page;
  if (page == Page::FOX_HUNT) current = fox_hunt_page;
  if (current) lv_obj_clear_flag(current, LV_OBJ_FLAG_HIDDEN);

  int idx = static_cast<int>(page);
  for (int i = 0; i < PAGE_COUNT; i++) {
    if (!dot[i]) continue;
    lv_obj_set_style_bg_color(dot[i], i == idx ? lv_color_white() : lv_color_hex(0x555555), 0);
  }

  // Neither page has a tap-to-rescan control of its own anymore (no tabs)
  // - arriving on Scan kicks off a fresh one-shot scan (replacing the old
  // tab-click behavior), and arriving on Fox Hunt with a target already
  // locked in kicks off its continuous loop (poll_scan() keeps that going
  // for as long as this page stays open - see its own comment). Guarded
  // on IDLE so swiping back and forth doesn't stack/interrupt an
  // already-running scan.
  if (scan_state == ScanState::IDLE) {
    if (page == Page::SCAN) {
      run_scan();
    } else if (page == Page::FOX_HUNT && has_target) {
      run_scan();
    }
  }
}

void go_to_page(Page p) {
  page = p;
  update_page();
}

// Swipe between pages - this panel has no tap target to switch pages with
// anymore (see on_open's comment on why the old Scan/Status tabs were
// dropped), so this is the only way to switch. LV_DIR_LEFT (finger
// dragging leftward) advances to the next page, matching the dot order
// on-screen left to right, the same convention as the launcher home
// screen's own page swipe (see launcher.cpp's page_gesture_cb).
void page_gesture_cb(lv_event_t * /*e*/) {
  lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
  int idx = static_cast<int>(page);
  if (dir == LV_DIR_LEFT && idx + 1 < PAGE_COUNT) {
    go_to_page(static_cast<Page>(idx + 1));
  } else if (dir == LV_DIR_RIGHT && idx > 0) {
    go_to_page(static_cast<Page>(idx - 1));
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
  Serial.println("[wifi_app] run_scan: disconnecting radio to scan");
  lv_label_set_text(status_label, "Scanning...");
  for (size_t i = 0; i < MAX_ROWS; i++) lv_obj_add_flag(row_container[i], LV_OBJ_FLAG_HIDDEN);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  scan_state = ScanState::WAITING_DISCONNECT;
  scan_state_started_ms = millis();
}

void update_fox_hunt_reading(bool found, int rssi) {
  if (!fox_rssi_label) return;
  if (!found) {
    lv_label_set_text(fox_rssi_label, "--");
    lv_obj_set_style_text_color(fox_rssi_label, lv_color_hex(0x555555), 0);
    lv_bar_set_value(fox_bar, 0, LV_ANIM_OFF);
    lv_label_set_text(fox_status_label, "Not seen this scan - keep moving");
    return;
  }
  lv_color_t c = rssi_to_color(rssi);
  lv_label_set_text_fmt(fox_rssi_label, "%d dBm", rssi);
  lv_obj_set_style_text_color(fox_rssi_label, c, 0);
  lv_bar_set_value(fox_bar, rssi_to_percent(rssi), LV_ANIM_OFF);
  lv_obj_set_style_bg_color(fox_bar, c, LV_PART_INDICATOR);
  lv_label_set_text(fox_status_label, "Higher = closer");
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
    if (has_target) update_fox_hunt_reading(false, 0);
    if (page == Page::FOX_HUNT && has_target) {
      run_scan();
    } else {
      // Hand the radio back right away rather than leaving it disconnected
      // - see the comment on the same call below.
      wifi_status_reconnect();
    }
    return;
  }

  std::vector<int> order(n);
  for (int i = 0; i < n; i++) order[i] = i;
  std::sort(order.begin(), order.end(),
            [](int a, int b) { return WiFi.RSSI(a) > WiFi.RSSI(b); });

  // Keep the Fox Hunt/pinned target visible even if it's not among the
  // MAX_ROWS strongest networks right now - otherwise it just silently
  // drops off the Scan page (taking its highlight with it) the moment
  // something else briefly outranks it, which defeats the point of
  // having picked a network to keep watching/connecting to. Swapping it
  // into the last shown slot displaces whatever was weakest there rather
  // than disturbing the stronger ones above it.
  if (has_target) {
    for (int i = 0; i < n; i++) {
      if (WiFi.SSID(i) != target_ssid) continue;
      auto it = std::find(order.begin(), order.end(), i);
      size_t pos = static_cast<size_t>(std::distance(order.begin(), it));
      if (pos >= MAX_ROWS) {
        int tmp = order[MAX_ROWS - 1];
        order[MAX_ROWS - 1] = order[pos];
        order[pos] = tmp;
      }
      break;
    }
  }

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

    strncpy(row_ssid[r], WiFi.SSID(idx).c_str(), sizeof(row_ssid[r]) - 1);
    row_ssid[r][sizeof(row_ssid[r]) - 1] = '\0';
  }
  highlight_target_row();

  lv_label_set_text_fmt(status_label, "%d network%s found", n, n == 1 ? "" : "s");

  // Fox Hunt's target search runs over *every* result, not just the
  // MAX_ROWS shown above - once you're actually walking around hunting it
  // down (rather than glancing at the strongest few), the target often
  // ranks outside the top MAX_ROWS shown on the Scan page.
  if (has_target) {
    bool found = false;
    int found_rssi = 0;
    for (int i = 0; i < n; i++) {
      if (WiFi.SSID(i) == target_ssid) {
        found = true;
        found_rssi = WiFi.RSSI(i);
        break;
      }
    }
    update_fox_hunt_reading(found, found_rssi);
  }

  WiFi.scanDelete();

  if (page == Page::FOX_HUNT && has_target) {
    run_scan();
  } else {
    // run_scan() disconnected the radio to scan (see its own comment);
    // wifi_status.cpp's own reconnect logic doesn't know that happened
    // and won't retry for up to WIFI_PRIMARY_TIMEOUT_MS since it
    // still thinks it's within the grace period from whenever it last
    // saw a connection - so without this, the radio just sits
    // disconnected after every scan until that timer happens to catch
    // up, which is what made WiFi feel unreliable/slow to (re)connect
    // whenever this app had been opened. Handing it back immediately
    // once we're done with it (matching on_close's same call, just now
    // also after each individual scan, not only when the app closes)
    // fixes that instead of relying on lucky timing.
    wifi_status_reconnect();
  }
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
  // nothing listens. Swipe is now the *only* way to change pages (the old
  // Scan/Status tab buttons were dropped in favor of the dots at the
  // bottom, matching the launcher home screen's own paging) - swiping had
  // previously been flagged as unreliable on this hardware when it was
  // just a best-effort bonus alongside the tabs; worth watching closely
  // now that it's load-bearing.
  lv_obj_clear_flag(root, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(root, page_gesture_cb, LV_EVENT_GESTURE, nullptr);
#endif

  // --- Scan page: nearby networks, same as before. Tap a row to lock it
  // in as the Fox Hunt target (see on_row_tap). ---
  scan_page = lv_obj_create(root);
  lv_obj_remove_style_all(scan_page);
  lv_obj_set_size(scan_page, LCD_PANEL_WIDTH, LCD_PANEL_HEIGHT - AREA_TOP);
  lv_obj_set_pos(scan_page, 0, 0);
  lv_obj_clear_flag(scan_page, LV_OBJ_FLAG_SCROLLABLE);

  status_label = lv_label_create(scan_page);
  lv_obj_set_style_text_font(status_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(status_label, lv_color_hex(0xDDDDDD), 0);
  lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 8);

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
    // Hidden until run_scan()/poll_scan() actually has a result for this
    // row - otherwise LVGL's default placeholder text ("Text") shows in
    // all MAX_ROWS slots for however long it takes the first scan to
    // complete.
    lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    row_container[i] = row;
    lv_obj_add_event_cb(row, on_row_tap, LV_EVENT_SHORT_CLICKED,
                         reinterpret_cast<void *>(static_cast<intptr_t>(i)));

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
  lv_obj_add_flag(status_page, LV_OBJ_FLAG_HIDDEN);

  // Each network's block is wrapped in its own row container (rather than
  // positioning the labels straight on status_page) so the whole block is
  // one tap target - same idea as the Scan page's rows - for
  // on_known_network_tap to select/pin it, not just the Scan page's live
  // scan results. 76px tall: header(0) + ssid(20) + state(44), each ~20px,
  // plus a little breathing room before the next row starts.
  primary_row = lv_obj_create(status_page);
  lv_obj_remove_style_all(primary_row);
  lv_obj_set_size(primary_row, LCD_PANEL_WIDTH, 76);
  lv_obj_set_pos(primary_row, 0, AREA_TOP);
  lv_obj_clear_flag(primary_row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(primary_row, on_known_network_tap, LV_EVENT_SHORT_CLICKED,
                       reinterpret_cast<void *>(static_cast<intptr_t>(WifiNetwork::PRIMARY)));

  lv_obj_t *primary_header = lv_label_create(primary_row);
  lv_label_set_text(primary_header, "Primary");
  lv_obj_set_style_text_font(primary_header, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(primary_header, lv_color_hex(0xDDDDDD), 0);
  lv_obj_set_pos(primary_header, 10, 0);

  primary_ssid_label = lv_label_create(primary_row);
  lv_obj_set_style_text_font(primary_ssid_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(primary_ssid_label, lv_color_white(), 0);
  lv_label_set_long_mode(primary_ssid_label, LV_LABEL_LONG_DOT);
  // LV_LABEL_LONG_DOT only truncates (rather than wrapping) once the
  // label's height, not just its width, is constrained to one line -
  // without this, a long SSID like "Red Thing Rides Again" wrapped to a
  // second line and overflowed into the state label below it.
  lv_obj_set_size(primary_ssid_label, LCD_PANEL_WIDTH - 20, 20);
  lv_obj_set_pos(primary_ssid_label, 10, 20);

  primary_state_label = lv_label_create(primary_row);
  lv_obj_set_style_text_font(primary_state_label, &lv_font_montserrat_16, 0);
  // Continuous horizontal scroll rather than truncating, so a long status
  // message (e.g. "Retrying later: connect failed (bad password?)") is
  // fully readable instead of just cut off at the panel edge - same
  // technique as the Hacker News app's headlines and the Scan page's rows.
  lv_label_set_long_mode(primary_state_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_width(primary_state_label, LCD_PANEL_WIDTH - 20);
  lv_obj_set_pos(primary_state_label, 10, 44);

  fallback_row = lv_obj_create(status_page);
  lv_obj_remove_style_all(fallback_row);
  lv_obj_set_size(fallback_row, LCD_PANEL_WIDTH, 76);
  lv_obj_set_pos(fallback_row, 0, AREA_TOP + 84);
  lv_obj_clear_flag(fallback_row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(fallback_row, on_known_network_tap, LV_EVENT_SHORT_CLICKED,
                       reinterpret_cast<void *>(static_cast<intptr_t>(WifiNetwork::FALLBACK)));

  lv_obj_t *fallback_header = lv_label_create(fallback_row);
  lv_label_set_text(fallback_header, "Fallback");
  lv_obj_set_style_text_font(fallback_header, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(fallback_header, lv_color_hex(0xDDDDDD), 0);
  lv_obj_set_pos(fallback_header, 10, 0);

  fallback_ssid_label = lv_label_create(fallback_row);
  lv_obj_set_style_text_font(fallback_ssid_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(fallback_ssid_label, lv_color_white(), 0);
  lv_label_set_long_mode(fallback_ssid_label, LV_LABEL_LONG_DOT);
  lv_obj_set_size(fallback_ssid_label, LCD_PANEL_WIDTH - 20, 20);
  lv_obj_set_pos(fallback_ssid_label, 10, 20);

  fallback_state_label = lv_label_create(fallback_row);
  lv_obj_set_style_text_font(fallback_state_label, &lv_font_montserrat_16, 0);
  lv_label_set_long_mode(fallback_state_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_width(fallback_state_label, LCD_PANEL_WIDTH - 20);
  lv_obj_set_pos(fallback_state_label, 10, 44);

  highlight_status_rows();

  // --- Fox Hunt page: live-ish signal-strength walk toward whatever
  // target got locked in via on_row_tap on the Scan page. ---
  fox_hunt_page = lv_obj_create(root);
  lv_obj_remove_style_all(fox_hunt_page);
  lv_obj_set_size(fox_hunt_page, LCD_PANEL_WIDTH, LCD_PANEL_HEIGHT - AREA_TOP);
  lv_obj_set_pos(fox_hunt_page, 0, 0);
  lv_obj_clear_flag(fox_hunt_page, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(fox_hunt_page, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t *fox_header = lv_label_create(fox_hunt_page);
  lv_label_set_text(fox_header, "Fox Hunt");
  lv_obj_set_style_text_font(fox_header, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(fox_header, lv_color_hex(0xDDDDDD), 0);
  lv_obj_set_pos(fox_header, 10, AREA_TOP);

  fox_target_label = lv_label_create(fox_hunt_page);
  lv_obj_set_style_text_font(fox_target_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(fox_target_label, lv_color_white(), 0);
  lv_label_set_long_mode(fox_target_label, LV_LABEL_LONG_DOT);
  lv_obj_set_size(fox_target_label, LCD_PANEL_WIDTH - 20, 20);
  lv_obj_set_pos(fox_target_label, 10, AREA_TOP + 20);
  lv_label_set_text(fox_target_label, has_target ? target_ssid : "Tap a network on Scan to hunt it");

  fox_rssi_label = lv_label_create(fox_hunt_page);
  lv_obj_set_style_text_font(fox_rssi_label, &lv_font_montserrat_32, 0);
  lv_obj_set_style_text_color(fox_rssi_label, lv_color_hex(0x555555), 0);
  lv_label_set_text(fox_rssi_label, "--");
  lv_obj_align(fox_rssi_label, LV_ALIGN_TOP_MID, 0, AREA_TOP + 50);

  fox_bar = lv_bar_create(fox_hunt_page);
  lv_obj_set_size(fox_bar, LCD_PANEL_WIDTH - 40, 24);
  lv_bar_set_range(fox_bar, 0, 100);
  lv_obj_set_style_radius(fox_bar, 12, LV_PART_MAIN);
  lv_obj_set_style_radius(fox_bar, 12, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(fox_bar, lv_color_hex(0x2C2C2E), LV_PART_MAIN);
  lv_bar_set_value(fox_bar, 0, LV_ANIM_OFF);
  lv_obj_align(fox_bar, LV_ALIGN_TOP_MID, 0, AREA_TOP + 100);

  fox_status_label = lv_label_create(fox_hunt_page);
  lv_obj_set_style_text_font(fox_status_label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(fox_status_label, lv_color_hex(0x888888), 0);
  lv_label_set_text(fox_status_label, "");
  lv_obj_align(fox_status_label, LV_ALIGN_TOP_MID, 0, AREA_TOP + 130);

  // --- Page dots, replacing the old tab buttons and the bottom hint text
  // (to make room for them) - same white-active/gray-inactive convention
  // as the launcher's own home-screen dots (see launcher.cpp). ---
  dots_row = lv_obj_create(root);
  lv_obj_remove_style_all(dots_row);
  lv_obj_set_size(dots_row, LCD_PANEL_WIDTH, 16);
  lv_obj_align(dots_row, LV_ALIGN_BOTTOM_MID, 0, -8);
  lv_obj_clear_flag(dots_row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(dots_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(dots_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(dots_row, 8, 0);
  for (int i = 0; i < PAGE_COUNT; i++) {
    lv_obj_t *d = lv_obj_create(dots_row);
    lv_obj_remove_style_all(d);
    lv_obj_set_size(d, 8, 8);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    dot[i] = d;
  }
#endif

  scan_state = ScanState::IDLE;
  scan_timer = lv_timer_create(poll_scan, 100, nullptr);

#if defined(BOARD_TOUCH_LCD147)
  update_page();
  status_timer = lv_timer_create(update_status_page, 500, nullptr);
  update_status_page(nullptr);
#else
  // Non-touch board keeps the old bottom hint text (no dots/paging here -
  // this board never had the Status/Fox Hunt pages to swipe between).
  hint_label = lv_label_create(root);
  lv_obj_set_style_text_font(hint_label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(hint_label, lv_color_hex(0xDDDDDD), 0);
  lv_obj_align(hint_label, LV_ALIGN_BOTTOM_MID, 0, -16);
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
  scan_page = status_label = nullptr;
  status_page = primary_row = fallback_row = nullptr;
  primary_ssid_label = primary_state_label = fallback_ssid_label = fallback_state_label = nullptr;
  fox_hunt_page = fox_target_label = fox_rssi_label = fox_bar = fox_status_label = nullptr;
  dots_row = nullptr;
  for (int i = 0; i < PAGE_COUNT; i++) dot[i] = nullptr;
  has_target = false;
  hint_label = nullptr;
  for (size_t i = 0; i < MAX_ROWS; i++) {
    row_container[i] = row_label[i] = row_bar[i] = nullptr;
  }
}

// Non-touch board only in practice: wants_raw_touch means the touch
// board's generic tap-anywhere overlay is skipped for this app, and its
// physical button is solely a quick-press-to-home shortcut now (see
// launcher_handle_button) - so on the touch board nothing ever calls
// this. On the non-touch board `page` is always SCAN (no touch input at
// all on that board), so this is just "rescan".
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
    // page_gesture_cb's swipe and on_row_tap's row taps reach this app's
    // own widgets directly, instead of the launcher's generic
    // tap-anywhere-reaches-on_short_press overlay swallowing them first.
    .wants_raw_touch = true,
};

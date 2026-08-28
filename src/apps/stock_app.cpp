#include "stock_app.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <lvgl.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>

#include "config.h"

// SPY price + a same-day intraday trend line, from Yahoo Finance's public
// (unofficial, no API key needed) chart endpoint. The actual HTTPS
// request + JSON parse happens on its own FreeRTOS task, not inline in
// on_open()/on_short_press(): clock_app used to do exactly that kind of
// blocking network call straight in on_open() and it froze all
// button/touch input for several seconds (see its history) - the Arduino
// loop() that polls input doesn't run again until on_open() returns. This
// operation is slower still (TLS handshake + ~8KB JSON), so it'd be worse
// here. The background task never touches LVGL directly (LVGL isn't
// thread-safe) - it only writes into a mutex-guarded struct; a periodic
// lv_timer on the normal LVGL thread polls that struct and updates the UI.

namespace {

constexpr int MAX_POINTS = 80;
constexpr uint32_t POLL_MS = 500;
constexpr uint32_t WIFI_WAIT_MS = 5000;
// Absolute ceiling on the whole fetch, enforced independently of whatever
// http.setTimeout()/setConnectTimeout() do internally: real-world testing
// showed a "GET=-1" connect failure can apparently take far longer than
// the 8s configured timeout to actually resolve (a multi-minute hang was
// observed with only ~1 timeout logged in that whole window), so those
// settings alone can't be trusted as a hard bound. poll_fetch() below
// force-kills the task if this elapses, so the UI can never get stuck.
constexpr uint32_t HARD_TIMEOUT_MS = 20000;

// Swipeable chart timescales. interval/range are Yahoo chart API params;
// each pairing is chosen to land comfortably under MAX_POINTS (a 1y daily
// chart alone would be ~252 points) without needing to grow that buffer:
// 1d@5m ~78 points, 5d@30m ~65, 1mo@1d ~21, 1y@1wk ~52.
enum class Timescale { DAY, WEEK, MONTH, YEAR };
struct TimescaleInfo {
  const char *label;
  const char *interval;
  const char *range;
};
constexpr TimescaleInfo TIMESCALES[] = {
    {"1D", "5m", "1d"},
    {"1W", "30m", "5d"},
    {"1M", "1d", "1mo"},
    {"1Y", "1wk", "1y"},
};
constexpr int TIMESCALE_COUNT = sizeof(TIMESCALES) / sizeof(TIMESCALES[0]);

struct StockData {
  bool valid = false;
  double price = 0;
  double prev_close = 0;
  double day_low = 0;
  double day_high = 0;
  float points[MAX_POINTS];
  int point_count = 0;
  uint32_t generation = 0;
};

SemaphoreHandle_t data_mutex = nullptr;
StockData shared_data;
volatile bool fetch_in_progress = false;
volatile bool fetch_failed = false;
// Serial logging is unreliable on this board's native USB-CDC (confirmed
// earlier this session during touch debugging - output silently drops
// unless a host has a proper CDC connection open, which is inconsistent),
// so failures are surfaced directly in the UI instead. Only ever written
// by fetch_task and only ever read after fetch_failed is observed true,
// which fetch_task sets strictly after finishing all writes to this - no
// real race despite no mutex.
char last_error[80] = "";

lv_obj_t *status_label = nullptr;
lv_obj_t *price_label = nullptr;
lv_obj_t *change_label = nullptr;
lv_obj_t *range_label = nullptr;
lv_obj_t *timescale_label = nullptr;
lv_obj_t *chart = nullptr;
lv_chart_series_t *chart_series = nullptr;
Timescale current_timescale = Timescale::DAY;
lv_timer_t *poll_timer = nullptr;
uint32_t shown_generation = 0;
TaskHandle_t fetch_task_handle = nullptr;
uint32_t fetch_started_ms = 0;

// One attempt at the whole DNS + HTTPS + JSON round trip. Split out from
// fetch_task so it can be retried below - real-world testing on this
// board showed the connect step (WiFiClientSecure/HTTPClient) fails
// intermittently (HTTPC_ERROR_CONNECTION_REFUSED/-LOST) even against an
// unrelated host, i.e. transient WiFi flakiness rather than anything
// Yahoo- or code-specific, so it's worth just trying again a couple of
// times before giving up.
bool try_fetch_once(StockData &result, Timescale ts) {
  IPAddress resolved;
  if (!WiFi.hostByName("query1.finance.yahoo.com", resolved)) {
    snprintf(last_error, sizeof(last_error), "DNS lookup failed");
    return false;
  }

  char url[160];
  snprintf(url, sizeof(url), "https://query1.finance.yahoo.com/v8/finance/chart/SPY?interval=%s&range=%s",
           TIMESCALES[static_cast<int>(ts)].interval, TIMESCALES[static_cast<int>(ts)].range);

  WiFiClientSecure client;
  client.setInsecure();  // public read-only market data, not worth carrying a CA bundle for
  HTTPClient http;
  http.setUserAgent("Mozilla/5.0");
  http.setTimeout(5000);
  http.setConnectTimeout(5000);

  if (!http.begin(client, url)) {
    snprintf(last_error, sizeof(last_error), "http.begin failed");
    return false;
  }

  int code = http.GET();
  if (code != 200) {
    snprintf(last_error, sizeof(last_error), "GET=%d heap=%u", code, ESP.getFreeHeap());
    http.end();
    return false;
  }

  // Only pull the handful of fields we use out of Yahoo's much larger
  // response - keeps the JSON document small.
  JsonDocument filter;
  JsonObject meta = filter["chart"]["result"][0]["meta"].to<JsonObject>();
  meta["regularMarketPrice"] = true;
  meta["previousClose"] = true;
  meta["regularMarketDayLow"] = true;
  meta["regularMarketDayHigh"] = true;
  filter["chart"]["result"][0]["indicators"]["quote"][0]["close"] = true;

  // Reading the full body into a String first (instead of parsing
  // directly from http.getStream()) turned out to matter: the streaming
  // parser intermittently failed with "IncompleteInput" - likely an edge
  // case in how HTTPClient's stream interacts with chunked transfer
  // encoding. ~8KB response, and there's plenty of free heap for it
  // (confirmed >140KB free elsewhere).
  String payload = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
  if (err) {
    snprintf(last_error, sizeof(last_error), "JSON err: %s", err.c_str());
    return false;
  }

  JsonObject r0 = doc["chart"]["result"][0];
  result.price = r0["meta"]["regularMarketPrice"] | 0.0;
  result.prev_close = r0["meta"]["previousClose"] | 0.0;
  result.day_low = r0["meta"]["regularMarketDayLow"] | 0.0;
  result.day_high = r0["meta"]["regularMarketDayHigh"] | 0.0;

  JsonArray closes = r0["indicators"]["quote"][0]["close"];
  int n = 0;
  for (JsonVariant v : closes) {
    if (n >= MAX_POINTS) break;
    if (!v.isNull()) result.points[n++] = v.as<float>();
  }
  result.point_count = n;
  result.valid = (result.price > 0);
  if (!result.valid) snprintf(last_error, sizeof(last_error), "parsed but price=0");
  return result.valid;
}

constexpr int MAX_ATTEMPTS = 2;

void fetch_task(void *param) {
  // The timescale to fetch is captured into the task's own parameter by
  // start_fetch() rather than read from current_timescale here, so a
  // swipe that changes current_timescale while this task is already
  // running can't race with it - this task always fetches whatever
  // timescale was selected at the moment it was created.
  Timescale ts = static_cast<Timescale>(reinterpret_cast<intptr_t>(param));
  StockData result;
  bool ok = false;
  last_error[0] = '\0';

  uint32_t wait_start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wait_start < WIFI_WAIT_MS) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  if (WiFi.status() != WL_CONNECTED) {
    snprintf(last_error, sizeof(last_error), "no WiFi (status=%d)", WiFi.status());
  } else {
    for (int attempt = 0; attempt < MAX_ATTEMPTS && !ok; attempt++) {
      if (attempt > 0) vTaskDelay(pdMS_TO_TICKS(1500));
      ok = try_fetch_once(result, ts);
    }
  }

  if (ok) {
    xSemaphoreTake(data_mutex, portMAX_DELAY);
    result.generation = shared_data.generation + 1;
    shared_data = result;
    xSemaphoreGive(data_mutex);
  } else {
    fetch_failed = true;
  }

  fetch_in_progress = false;
  fetch_task_handle = nullptr;
  vTaskDelete(nullptr);
}

void start_fetch() {
  if (fetch_in_progress) return;
  fetch_in_progress = true;
  fetch_failed = false;
  fetch_started_ms = millis();
  lv_label_set_text_fmt(status_label, "Fetching SPY (%s)...", TIMESCALES[static_cast<int>(current_timescale)].label);
  // TLS handshake (mbedTLS) + HTTPClient + ArduinoJson's parsing recursion
  // together need more than the 12KB this started with - that overflowed
  // the task's stack and crashed the whole board (Guru Meditation /
  // "Load access fault", confirmed via serial: the stack dump was full of
  // ESP-IDF's 0xa5a5a5a5 unused-stack guard pattern all the way through).
  xTaskCreate(fetch_task, "stock_fetch", 24576,
              reinterpret_cast<void *>(static_cast<intptr_t>(current_timescale)), 1, &fetch_task_handle);
}

void apply_data(const StockData &d) {
  double change = d.price - d.prev_close;
  double change_pct = d.prev_close != 0 ? (change / d.prev_close) * 100.0 : 0.0;
  lv_color_t c = change >= 0 ? lv_color_hex(0x30D158) : lv_color_hex(0xFF453A);

  // Built with the standard libc snprintf + lv_label_set_text, not
  // lv_label_set_text_fmt: this project's lv_conf.h has
  // LV_SPRINTF_USE_FLOAT set to 0, so LVGL's own minimal printf has never
  // supported %f and prints the literal character 'f' for it *without*
  // consuming the corresponding argument (root-caused by checking that
  // config flag after seeing "$f" on screen instead of a real price).
  // That silently shifts every argument after it by one position, which
  // is exactly what caused the earlier crash here too: with %.2f eating
  // no argument, the %s right after it in "%s%.2f (%s%.2f%%)" read the
  // `change` double's bit pattern as if it were a char* and dereferenced
  // it. libc's snprintf has no such restriction, so it's used everywhere
  // a value here needs float formatting.
  char price_buf[16], change_buf[48], range_buf[40];
  snprintf(price_buf, sizeof(price_buf), "$%.2f", d.price);
  lv_label_set_text(price_label, price_buf);
  lv_obj_set_style_text_color(price_label, c, 0);

  snprintf(change_buf, sizeof(change_buf), "%s%.2f (%s%.2f%%)", change >= 0 ? "+" : "", change,
           change_pct >= 0 ? "+" : "", change_pct);
  lv_label_set_text(change_label, change_buf);
  lv_obj_set_style_text_color(change_label, c, 0);

  snprintf(range_buf, sizeof(range_buf), "Range: $%.2f - $%.2f", d.day_low, d.day_high);
  lv_label_set_text(range_label, range_buf);
  lv_label_set_text(status_label, "SPY - S&P 500 ETF");

  if (d.point_count > 0) {
    float lo = d.points[0], hi = d.points[0];
    for (int i = 1; i < d.point_count; i++) {
      lo = std::min(lo, d.points[i]);
      hi = std::max(hi, d.points[i]);
    }
    if (hi <= lo) hi = lo + 1;  // avoid a degenerate 0-height range

    lv_chart_set_point_count(chart, d.point_count);
    for (int i = 0; i < d.point_count; i++) {
      int32_t norm = static_cast<int32_t>((d.points[i] - lo) / (hi - lo) * 1000.0f);
      lv_chart_set_value_by_id(chart, chart_series, i, norm);
    }
    lv_chart_refresh(chart);
    lv_obj_clear_flag(chart, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(chart, LV_OBJ_FLAG_HIDDEN);
  }
}

void poll_fetch(lv_timer_t * /*t*/) {
  if (fetch_in_progress && millis() - fetch_started_ms > HARD_TIMEOUT_MS) {
    // Absolute ceiling, independent of whatever http.setTimeout() etc. do
    // internally - see HARD_TIMEOUT_MS. Force-killing a task stuck inside
    // a blocking network call is blunt (skips its own cleanup, e.g.
    // http.end()) but the priority here is that the UI can never get
    // permanently stuck, which a graceful-but-unbounded wait can't
    // guarantee.
    if (fetch_task_handle) {
      vTaskDelete(fetch_task_handle);
      fetch_task_handle = nullptr;
    }
    fetch_in_progress = false;
    snprintf(last_error, sizeof(last_error), "timed out after %lus", HARD_TIMEOUT_MS / 1000);
    fetch_failed = true;
  }

  if (fetch_failed) {
    fetch_failed = false;
    lv_label_set_text_fmt(status_label, "Failed: %s", last_error);
    return;
  }

  xSemaphoreTake(data_mutex, portMAX_DELAY);
  bool has_update = shared_data.valid && shared_data.generation != shown_generation;
  StockData snapshot = shared_data;
  xSemaphoreGive(data_mutex);

  if (has_update) {
    shown_generation = snapshot.generation;
    apply_data(snapshot);
  }
}

void update_timescale_label() {
  lv_label_set_text(timescale_label, TIMESCALES[static_cast<int>(current_timescale)].label);
}

void change_timescale(int delta) {
  int idx = (static_cast<int>(current_timescale) + delta + TIMESCALE_COUNT) % TIMESCALE_COUNT;
  current_timescale = static_cast<Timescale>(idx);
  update_timescale_label();
  start_fetch();
}

// Swipe to change timescale - LV_DIR_LEFT advances (1D -> 1W -> 1M -> 1Y,
// wrapping back to 1D), matching the swipe-left-advances convention used
// elsewhere in this project (launcher.cpp's page swipe, wifi_app's
// Scan/Status tabs).
void timescale_gesture_cb(lv_event_t * /*e*/) {
  lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
  if (dir == LV_DIR_LEFT) {
    change_timescale(1);
  } else if (dir == LV_DIR_RIGHT) {
    change_timescale(-1);
  }
}

void refresh_tap_cb(lv_event_t * /*e*/) {
  start_fetch();
}

void on_open(lv_obj_t *parent) {
  if (!data_mutex) data_mutex = xSemaphoreCreateMutex();
  current_timescale = Timescale::DAY;

  // A stock_app-owned wrapper, not `parent` (app_root) directly - app_root
  // is shared and reused across every app, so an event callback added
  // straight to it (as timescale_gesture_cb/refresh_tap_cb are below)
  // would silently accumulate a fresh duplicate registration every time
  // this app reopens and go on firing while some other app is open (see
  // wifi_app.cpp's on_open for the same reasoning, found the hard way
  // there). Parenting everything to `root` means it - and its callbacks -
  // are destroyed along with the rest of this app's widgets when the
  // launcher cleans app_root on close.
  lv_obj_t *root = lv_obj_create(parent);
  lv_obj_remove_style_all(root);
  lv_obj_set_size(root, LCD_PANEL_WIDTH, LCD_PANEL_HEIGHT);
  lv_obj_set_pos(root, 0, 0);
  lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
  // Gesture events bubble by default (LV_OBJ_FLAG_GESTURE_BUBBLE, set on
  // every object with a parent), so without clearing it here the swipe
  // handled below would keep bubbling past `root` up to app_root and the
  // screen, where nothing listens - same as launcher_root in launcher.cpp.
  lv_obj_clear_flag(root, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(root, timescale_gesture_cb, LV_EVENT_GESTURE, nullptr);
  // Unlike gestures, LV_EVENT_CLICKED-family events do *not* bubble by
  // default, so a tap needs to land directly on `root` to reach this -
  // fine for the empty space around the labels, but the chart below is a
  // full lv_obj-derived widget and clickable by default, which would
  // otherwise swallow taps over it before they ever reach root.
  lv_obj_add_event_cb(root, refresh_tap_cb, LV_EVENT_SHORT_CLICKED, nullptr);

  status_label = lv_label_create(root);
  lv_obj_set_style_text_font(status_label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(status_label, lv_color_hex(0xDDDDDD), 0);
  lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(status_label, LCD_PANEL_WIDTH - 12);
  lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 8);

  price_label = lv_label_create(root);
  lv_obj_set_style_text_font(price_label, &lv_font_montserrat_32, 0);
  lv_label_set_text(price_label, "$--.--");
  lv_obj_align(price_label, LV_ALIGN_TOP_MID, 0, 30);

  change_label = lv_label_create(root);
  lv_obj_set_style_text_font(change_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(change_label, lv_color_hex(0xDDDDDD), 0);
  lv_obj_align(change_label, LV_ALIGN_TOP_MID, 0, 74);

  range_label = lv_label_create(root);
  lv_obj_set_style_text_font(range_label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(range_label, lv_color_hex(0xDDDDDD), 0);
  lv_obj_align(range_label, LV_ALIGN_TOP_MID, 0, 98);

  timescale_label = lv_label_create(root);
  lv_obj_set_style_text_font(timescale_label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(timescale_label, lv_color_hex(0x0A84FF), 0);
  lv_obj_align(timescale_label, LV_ALIGN_TOP_RIGHT, -10, 122);
  update_timescale_label();

  chart = lv_chart_create(root);
  lv_obj_set_size(chart, 156, 140);
  lv_obj_align(chart, LV_ALIGN_TOP_MID, 0, 122);
  lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
  lv_chart_set_div_line_count(chart, 3, 0);
  lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 1000);
  lv_obj_set_style_size(chart, 0, LV_PART_INDICATOR);  // no point markers, just the line
  lv_obj_set_style_bg_color(chart, lv_color_hex(0x111111), 0);
  lv_obj_set_style_border_width(chart, 0, 0);
  lv_obj_clear_flag(chart, LV_OBJ_FLAG_CLICKABLE);  // see the comment above refresh_tap_cb's registration
  chart_series = lv_chart_add_series(chart, lv_color_hex(0x0A84FF), LV_CHART_AXIS_PRIMARY_Y);
  lv_obj_add_flag(chart, LV_OBJ_FLAG_HIDDEN);  // shown once real data arrives

  lv_obj_t *hint = lv_label_create(root);
  lv_label_set_text(hint, "swipe: scale  |  " ACTION_WORD ": refresh  |  " HOME_HINT);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(hint, lv_color_hex(0xDDDDDD), 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);

  shown_generation = 0;
  poll_timer = lv_timer_create(poll_fetch, POLL_MS, nullptr);
  start_fetch();
}

void on_close() {
  if (poll_timer) {
    lv_timer_del(poll_timer);
    poll_timer = nullptr;
  }
  // The HARD_TIMEOUT_MS watchdog above only runs while poll_timer does,
  // i.e. only while this app is open - force-kill a still-stuck fetch
  // here too, otherwise fetch_in_progress would stay true forever
  // (nothing left to clear it) and every future reopen's start_fetch()
  // would silently no-op forever.
  if (fetch_in_progress && fetch_task_handle) {
    vTaskDelete(fetch_task_handle);
    fetch_task_handle = nullptr;
    fetch_in_progress = false;
  }
  status_label = price_label = change_label = range_label = timescale_label = chart = nullptr;
  chart_series = nullptr;
}

void on_short_press() {
  start_fetch();
}

}  // namespace

const AppDescriptor stock_app = {
    .name = "Stocks",
    .icon_symbol = LV_SYMBOL_UPLOAD,
    .icon_color = lv_color_hex(0xD4AF37),
    .on_open = on_open,
    .on_close = on_close,
    .on_short_press = on_short_press,
    // Touch board only (no-op elsewhere, see app_interface.h) - needed so
    // this app can handle its own swipe-to-change-timescale gesture; the
    // launcher's generic tap-anywhere overlay has no gesture support.
    .wants_raw_touch = true,
};

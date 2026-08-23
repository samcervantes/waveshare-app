#include "hn_app.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <lvgl.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "config.h"

// Top Hacker News front-page stories, via Algolia's HN Search API
// (hn.algolia.com - a public, unofficial, no-API-key-needed mirror of HN
// that conveniently returns titles/points in one request, unlike the
// official Firebase API which would need one request for the ID list plus
// one more per story). Follows the exact same background-fetch shape as
// stock_app: the HTTPS request + JSON parse run on their own FreeRTOS
// task, never inline in on_open()/on_short_press(), because clock_app
// used to do a blocking network call straight in on_open() and froze all
// button/touch input for several seconds (the Arduino loop() that polls
// input doesn't run again until on_open() returns). The task never
// touches LVGL directly (LVGL isn't thread-safe) - it only writes into a
// mutex-guarded struct; a periodic lv_timer on the normal LVGL thread
// polls that struct and updates the UI.

namespace {

constexpr size_t MAX_STORIES = 6;
constexpr lv_coord_t ROW_H = 36;
constexpr lv_coord_t AREA_TOP = 34;
constexpr uint32_t POLL_MS = 500;
constexpr uint32_t WIFI_WAIT_MS = 5000;
// See stock_app.cpp for why this exists: a hard ceiling independent of
// whatever http.setTimeout()/setConnectTimeout() do internally, since
// real-world testing showed a connect failure can hang far longer than
// its configured timeout. poll_fetch() below force-kills the task if this
// elapses, so the UI can never get stuck.
constexpr uint32_t HARD_TIMEOUT_MS = 20000;
constexpr const char *STORIES_URL = "https://hn.algolia.com/api/v1/search?tags=front_page&hitsPerPage=6";

struct Story {
  char title[96];
};

struct HNData {
  bool valid = false;
  Story stories[MAX_STORIES];
  int count = 0;
  uint32_t generation = 0;
};

SemaphoreHandle_t data_mutex = nullptr;
HNData shared_data;
volatile bool fetch_in_progress = false;
volatile bool fetch_failed = false;
// Serial logging is unreliable on this board's native USB-CDC (see
// stock_app.cpp/CLAUDE.md), so failures are surfaced directly in the UI
// instead. Only ever written by fetch_task and only ever read after
// fetch_failed is observed true, which fetch_task sets strictly after
// finishing all writes to this - no real race despite no mutex.
char last_error[80] = "";

lv_obj_t *status_label = nullptr;
lv_obj_t *hint_label = nullptr;
lv_obj_t *row_container[MAX_STORIES] = {nullptr};
lv_obj_t *row_title[MAX_STORIES] = {nullptr};
lv_timer_t *poll_timer = nullptr;
uint32_t shown_generation = 0;
TaskHandle_t fetch_task_handle = nullptr;
uint32_t fetch_started_ms = 0;

// One attempt at the whole DNS + HTTPS + JSON round trip. Split out from
// fetch_task so it can be retried below - see stock_app.cpp: real-world
// testing on this board showed the connect step fails intermittently even
// against an unrelated host, i.e. transient WiFi flakiness rather than
// anything server- or code-specific, so it's worth trying again once.
bool try_fetch_once(HNData &result) {
  IPAddress resolved;
  if (!WiFi.hostByName("hn.algolia.com", resolved)) {
    snprintf(last_error, sizeof(last_error), "DNS lookup failed");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();  // public read-only data, not worth carrying a CA bundle for
  HTTPClient http;
  http.setUserAgent("Mozilla/5.0");
  http.setTimeout(5000);
  http.setConnectTimeout(5000);

  if (!http.begin(client, STORIES_URL)) {
    snprintf(last_error, sizeof(last_error), "http.begin failed");
    return false;
  }

  int code = http.GET();
  if (code != 200) {
    snprintf(last_error, sizeof(last_error), "GET=%d heap=%u", code, ESP.getFreeHeap());
    http.end();
    return false;
  }

  // Only pull the title out of each hit - the full response also has
  // author, url, tags, points, story text, etc. that this app never uses.
  JsonDocument filter;
  JsonObject hit0 = filter["hits"][0].to<JsonObject>();
  hit0["title"] = true;

  // Read the full body into a String first rather than parsing directly
  // from http.getStream() - see stock_app.cpp: the streaming parser
  // intermittently failed with "IncompleteInput" there, likely an edge
  // case in how HTTPClient's stream interacts with chunked transfer
  // encoding.
  String payload = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
  if (err) {
    snprintf(last_error, sizeof(last_error), "JSON err: %s", err.c_str());
    return false;
  }

  JsonArray hits = doc["hits"];
  int n = 0;
  for (JsonVariant hit : hits) {
    if (n >= static_cast<int>(MAX_STORIES)) break;
    const char *title = hit["title"] | "(untitled)";
    snprintf(result.stories[n].title, sizeof(result.stories[n].title), "%s", title);
    n++;
  }
  result.count = n;
  result.valid = n > 0;
  if (!result.valid) snprintf(last_error, sizeof(last_error), "parsed but no stories");
  return result.valid;
}

constexpr int MAX_ATTEMPTS = 2;

void fetch_task(void * /*param*/) {
  HNData result;
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
      ok = try_fetch_once(result);
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
  lv_label_set_text(status_label, "Fetching top stories...");
  for (size_t i = 0; i < MAX_STORIES; i++) lv_obj_add_flag(row_container[i], LV_OBJ_FLAG_HIDDEN);
  // Same stack size as stock_app's fetch task: TLS handshake (mbedTLS) +
  // HTTPClient + ArduinoJson's parsing recursion overflowed a 12KB stack
  // and crashed the board there, so this starts at the size already
  // confirmed sufficient rather than re-discovering that the hard way.
  xTaskCreate(fetch_task, "hn_fetch", 24576, nullptr, 1, &fetch_task_handle);
}

void apply_data(const HNData &d) {
  lv_label_set_text(status_label, "Top Stories");
  for (int i = 0; i < static_cast<int>(MAX_STORIES); i++) {
    if (i >= d.count) {
      lv_obj_add_flag(row_container[i], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    lv_label_set_text_fmt(row_title[i], "%d. %s", i + 1, d.stories[i].title);
    lv_obj_clear_flag(row_container[i], LV_OBJ_FLAG_HIDDEN);
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
  HNData snapshot = shared_data;
  xSemaphoreGive(data_mutex);

  if (has_update) {
    shown_generation = snapshot.generation;
    apply_data(snapshot);
  }
}

void on_open(lv_obj_t *parent) {
  if (!data_mutex) data_mutex = xSemaphoreCreateMutex();

  status_label = lv_label_create(parent);
  lv_obj_set_style_text_font(status_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(status_label, lv_color_hex(0xFF6600), 0);
  lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 8);

  for (size_t i = 0; i < MAX_STORIES; i++) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LCD_PANEL_WIDTH - 12, ROW_H);
    lv_obj_set_pos(row, 6, AREA_TOP + static_cast<lv_coord_t>(i) * ROW_H);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    row_container[i] = row;

    lv_obj_t *title = lv_label_create(row);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    // Continuous one-direction loop (never reverses) for titles too long
    // to fit the panel's width, rather than truncating with "..." or
    // scrolling back and forth - requires a fixed width (not
    // LV_SIZE_CONTENT) narrower than the text to kick in.
    lv_label_set_long_mode(title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(title, LCD_PANEL_WIDTH - 12);
    lv_obj_set_pos(title, 0, 0);
    row_title[i] = title;
  }

  hint_label = lv_label_create(parent);
  lv_label_set_text(hint_label, "short: refresh  |  hold: home");
  lv_obj_set_style_text_font(hint_label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(hint_label, lv_color_hex(0x555555), 0);
  lv_obj_align(hint_label, LV_ALIGN_BOTTOM_MID, 0, -8);

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
  status_label = hint_label = nullptr;
  for (size_t i = 0; i < MAX_STORIES; i++) row_container[i] = row_title[i] = nullptr;
}

void on_short_press() {
  start_fetch();
}

}  // namespace

const AppDescriptor hn_app = {
    .name = "Hacker News",
    .icon_symbol = LV_SYMBOL_LIST,
    .icon_color = lv_color_hex(0xFF6600),
    .on_open = on_open,
    .on_close = on_close,
    .on_short_press = on_short_press,
};

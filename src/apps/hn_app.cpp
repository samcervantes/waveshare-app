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
//
// Touch board only, a second swipeable page (see Page/PAGE_COUNT below)
// adds general headlines from the BBC World RSS feed, fetched/parsed the
// same way (background task + mutex-guarded struct + polling timer, its
// own copy rather than a shared helper - matches how stock_app/hn_app
// already each independently duplicate this pattern rather than sharing
// one). BBC over other sources: it's a free, no-API-key, no-rate-limit
// feed, and its own <description> per item is a real (if brief) editorial
// summary - not just a truncated stub some aggregator APIs return on
// their free tiers to push you toward a paid plan. It's XML, not JSON,
// but simple/well-formed enough (CDATA-wrapped title/description per
// <item>, verified directly against the live feed) that hand-scanning
// for it is a lot less code/flash than pulling in a full XML parser for
// the two fields this app actually needs - see extract_cdata.

namespace {

constexpr size_t MAX_STORIES = 6;
constexpr lv_coord_t ROW_H = 36;
constexpr lv_coord_t AREA_TOP = 40;
constexpr uint32_t POLL_MS = 500;
constexpr uint32_t WIFI_WAIT_MS = 5000;
// See stock_app.cpp for why this exists: a hard ceiling independent of
// whatever http.setTimeout()/setConnectTimeout() do internally, since
// real-world testing showed a connect failure can hang far longer than
// its configured timeout. poll_fetch()/poll_headlines_fetch() below
// force-kill their task if this elapses, so the UI can never get stuck.
constexpr uint32_t HARD_TIMEOUT_MS = 20000;
constexpr int MAX_ATTEMPTS = 2;
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

lv_obj_t *root = nullptr;
lv_obj_t *hn_page = nullptr;
// heading_label is a static "Hacker News" title, always visible; status_label
// is the line below it for transient fetch state ("Fetching...", "Failed:
// ...") - see apply_data/start_fetch. Both white: this screen is black, and
// anything dimmer than that (the dark red/gray this used to be) was
// reported repeatedly hard to read - see CLAUDE.md's readability note.
lv_obj_t *heading_label = nullptr;
lv_obj_t *status_label = nullptr;
lv_obj_t *hint_label = nullptr;
lv_obj_t *row_container[MAX_STORIES] = {nullptr};
lv_obj_t *row_title[MAX_STORIES] = {nullptr};
lv_timer_t *poll_timer = nullptr;
uint32_t shown_generation = 0;
TaskHandle_t fetch_task_handle = nullptr;
uint32_t fetch_started_ms = 0;

#if defined(BOARD_TOUCH_LCD147)
constexpr size_t MAX_HEADLINES = 6;
constexpr const char *HEADLINES_URL = "https://feeds.bbci.co.uk/news/world/rss.xml";

enum class Page { HN, HEADLINES };
constexpr int PAGE_COUNT = 2;
Page page = Page::HN;

struct Headline {
  char title[128];
  char description[200];
};

struct BBCData {
  bool valid = false;
  Headline headlines[MAX_HEADLINES];
  int count = 0;
  uint32_t generation = 0;
};

SemaphoreHandle_t bbc_mutex = nullptr;
BBCData bbc_shared_data;
BBCData shown_bbc_data;  // last-applied snapshot, so a tapped row can show its description without re-fetching
volatile bool bbc_fetch_in_progress = false;
volatile bool bbc_fetch_failed = false;
char bbc_last_error[80] = "";

lv_obj_t *headlines_page = nullptr;
lv_obj_t *headlines_heading_label = nullptr;  // static "BBC News" title
lv_obj_t *headlines_status_label = nullptr;   // fetch state below it
lv_obj_t *headline_row[MAX_HEADLINES] = {nullptr};
lv_obj_t *headline_title[MAX_HEADLINES] = {nullptr};
lv_timer_t *bbc_poll_timer = nullptr;
uint32_t bbc_shown_generation = 0;
TaskHandle_t bbc_fetch_task_handle = nullptr;
uint32_t bbc_fetch_started_ms = 0;

// Detail view: tapping a headline swaps the list for just that headline's
// title + BBC's own description - a small screen has no room for (and
// this app has no way to render) the full article, but the feed's own
// summary is a reasonable stand-in. Tapping again returns to the list.
// Not a separate page in the dots, just a sub-state of the Headlines page.
lv_obj_t *detail_container = nullptr;
lv_obj_t *detail_title_label = nullptr;
lv_obj_t *detail_desc_label = nullptr;
lv_obj_t *detail_hint = nullptr;  // "tap to go back" - a sibling of detail_container,
                                   // not a child of it, so it doesn't scroll away with the article

lv_obj_t *dots_row = nullptr;
lv_obj_t *dot[PAGE_COUNT] = {nullptr};
#endif

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

#if defined(BOARD_TOUCH_LCD147)
// Finds `tag` at or after `from`, then the <![CDATA[ ... ]]> immediately
// following it, and copies the text inside into `out`. Returns the
// position just past the CDATA close (via `next_pos`, if non-null) so the
// caller can keep scanning forward for the next tag without re-finding
// this one - see try_bbc_fetch_once's title-then-description lookup.
bool extract_cdata(const String &s, int from, const char *tag, char *out, size_t out_size, int *next_pos) {
  int tag_pos = s.indexOf(tag, from);
  if (tag_pos < 0) return false;
  int cdata_start = s.indexOf("<![CDATA[", tag_pos);
  if (cdata_start < 0) return false;
  cdata_start += 9;  // strlen("<![CDATA[")
  int cdata_end = s.indexOf("]]>", cdata_start);
  if (cdata_end < 0) return false;
  snprintf(out, out_size, "%s", s.substring(cdata_start, cdata_end).c_str());
  if (next_pos) *next_pos = cdata_end + 3;
  return true;
}

bool try_bbc_fetch_once(BBCData &result) {
  IPAddress resolved;
  if (!WiFi.hostByName("feeds.bbci.co.uk", resolved)) {
    snprintf(bbc_last_error, sizeof(bbc_last_error), "DNS lookup failed");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();  // public read-only data, not worth carrying a CA bundle for
  HTTPClient http;
  http.setUserAgent("Mozilla/5.0");
  http.setTimeout(8000);
  http.setConnectTimeout(8000);

  if (!http.begin(client, HEADLINES_URL)) {
    snprintf(bbc_last_error, sizeof(bbc_last_error), "http.begin failed");
    return false;
  }

  int code = http.GET();
  if (code != 200) {
    snprintf(bbc_last_error, sizeof(bbc_last_error), "GET=%d heap=%u", code, ESP.getFreeHeap());
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  int n = 0;
  int pos = 0;
  while (n < static_cast<int>(MAX_HEADLINES)) {
    int item_start = payload.indexOf("<item>", pos);
    if (item_start < 0) break;
    int item_end = payload.indexOf("</item>", item_start);
    if (item_end < 0) break;

    int after_title = 0;
    bool got_title = extract_cdata(payload, item_start, "<title>", result.headlines[n].title,
                                    sizeof(result.headlines[n].title), &after_title);
    bool got_desc = got_title && extract_cdata(payload, after_title, "<description>",
                                                result.headlines[n].description,
                                                sizeof(result.headlines[n].description), nullptr);
    if (got_title && got_desc) n++;
    pos = item_end + 7;
  }

  result.count = n;
  result.valid = n > 0;
  if (!result.valid) snprintf(bbc_last_error, sizeof(bbc_last_error), "parsed but no headlines");
  return result.valid;
}

void bbc_fetch_task(void * /*param*/) {
  BBCData result;
  bool ok = false;
  bbc_last_error[0] = '\0';

  uint32_t wait_start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wait_start < WIFI_WAIT_MS) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  if (WiFi.status() != WL_CONNECTED) {
    snprintf(bbc_last_error, sizeof(bbc_last_error), "no WiFi (status=%d)", WiFi.status());
  } else {
    for (int attempt = 0; attempt < MAX_ATTEMPTS && !ok; attempt++) {
      if (attempt > 0) vTaskDelay(pdMS_TO_TICKS(1500));
      ok = try_bbc_fetch_once(result);
    }
  }

  if (ok) {
    xSemaphoreTake(bbc_mutex, portMAX_DELAY);
    result.generation = bbc_shared_data.generation + 1;
    bbc_shared_data = result;
    xSemaphoreGive(bbc_mutex);
  } else {
    bbc_fetch_failed = true;
  }

  bbc_fetch_in_progress = false;
  bbc_fetch_task_handle = nullptr;
  vTaskDelete(nullptr);
}

void start_bbc_fetch() {
  if (bbc_fetch_in_progress) return;
  bbc_fetch_in_progress = true;
  bbc_fetch_failed = false;
  bbc_fetch_started_ms = millis();
  lv_label_set_text(headlines_status_label, "Fetching headlines...");
  for (size_t i = 0; i < MAX_HEADLINES; i++) lv_obj_add_flag(headline_row[i], LV_OBJ_FLAG_HIDDEN);
  xTaskCreate(bbc_fetch_task, "bbc_fetch", 24576, nullptr, 1, &bbc_fetch_task_handle);
}

void apply_bbc_data(const BBCData &d) {
  lv_label_set_text(headlines_status_label, "Headlines");
  shown_bbc_data = d;
  for (int i = 0; i < static_cast<int>(MAX_HEADLINES); i++) {
    if (i >= d.count) {
      lv_obj_add_flag(headline_row[i], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    lv_label_set_text(headline_title[i], d.headlines[i].title);
    lv_obj_clear_flag(headline_row[i], LV_OBJ_FLAG_HIDDEN);
  }
}

void poll_bbc_fetch(lv_timer_t * /*t*/) {
  if (bbc_fetch_in_progress && millis() - bbc_fetch_started_ms > HARD_TIMEOUT_MS) {
    if (bbc_fetch_task_handle) {
      vTaskDelete(bbc_fetch_task_handle);
      bbc_fetch_task_handle = nullptr;
    }
    bbc_fetch_in_progress = false;
    snprintf(bbc_last_error, sizeof(bbc_last_error), "timed out after %lus", HARD_TIMEOUT_MS / 1000);
    bbc_fetch_failed = true;
  }

  if (bbc_fetch_failed) {
    bbc_fetch_failed = false;
    lv_label_set_text_fmt(headlines_status_label, "Failed: %s", bbc_last_error);
    return;
  }

  xSemaphoreTake(bbc_mutex, portMAX_DELAY);
  bool has_update = bbc_shared_data.valid && bbc_shared_data.generation != bbc_shown_generation;
  BBCData snapshot = bbc_shared_data;
  xSemaphoreGive(bbc_mutex);

  if (has_update) {
    bbc_shown_generation = snapshot.generation;
    apply_bbc_data(snapshot);
  }
}

void show_detail(int idx) {
  lv_obj_scroll_to_y(detail_container, 0, LV_ANIM_OFF);  // reset scroll from any previous headline
  lv_label_set_text(detail_title_label, shown_bbc_data.headlines[idx].title);
  lv_label_set_text(detail_desc_label, shown_bbc_data.headlines[idx].description);
  // detail_title_label's LV_SIZE_CONTENT height doesn't recompute until
  // LVGL's next layout pass, which hasn't happened yet at this point in
  // the code - without forcing it here, align_to below reads whatever
  // height was left over from the *previous* headline shown, so a
  // shorter-then-longer title would have the description overlapping
  // ("overflowing") into it instead of sitting cleanly below.
  lv_obj_update_layout(detail_title_label);
  lv_obj_align_to(detail_desc_label, detail_title_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 12);
  lv_obj_clear_flag(detail_container, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(detail_hint, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(headlines_status_label, LV_OBJ_FLAG_HIDDEN);
  for (size_t i = 0; i < MAX_HEADLINES; i++) lv_obj_add_flag(headline_row[i], LV_OBJ_FLAG_HIDDEN);
}

void hide_detail() {
  lv_obj_add_flag(detail_container, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(detail_hint, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(headlines_status_label, LV_OBJ_FLAG_HIDDEN);
  apply_bbc_data(shown_bbc_data);  // restores correct row text/visibility
}

void on_headline_tap(lv_event_t *e) {
  int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
  if (lv_obj_has_flag(headline_row[idx], LV_OBJ_FLAG_HIDDEN)) return;  // empty row
  show_detail(idx);
}

void on_detail_tap(lv_event_t * /*e*/) {
  hide_detail();
}

// Shows/hides the right page and updates the dot row - same white-active/
// gray-inactive convention as the launcher's own home-screen page dots.
void update_page() {
  lv_obj_add_flag(hn_page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(headlines_page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(page == Page::HN ? hn_page : headlines_page, LV_OBJ_FLAG_HIDDEN);

  int idx = static_cast<int>(page);
  for (int i = 0; i < PAGE_COUNT; i++) {
    lv_obj_set_style_bg_color(dot[i], i == idx ? lv_color_white() : lv_color_hex(0x555555), 0);
  }

  // Neither page has a tap-to-refresh control of its own - arriving on
  // either one kicks off a fresh fetch instead, same convention as the
  // WiFi app's Scan page.
  if (page == Page::HN && !fetch_in_progress) {
    start_fetch();
  } else if (page == Page::HEADLINES && !bbc_fetch_in_progress) {
    hide_detail();  // fresh list view, not wherever a previous visit left off
    start_bbc_fetch();
  }
}

void go_to_page(Page p) {
  page = p;
  update_page();
}

// Swipe between pages - same left-advances/right-goes-back convention as
// the launcher home screen and the WiFi app (see their page_gesture_cb).
void page_gesture_cb(lv_event_t * /*e*/) {
  lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
  int idx = static_cast<int>(page);
  if (dir == LV_DIR_LEFT && idx + 1 < PAGE_COUNT) {
    go_to_page(static_cast<Page>(idx + 1));
  } else if (dir == LV_DIR_RIGHT && idx > 0) {
    go_to_page(static_cast<Page>(idx - 1));
  }
}
#endif

void on_open(lv_obj_t *parent) {
  if (!data_mutex) data_mutex = xSemaphoreCreateMutex();

  // A dedicated wrapper, not `parent` (app_root) directly - app_root is a
  // single object shared and reused across every app, so registering the
  // gesture handler below straight on it would silently accumulate a
  // fresh duplicate registration every time this app reopens (see
  // wifi_app.cpp's on_open for the full version of this reasoning).
  root = lv_obj_create(parent);
  lv_obj_remove_style_all(root);
  lv_obj_set_size(root, LCD_PANEL_WIDTH, LCD_PANEL_HEIGHT);
  lv_obj_set_pos(root, 0, 0);
  lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

#if defined(BOARD_TOUCH_LCD147)
  page = Page::HN;
  if (!bbc_mutex) bbc_mutex = xSemaphoreCreateMutex();
  lv_obj_clear_flag(root, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(root, page_gesture_cb, LV_EVENT_GESTURE, nullptr);
#endif

  // --- Hacker News page: top front-page stories, same as before. ---
  hn_page = lv_obj_create(root);
  lv_obj_remove_style_all(hn_page);
  lv_obj_set_size(hn_page, LCD_PANEL_WIDTH, LCD_PANEL_HEIGHT - AREA_TOP);
  lv_obj_set_pos(hn_page, 0, 0);
  lv_obj_clear_flag(hn_page, LV_OBJ_FLAG_SCROLLABLE);

  heading_label = lv_label_create(hn_page);
  lv_label_set_text(heading_label, "Hacker News");
  lv_obj_set_style_text_font(heading_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(heading_label, lv_color_white(), 0);
  lv_obj_align(heading_label, LV_ALIGN_TOP_MID, 0, 4);

  status_label = lv_label_create(hn_page);
  lv_obj_set_style_text_font(status_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(status_label, lv_color_white(), 0);
  // Wraps instead of running off the right edge - matters most for error
  // text ("Failed: GET=... heap=..."), which is long and otherwise
  // unreadable both from being cut off and (before this) too small.
  // Rows are hidden whenever this has enough to say that it might wrap
  // past one line (fetching/failed), so there's nothing under it to
  // collide with even though AREA_TOP doesn't grow to make room.
  lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(status_label, LCD_PANEL_WIDTH - 16);
  lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 22);

  for (size_t i = 0; i < MAX_STORIES; i++) {
    lv_obj_t *row = lv_obj_create(hn_page);
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

#if defined(BOARD_TOUCH_LCD147)
  // --- Headlines page: general/world news from the BBC RSS feed. Tap a
  // row for its description (see show_detail/hide_detail). ---
  headlines_page = lv_obj_create(root);
  lv_obj_remove_style_all(headlines_page);
  lv_obj_set_size(headlines_page, LCD_PANEL_WIDTH, LCD_PANEL_HEIGHT - AREA_TOP);
  lv_obj_set_pos(headlines_page, 0, 0);
  lv_obj_clear_flag(headlines_page, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(headlines_page, LV_OBJ_FLAG_HIDDEN);

  headlines_heading_label = lv_label_create(headlines_page);
  lv_label_set_text(headlines_heading_label, "BBC News");
  lv_obj_set_style_text_font(headlines_heading_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(headlines_heading_label, lv_color_white(), 0);
  lv_obj_align(headlines_heading_label, LV_ALIGN_TOP_MID, 0, 4);

  headlines_status_label = lv_label_create(headlines_page);
  lv_obj_set_style_text_font(headlines_status_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(headlines_status_label, lv_color_white(), 0);
  // See status_label's comment on the HN page - same fix, same reasoning.
  lv_label_set_long_mode(headlines_status_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(headlines_status_label, LCD_PANEL_WIDTH - 16);
  lv_obj_align(headlines_status_label, LV_ALIGN_TOP_MID, 0, 22);

  for (size_t i = 0; i < MAX_HEADLINES; i++) {
    lv_obj_t *row = lv_obj_create(headlines_page);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LCD_PANEL_WIDTH - 12, ROW_H);
    lv_obj_set_pos(row, 6, AREA_TOP + static_cast<lv_coord_t>(i) * ROW_H);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    headline_row[i] = row;
    lv_obj_add_event_cb(row, on_headline_tap, LV_EVENT_SHORT_CLICKED,
                         reinterpret_cast<void *>(static_cast<intptr_t>(i)));

    lv_obj_t *title = lv_label_create(row);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(title, LCD_PANEL_WIDTH - 12);
    lv_obj_set_pos(title, 0, 0);
    headline_title[i] = title;
  }

  detail_container = lv_obj_create(headlines_page);
  lv_obj_remove_style_all(detail_container);
  lv_obj_set_size(detail_container, LCD_PANEL_WIDTH, LCD_PANEL_HEIGHT - AREA_TOP);
  lv_obj_set_pos(detail_container, 0, 0);
  // Scrollable (vertical only, so it can't fight page_gesture_cb's
  // horizontal swipe) with smooth kinetic scrolling for free - LVGL's
  // default drag-to-scroll on a scrollable object already is smooth/
  // momentum-based, nothing extra needed for that part. A tap (press+
  // release without crossing the scroll threshold) still reaches
  // on_detail_tap below same as on any other scrollable LVGL widget.
  lv_obj_set_scroll_dir(detail_container, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(detail_container, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_style_bg_opa(detail_container, LV_OPA_COVER, LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_color(detail_container, lv_color_hex(0x5AC8FA), LV_PART_SCROLLBAR);
  lv_obj_set_style_width(detail_container, 4, LV_PART_SCROLLBAR);
  lv_obj_add_flag(detail_container, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(detail_container, on_detail_tap, LV_EVENT_SHORT_CLICKED, nullptr);

  detail_title_label = lv_label_create(detail_container);
  lv_obj_set_style_text_font(detail_title_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(detail_title_label, lv_color_white(), 0);
  lv_label_set_long_mode(detail_title_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(detail_title_label, LCD_PANEL_WIDTH - 20);
  lv_obj_set_height(detail_title_label, LV_SIZE_CONTENT);
  // Starts at AREA_TOP, not right at the container's own top (0) - the
  // "BBC News" heading (headlines_heading_label) lives there and stays
  // visible/unhidden during detail view (only headlines_status_label
  // gets hidden - see show_detail), so content starting at 0 would
  // overlap straight through it.
  lv_obj_set_pos(detail_title_label, 10, AREA_TOP);

  detail_desc_label = lv_label_create(detail_container);
  lv_obj_set_style_text_font(detail_desc_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(detail_desc_label, lv_color_white(), 0);
  lv_label_set_long_mode(detail_desc_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(detail_desc_label, LCD_PANEL_WIDTH - 20);
  lv_obj_set_height(detail_desc_label, LV_SIZE_CONTENT);
  // x/y set for real in show_detail (lv_obj_align_to, below the title -
  // its height varies with how many lines a given headline wraps to).
  // Bottom padding so a fully-scrolled article's last line clears the
  // fixed "tap to go back" hint below instead of scrolling under it.
  lv_obj_set_style_pad_bottom(detail_container, 24, 0);

  // A sibling of detail_container, not a child of it: children of a
  // scrollable object scroll away with the rest of its content in LVGL,
  // and this needs to stay put as a fixed "you can tap anywhere" cue
  // regardless of scroll position.
  detail_hint = lv_label_create(headlines_page);
  lv_label_set_text(detail_hint, "tap to go back");
  lv_obj_set_style_text_font(detail_hint, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(detail_hint, lv_color_hex(0x555555), 0);
  lv_obj_align(detail_hint, LV_ALIGN_BOTTOM_MID, 0, -8);
  lv_obj_add_flag(detail_hint, LV_OBJ_FLAG_HIDDEN);

  // --- Page dots. ---
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

  shown_generation = 0;
  poll_timer = lv_timer_create(poll_fetch, POLL_MS, nullptr);

#if defined(BOARD_TOUCH_LCD147)
  bbc_shown_generation = 0;
  bbc_poll_timer = lv_timer_create(poll_bbc_fetch, POLL_MS, nullptr);
  update_page();
#else
  // Non-touch board keeps the old bottom hint text (no dots/paging here -
  // this board never had the Headlines page to swipe to).
  hint_label = lv_label_create(hn_page);
  lv_obj_set_style_text_font(hint_label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(hint_label, lv_color_hex(0x555555), 0);
  lv_obj_align(hint_label, LV_ALIGN_BOTTOM_MID, 0, -8);
  lv_label_set_text(hint_label, ACTION_WORD ": refresh  |  " HOME_HINT);
  start_fetch();
#endif
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
  root = hn_page = nullptr;
  heading_label = status_label = hint_label = nullptr;
  for (size_t i = 0; i < MAX_STORIES; i++) row_container[i] = row_title[i] = nullptr;

#if defined(BOARD_TOUCH_LCD147)
  if (bbc_poll_timer) {
    lv_timer_del(bbc_poll_timer);
    bbc_poll_timer = nullptr;
  }
  if (bbc_fetch_in_progress && bbc_fetch_task_handle) {
    vTaskDelete(bbc_fetch_task_handle);
    bbc_fetch_task_handle = nullptr;
    bbc_fetch_in_progress = false;
  }
  headlines_page = headlines_heading_label = headlines_status_label = nullptr;
  detail_container = detail_title_label = detail_desc_label = detail_hint = nullptr;
  dots_row = nullptr;
  for (int i = 0; i < PAGE_COUNT; i++) dot[i] = nullptr;
  for (size_t i = 0; i < MAX_HEADLINES; i++) headline_row[i] = headline_title[i] = nullptr;
#endif
}

// Non-touch board only in practice: wants_raw_touch means the touch
// board's generic tap-anywhere overlay is skipped for this app, and its
// physical button is solely a quick-press-to-home shortcut now (see
// launcher_handle_button) - so on the touch board nothing ever calls
// this. On the non-touch board this is just "refresh".
void on_short_press() {
  start_fetch();
}

}  // namespace

const AppDescriptor hn_app = {
    .name = "News",
    .icon_symbol = LV_SYMBOL_LIST,
    .icon_color = lv_color_hex(0xFF6600),
    .on_open = on_open,
    .on_close = on_close,
    .on_short_press = on_short_press,
    // Touch board only (no-op elsewhere, see app_interface.h) - needed so
    // page_gesture_cb's swipe and the Headlines page's row/detail taps
    // reach this app's own widgets directly, instead of the launcher's
    // generic tap-anywhere-reaches-on_short_press overlay swallowing
    // them first.
    .wants_raw_touch = true,
};

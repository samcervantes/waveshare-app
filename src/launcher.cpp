#include "launcher.h"

#include <Arduino.h>
#include <lvgl.h>

#include "app_registry.h"
#include "config.h"
#include "wifi_status.h"

namespace {

// Room for future vibe-coded apps without touching the layout code.
constexpr size_t MAX_APPS = 16;
constexpr size_t MAX_PAGES = 8;
constexpr size_t APPS_PER_PAGE = 2;

constexpr lv_coord_t SCREEN_W = LCD_PANEL_WIDTH;
constexpr lv_coord_t SCREEN_H = LCD_PANEL_HEIGHT;
constexpr lv_coord_t DOTS_H = 24;
constexpr lv_coord_t PAGE_H = SCREEN_H - DOTS_H;
// 90px (physically-accurate 60pt) read too small, 108px read too big -
// settled here per feedback.
constexpr lv_coord_t TILE_SIZE = 100;

constexpr uint32_t PAGE_ANIM_MS = 220;

lv_obj_t *launcher_root = nullptr;
lv_obj_t *app_root = nullptr;
lv_obj_t *app_touch_overlay = nullptr;
lv_obj_t *track = nullptr;
lv_obj_t *icon_tile[MAX_APPS] = {nullptr};
lv_obj_t *dot[MAX_PAGES] = {nullptr};

size_t page_count = 0;
int cursor = 0;
int active_app = -1;

void anim_track_x_cb(void *var, int32_t v) {
  lv_obj_set_x(static_cast<lv_obj_t *>(var), v);
}

void update_highlight(bool animate) {
  for (size_t i = 0; i < APP_COUNT; i++) {
    bool selected = (static_cast<int>(i) == cursor);
    lv_obj_set_style_border_width(icon_tile[i], selected ? 4 : 0, 0);
    lv_obj_set_style_border_color(icon_tile[i], lv_color_white(), 0);
    lv_obj_set_style_border_opa(icon_tile[i], LV_OPA_COVER, 0);
  }

  size_t page = cursor / APPS_PER_PAGE;
  for (size_t p = 0; p < page_count; p++) {
    bool active = (p == page);
    lv_obj_set_style_bg_color(dot[p], active ? lv_color_white() : lv_color_hex(0x555555), 0);
  }

  lv_coord_t target_x = -static_cast<lv_coord_t>(page) * SCREEN_W;
  if (animate) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, track);
    lv_anim_set_exec_cb(&a, anim_track_x_cb);
    lv_anim_set_values(&a, lv_obj_get_x(track), target_x);
    lv_anim_set_time(&a, PAGE_ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
  } else {
    lv_obj_set_x(track, target_x);
  }
}

void open_app(int index) {
  active_app = index;
  lv_obj_add_flag(launcher_root, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(app_root, LV_OBJ_FLAG_HIDDEN);
  // Leave the overlay hidden for wants_raw_touch apps so it doesn't win
  // every hit-test over the widgets they create themselves - see
  // app_interface.h.
  if (!app_registry[index]->wants_raw_touch) {
    lv_obj_clear_flag(app_touch_overlay, LV_OBJ_FLAG_HIDDEN);
  }
  Serial.printf("[launcher] opening %s\n", app_registry[index]->name);
  app_registry[index]->on_open(app_root);
}

void close_app() {
  Serial.printf("[launcher] closing %s\n", app_registry[active_app]->name);
  if (app_registry[active_app]->on_close) {
    app_registry[active_app]->on_close();
  }
  lv_obj_clean(app_root);
  lv_obj_add_flag(app_root, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(app_touch_overlay, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(launcher_root, LV_OBJ_FLAG_HIDDEN);
  active_app = -1;
}

// Touch support (touch board only - these events only ever fire if
// display.cpp registered a pointer indev, see BOARD_TOUCH_LCD147 there).
// Tapping an icon opens it directly rather than stepping the cursor over
// with repeated short presses, since "tap what you want" is what a
// touchscreen user expects. LV_EVENT_SHORT_CLICKED (not LV_EVENT_CLICKED)
// is deliberate: LVGL still fires CLICKED on release even after a long
// press, and holding a finger on an icon shouldn't also open it.
void icon_touch_cb(lv_event_t *e) {
  int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
  cursor = idx;
  update_highlight(false);
  open_app(cursor);
}

// Swipe between pages. LV_EVENT_GESTURE fires on the object the touch
// started on (icon_tile, most of the time) and bubbles up through parents
// that have LV_OBJ_FLAG_GESTURE_BUBBLE - which LVGL sets by default on
// every object that has a parent, so it bubbles all the way up to
// launcher_root for free; launcher_root has that flag explicitly cleared
// (see launcher_init) so the bubble stops there instead of continuing
// past it to the screen object, where nothing would be listening.
// LV_DIR_LEFT (finger dragging leftward) advances to the next page,
// matching how the track already slides left as `page` increases.
void page_gesture_cb(lv_event_t * /*e*/) {
  if (active_app != -1) return;

  lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
  int page = cursor / static_cast<int>(APPS_PER_PAGE);
  if (dir == LV_DIR_LEFT && page + 1 < static_cast<int>(page_count)) {
    page++;
  } else if (dir == LV_DIR_RIGHT && page > 0) {
    page--;
  } else {
    return;
  }

  cursor = page * static_cast<int>(APPS_PER_PAGE);
  if (cursor >= static_cast<int>(APP_COUNT)) cursor = static_cast<int>(APP_COUNT) - 1;
  update_highlight(true);
}

// A transparent full-screen object stacked above app_root (see its
// creation below) so a tap anywhere in an app - regardless of what
// widgets the app itself put there - reaches this handler and triggers
// the app's per-app action, same as on_short_press. Skipped for apps with
// wants_raw_touch set (see open_app/close_app) so their own widgets can
// be tapped directly instead.
//
// Touch board only: touch is the whole interface for interacting with
// apps here, so this - not the physical button - is how per-app actions
// fire (see launcher_handle_button, where BOARD_TOUCH_LCD147 reserves the
// button solely for a quick press back to home). Going home is
// deliberately not also a touch long-press: holding a finger still is a
// normal part of some apps' gameplay (e.g. Pong - pausing mid-drag to
// track the ball), so a touch long-press-to-home kept firing by accident.
void app_overlay_short_click_cb(lv_event_t * /*e*/) {
  if (active_app == -1) return;
  if (app_registry[active_app]->on_short_press) {
    app_registry[active_app]->on_short_press();
  }
}

}  // namespace

void launcher_init() {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

  page_count = (APP_COUNT + APPS_PER_PAGE - 1) / APPS_PER_PAGE;

  launcher_root = lv_obj_create(scr);
  lv_obj_remove_style_all(launcher_root);
  lv_obj_set_size(launcher_root, SCREEN_W, SCREEN_H);
  lv_obj_set_pos(launcher_root, 0, 0);
  lv_obj_clear_flag(launcher_root, LV_OBJ_FLAG_SCROLLABLE);
  // Every LVGL object with a parent gets LV_OBJ_FLAG_GESTURE_BUBBLE set by
  // default (see lv_obj_create in lv_obj.c) - without clearing it here,
  // the bubble in page_gesture_cb's comment keeps climbing right past
  // launcher_root (where the handler below is attached) up to the screen
  // object, where nothing listens, and the gesture is silently dropped.
  lv_obj_clear_flag(launcher_root, LV_OBJ_FLAG_GESTURE_BUBBLE);

  track = lv_obj_create(launcher_root);
  lv_obj_remove_style_all(track);
  lv_obj_set_size(track, SCREEN_W * page_count, PAGE_H);
  lv_obj_set_pos(track, 0, 0);
  lv_obj_clear_flag(track, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(launcher_root, page_gesture_cb, LV_EVENT_GESTURE, nullptr);

  for (size_t p = 0; p < page_count; p++) {
    lv_obj_t *page = lv_obj_create(track);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, SCREEN_W, PAGE_H);
    lv_obj_set_pos(page, p * SCREEN_W, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (size_t slot = 0; slot < APPS_PER_PAGE; slot++) {
      size_t idx = p * APPS_PER_PAGE + slot;
      if (idx >= APP_COUNT) break;
      const AppDescriptor *app = app_registry[idx];

      lv_obj_t *block = lv_obj_create(page);
      lv_obj_remove_style_all(block);
      lv_obj_set_size(block, SCREEN_W, PAGE_H / APPS_PER_PAGE);
      lv_obj_clear_flag(block, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_set_flex_flow(block, LV_FLEX_FLOW_COLUMN);
      lv_obj_set_flex_align(block, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

      lv_obj_t *tile = lv_obj_create(block);
      lv_obj_set_size(tile, TILE_SIZE, TILE_SIZE);
      lv_obj_set_style_radius(tile, 23, 0);
      lv_obj_set_style_bg_color(tile, app->icon_color, 0);
      lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
      lv_obj_set_style_pad_all(tile, 0, 0);
      lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_add_event_cb(tile, icon_touch_cb, LV_EVENT_SHORT_CLICKED,
                           reinterpret_cast<void *>(static_cast<intptr_t>(idx)));

      lv_obj_t *sym = lv_label_create(tile);
      lv_label_set_text(sym, app->icon_symbol);
      lv_obj_set_style_text_font(sym, &lv_font_montserrat_32, 0);
      lv_obj_set_style_text_color(sym, lv_color_white(), 0);
      lv_obj_center(sym);

      lv_obj_t *label = lv_label_create(block);
      lv_label_set_text(label, app->name);
      lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
      lv_obj_set_style_text_color(label, lv_color_white(), 0);
      lv_obj_set_style_pad_top(label, 6, 0);

      icon_tile[idx] = tile;
    }
  }

  lv_obj_t *dots_row = lv_obj_create(launcher_root);
  lv_obj_remove_style_all(dots_row);
  lv_obj_set_size(dots_row, SCREEN_W, DOTS_H);
  lv_obj_set_pos(dots_row, 0, PAGE_H);
  lv_obj_clear_flag(dots_row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(dots_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(dots_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(dots_row, 8, 0);

  for (size_t p = 0; p < page_count; p++) {
    lv_obj_t *d = lv_obj_create(dots_row);
    lv_obj_remove_style_all(d);
    lv_obj_set_size(d, 8, 8);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(d, lv_color_hex(0x555555), 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    dot[p] = d;
  }

  // Home-screen-only WiFi status dot - a child of launcher_root (not
  // app_root) so it's automatically hidden/shown alongside the rest of
  // the home screen by open_app/close_app, with no extra visibility
  // bookkeeping needed here. Icon grid sizing (TILE_SIZE) is untouched;
  // this just overlays the empty top-right corner.
  lv_obj_t *wifi_icon = lv_label_create(launcher_root);
  lv_obj_set_style_text_font(wifi_icon, &lv_font_montserrat_16, 0);
  lv_label_set_text(wifi_icon, LV_SYMBOL_WIFI);
  lv_obj_align(wifi_icon, LV_ALIGN_TOP_RIGHT, -8, 6);
  wifi_status_init(wifi_icon);

  app_root = lv_obj_create(scr);
  lv_obj_remove_style_all(app_root);
  lv_obj_set_size(app_root, SCREEN_W, SCREEN_H);
  lv_obj_set_pos(app_root, 0, 0);
  lv_obj_set_style_bg_color(app_root, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(app_root, LV_OPA_COVER, 0);
  lv_obj_clear_flag(app_root, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(app_root, LV_OBJ_FLAG_HIDDEN);

  // A sibling created after app_root (so it always paints on top of
  // whatever the active app builds), not a child of it - app_root's
  // children get wiped by lv_obj_clean() on every close_app(), which
  // would destroy this if it lived underneath.
  app_touch_overlay = lv_obj_create(scr);
  lv_obj_remove_style_all(app_touch_overlay);
  lv_obj_set_size(app_touch_overlay, SCREEN_W, SCREEN_H);
  lv_obj_set_pos(app_touch_overlay, 0, 0);
  lv_obj_clear_flag(app_touch_overlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(app_touch_overlay, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(app_touch_overlay, app_overlay_short_click_cb, LV_EVENT_SHORT_CLICKED, nullptr);

  cursor = 0;
  active_app = -1;
  update_highlight(false);

  Serial.printf("[launcher] ready: %u apps across %u pages\n", APP_COUNT, page_count);
}

void launcher_handle_button(ButtonEvent event) {
  if (event == ButtonEvent::None) return;

#if defined(BOARD_TOUCH_LCD147)
  // Touch is the whole interface here - tapping icons opens them
  // (icon_touch_cb), tapping/dragging within an app drives its per-app
  // action (app_overlay_short_click_cb, or the app's own touch handling
  // for wants_raw_touch apps like Pong). The physical button's only job
  // left is a quick press back to home from inside an app; every other
  // button event (a press on the home screen, or a long press anywhere)
  // is intentionally ignored.
  if (active_app != -1 && event == ButtonEvent::ShortPress) {
    close_app();
  }
#else
  if (active_app == -1) {
    if (event == ButtonEvent::ShortPress) {
      cursor = (cursor + 1) % static_cast<int>(APP_COUNT);
      update_highlight(true);
    } else if (event == ButtonEvent::LongPress) {
      open_app(cursor);
    }
  } else {
    if (event == ButtonEvent::ShortPress) {
      if (app_registry[active_app]->on_short_press) {
        app_registry[active_app]->on_short_press();
      }
    } else if (event == ButtonEvent::LongPress) {
      close_app();
    }
  }
#endif
}

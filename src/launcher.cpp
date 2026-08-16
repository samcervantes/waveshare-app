#include "launcher.h"

#include <Arduino.h>
#include <lvgl.h>

#include "app_registry.h"
#include "config.h"

namespace {

// Room for future vibe-coded apps without touching the layout code.
constexpr size_t MAX_APPS = 16;
constexpr size_t MAX_PAGES = 8;
constexpr size_t APPS_PER_PAGE = 2;

constexpr lv_coord_t SCREEN_W = LCD_PANEL_WIDTH;
constexpr lv_coord_t SCREEN_H = LCD_PANEL_HEIGHT;
constexpr lv_coord_t DOTS_H = 24;
constexpr lv_coord_t PAGE_H = SCREEN_H - DOTS_H;
// Physically-accurate (60pt) size was 90px; bumped up further per feedback
// that it still read small on this screen.
constexpr lv_coord_t TILE_SIZE = 108;

constexpr uint32_t PAGE_ANIM_MS = 220;

lv_obj_t *launcher_root = nullptr;
lv_obj_t *app_root = nullptr;
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
  lv_obj_clear_flag(launcher_root, LV_OBJ_FLAG_HIDDEN);
  active_app = -1;
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

  track = lv_obj_create(launcher_root);
  lv_obj_remove_style_all(track);
  lv_obj_set_size(track, SCREEN_W * page_count, PAGE_H);
  lv_obj_set_pos(track, 0, 0);
  lv_obj_clear_flag(track, LV_OBJ_FLAG_SCROLLABLE);

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
      lv_obj_set_style_radius(tile, 25, 0);
      lv_obj_set_style_bg_color(tile, app->icon_color, 0);
      lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
      lv_obj_set_style_pad_all(tile, 0, 0);
      lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

      lv_obj_t *sym = lv_label_create(tile);
      lv_label_set_text(sym, app->icon_symbol);
      lv_obj_set_style_text_font(sym, &lv_font_montserrat_40, 0);
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

  app_root = lv_obj_create(scr);
  lv_obj_remove_style_all(app_root);
  lv_obj_set_size(app_root, SCREEN_W, SCREEN_H);
  lv_obj_set_pos(app_root, 0, 0);
  lv_obj_set_style_bg_color(app_root, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(app_root, LV_OPA_COVER, 0);
  lv_obj_clear_flag(app_root, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(app_root, LV_OBJ_FLAG_HIDDEN);

  cursor = 0;
  active_app = -1;
  update_highlight(false);

  Serial.printf("[launcher] ready: %u apps across %u pages\n", APP_COUNT, page_count);
}

void launcher_handle_button(ButtonEvent event) {
  if (event == ButtonEvent::None) return;

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
}

#include "battery_app.h"

#include <Arduino.h>
#include <lvgl.h>

#include "config.h"

// PIN_BAT_ADC only exists on this board (see config.h's comment - the
// non-touch board's battery-sense circuitry, if any, hasn't been verified)
// - this whole app is a no-op on that build; see app_registry.cpp, same
// pattern as rgb_app.cpp in reverse.
#if defined(BOARD_TOUCH_LCD147)

// Shows battery charge (as a %, from the ADC-sensed voltage on PIN_BAT_ADC
// - see config.h's comment) and whether it's charging.
//
// This board has no dedicated charging-status GPIO (confirmed against
// Waveshare's own docs - the onboard ETA6098 charging IC's STAT/CHRG-style
// output isn't broken out anywhere) - voltage sensing is all there is. So
// "charging" here is inferred from the voltage *trend* over a rolling
// window (rising -> charging, falling/flat -> on battery, flat-and-nearly-
// full -> fully charged) rather than read directly. This is a heuristic,
// not a hardware fact - it needs the trend window to fill before it can
// say anything (shows "Detecting..." until then), and quick plug/unplug
// right at the threshold could read ambiguously for a few seconds.

namespace {

constexpr uint32_t SAMPLE_MS = 1000;
// ~20s rolling window to smooth out ADC noise before judging a trend -
// short enough to notice charging/unplugging fairly promptly, long enough
// that single-sample noise doesn't flip the status back and forth.
constexpr int TREND_SAMPLES = 20;
constexpr float CHARGING_TREND_V = 0.02f;    // rising more than this over the window -> charging
constexpr float DISCHARGING_TREND_V = 0.02f;  // falling more than this -> on battery
constexpr float FULL_VOLTAGE = 4.15f;         // flat at/above this -> call it fully charged, not just "on battery"

// Standard-ish 1S LiPo discharge curve (voltage -> percent), descending by
// voltage - linearly interpolated between points. LiPo voltage sags
// non-linearly with charge, so a straight line from 3.0V-4.2V would read
// very wrong in the middle of the range; this is the commonly used
// reference curve for a generic 1S cell, not calibrated against this
// specific battery.
struct VoltPoint {
  float volts;
  float percent;
};
constexpr VoltPoint CURVE[] = {
    {4.20f, 100.0f}, {4.15f, 95.0f}, {4.11f, 90.0f}, {4.08f, 85.0f}, {4.02f, 80.0f}, {3.98f, 75.0f},
    {3.95f, 70.0f},  {3.91f, 65.0f}, {3.87f, 60.0f},  {3.85f, 55.0f}, {3.84f, 50.0f}, {3.82f, 45.0f},
    {3.80f, 40.0f},  {3.79f, 35.0f}, {3.77f, 30.0f},  {3.75f, 25.0f}, {3.73f, 20.0f}, {3.71f, 15.0f},
    {3.69f, 10.0f},  {3.61f, 5.0f},  {3.27f, 0.0f},
};
constexpr int CURVE_LEN = sizeof(CURVE) / sizeof(CURVE[0]);

float voltage_to_percent(float v) {
  if (v >= CURVE[0].volts) return 100.0f;
  if (v <= CURVE[CURVE_LEN - 1].volts) return 0.0f;
  for (int i = 0; i < CURVE_LEN - 1; i++) {
    if (v <= CURVE[i].volts && v >= CURVE[i + 1].volts) {
      float span = CURVE[i].volts - CURVE[i + 1].volts;
      float t = (v - CURVE[i + 1].volts) / span;
      return CURVE[i + 1].percent + t * (CURVE[i].percent - CURVE[i + 1].percent);
    }
  }
  return 0.0f;
}

lv_obj_t *batt_body = nullptr;
lv_obj_t *batt_fill = nullptr;
lv_obj_t *percent_label = nullptr;
lv_obj_t *status_label = nullptr;
lv_obj_t *voltage_label = nullptr;
lv_timer_t *sample_timer = nullptr;

float trend_buf[TREND_SAMPLES];
int trend_count = 0;   // how many samples collected so far (caps at TREND_SAMPLES)
int trend_next = 0;    // circular write index

constexpr lv_coord_t BATT_W = 90;
constexpr lv_coord_t BATT_H = 170;
constexpr lv_coord_t BATT_TOP = 24;
constexpr lv_coord_t NUB_W = 30;
constexpr lv_coord_t NUB_H = 10;

void apply_reading(float voltage) {
  float pct = voltage_to_percent(voltage);
  lv_coord_t fill_h = static_cast<lv_coord_t>((BATT_H - 8) * (pct / 100.0f));
  lv_obj_set_height(batt_fill, fill_h);
  lv_obj_align(batt_fill, LV_ALIGN_BOTTOM_MID, 0, -4);

  lv_color_t level_color;
  if (pct > 50.0f) {
    level_color = lv_color_hex(0x30D158);  // green
  } else if (pct > 20.0f) {
    level_color = lv_color_hex(0xFFD60A);  // yellow
  } else {
    level_color = lv_color_hex(0xFF453A);  // red
  }
  lv_obj_set_style_bg_color(batt_fill, level_color, 0);

  char buf[16];
  snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(pct + 0.5f));
  lv_label_set_text(percent_label, buf);

  trend_buf[trend_next] = voltage;
  trend_next = (trend_next + 1) % TREND_SAMPLES;
  if (trend_count < TREND_SAMPLES) trend_count++;

  if (trend_count < TREND_SAMPLES) {
    lv_label_set_text(status_label, "Detecting...");
    lv_obj_set_style_text_color(status_label, lv_color_hex(0x8E8E93), 0);
  } else {
    // Oldest sample in the ring is the one right after the write cursor
    // (it's about to be overwritten next).
    float oldest = trend_buf[trend_next];
    float trend = voltage - oldest;
    if (trend > CHARGING_TREND_V) {
      lv_label_set_text(status_label, LV_SYMBOL_CHARGE " Charging");
      lv_obj_set_style_text_color(status_label, lv_color_hex(0x30D158), 0);
    } else if (trend < -DISCHARGING_TREND_V) {
      lv_label_set_text(status_label, "On Battery");
      lv_obj_set_style_text_color(status_label, lv_color_white(), 0);
    } else if (voltage >= FULL_VOLTAGE) {
      lv_label_set_text(status_label, "Fully Charged");
      lv_obj_set_style_text_color(status_label, lv_color_hex(0x30D158), 0);
    } else {
      lv_label_set_text(status_label, "On Battery");
      lv_obj_set_style_text_color(status_label, lv_color_white(), 0);
    }
  }

  char vbuf[24];
  snprintf(vbuf, sizeof(vbuf), "%.2f V", voltage);
  lv_label_set_text(voltage_label, vbuf);
}

void sample_timer_cb(lv_timer_t * /*t*/) {
  // 3.0x undoes this board's onboard voltage divider - see config.h's
  // PIN_BAT_ADC comment, confirmed against Waveshare's own demo code.
  float voltage = analogReadMilliVolts(PIN_BAT_ADC) * 3.0f / 1000.0f;
  apply_reading(voltage);
}

void on_open(lv_obj_t *parent) {
  trend_count = 0;
  trend_next = 0;

  // Nub - drawn first so the body's border sits on top of it where they
  // meet, matching a normal battery icon silhouette.
  lv_obj_t *nub = lv_obj_create(parent);
  lv_obj_remove_style_all(nub);
  lv_obj_set_size(nub, NUB_W, NUB_H);
  lv_obj_align(nub, LV_ALIGN_TOP_MID, 0, BATT_TOP);
  lv_obj_set_style_bg_opa(nub, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(nub, lv_color_hex(0x8E8E93), 0);
  lv_obj_clear_flag(nub, LV_OBJ_FLAG_SCROLLABLE);

  batt_body = lv_obj_create(parent);
  lv_obj_remove_style_all(batt_body);
  lv_obj_set_size(batt_body, BATT_W, BATT_H);
  lv_obj_align(batt_body, LV_ALIGN_TOP_MID, 0, BATT_TOP + NUB_H - 2);
  lv_obj_set_style_border_width(batt_body, 4, 0);
  lv_obj_set_style_border_color(batt_body, lv_color_hex(0x8E8E93), 0);
  lv_obj_set_style_radius(batt_body, 8, 0);
  lv_obj_set_style_bg_opa(batt_body, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(batt_body, lv_color_hex(0x1C1C1E), 0);
  lv_obj_clear_flag(batt_body, LV_OBJ_FLAG_SCROLLABLE);

  // Fill bar - a plain rect whose height/color change with the reading, not
  // an lv_bar: only its size and bg_color are ever touched (cheap property
  // writes, no LVGL "layer"/transform involved - see gyro_app.cpp for why
  // that distinction has mattered on this board before).
  batt_fill = lv_obj_create(batt_body);
  lv_obj_remove_style_all(batt_fill);
  lv_obj_set_width(batt_fill, BATT_W - 8);
  lv_obj_set_style_radius(batt_fill, 4, 0);
  lv_obj_set_style_bg_opa(batt_fill, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(batt_fill, lv_color_hex(0x30D158), 0);
  lv_obj_clear_flag(batt_fill, LV_OBJ_FLAG_SCROLLABLE);

  // Fixed width + center align, not content-sized alignment: these labels
  // are empty at creation time (real text lands later, in apply_reading,
  // once the first ADC sample comes in) and lv_obj_align_to reads an
  // object's width at call time - aligning an empty label then filling it
  // in afterward anchors it from an empty-content position and leaves it
  // lopsided (and overlapping the hint below it) once real, wider text
  // arrives. Fixed width sidesteps it entirely: the object's alignment no
  // longer depends on its content. Same gotcha, same fix as gyro_app.cpp.
  constexpr lv_coord_t LABEL_WIDTH = LCD_PANEL_WIDTH - 8;
  percent_label = lv_label_create(parent);
  lv_obj_set_style_text_font(percent_label, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(percent_label, lv_color_white(), 0);
  lv_obj_set_width(percent_label, LABEL_WIDTH);
  lv_obj_set_style_text_align(percent_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align_to(percent_label, batt_body, LV_ALIGN_OUT_BOTTOM_MID, 0, 14);

  status_label = lv_label_create(parent);
  lv_obj_set_style_text_font(status_label, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(status_label, lv_color_white(), 0);
  lv_obj_set_width(status_label, LABEL_WIDTH);
  lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align_to(status_label, percent_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

  voltage_label = lv_label_create(parent);
  lv_obj_set_style_text_font(voltage_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(voltage_label, lv_color_hex(0xAAAAAA), 0);
  lv_obj_set_width(voltage_label, LABEL_WIDTH);
  lv_obj_set_style_text_align(voltage_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align_to(voltage_label, status_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

  lv_obj_t *hint = lv_label_create(parent);
  lv_label_set_text(hint, HOME_HINT);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(hint, lv_color_hex(0x555555), 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -16);

  sample_timer_cb(nullptr);
  sample_timer = lv_timer_create(sample_timer_cb, SAMPLE_MS, nullptr);
}

void on_close() {
  if (sample_timer) {
    lv_timer_del(sample_timer);
    sample_timer = nullptr;
  }
  batt_body = batt_fill = percent_label = status_label = voltage_label = nullptr;
}

}  // namespace

const AppDescriptor battery_app = {
    .name = "Battery",
    .icon_symbol = LV_SYMBOL_BATTERY_FULL,
    .icon_color = lv_color_hex(0x30D158),
    .on_open = on_open,
    .on_close = on_close,
    .on_short_press = nullptr,
};

#endif  // BOARD_TOUCH_LCD147

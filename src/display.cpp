#include "display.h"
#include "config.h"

#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <lvgl.h>

#if defined(BOARD_TOUCH_LCD147)
#include "imu.h"
#include "touch.h"
#endif

namespace {

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI _bus;

 public:
  LGFX() {
    auto bcfg = _bus.config();
    bcfg.spi_host = SPI2_HOST;
    bcfg.spi_mode = 0;
    bcfg.freq_write = 40000000;
    bcfg.spi_3wire = true;
    bcfg.use_lock = true;
    bcfg.dma_channel = SPI_DMA_CH_AUTO;
    bcfg.pin_sclk = PIN_LCD_SCLK;
    bcfg.pin_mosi = PIN_LCD_MOSI;
    bcfg.pin_miso = -1;
    bcfg.pin_dc = PIN_LCD_DC;
    _bus.config(bcfg);
    _panel.setBus(&_bus);

    auto pcfg = _panel.config();
    pcfg.pin_cs = PIN_LCD_CS;
    pcfg.pin_rst = PIN_LCD_RST;
    pcfg.pin_busy = -1;
    pcfg.panel_width = LCD_PANEL_WIDTH;
    pcfg.panel_height = LCD_PANEL_HEIGHT;
    pcfg.offset_x = LCD_OFFSET_X;
    pcfg.offset_y = LCD_OFFSET_Y;
#if defined(BOARD_TOUCH_LCD147)
    // JD9853 on this board reports non-inverted colors, opposite of the
    // ST7789 board's panel. rgb_order was previously set true (BGR) based
    // on how solid UI colors looked, but that was never checked against
    // full-color continuous-tone content - Photos app testing showed every
    // photo with a strong red/blue cast (skin tones and a sunset both came
    // out blue), which is exactly what an R/B-swapped panel order does to
    // that kind of content while leaving green/white/gray UI elements
    // (most of this app's icons/text) looking approximately fine. Flipped
    // back to false (RGB, matching the non-touch board) - tune here again
    // if colors still look wrong.
    pcfg.invert = false;
    pcfg.rgb_order = false;
#else
    pcfg.invert = true;
    pcfg.rgb_order = false;
#endif
    pcfg.bus_shared = true;
    _panel.config(pcfg);

    setPanel(&_panel);
  }
};

LGFX lcd;

// LVGL draws into a partial buffer (a handful of rows) rather than a full
// frame buffer to keep RAM use low on this chip's 512KB SRAM.
constexpr uint16_t DRAW_BUF_LINES = 40;
lv_disp_draw_buf_t draw_buf;
lv_color_t buf1[LCD_PANEL_WIDTH * DRAW_BUF_LINES];
lv_color_t buf2[LCD_PANEL_WIDTH * DRAW_BUF_LINES];
lv_disp_drv_t disp_drv;

void lvgl_flush_cb(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;

  lcd.startWrite();
  lcd.setAddrWindow(area->x1, area->y1, w, h);
  lcd.writePixels(reinterpret_cast<lgfx::rgb565_t *>(color_p), w * h);
  lcd.endWrite();

  lv_disp_flush_ready(disp);
}

#if defined(BOARD_TOUCH_LCD147)
lv_indev_drv_t indev_drv;

void touch_read_cb(lv_indev_drv_t * /*drv*/, lv_indev_data_t *data) {
  int16_t x = 0, y = 0;
  bool pressed = touch_read(&x, &y);
  data->point.x = x;
  data->point.y = y;
  data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
#endif

}  // namespace

void display_init() {
  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, HIGH);

  lcd.init();
  lcd.setRotation(LCD_ROTATION);
  lcd.setBrightness(255);

  lv_init();

  lv_disp_draw_buf_init(&draw_buf, buf1, buf2, LCD_PANEL_WIDTH * DRAW_BUF_LINES);

  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = LCD_PANEL_WIDTH;
  disp_drv.ver_res = LCD_PANEL_HEIGHT;
  disp_drv.flush_cb = lvgl_flush_cb;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

#if defined(BOARD_TOUCH_LCD147)
  touch_init();
  imu_init();  // shares touch_init()'s already-begun Wire bus (see config.h)

  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = touch_read_cb;
  indev_drv.long_press_time = 500;  // matches input.cpp's LONG_PRESS_MS
  lv_indev_drv_register(&indev_drv);
#endif
}

void display_tick() {
  lv_timer_handler();
}

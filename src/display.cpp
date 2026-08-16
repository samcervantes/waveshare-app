#include "display.h"
#include "config.h"

#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <lvgl.h>

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
    pcfg.invert = true;
    pcfg.rgb_order = false;
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
}

void display_tick() {
  lv_timer_handler();
}

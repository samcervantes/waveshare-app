#pragma once

#if defined(BOARD_TOUCH_LCD147)

// Waveshare ESP32-C6-Touch-LCD-1.47 pin map
// https://docs.waveshare.com/ESP32-C6-Touch-LCD-1.47
// Display is a JD9853 controller, but it speaks the same command set as
// ST7789, so display.cpp reuses LovyanGFX's Panel_ST7789 driver unchanged -
// only the pins/offsets below differ from the non-touch board.

// JD9853 display (SPI) - shares the SCLK/MOSI lines with the TF card slot,
// which this project doesn't use, so no extra CS handling is needed here.
#define PIN_LCD_SCLK 1
#define PIN_LCD_MOSI 2
#define PIN_LCD_CS   14
#define PIN_LCD_DC   15
#define PIN_LCD_RST  22
#define PIN_LCD_BL   23

// Waveshare's own docs don't mention it, but community demo code for this
// exact JD9853 panel uses the same 34px column-window offset as the
// ST7789 board (confirmed: image was shifted left without it).
#define LCD_PANEL_WIDTH  172
#define LCD_PANEL_HEIGHT 320
#define LCD_OFFSET_X     34
#define LCD_OFFSET_Y     0

// This panel's glass is mounted mirrored relative to the ST7789 board, so
// plain rotation 0 (MADCTL=0x00) comes out horizontally flipped. LovyanGFX
// accepts rotation values 0-7 (not just 0-3); 6 selects the MADCTL
// MX|MH combination, which cancels that mirror back to normal reading
// orientation. Revisit if the image still looks off.
#define LCD_ROTATION 6

// No onboard NeoPixel on this board, so rgb_app is excluded from the
// registry (see app_registry.cpp).

// BOOT button - same GPIO9 as the non-touch board. Waveshare's docs say
// GPIO8 is the default BOOT pin, but on this actual unit that's wrong:
// confirmed by scanning GPIO4/5/8/9/0 live and watching which one the
// physical button pulls low - it's GPIO9, not GPIO8.
#define PIN_BOOT_BUTTON 9

// AXS5106L capacitive touch controller (I2C) and QMI8658A IMU, shared bus.
// Touch is the primary interface on this board (tap to navigate/interact);
// the physical button's only remaining job is a quick way home from inside
// an app - see HOME_HINT below and launcher.cpp's launcher_handle_button.
#define PIN_TOUCH_SDA 18
#define PIN_TOUCH_SCL 19
#define PIN_TOUCH_RST 20
#define PIN_TOUCH_INT 21

// Bottom-hint text apps show for how to get back to the home screen -
// differs by board because the physical button's role differs: on this
// board touch handles navigation/actions, so the button is solely a quick
// press (not a hold) to go home; see the non-touch board's branch below
// for the older press-and-hold-for-everything behavior.
#define HOME_HINT "button: home"
// Apps' hint text describes their per-app action as "<ACTION_WORD>: foo"
// (e.g. "tap: refresh") - on this board that action only ever happens via
// touch now, never the physical button (see launcher_handle_button), so
// it reads "tap" rather than "short".
#define ACTION_WORD "tap"

#else

// Waveshare ESP32-C6-LCD-1.47 pin map
// https://www.waveshare.com/wiki/ESP32-C6-LCD-1.47

// ST7789 display (SPI)
#define PIN_LCD_SCLK 7
#define PIN_LCD_MOSI 6
#define PIN_LCD_CS   14
#define PIN_LCD_DC   15
#define PIN_LCD_RST  21
#define PIN_LCD_BL   22

// Panel is a 172x320 window inside the ST7789's 240x320 controller RAM,
// centered horizontally -> offset_x = (240-172)/2 = 34. Verified working
// config, see https://github.com/AlexWHughes/ESP32-C6-Basic-Sketch
#define LCD_PANEL_WIDTH  172
#define LCD_PANEL_HEIGHT 320
#define LCD_OFFSET_X     34
#define LCD_OFFSET_Y     0

// Set to 2 instead of 0 if the display appears upside down for your build.
#define LCD_ROTATION 0

// Onboard NeoPixel (WS2812) RGB LED
#define PIN_RGB_LED 8
#define RGB_LED_COUNT 1

// BOOT button - the only user input on the non-touch board.
// GPIO9 is an ESP32-C6 strapping pin, so this button also selects the boot
// mode on reset. Fine to use as a normal input once the app is running.
#define PIN_BOOT_BUTTON 9

// See the touch board's branch above for why this differs: no touch here,
// so the button is still the only input, same as always (short = per-app
// action, hold = home).
#define HOME_HINT "hold: home"
#define ACTION_WORD "short"

#endif  // BOARD_TOUCH_LCD147

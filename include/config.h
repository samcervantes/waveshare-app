#pragma once

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

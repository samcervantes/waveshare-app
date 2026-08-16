#pragma once

// Initializes the ST7789 panel (via LovyanGFX) and the LVGL core + display
// driver. Call once from setup().
void display_init();

// Pumps LVGL's timer/animation/redraw handler. Call every loop() iteration.
void display_tick();

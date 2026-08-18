#pragma once

#include <stdint.h>

// AXS5106L capacitive touch controller driver (touch board only - see
// config.h's BOARD_TOUCH_LCD147). No-op stubs elsewhere.

// Resets and probes the AXS5106L over I2C. Call once from display_init(),
// after Wire pins are otherwise free. Logs a warning and leaves touch
// non-functional (touch_read always returns false) if the chip doesn't ack.
void touch_init();

// Polls the current touch state. Returns true and fills x/y (in panel
// pixel coordinates, already corrected for this board's mirrored axis)
// if the panel is currently touched; false otherwise.
bool touch_read(int16_t *x, int16_t *y);

#include "touch.h"

#include "config.h"

// Only the touch board wires up an AXS5106L; config.h doesn't define the
// PIN_TOUCH_* pins on the non-touch board, so this whole driver compiles
// to nothing there (see rgb_app.cpp for the same pattern in reverse).
#if defined(BOARD_TOUCH_LCD147)

#include <Arduino.h>
#include <Wire.h>

namespace {

// Protocol reverse-engineered from Waveshare's own AXS5106L C demo (via
// https://github.com/toto04/axs5106l, a Rust port of it for this exact
// board): I2C address 0x63, register 0x08 returns a 3-byte chip ID (valid
// if byte 0 is non-zero), register 0x01 returns 14 bytes of touch data -
// byte 1 is the touch point count, then up to 2 points of 6 bytes each
// starting at byte 2: [x_hi(4b)][x_lo][y_hi(4b)][y_lo][[pressure/reserved].
constexpr uint8_t AXS5106L_ADDR = 0x63;
constexpr uint8_t REG_ID = 0x08;
constexpr uint8_t REG_TOUCH_DATA = 0x01;

bool read_registers(uint8_t reg, uint8_t *buf, size_t len) {
  // Deliberately two full stop/start transactions rather than a
  // repeated-start (endTransmission(false) + requestFrom): arduino-esp32
  // 3.x's newer I2C driver (esp32-hal-i2c-ng) fails combined write-then-
  // read transactions on this chip with ESP_ERR_INVALID_STATE. Splitting
  // them avoids that codepath entirely; the AXS5106L tolerates the bus
  // going idle between the register-address write and the read.
  Wire.beginTransmission(AXS5106L_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(true) != 0) return false;

  if (Wire.requestFrom(static_cast<int>(AXS5106L_ADDR), static_cast<int>(len)) != static_cast<int>(len)) {
    return false;
  }
  for (size_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

}  // namespace

void touch_init() {
  pinMode(PIN_TOUCH_RST, OUTPUT);
  digitalWrite(PIN_TOUCH_RST, LOW);
  delay(200);
  digitalWrite(PIN_TOUCH_RST, HIGH);
  delay(300);

  Wire.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL);
  Wire.setClock(400000);

  uint8_t id[3] = {0};
  if (!read_registers(REG_ID, id, sizeof(id)) || id[0] == 0) {
    Serial.println("[touch] AXS5106L did not ack ID read - touch disabled");
  }
}

bool touch_read(int16_t *x, int16_t *y) {
  uint8_t data[14];
  if (!read_registers(REG_TOUCH_DATA, data, sizeof(data))) return false;
  if (data[1] == 0) return false;

  int16_t raw_x = static_cast<int16_t>(((data[2] & 0x0F) << 8) | data[3]);
  int16_t raw_y = static_cast<int16_t>(((data[4] & 0x0F) << 8) | data[5]);

  // The digitizer's native X axis runs opposite to the panel's, matching
  // display.cpp's LCD_ROTATION=6 mirror fix - flip X to line touch up with
  // what's drawn on screen. Revisit (along with LCD_ROTATION) if taps land
  // on the wrong icon.
  *x = (LCD_PANEL_WIDTH - 1) - raw_x;
  *y = raw_y;

  if (*x < 0) *x = 0;
  if (*x >= LCD_PANEL_WIDTH) *x = LCD_PANEL_WIDTH - 1;
  if (*y < 0) *y = 0;
  if (*y >= LCD_PANEL_HEIGHT) *y = LCD_PANEL_HEIGHT - 1;
  return true;
}

#else

void touch_init() {}
bool touch_read(int16_t *, int16_t *) { return false; }

#endif  // BOARD_TOUCH_LCD147

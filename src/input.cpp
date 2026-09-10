#include "input.h"
#include "config.h"

#include <Arduino.h>

namespace {

constexpr uint32_t DEBOUNCE_MS = 30;
constexpr uint32_t LONG_PRESS_MS = 500;

bool raw_pressed() {
  return digitalRead(PIN_BOOT_BUTTON) == LOW;  // active low, INPUT_PULLUP
}

bool stable_state = false;  // debounced pressed/released state
bool last_raw = false;
uint32_t last_change_ms = 0;
uint32_t press_start_ms = 0;
bool long_press_fired = false;

}  // namespace

void input_init() {
  pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);
  last_raw = raw_pressed();
  stable_state = last_raw;
  last_change_ms = millis();
}

// Plain polled digitalRead(), not an interrupt - which means this only
// ever sees whatever the pin's level happens to be at the moment it's
// called, not a history of transitions. If something else blocks
// loop() (and therefore this call) for long enough, a full quick
// press-then-release can happen entirely in the gap and never register -
// found via gyro_app.cpp's Ball page, whose per-frame render was long
// enough to occasionally do exactly that (see its own comment). Normal
// UI work is short enough this has never otherwise been an issue, but it's
// the reason any future app with a chunky per-frame render should be
// careful about how long a single call can block the main loop for.
ButtonEvent input_poll() {
  bool raw = raw_pressed();
  uint32_t now = millis();

  if (raw != last_raw) {
    last_raw = raw;
    last_change_ms = now;
  }

  if (now - last_change_ms > DEBOUNCE_MS && raw != stable_state) {
    stable_state = raw;
    if (stable_state) {
      press_start_ms = now;
      long_press_fired = false;
    } else if (!long_press_fired) {
      return ButtonEvent::ShortPress;
    }
  }

  if (stable_state && !long_press_fired && (now - press_start_ms >= LONG_PRESS_MS)) {
    long_press_fired = true;
    return ButtonEvent::LongPress;
  }

  return ButtonEvent::None;
}

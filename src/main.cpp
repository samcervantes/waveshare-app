#include <Arduino.h>

#include "display.h"
#include "input.h"
#include "launcher.h"

void setup() {
  Serial.begin(115200);
  Serial.println("\n[main] Waveshare launcher booting");

  display_init();
  input_init();
  launcher_init();
}

void loop() {
  launcher_handle_button(input_poll());
  display_tick();
  delay(5);
}

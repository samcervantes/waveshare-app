#pragma once

// Events produced by the single BOOT button - the only input this
// non-touch board has. See README.md for the interaction model.
enum class ButtonEvent {
  None,
  ShortPress,
  LongPress,
};

// Sets up the BOOT button GPIO. Call once from setup().
void input_init();

// Call every loop() iteration. Returns at most one event: ShortPress fires
// on release (if the hold wasn't long enough to already fire LongPress).
// LongPress fires once, immediately, as soon as the hold crosses the
// threshold - it doesn't wait for release, so it feels responsive.
ButtonEvent input_poll();

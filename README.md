# Waveshare ESP32-C6-LCD-1.47 App Launcher

An iPhone-style home screen for the [Waveshare ESP32-C6-LCD-1.47](https://www.waveshare.com/wiki/ESP32-C6-LCD-1.47)
(non-touch, 172x320 ST7789 display). Two apps per page, page dots at the
bottom, animated swipe-style transitions between pages - all driven by the
board's single BOOT button, since this board has no touchscreen.

## Hardware

- Waveshare ESP32-C6-LCD-1.47, 172x320, ST7789 driver
- Only user input: the **BOOT button** (GPIO9). RESET just restarts the chip.
- Onboard NeoPixel RGB LED (GPIO8), used by the RGB Light demo app.
- Pin map lives in `include/config.h`.

## One-button interaction model

There's no touchscreen and no second button, so navigation is built around
short vs. long presses of BOOT:

**On the home screen:**
- **Short press** - move the highlight to the next app (wraps around; slides
  to the next page with an animation when it crosses a page boundary).
- **Long press** (~500ms) - launch the highlighted app.

**Inside an app:**
- **Short press** - whatever that app defines (see each app's on-screen hint).
- **Long press** - go back to the home screen.

## Building & flashing

Requires [PlatformIO](https://platformio.org/) (`pipx install platformio`).

```sh
pio run                                    # compile
pio device list                            # find the board's serial port
pio run -t upload --upload-port /dev/cu.usbmodemXXXX
```

If you only have one board plugged in, `pio run -t upload` will usually
find the port automatically and you can drop `--upload-port`.

Serial logs (`pio device monitor`) print launcher state (which app opened/
closed) at 115200 baud - handy for debugging new apps. Note: this board's
USB-serial is a native USB-CDC port, so it briefly re-enumerates on reset;
if the monitor shows nothing, reconnect it after the board has settled.

### If the screen looks wrong

- **Upside down** - flip `LCD_ROTATION` in `include/config.h` from `0` to `2`.
- **Colors look off/inverted** - toggle `LV_COLOR_16_SWAP` in
  `include/lv_conf.h` (0 -> 1) and/or `pcfg.invert` in `src/display.cpp`.
- **Image shifted / black bars** - the panel sits inside a wider 240px-wide
  controller RAM window; `LCD_OFFSET_X` in `config.h` centers it. This is
  already set to the verified value (34) for this exact board.

## Adding a new app

This is the part meant for "vibe coding" - see `CLAUDE.md` for the full
walkthrough aimed at an AI assistant. Short version:

1. Copy `src/apps/counter_app.cpp` and `.h` to `src/apps/your_app.cpp/.h`,
   rename `counter_app` to `your_app` throughout.
2. Write your UI in `on_open(lv_obj_t *parent)`, clean up timers/hardware in
   `on_close()`, and optionally handle `on_short_press()`.
3. Register it in `src/app_registry.cpp` (add the include + one array entry).
4. `pio run -t upload`.

The launcher automatically adds a new icon/page for it - no layout code to
touch, up to 16 apps (see `MAX_APPS` in `src/launcher.cpp`).

## Project layout

```
include/            Public headers: config.h (pins), app_interface.h,
                     app_registry.h, display.h, input.h, launcher.h, lv_conf.h
src/
  main.cpp           setup()/loop() glue
  display.cpp         LovyanGFX (ST7789) + LVGL driver glue
  input.cpp            BOOT button short/long press state machine
  launcher.cpp          Home screen: paging, highlight, app open/close
  app_registry.cpp       The list of installed apps
  apps/
    clock_app.cpp/.h        Uptime digital clock
    rgb_app.cpp/.h           NeoPixel color cycler
    counter_app.cpp/.h        Tally counter
    bounce_app.cpp/.h          DVD-logo-style bouncing ball
    flappy_app.cpp/.h          Flappy-Bird-style single-button game
    reflex_app.cpp/.h           Timing/reflex game: stop the sweeping bar in the zone
```

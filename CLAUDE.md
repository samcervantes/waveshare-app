# Waveshare ESP32-C6 Touch Display launcher - notes for Claude

This is a PlatformIO/Arduino firmware project: an iPhone-like app launcher.
The primary target hardware is the **Waveshare ESP32-C6 1.47" Touch
Display** (172x320, JD9853 driver, AXS5106L touch controller) - tap icons
to open apps, tap/swipe within an app for its per-app action, BOOT button
for a quick press-to-go-home. The codebase also still supports the older
plain (non-touch) Waveshare ESP32-C6-LCD-1.47 board via a second
PlatformIO environment - see the "Two board variants" section below. Read
`README.md` first for the user-facing overview.

## Adding a new app - the expected workflow when the user asks for one

1. Copy `src/apps/stopwatch_app.cpp` and `src/apps/stopwatch_app.h` to
   `src/apps/<name>_app.cpp/.h`. Rename `stopwatch_app` -> `<name>_app`
   everywhere (the file-scope `namespace {}` functions can keep their
   generic names like `on_open`/`on_close`/`on_short_press` - they're
   already scoped per translation unit).
2. Build the UI in `on_open(lv_obj_t *parent)` using LVGL calls, e.g.
   `lv_label_create(parent)`, `lv_obj_create(parent)`. `parent` is a fresh
   172x320 full-screen container the launcher gives you; don't create your
   own screen.
3. If you start an `lv_timer_t` or touch hardware (GPIO, NeoPixel, etc.) in
   `on_open`, tear it down in `on_close()` - the launcher calls `on_close()`
   *before* deleting `parent` and its LVGL children, so `on_close` only
   needs to handle things that outlive widget deletion (timers, peripheral
   state). See `rgb_app.cpp` (turns the LED off) and `clock_app.cpp`/
   `bounce_app.cpp` (delete their `lv_timer_t`) for the pattern.
4. `on_short_press` is optional (set to `nullptr` if unused) - it's the
   per-app gesture, since long press is globally reserved for "go home".
5. Register the new `AppDescriptor` in `src/app_registry.cpp`: add the
   `#include "apps/<name>_app.h"` and one line in the `app_registry[]`
   array. That's the only place layout/paging code needs to change - the
   launcher (`src/launcher.cpp`) builds pages/icons/dots dynamically from
   `APP_COUNT`, up to `MAX_APPS = 16` (8 pages of 2).
6. Build with `pio run -e waveshare-esp32c6-touch-lcd147`. Flash with
   `pio run -e waveshare-esp32c6-touch-lcd147 -t upload --upload-port $(pio device list | grep -o '/dev/cu.usbmodem[0-9]*' | head -1)`
   (or drop `--upload-port` if only one board is plugged in). **Always
   pass `-e waveshare-esp32c6-touch-lcd147` explicitly** - `pio run`/
   `pio run -t upload` with no `-e` builds and flashes *both* board
   environments back-to-back onto whatever's plugged in, and the
   non-touch env that lands last uses different display pins (wrong
   SCLK/MOSI/RST/backlight GPIOs for this board), leaving the screen
   dark. This has actually happened - see the "Two board variants"
   section below.

Icons use `LV_SYMBOL_*` macros (built into the Montserrat fonts LVGL ships)
rather than custom bitmaps/emoji - there's no real icon set for this
project, so pick whichever symbol reads closest to the app's purpose (see
existing apps for examples: `LV_SYMBOL_LOOP`, `LV_SYMBOL_CHARGE`,
`LV_SYMBOL_PLUS`, `LV_SYMBOL_SHUFFLE`).

**Text color: default to `lv_color_white()` for anything the user needs to
read normally** (headings, body/status text, list items). The screen
background is black, and dark/muted colors on it - `0x555555`-style grays,
but also darker saturated colors like a deep red (`0xBB1919`) - read as
low-contrast and hard to see on this specific display, not just "a bit
dim." This has been raised as a repeated problem, so don't reach for a
dim color for primary content text; save dim grays (the existing
`0x555555` hint-text convention, e.g. `HOME_HINT` labels) for genuinely
secondary/de-emphasized text only, and when in doubt, use white.

## Architecture

```
input.cpp    -> ButtonEvent (ShortPress/LongPress) from the BOOT button (GPIO9)
launcher.cpp -> owns the LVGL screen; routes ButtonEvent to either the
                home-screen cursor/paging, or the active app's on_short_press
                / global "long press = home"
app_registry.cpp -> static list of AppDescriptor* (src/apps/*)
display.cpp  -> LovyanGFX (JD9853/ST7789) init + LVGL flush callback glue
touch.cpp    -> AXS5106L touch controller driver (touch board only)
```

`AppDescriptor` (in `include/app_interface.h`) is the entire plugin
contract: `name`, `icon_symbol`, `icon_color`, `on_open`, `on_close`,
`on_short_press`. Nothing else needs to know an app exists besides the one
line in `app_registry.cpp`.

## Two board variants

`platformio.ini` defines two environments, selected by `-e`:

- `waveshare-esp32c6-touch-lcd147` - **the primary target, and the board
  actually in use.** AXS5106L capacitive touch (I2C) is the main
  interface; `-DBOARD_TOUCH_LCD147` gates touch-specific code
  (`src/touch.cpp`, `whack_app`, the touch overlay/gesture handling in
  `launcher.cpp`). No onboard NeoPixel, so `rgb_app` is excluded (see
  `app_registry.cpp`).
- `waveshare-esp32c6-lcd147` - the older plain (non-touch) board. Still
  supported by the shared codebase, kept building/working, but not what's
  physically plugged in for this project - don't assume it's the default.

The two boards' display pins differ (touch: SCLK1/MOSI2/RST22/BL23;
non-touch: SCLK7/MOSI6/RST21/BL22 - see `include/config.h`), which is why
flashing the wrong environment doesn't just misbehave, it leaves the
screen dark (wrong backlight GPIO) - always pass `-e
waveshare-esp32c6-touch-lcd147` explicitly rather than relying on
`pio run`'s default of building/uploading every environment.

## Hardware facts worth remembering

- Input: the AXS5106L touch controller (I2C, pins in `config.h`) for the
  primary tap/swipe interface, plus the BOOT button on GPIO9
  (`PIN_BOOT_BUTTON`) as a physical quick-press-home. RESET is a hard
  reset, not software-readable.
- GPIO8 is the NeoPixel RGB LED data pin *on the non-touch board only* -
  on the touch board GPIO8 is unrelated (no onboard NeoPixel there).
  Easy to confuse with GPIO9 on the non-touch board - they're adjacent
  strapping pins on the ESP32-C6.
- Display pins/offsets in `config.h` (touch board:
  SCLK1/MOSI2/CS14/DC15/RST22/BL23, `offset_x=34`) were confirmed against
  a working reference for this exact board - don't change them without
  testing on hardware, ST7789/JD9853 offset values are notoriously
  board-specific.
- 512KB SRAM total, no PSRAM. LVGL uses partial draw buffers (40 rows), not
  a full framebuffer - keep that pattern if you touch `display.cpp`.

## PlatformIO / toolchain gotchas hit while setting this up

- **The official `platformio/platform-espressif32` platform does not list
  `arduino` as a supported framework for any ESP32-C6 board** (espidf
  only, as of platform 6.13.0) - `pio run` fails immediately with "This
  board doesn't support arduino framework!". Fix already applied in
  `platformio.ini`: use the community `pioarduino` fork instead
  (`platform = https://github.com/pioarduino/platform-espressif32/releases/download/stable/platform-espressif32.zip`).
  If PlatformIO's official platform ever adds native Arduino support for
  C6, switching back is an option, but there's no need to chase that.
- **`lv_conf.h` isn't found when building the `lvgl` library itself**
  unless the project's `include/` dir is explicitly added to
  `build_flags` (`-Iinclude`) - PlatformIO's automatic project-include-dir
  propagation doesn't reach library compilation in this LDF mode. Already
  set in `platformio.ini`; don't remove it.
- This project pins **LVGL 8.3.x/8.4.x** (`lv_conf.h` is the 8.4.0
  template). LVGL 9 has a different, incompatible API - don't upgrade the
  `lvgl` dependency without rewriting `display.cpp`/`launcher.cpp`/apps.
  Some LVGL 8 API gaps to know about: there's no
  `lv_obj_set_style_margin_hor/left/right` - use
  `lv_obj_set_style_pad_column`/`pad_row` on the flex container instead for
  gaps between children.
- The board's serial port shows up as a native **USB-CDC** device (e.g.
  `/dev/cu.usbmodemXXXX` on macOS), not a UART bridge. It re-enumerates on
  reset, so scripted serial capture (e.g. via raw pyserial without a
  terminal) is unreliable right after a reset/flash - `pio device monitor`
  in an actual terminal works fine, but don't rely on quick automated
  serial reads to verify a flash immediately after upload.
- **`platformio.ini` uses `board_build.partitions = huge_app.csv`** (single
  ~3MB app partition, no OTA) instead of the default OTA-capable partition
  table (two ~1.3MB app slots). Adding `WiFi.h` (`wifi_app.cpp`) alone
  pulled in enough of the TLS/lwIP/network stack to blow past the default
  1.3MB slot even though the app barely uses WiFi. If a future app adds
  another heavy library and hits "program size ... greater than maximum
  allowed" again, this is already about as roomy as it gets on this
  board's 4MB flash - the fix at that point is trimming dependencies, not
  the partition table.

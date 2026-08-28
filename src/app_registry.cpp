#include "app_registry.h"

#include "apps/birds_app.h"
#include "apps/bluetooth_app.h"
#include "apps/bounce_app.h"
#include "apps/clock_app.h"
#include "apps/counter_app.h"
#include "apps/hn_app.h"
#include "apps/photos_app.h"
#include "apps/pong_app.h"
#include "apps/reflex_app.h"
#include "apps/snake_app.h"
#include "apps/stack_app.h"
#include "apps/stock_app.h"
#include "apps/stopwatch_app.h"
#include "apps/wifi_app.h"

#ifndef BOARD_TOUCH_LCD147
// The touch board has no onboard NeoPixel (GPIO8 is its BOOT button
// instead) - rgb_app is a no-op on that build, see rgb_app.cpp.
#include "apps/rgb_app.h"
#else
// Whack cares where on screen a tap landed (via lv_indev_get_point), which
// only means anything with a pointer indev registered - touch board only.
#include "apps/whack_app.h"
#endif

// Launcher order: page 1 = {clock, rgb}, page 2 = {counter, bounce},
// page 3 = {pong, reflex}, page 4 = {stopwatch, wifi}, page 5 =
// {photos, bluetooth}, page 6 = {stack, stocks}, page 7 = {hn, snake},
// page 8 = {birds}. (rgb is skipped on the touch board, which has no
// NeoPixel; whack is touch board only, since it needs tap position.)
// To add an app: write src/apps/your_app.cpp/.h (copy counter_app as a
// template), then add it here.
const AppDescriptor *const app_registry[] = {
    &clock_app,
#ifndef BOARD_TOUCH_LCD147
    &rgb_app,
#endif
    &counter_app,
    &bounce_app,
    &pong_app,
    &reflex_app,
    &stopwatch_app,
    &wifi_app,
    &photos_app,
#if defined(BOARD_TOUCH_LCD147)
    &whack_app,
#endif
    &bluetooth_app,
    &stack_app,
    &stock_app,
    &hn_app,
    &snake_app,
    &birds_app,
};

const size_t APP_COUNT = sizeof(app_registry) / sizeof(app_registry[0]);

#include "app_registry.h"

#include "apps/birds_app.h"
#include "apps/bluetooth_app.h"
#include "apps/bounce_app.h"
#include "apps/breathe_app.h"
#include "apps/clock_app.h"
#include "apps/hn_app.h"
#include "apps/photos_app.h"
#include "apps/pong_app.h"
#include "apps/stack_app.h"
#include "apps/stock_app.h"
#include "apps/stopwatch_app.h"
#include "apps/wifi_app.h"

#ifndef BOARD_TOUCH_LCD147
// The touch board has no onboard NeoPixel (GPIO8 is its BOOT button
// instead) - rgb_app is a no-op on that build, see rgb_app.cpp.
#include "apps/rgb_app.h"
#endif

// Launcher order: page 1 = {clock, rgb}, page 2 = {bounce, breathe},
// page 3 = {pong, stopwatch}, page 4 = {wifi, photos}, page 5 =
// {bluetooth, stack}, page 6 = {stock, hn}, page 7 = {birds}. (rgb is
// skipped on the touch board, which has no NeoPixel, so the touch
// board's pairings shift by one from the above.)
// To add an app: write src/apps/your_app.cpp/.h (copy stopwatch_app as a
// template), then add it here.
const AppDescriptor *const app_registry[] = {
    &clock_app,
#ifndef BOARD_TOUCH_LCD147
    &rgb_app,
#endif
    &bounce_app,
    &breathe_app,
    &pong_app,
    &stopwatch_app,
    &wifi_app,
    &photos_app,
    &bluetooth_app,
    &stack_app,
    &stock_app,
    &hn_app,
    &birds_app,
};

const size_t APP_COUNT = sizeof(app_registry) / sizeof(app_registry[0]);

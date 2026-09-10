#include "app_registry.h"

#include "apps/birds_app.h"
#include "apps/bluetooth_app.h"
#include "apps/bounce_app.h"
#include "apps/breathe_app.h"
#include "apps/clock_app.h"
#include "apps/hn_app.h"
#ifdef BOARD_TOUCH_LCD147
// The IMU (imu.cpp) and battery-sense ADC pin (config.h's PIN_BAT_ADC)
// only exist on the touch board - see each app's own file header comment
// - so gyro_app/battery_app are excluded on the non-touch board the same
// way rgb_app is excluded here.
#include "apps/battery_app.h"
#include "apps/gyro_app.h"
#endif
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
// {bluetooth, stack}, page 6 = {stock, hn}, page 7 = {birds, gyro},
// page 8 = {battery}. (rgb, gyro, and battery are each excluded on one
// board variant - rgb needs the non-touch board's NeoPixel, gyro/battery
// need the touch board's IMU/battery-sense ADC - so the two boards'
// pairings shift relative to each other from that point on.)
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
#ifdef BOARD_TOUCH_LCD147
    &gyro_app,
    &battery_app,
#endif
};

const size_t APP_COUNT = sizeof(app_registry) / sizeof(app_registry[0]);

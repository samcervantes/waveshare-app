#include "app_registry.h"

#include "apps/bounce_app.h"
#include "apps/clock_app.h"
#include "apps/counter_app.h"
#include "apps/flappy_app.h"
#include "apps/reflex_app.h"
#include "apps/rgb_app.h"
#include "apps/stopwatch_app.h"
#include "apps/wifi_app.h"

// Launcher order: page 1 = {clock, rgb}, page 2 = {counter, bounce},
// page 3 = {flappy, reflex}, page 4 = {stopwatch, wifi}.
// To add an app: write src/apps/your_app.cpp/.h (copy counter_app as a
// template), then add it here.
const AppDescriptor *const app_registry[] = {
    &clock_app,
    &rgb_app,
    &counter_app,
    &bounce_app,
    &flappy_app,
    &reflex_app,
    &stopwatch_app,
    &wifi_app,
};

const size_t APP_COUNT = sizeof(app_registry) / sizeof(app_registry[0]);

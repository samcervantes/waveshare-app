#pragma once

#include "input.h"

// Builds the home screen (app grid + page dots) as the LVGL active screen.
// Call once from setup(), after display_init().
void launcher_init();

// Routes a button event to whichever is in the foreground: the launcher's
// cursor/paging when on the home screen, or the active app's on_short_press
// / the global "long press = go home" behavior when an app is open.
void launcher_handle_button(ButtonEvent event);

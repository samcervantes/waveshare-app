#pragma once

#include <cstddef>

#include "app_interface.h"

// All installed apps, in launcher display order. Defined in
// src/app_registry.cpp - add a new app by listing it there.
extern const AppDescriptor *const app_registry[];
extern const size_t APP_COUNT;

#pragma once

#include <stdint.h>

// #define, not constexpr - this header is included from earth.c (compiled
// as plain C by PlatformIO's toolchain, based on the .c extension) as well
// as gyro_app.cpp.
#define EARTH_TEX_W 128
#define EARTH_TEX_H 64

// Public-domain NASA equirectangular earth map (Blue Marble family),
// via https://commons.wikimedia.org/wiki/File:Equirectangular-projection.jpg
// - a work of the US federal government, not protected by copyright, same
// as photos_app's CC0 photos (no attribution required). Downsampled to
// 128x64 and stored as flat RGB888 triples (not LVGL's lv_img_dsc_t format -
// this is sampled manually as a texture in gyro_app.cpp's sphere renderer,
// not displayed directly).
extern const uint8_t EARTH_TEX_RGB[24576];

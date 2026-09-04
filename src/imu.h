#pragma once

// QMI8658A 6-axis IMU driver (touch board only - see config.h's
// BOARD_TOUCH_LCD147). No-op stubs elsewhere. Shares the AXS5106L touch
// controller's I2C bus (PIN_TOUCH_SDA/SCL in config.h).

struct ImuSample {
  float accel_x, accel_y, accel_z;  // g
  float gyro_x, gyro_y, gyro_z;     // deg/s
};

// Resets and configures the QMI8658A for continuous accel+gyro sampling.
// Call once from display_init(), after touch_init() - reuses the Wire bus
// touch_init() already began rather than calling Wire.begin() again. Logs
// a warning and leaves the IMU non-functional (imu_read always returns
// false) if the chip doesn't ack.
void imu_init();

// Reads the latest accel (g) + gyro (deg/s) sample. Returns false if the
// IMU isn't present/initialized.
bool imu_read(ImuSample *out);

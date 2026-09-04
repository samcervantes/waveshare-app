#include "imu.h"

#include "config.h"

// Only the touch board wires up a QMI8658A; config.h doesn't define the
// PIN_TOUCH_* pins on the non-touch board, so this whole driver compiles
// to nothing there (same pattern as touch.cpp).
#if defined(BOARD_TOUCH_LCD147)

#include <Arduino.h>
#include <Wire.h>

// Register map and init sequence taken from Waveshare's own QMI8658 demo
// for this exact board (FastIMU's F_QMI8658.cpp, bundled in
// https://files.waveshare.com/wiki/ESP32-C6-Touch-LCD-1.47/ESP32-C6-Touch-LCD-1.47-Demo.zip)
// rather than the general QST datasheet, so these exact register values
// are confirmed against Waveshare's own working example for this board.
namespace {

constexpr uint8_t QMI8658_ADDR = 0x6B;
constexpr uint8_t REG_WHO_AM_I = 0x00;
constexpr uint8_t WHO_AM_I_VALUE = 0x05;
constexpr uint8_t REG_CTRL1 = 0x02;
constexpr uint8_t REG_CTRL2 = 0x03;
constexpr uint8_t REG_CTRL3 = 0x04;
constexpr uint8_t REG_CTRL5 = 0x06;
constexpr uint8_t REG_CTRL7 = 0x08;
constexpr uint8_t REG_RESET = 0x60;
constexpr uint8_t REG_AX_L = 0x35;  // 12-byte burst: AX,AY,AZ,GX,GY,GZ (2 bytes each, LE)

// +-16g accel / +-2048dps gyro full scale - matches the CTRL2/CTRL3 values
// written in imu_init() below.
constexpr float ACCEL_RES = 16.0f / 32768.0f;   // g per LSB
constexpr float GYRO_RES = 2048.0f / 32768.0f;  // deg/s per LSB

bool imu_present = false;

// Deliberately two full stop/start I2C transactions rather than a
// repeated-start, same reasoning as touch.cpp's read_registers (the
// AXS5106L and QMI8658A share this bus, and arduino-esp32 3.x's I2C driver
// fails combined write-then-read transactions on this bus).
bool read_registers(uint8_t reg, uint8_t *buf, size_t len) {
  Wire.beginTransmission(QMI8658_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(true) != 0) return false;

  if (Wire.requestFrom(static_cast<int>(QMI8658_ADDR), static_cast<int>(len)) != static_cast<int>(len)) {
    return false;
  }
  for (size_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

void write_register(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(QMI8658_ADDR);
  Wire.write(reg);
  Wire.write(value);
  Wire.endTransmission(true);
}

}  // namespace

void imu_init() {
  // Wire.begin() already happened in touch_init() (shared bus) - just probe.
  uint8_t who = 0;
  if (!read_registers(REG_WHO_AM_I, &who, 1) || who != WHO_AM_I_VALUE) {
    Serial.println("[imu] QMI8658A did not ack WHO_AM_I - IMU disabled");
    imu_present = false;
    return;
  }

  write_register(REG_RESET, 0xFF);
  delay(100);
  write_register(REG_CTRL1, 0x40);  // enable address auto-increment (needed for the burst read below)
  write_register(REG_CTRL2, 0x34);  // accel: +-16g, 500Hz ODR
  write_register(REG_CTRL3, 0x74);  // gyro: +-2048dps, 500Hz ODR
  write_register(REG_CTRL5, 0x55);  // low-pass filter, both accel and gyro
  write_register(REG_CTRL7, 0x03);  // enable accel + gyro
  delay(100);

  imu_present = true;
}

bool imu_read(ImuSample *out) {
  if (!imu_present) return false;

  uint8_t raw[12];
  if (!read_registers(REG_AX_L, raw, sizeof(raw))) return false;

  int16_t ax = static_cast<int16_t>((raw[1] << 8) | raw[0]);
  int16_t ay = static_cast<int16_t>((raw[3] << 8) | raw[2]);
  int16_t az = static_cast<int16_t>((raw[5] << 8) | raw[4]);
  int16_t gx = static_cast<int16_t>((raw[7] << 8) | raw[6]);
  int16_t gy = static_cast<int16_t>((raw[9] << 8) | raw[8]);
  int16_t gz = static_cast<int16_t>((raw[11] << 8) | raw[10]);

  out->accel_x = ax * ACCEL_RES;
  out->accel_y = ay * ACCEL_RES;
  out->accel_z = az * ACCEL_RES;
  out->gyro_x = gx * GYRO_RES;
  out->gyro_y = gy * GYRO_RES;
  out->gyro_z = gz * GYRO_RES;
  return true;
}

#else

void imu_init() {}
bool imu_read(ImuSample *) { return false; }

#endif  // BOARD_TOUCH_LCD147

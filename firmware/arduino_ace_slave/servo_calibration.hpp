#pragma once

#include <Arduino.h>

struct ServoProfile {
  const char* name;
  uint16_t minPulseUs;
  uint16_t maxPulseUs;
  uint8_t minAngleDeg;
  uint8_t maxAngleDeg;
  uint8_t homeAngleDeg;
};

// Keep calibration data separate from drive logic. FS90 uses datasheet pulse
// width/range values; MG996R remains a placeholder until measured on the bench.
constexpr ServoProfile DEFAULT_SERVO_PROFILE = {"GENERIC_180", 600, 2400, 0, 180, 90};
constexpr ServoProfile FS90_SERVO_PROFILE = {"FS90", 900, 2100, 0, 120, 60};
constexpr ServoProfile MG996R_SERVO_PROFILE = {"MG996R", 500, 2500, 0, 180, 90};

inline void loadServoCalibration(ServoProfile profiles[], uint8_t channelCount) {
  for (uint8_t channel = 0; channel < channelCount; ++channel) {
    profiles[channel] = DEFAULT_SERVO_PROFILE;
  }

  if (channelCount > 0) {
    profiles[0] = FS90_SERVO_PROFILE;
  }
  if (channelCount > 1) {
    profiles[1] = MG996R_SERVO_PROFILE;
  }
}

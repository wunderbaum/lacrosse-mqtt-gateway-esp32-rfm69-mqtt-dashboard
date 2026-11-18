#pragma once
#include <stdint.h>
#include <stdbool.h>

struct LaCrosseReading {
  uint8_t id = 0;
  float temperature = 0.0f;
  uint8_t humidity = 0;
  bool lowBattery = false;
  bool newBattery = false;
};

// Decode a five-byte LaCrosse frame and verify its header, BCD temperature,
// supported humidity range, and CRC-8. Returns false for invalid frames.
bool decodeLaCrosseFrame(const uint8_t *bytes, LaCrosseReading &reading);

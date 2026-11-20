#include "LaCrosseDecoder.h"

namespace {
uint8_t crc8(const uint8_t *data, uint8_t length) {
  uint8_t crc = 0;
  for (uint8_t j = 0; j < length; ++j) {
    uint8_t value = data[j];
    for (uint8_t i = 0; i < 8; ++i) {
      const bool mix = ((crc ^ value) & 0x80) != 0;
      crc <<= 1;
      if (mix) crc ^= 0x31;
      value <<= 1;
    }
  }
  return crc;
}
bool isBcd(uint8_t n) { return n <= 9; }
constexpr uint8_t NO_HUMIDITY = 0x6a;
}

bool decodeLaCrosseFrame(const uint8_t *bytes, LaCrosseReading &reading) {
  if (bytes == nullptr || (bytes[0] >> 4) != 9 || bytes[4] != crc8(bytes, 4)) return false;
  const uint8_t hundreds = bytes[1] & 0x0f;
  const uint8_t tens = (bytes[2] >> 4) & 0x0f;
  const uint8_t ones = bytes[2] & 0x0f;
  if (!isBcd(hundreds) || !isBcd(tens) || !isBcd(ones)) return false;

  const uint16_t tenths = uint16_t(hundreds) * 100 + uint16_t(tens) * 10 + ones;
  const float temperature = tenths / 10.0f - 40.0f;
  const uint8_t humidity = bytes[3] & 0x7f;
  const bool hasHumidity = humidity != NO_HUMIDITY;
  if (temperature < -40.0f || temperature > 60.0f || (hasHumidity && humidity > 100)) return false;

  reading.id = uint8_t(((bytes[0] & 0x0f) << 2) | ((bytes[1] & 0xc0) >> 6));
  reading.temperature = temperature;
  reading.humidity = hasHumidity ? humidity : 0;
  reading.hasHumidity = hasHumidity;
  reading.lowBattery = (bytes[3] & 0x80) != 0;
  reading.newBattery = (bytes[1] & 0x20) != 0;
  return true;
}

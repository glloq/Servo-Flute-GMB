#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

inline bool isJsonInteger(JsonVariantConst value) {
  return value.is<int>() || value.is<long>() || value.is<unsigned int>() || value.is<unsigned long>();
}

inline uint8_t clampMidi7Bit(int value) {
  return (uint8_t)constrain(value, 0, 127);
}

inline uint8_t clampPercent(int value) {
  return (uint8_t)constrain(value, 0, 100);
}

inline uint16_t clampServoAngle(int value) {
  return (uint16_t)constrain(value, 0, 180);
}

inline uint8_t getMidi7Bit(JsonDocument& doc, const char* key, uint8_t def = 0) {
  JsonVariantConst value = doc[key];
  return isJsonInteger(value) ? clampMidi7Bit(value.as<int>()) : def;
}

inline uint8_t getPercent(JsonDocument& doc, const char* key, uint8_t def = 0) {
  JsonVariantConst value = doc[key];
  return isJsonInteger(value) ? clampPercent(value.as<int>()) : def;
}

inline uint16_t getServoAngle(JsonDocument& doc, const char* key, uint16_t def = 0) {
  JsonVariantConst value = doc[key];
  return isJsonInteger(value) ? clampServoAngle(value.as<int>()) : def;
}

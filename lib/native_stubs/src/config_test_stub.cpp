#include "ConfigStorage.h"
RuntimeConfig cfg{};

// Native test stub: the real ConfigStorage.cpp (LittleFS/ArduinoJson) is excluded
// from the host build, but AutoCalibrator::applyResults() persists via save().
// The result is settable so tests can exercise the storage-failure path.
bool __config_save_result = true;
int __config_save_calls = 0;
bool ConfigStorage::save() { __config_save_calls++; return __config_save_result; }

// saveFrom() persists a CANDIDATE without touching the active `cfg`. The host
// stub records the candidate so a test can assert what was actually offered to
// the flash, and reuses __config_save_result/__config_save_calls so an existing
// test that forces a storage failure covers this path too.
RuntimeConfig __config_last_saved{};
bool ConfigStorage::saveFrom(const RuntimeConfig& source) {
  __config_save_calls++;
  __config_last_saved = source;
  return __config_save_result;
}

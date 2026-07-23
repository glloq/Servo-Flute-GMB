#include "ConfigStorage.h"
RuntimeConfig cfg{};

// Native test stub: the real ConfigStorage.cpp (LittleFS/ArduinoJson) is excluded
// from the host build, but AutoCalibrator::applyResults() persists via save().
// The result is settable so tests can exercise the storage-failure path.
bool __config_save_result = true;
int __config_save_calls = 0;
bool ConfigStorage::save() { __config_save_calls++; return __config_save_result; }
